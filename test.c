/*
 * Libusb Bulk Endpoint Test for M-Stack
 *
 * This file may be used by anyone for any purpose and may be used as a
 * starting point making your own application using M-Stack.
 *
 * It is worth noting that M-Stack itself is not under the same license as
 * this file.  See the top-level README.txt for more information.
 *
 * M-Stack is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  For details, see sections 7, 8, and 9
 * of the Apache License, version 2.0 which apply to this file.  If you have
 * purchased a commercial license for this software from Signal 11 Software,
 * your commerical license superceeds the information in this header.
 *
 * Alan Ott
 * Signal 11 Software
 * 2013-04-09

 */

/* C */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <locale.h>
#include <errno.h>
#include <stdbool.h>

/* Unix */
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <fcntl.h>
#include <pthread.h>
#include <wchar.h>

/* GNU / LibUSB */
#include "libusb.h"

#include <sys/time.h>
#include <signal.h>
unsigned int packets[2][4];
unsigned long long bytes;
struct timeval start;
int seq = -1;

int buflen = 64; /* Overwritten by command line param */


void int_handler(int signal) {
	struct timeval end;
	struct timeval res;
	double elapsed;
	unsigned send, ep;
	
	gettimeofday(&end, NULL);

	timersub(&end, &start, &res);
	elapsed = res.tv_sec + (double) res.tv_usec / 1000000.0;
	
	printf("\nelapsed: %.6f seconds\n", elapsed);
	for(send = 0 ; send < 2 ; ++send) {
		for(ep = 1 ; ep < 5 ; ++ep) {
			printf("packets: ep%d-%s %u\n",
					ep, (send?"out":"in"),
					packets[send][ep-1]);
			printf("packets/sec: ep%d-%s %f\n",
					ep, (send?"out":"in"),
					(double)packets[send][ep-1]/elapsed);
		}
	}
	printf("bytes/sec: %f\n", (double)bytes/elapsed);
	printf("MBit/sec: %f\n", (double)bytes/elapsed * 8 / 1000000);

	exit(1);
}

static void read_callback(struct libusb_transfer *transfer)
{
	int res, i;
	
	if (transfer->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
		unsigned send = (transfer->endpoint & 0x80 ? 0 : 1);
		unsigned ep = (transfer->endpoint & 0xF) - 1;

		for (i = 0; i < transfer->num_iso_packets; i++) {
			struct libusb_iso_packet_descriptor *pack = &transfer->iso_packet_desc[i];

			if (pack->status != LIBUSB_TRANSFER_COMPLETED)
				fprintf(stderr, "Error: pack %u status %d\n", i, pack->status);
			else {
				packets[send][ep]++;
				bytes += pack->actual_length;
			}
		}
	}
//	printf("Read callback\n");
	else if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
		unsigned char pkt_seq = transfer->buffer[0];
		unsigned send = (transfer->endpoint & 0x80 ? 0 : 1);
		unsigned ep = (transfer->endpoint & 0xF) - 1;

		packets[send][ep]++;
		bytes += transfer->actual_length;
		
		seq = pkt_seq;
	}
	else if (transfer->status == LIBUSB_TRANSFER_CANCELLED) {
		printf("Cancelled\n");
		return;
	}
	else if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
		printf("No Device\n");
		return;
	}
	else if (transfer->status == LIBUSB_TRANSFER_TIMED_OUT) {
		printf("Timeout (normal)\n");
	}
	else {
		printf("Unknown transfer code: %d\n", transfer->status);
	}

	/* Re-submit the transfer object. */
	res = libusb_submit_transfer(transfer);
	if (res != 0) {
		printf("Unable to submit URB. libusb error code: %d\n", res);
	}
}


static struct libusb_transfer *create_transfer(libusb_device_handle *handle, size_t length, bool out, unsigned ep)
{
	struct libusb_transfer *transfer;
	unsigned char *buf;

	/* Set up the transfer object. */
	if (ep == 1) { // iso
		buf = calloc(1, length*16);
		transfer = libusb_alloc_transfer(16);
		if (out) {
			/* ISO OUT */
			libusb_fill_iso_transfer(transfer,
				handle,
				ep & 0xF /*ep*/,
				buf,
				length*16,
				16,
				read_callback,
				NULL/*cb data*/,
				5000/*timeout*/);
		}
		else {
			/* ISO IN */
			libusb_fill_iso_transfer(transfer,
				handle,
				0x80 | (ep & 0xF) /*ep*/,
				buf,
				length*16,
				16,
				read_callback,
				NULL/*cb data*/,
				5000/*timeout*/);
		}
		libusb_set_iso_packet_lengths(transfer,
				length);
	} else {
		buf = calloc(1, length);
		transfer = libusb_alloc_transfer(0);
		if (out) {
			/* Bulk OUT */
			libusb_fill_bulk_transfer(transfer,
				handle,
				ep & 0xF /*ep*/,
				buf,
				length,
				read_callback,
				NULL/*cb data*/,
				5000/*timeout*/);
		}
		else {
			/* Bulk IN */
			libusb_fill_bulk_transfer(transfer,
				handle,
				0x80 | (ep & 0xF) /*ep*/,
				buf,
				length,
				read_callback,
				NULL/*cb data*/,
				5000/*timeout*/);
		}
	}
	return transfer;
}

