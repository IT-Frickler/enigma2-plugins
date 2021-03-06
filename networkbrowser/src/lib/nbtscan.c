/*###########################################################################
#
# written by :	Stephen J. Friedl
#		Software Consultant
#		steve@unixwiz.net
#
# Copyright (C) 2007 - 2008 by
# nixkoenner <nixkoenner@newnigma2.to>
# License: GPL
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
#
###########################################################################*/

#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <sys/time.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#if HAVE_STDINT_H
#include <stdint.h>
#endif

#include "nbtscan.h"

static int set_range(const char *range_str, struct ip_range *range_struct)
{
	if (is_ip(range_str, range_struct))
		return 1;
	if (is_range1(range_str, range_struct))
		return 1;
	if (is_range2(range_str, range_struct))
		return 1;
	return 0;
};

static bool python_hostinfo(struct in_addr addr, const struct nb_host_info *hostinfo, netinfo *nInfo)
{
	int unique;
	uint8_t service;

	if (hostinfo->names[0].ascii_name[0] == '\0')
		return false;

	memset(nInfo, 0, sizeof(netinfo));

	service = hostinfo->names[0].ascii_name[15];
	unique = !(hostinfo->names[0].rr_flags & 0x0080);
	strncpy(nInfo->name, hostinfo->names[0].ascii_name, 15);
	strncpy(nInfo->domain, hostinfo->names[1].ascii_name, 15);
	sprintf(nInfo->service, "%s", getnbservicename(service, unique, hostinfo->names[0].ascii_name));
	sprintf(nInfo->mac, "%02x:%02x:%02x:%02x:%02x:%02x",
		hostinfo->footer.adapter_address[0], hostinfo->footer.adapter_address[1],
		hostinfo->footer.adapter_address[2], hostinfo->footer.adapter_address[3],
		hostinfo->footer.adapter_address[4], hostinfo->footer.adapter_address[5]);
	strcpy(nInfo->ip, inet_ntoa(addr));
	return true;
}

#define BUFFSIZE 1024

unsigned int netInfo(const char *pythonIp, netinfo *nInfo, unsigned int n)
{
	int timeout = 10000, send_ok;
	struct ip_range range;
	char buff[BUFFSIZE];
	int sock;
	unsigned int addr_size;
	struct sockaddr_in dest_sockaddr;
	struct in_addr *prev_in_addr = NULL;
	struct in_addr next_in_addr;
	struct timeval select_timeout, last_send_time, current_time, diff_time, send_interval;
	struct timeval transmit_started, now, recv_time;
	struct nb_host_info hostinfo;
	fd_set fdsr;
	fd_set fdsw;
	int size;
	unsigned int pos = 0;
	struct list *scanned;
	uint32_t rtt_base;	/* Base time (seconds) for round trip time calculations */
	float rtt;		/* most recent measured RTT, seconds */
	float srtt = 0;		/* smoothed rtt estimator, seconds */
	float rttvar = 0.75;	/* smoothed mean deviation, seconds */
	double delta;		/* used in retransmit timeout calculations */
	int rto, retransmits = 0, more_to_send = 1, i;
	char errmsg[80];

	if (!set_range(pythonIp, &range)) {
		printf("Error: %s is not an IP address or address range.\n", pythonIp);
		return 0;
	}
	/* Finished with options */

	/* Prepare socket and address structures */
	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		perror("Failed to create socket");
		return 0;
	}

	FD_ZERO(&fdsr);
	FD_SET(sock, &fdsr);

	FD_ZERO(&fdsw);
	FD_SET(sock, &fdsw);

	/* timeout is in milliseconds */
	select_timeout.tv_sec = 60;	/* Default 1 min to survive ARP timeouts */
	select_timeout.tv_usec = 0;

	addr_size = sizeof(struct sockaddr_in);

	/* Calculate interval between subsequent sends */

	timerclear(&send_interval);
	send_interval.tv_usec = 1;	/* for 10baseT interval should be about 1 ms */

	gettimeofday(&last_send_time, NULL);	/* Get current time */

	rtt_base = last_send_time.tv_sec;

	/* Send queries, receive answers and print results */

	scanned = new_list();
	if (scanned == NULL) {
		close(sock);
		return 0;
	}

	for (i = 0; i <= retransmits; i++) {
		gettimeofday(&transmit_started, NULL);
		while ((select(sock + 1, &fdsr, &fdsw, NULL, &select_timeout)) > 0) {
			if (FD_ISSET(sock, &fdsr)) {
				if ((size = recvfrom(sock, buff, BUFFSIZE, 0, (struct sockaddr *)&dest_sockaddr, &addr_size)) <= 0) {
					snprintf(errmsg, 80, "%s\tRecvfrom failed", inet_ntoa(dest_sockaddr.sin_addr));
					perror(errmsg);
					continue;
				};
				/* If this packet isn't a duplicate */
				if (insert(scanned, ntohl(dest_sockaddr.sin_addr.s_addr)) == 1) {
					gettimeofday(&recv_time, NULL);
					rtt = recv_time.tv_sec + recv_time.tv_usec / 1000000 - rtt_base - hostinfo.header.transaction_id / 1000;
					/* Using algorithm described in Stevens' 
					   Unix Network Programming */
					delta = rtt - srtt;
					srtt += delta / 8;
					if (delta < 0.0)
						delta = -delta;
					rttvar += (delta - rttvar) / 4;
					parse_response(buff, size, &hostinfo);
					if (python_hostinfo(dest_sockaddr.sin_addr, &hostinfo, &nInfo[pos]))
						pos++;
				};
			};

			if (pos == n)
				break;

			FD_ZERO(&fdsr);
			FD_SET(sock, &fdsr);

			/* check if send_interval time passed since last send */
			gettimeofday(&current_time, NULL);
			timersub(&current_time, &last_send_time, &diff_time);
			send_ok = timercmp(&diff_time, &send_interval, >=);

			if (more_to_send && FD_ISSET(sock, &fdsw) && send_ok) {
				if (next_address(&range, prev_in_addr, &next_in_addr)) {
					if (!in_list(scanned, ntohl(next_in_addr.s_addr)))
						send_query(sock, next_in_addr, rtt_base);
					prev_in_addr = &next_in_addr;
					/* Update last send time */
					gettimeofday(&last_send_time, NULL);
				} else {	/* No more queries to send */
					more_to_send = 0;
					FD_ZERO(&fdsw);
					/* timeout is in milliseconds */
					select_timeout.tv_sec = timeout / 1000;
					select_timeout.tv_usec = (timeout % 1000) * 1000;	/* Microseconds */
					continue;
				};
			};
			if (more_to_send) {
				FD_ZERO(&fdsw);
				FD_SET(sock, &fdsw);
			};
		};

		if (pos == n)
			break;

		if (i >= retransmits)
			break;	/* If we are not going to retransmit
				   we can finish right now without waiting */

		rto = (srtt + 4 * rttvar) * (i + 1);

		if (rto < 2.0)
			rto = 2.0;
		if (rto > 60.0)
			rto = 60.0;
		gettimeofday(&now, NULL);

		if (now.tv_sec < (transmit_started.tv_sec + rto))
			sleep((transmit_started.tv_sec + rto) - now.tv_sec);
		prev_in_addr = NULL;
		more_to_send = 1;
		FD_ZERO(&fdsw);
		FD_SET(sock, &fdsw);
		FD_ZERO(&fdsr);
		FD_SET(sock, &fdsr);
	};

	delete_list(scanned);
	close(sock);
	return pos;
};
