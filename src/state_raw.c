/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-3-Clause) */

#include "config.h"
#include "socketcand.h"
#include "statistics.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>

#include <linux/can.h>
#include <linux/can/raw.h>

int raw_socket;
struct ifreq ifr;
struct sockaddr_can addr;
struct msghdr msg;
struct canfd_frame frame;
struct iovec iov;
char ctrlmsg[CMSG_SPACE(sizeof(struct timeval)) + CMSG_SPACE(sizeof(__u32))];
struct timeval tv;
struct cmsghdr *cmsg;

void state_raw()
{
	char buf[MAXLEN];
	int i, ret, items;
	fd_set readfds;

	if (previous_state != STATE_RAW) {
		if ((raw_socket = socket(PF_CAN, SOCK_RAW, CAN_RAW)) < 0) {
			PRINT_ERROR("Error while creating RAW socket %s\n", strerror(errno));
			state = STATE_SHUTDOWN;
			return;
		}

		strcpy(ifr.ifr_name, bus_name);
		if (ioctl(raw_socket, SIOCGIFINDEX, &ifr) < 0) {
			PRINT_ERROR("Error while searching for bus %s\n", strerror(errno));
			state = STATE_SHUTDOWN;
			return;
		}

		addr.can_family = AF_CAN;
		addr.can_ifindex = ifr.ifr_ifindex;

		const int timestamp_on = 1;
		if (setsockopt(raw_socket, SOL_SOCKET, SO_TIMESTAMP, &timestamp_on, sizeof(timestamp_on)) < 0) {
			PRINT_ERROR("Could not enable CAN timestamps\n");
			state = STATE_SHUTDOWN;
			return;
		}

		if(ioctl(raw_socket,SIOCGIFMTU,&ifr) < 0) {
			PRINT_ERROR("Error while searching for bus MTU %s\n", strerror(errno));
			state = STATE_SHUTDOWN;
			return;
		}

		/*
		 *if the device supports CAN FD, use it
		 * if you don't want to use CAN FD, you should initialize the device as classic CAN
		*/
		if (ifr.ifr_mtu == CANFD_MTU) {
			const int canfd_on = 1;
			if(setsockopt(raw_socket, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &canfd_on, sizeof(canfd_on)) < 0) {
				PRINT_ERROR("Could not enable CAN FD support\n");
				state = STATE_SHUTDOWN;
				return;
			}
		}

		if(bind(raw_socket, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
			PRINT_ERROR("Error while binding RAW socket %s\n", strerror(errno));
			state = STATE_SHUTDOWN;
			return;
		}

		iov.iov_base = &frame;
		msg.msg_name = &addr;
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = &ctrlmsg;

		previous_state = STATE_RAW;
	}

	FD_ZERO(&readfds);
	FD_SET(raw_socket, &readfds);
	FD_SET(client_socket, &readfds);

	/*
	 * Check if there are more elements in the element buffer before calling select() and
	 * blocking for new packets.
	 */
	if (more_elements) {
		FD_CLR(raw_socket, &readfds);
	} else {
		ret = select((raw_socket > client_socket) ? raw_socket + 1 : client_socket + 1, &readfds, NULL, NULL, NULL);

		if (ret < 0) {
			PRINT_ERROR("Error in select()\n");
			state = STATE_SHUTDOWN;
			return;
		}
	}

	if (FD_ISSET(raw_socket, &readfds)) {
		iov.iov_len = sizeof(frame);
		msg.msg_namelen = sizeof(addr);
		msg.msg_flags = 0;
		msg.msg_controllen = sizeof(ctrlmsg);

		ret = recvmsg(raw_socket, &msg, 0);
		if(ret != sizeof(struct canfd_frame) && ret != sizeof(struct can_frame)) {
			PRINT_ERROR("Error reading frame from RAW socket\n");
		} else {
			/* read timestamp data */
			for (cmsg = CMSG_FIRSTHDR(&msg);
			     cmsg && (cmsg->cmsg_level == SOL_SOCKET);
			     cmsg = CMSG_NXTHDR(&msg, cmsg)) {
				if (cmsg->cmsg_type == SO_TIMESTAMP) {
					tv = *(struct timeval *)CMSG_DATA(cmsg);
				}
			}

			if (frame.can_id & CAN_ERR_FLAG) {
				canid_t class = frame.can_id & CAN_EFF_MASK;
				ret = sprintf(buf, "< error %03X %ld.%06ld >", class, tv.tv_sec, tv.tv_usec);
				send(client_socket, buf, strlen(buf), 0);
				tcp_quickack(client_socket);
			} else if (frame.can_id & CAN_RTR_FLAG) {
				/* TODO implement */
			} else {
				switch(ret) {
					// if the frame is a classic CAN frame
					case sizeof(struct can_frame):
						if(frame.can_id & CAN_EFF_FLAG) {
							ret = sprintf(buf, "< frame %08X %ld.%06ld ", frame.can_id & CAN_EFF_MASK, tv.tv_sec, tv.tv_usec);
						} else {
							ret = sprintf(buf, "< frame %03X %ld.%06ld ", frame.can_id & CAN_SFF_MASK, tv.tv_sec, tv.tv_usec);
						}
					break;
					// if the frame is a CAN FD frame
					case sizeof(struct canfd_frame):
						if(frame.can_id & CAN_EFF_FLAG) {
							ret = sprintf(buf, "< fdframe %08X %02X %ld.%06ld ", frame.can_id & CAN_EFF_MASK, frame.flags, tv.tv_sec, tv.tv_usec);
						} else {
							ret = sprintf(buf, "< fdframe %03X %02X %ld.%06ld ", frame.can_id & CAN_SFF_MASK, frame.flags, tv.tv_sec, tv.tv_usec);
						}
				}

				for(i=0;i<frame.len;i++) {
					ret += sprintf(buf+ret, "%02X", frame.data[i]);
				}
				sprintf(buf + ret, " >");
				send(client_socket, buf, strlen(buf), 0);
				tcp_quickack(client_socket);
			}
		}
	}

	if (FD_ISSET(client_socket, &readfds)) {
		ret = receive_command(client_socket, (char *)&buf);

		if (ret == 0) {
			if (state_changed(buf, state)) {
				close(raw_socket);
				strcpy(buf, "< ok >");
				send(client_socket, buf, strlen(buf), 0);
				tcp_quickack(client_socket);
				return;
			}

			if (!strcmp("< echo >", buf)) {
				send(client_socket, buf, strlen(buf), 0);
				tcp_quickack(client_socket);
				return;
			}

			/* Send a single frame */
			if (!strncmp("< send ", buf, 7)) {
				items = sscanf(buf, "< %*s %x %hhu "
					       "%hhx %hhx %hhx %hhx %hhx %hhx "
					       "%hhx %hhx >",
					       &frame.can_id,
					       &frame.len,
					       &frame.data[0],
					       &frame.data[1],
					       &frame.data[2],
					       &frame.data[3],
					       &frame.data[4],
					       &frame.data[5],
					       &frame.data[6],
					       &frame.data[7]);

				if ( (items < 2) ||
				     (frame.len > 8) ||
				     (items != 2 + frame.len)) {
					PRINT_ERROR("Syntax error in send command\n");
						return;
				}

				/* < send XXXXXXXX ... > check for extended identifier */
				if (element_length(buf, 2) == 8)
					frame.can_id |= CAN_EFF_FLAG;

				ret = send(raw_socket, &frame, sizeof(struct can_frame), 0);
				if (ret == -1) {
					state = STATE_SHUTDOWN;
					return;
				}
				/* Send a single CANFD frame */
			} else if(!strncmp("< fdsend ", buf, 9)) {
		    // First, read the fixed part of the frame
		    items = sscanf(buf, "< %*s %x %hhx %hhu",
		                   &frame.can_id,
		                   &frame.flags,
		                   &frame.len);

		    if (items != 3) {
		        PRINT_ERROR("Syntax error in fdsend command\n");
		        return;
		    }

		    // Ensure frame.len does not exceed the maximum allowed length
		    if (frame.len > 64) {
		        PRINT_ERROR("Frame length exceeds maximum allowed length\n");
		        return;
		    }

		    // Construct the format string dynamically based on frame.len
		    char format[512];
		    snprintf(format, sizeof(format), "< %%*s %%x %%hhx %%hhu");
		    for (int i = 0; i < frame.len; i++) {
		        strncat(format, " %hhx", sizeof(format) - strlen(format) - 1);
		    }
		    strncat(format, " >", sizeof(format) - strlen(format) - 1);

		    // Read the variable-length frame.data
		    items = sscanf(buf, format,
		                   &frame.can_id,
		                   &frame.flags,
		                   &frame.len,
		                   &frame.data[0], &frame.data[1], &frame.data[2], &frame.data[3],
		                   &frame.data[4], &frame.data[5], &frame.data[6], &frame.data[7],
		                   &frame.data[8], &frame.data[9], &frame.data[10], &frame.data[11],
		                   &frame.data[12], &frame.data[13], &frame.data[14], &frame.data[15],
		                   &frame.data[16], &frame.data[17], &frame.data[18], &frame.data[19],
		                   &frame.data[20], &frame.data[21], &frame.data[22], &frame.data[23],
		                   &frame.data[24], &frame.data[25], &frame.data[26], &frame.data[27],
		                   &frame.data[28], &frame.data[29], &frame.data[30], &frame.data[31],
		                   &frame.data[32], &frame.data[33], &frame.data[34], &frame.data[35],
		                   &frame.data[36], &frame.data[37], &frame.data[38], &frame.data[39],
		                   &frame.data[40], &frame.data[41], &frame.data[42], &frame.data[43],
		                   &frame.data[44], &frame.data[45], &frame.data[46], &frame.data[47],
		                   &frame.data[48], &frame.data[49], &frame.data[50], &frame.data[51],
		                   &frame.data[52], &frame.data[53], &frame.data[54], &frame.data[55],
		                   &frame.data[56], &frame.data[57], &frame.data[58], &frame.data[59],
		                   &frame.data[60], &frame.data[61], &frame.data[62], &frame.data[63]);

				if ( (items < 2) ||
				     (frame.len > 64) ||
				     (items != 3 + frame.len)) {
					PRINT_ERROR("Syntax error in fdsend command\n");
						return;
				}

				/* < fdsend XXXXXXXX ... > check for extended identifier */
				if(element_length(buf, 2) == 8)
					frame.can_id |= CAN_EFF_FLAG;

				ret = send(raw_socket, &frame, sizeof(struct canfd_frame), 0);
				if(ret==-1) {
					state = STATE_SHUTDOWN;
					return;
				}

			} else {
				PRINT_ERROR("unknown command '%s'\n", buf);
				strcpy(buf, "< error unknown command >");
				send(client_socket, buf, strlen(buf), 0);
				tcp_quickack(client_socket);
			}
		} else {
			state = STATE_SHUTDOWN;
			return;
		}
	} else {
		ret = read(client_socket, &buf, 0);
		tcp_quickack(client_socket);
		if (ret == -1) {
			state = STATE_SHUTDOWN;
			return;
		}
	}
}