static libusb_context *usb_context = NULL;

int main(int argc, char **argv)
{
	libusb_device_handle *handle;
	int i;
	int res = 0;
	int c;
	unsigned ep = 1;
	bool sync = false;
	bool show_help = false;
	bool send = false;

	/* Parse options */
	while ((c = getopt(argc, argv, "siohl:e:")) > 0) {
		switch (c) {
		case 's':
			sync = true;
			break;
		case 'l':
			buflen = atoi(optarg);
			if (buflen <= 0) {
				fprintf(stderr, "Invalid length\n");
				return 1;
			}
			break;
		case 'o':
			send = true;
			break;
		case 'h':
			show_help = true;
			break;
		case 'e':
			ep = atoi(optarg);
			break;
		default:
			printf("Invalid option -%c\n", c);
			show_help = true;
			break;
		};

		if (show_help)
			break;
	}
	if (optind < argc && !show_help) {
		fprintf(stderr, "Invalid arguments:\n");
		while (optind < argc)
			fprintf(stderr, "\t%s", argv[optind++]);
		printf("\n");

		return 1;
	}
	
	if (show_help) {
		fprintf(stderr,
			"%s: Test USB Device Performance\n"
			"\t-l <length>           length of transfer\n"
			"\t-s                    use synchronous API\n"
			"\t-o                    test OUT endpoint\n"
			"\t-h                    show help\n"
			"\t-e <epnum>            ep num\n",
			argv[0]);
		return 1;
	}

	/* Init Libusb */
	if (libusb_init(&usb_context))
		return -1;

	handle = libusb_open_device_with_vid_pid(NULL, 0x1a0a, 0xbadd);
//	handle = libusb_open_device_with_vid_pid(NULL, 0xa0a0, 0x1000);
//	handle = libusb_open_device_with_vid_pid(NULL, 0xa0a0, 0x0001);
	
	if (!handle) {
		fprintf(stderr, "libusb_open failed\n");
		return 1;
	}

#if 0
	libusb_set_configuration(handle, 1);
	if (res < 0) {
		fprintf(stderr, "set_configuration(): %s\n", libusb_error_name(res));
		return 1;
	}
#endif
	
	res = libusb_claim_interface(handle, 0);
	if (res < 0) {
		perror("claim interface");
		return 1;
	}

	gettimeofday(&start, NULL);
	signal(SIGINT, int_handler);
	
	printf("%s packets\n", (send)? "Sending": "Receiving");
	printf("Using transfer size of: %d\n", buflen);
	printf("using %s interface\n", (sync)? "synchronous": "asynchronous");

	if (!sync) {
		/* Asynchronous */

		for (i = 0; i < (ep > 4 ? 512 : 32); i++) {
			struct libusb_transfer *transfer;
			bool isend = (i / 16) % 2;
			unsigned iep = (i % 4) + 1;

			if (iep == 1 || ep == 1)
				buflen = 1024;

			if (ep > 4) {
				/*if (iep == 1 && isend)
					continue;*/
				printf("%d : ep%d-%s\n", i, iep, (isend?"out":"in"));
				transfer = create_transfer(handle, buflen, isend, iep);
			}
			else
				transfer = create_transfer(handle, buflen, send, ep);
			libusb_submit_transfer(transfer);
		}

		while (1) {
			res = libusb_handle_events(usb_context);
			if (res < 0) {
				/* There was an error. */
				printf("read_thread(): libusb reports error # %d\n", res);

				/* Break out of this loop only on fatal error.*/
				if (res != LIBUSB_ERROR_BUSY &&
				    res != LIBUSB_ERROR_TIMEOUT &&
				    res != LIBUSB_ERROR_OVERFLOW &&
				    res != LIBUSB_ERROR_INTERRUPTED) {
					break;
				}
			}
		}
	}
	else {
		/* Synchronous */
		unsigned char *buf = calloc(1, buflen);
		int actual_length;
		do {
			if (send) {
				/* Send data to the device */
				res = libusb_bulk_transfer(handle, 0x01,
					buf, buflen, &actual_length, 100000);
				if (res < 0) {
					fprintf(stderr, "bulk transfer (out): %s\n", libusb_error_name(res));
					return 1;
				}
			}
			else {
				/* Receive data from the device */
				res = libusb_bulk_transfer(handle, 0x81,
					buf, buflen, &actual_length, 100000);
				if (res < 0) {
					fprintf(stderr, "bulk transfer (in): %s\n", libusb_error_name(res));
					return 1;
				}
			}
			//packets++;
			bytes += actual_length;

			#if 0
			for (i = 0; i < actual_length; i++) {
				printf("%02hhx ", buf[i]);
				if ((i+1) % 8 == 0)
					printf("   ");
				if ((i+1) % 16 == 0)
					printf("\n");
			}
			printf("\n\n");
			#endif
		} while (res >= 0);
	}
	return 0;
}
