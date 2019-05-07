/* Zebra Mlag Code.
 * Copyright (C) 2018 Cumulus Networks, Inc.
 *                    Donald Sharp
 *
 * This file is part of FRR.
 *
 * FRR is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * FRR is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with FRR; see the file COPYING.  If not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */
#include "zebra.h"

#include "hook.h"
#include "module.h"
#include "thread.h"
#include "libfrr.h"
#include "version.h"
#include "network.h"

#include "lib/stream.h"

#include "zebra/debug.h"
#include "zebra/zebra_router.h"
#include "zebra/zebra_mlag.h"
#include "zebra/zebra_mlag_private.h"

#include <sys/un.h>


/*
 * This file will have platform specific apis to communicate with MCLAG.
 *
 */

#ifdef HAVE_CUMULUS

static struct thread_master *zmlag_master = NULL;
static int mlag_socket;

static int zebra_mlag_connect(struct thread *thread);


/*
 * Write teh data to MLAGD
 */
int zebra_mlag_private_write_data(uint8_t *data, uint32_t len)
{
	int rc = 0;

	if (IS_ZEBRA_DEBUG_MLAG) {
		zlog_debug("%s: Writing %d length Data to clag", __FUNCTION__,
			   len);
		zlog_hexdump(data, len);
	}
	rc = write(mlag_socket, data, len);
	return rc;
}

static int zebra_mlag_read(struct thread *thread)
{
	int data_len;
	uint16_t msglen;
	uint16_t h_msglen;
	uint16_t offset = 0;

	/*
	 * The read currently ends with a `\n` so let's make sure
	 * we don't read beyond the end of the world here
	 */
	memset(mlag_rd_buffer, 0, ZEBRA_MLAG_BUF_LIMIT);
	data_len = read(mlag_socket, mlag_rd_buffer, ZEBRA_MLAG_BUF_LIMIT);
	if (data_len <= 0) {
		if (IS_ZEBRA_DEBUG_MLAG)
			zlog_debug(
				"Failure to read mlag socket: %d %s(%d), "
				"starting over",
				mlag_socket, safe_strerror(errno), errno);
		zebra_mlag_handle_process_state(MLAG_DOWN);
		return -1;
	}

	if (IS_ZEBRA_DEBUG_MLAG) {
		zlog_debug("Received MLAG Data from socket: %d, len:%d",
			   mlag_socket, data_len);
		zlog_hexdump(mlag_rd_buffer, data_len);
	}

	/*
	 * Received message looks like below
	 * | len-1 (2 Bytes) | payload-1 (len-1) |
	 *   len-2 (2 Bytes) | payload-2 (len-2) | ..
	 */
	while (data_len > 0) {
		memcpy(&msglen, mlag_rd_buffer + offset, ZEBRA_MLAG_LEN_SIZE);
		h_msglen = ntohs(msglen);
		offset += ZEBRA_MLAG_LEN_SIZE;

		if (IS_ZEBRA_DEBUG_MLAG) {
			zlog_debug(
				"Processing a MLAG Message with len:%d "
				"offset:%d",
				h_msglen, offset);
			zlog_hexdump(mlag_rd_buffer + offset, h_msglen);
		}
		zebra_mlag_process_mlag_data(mlag_rd_buffer + offset, h_msglen);
		offset += h_msglen;
		data_len -= (h_msglen + ZEBRA_MLAG_LEN_SIZE);
	}

	zrouter.mlag_info.t_read = NULL;
	thread_add_read(zmlag_master, zebra_mlag_read, NULL, mlag_socket,
			&zrouter.mlag_info.t_read);

	return 0;
}

static int zebra_mlag_connect(struct thread *thread)
{
	struct sockaddr_un svr;
	struct ucred ucred;
	socklen_t len = 0;

	/* Reset the Timer-running flag */
	zrouter.mlag_info.timer_running = false;

	zrouter.mlag_info.t_read = NULL;
	memset(&svr, 0, sizeof(svr));
	svr.sun_family = AF_UNIX;
#define MLAG_SOCK_NAME "/var/run/clag-zebra.socket"
	strcpy(svr.sun_path, MLAG_SOCK_NAME);

	mlag_socket = socket(svr.sun_family, SOCK_STREAM, 0);
	if (mlag_socket < 0)
		return -1;

	if (connect(mlag_socket, (struct sockaddr *)&svr, sizeof(svr)) == -1) {
		if (IS_ZEBRA_DEBUG_MLAG)
			zlog_debug(
				"Unable to connect to %s trying again "
				"in 10 seconds",
				svr.sun_path);
		close(mlag_socket);
		zrouter.mlag_info.timer_running = true;
		thread_add_timer(zmlag_master, zebra_mlag_connect, NULL, 10,
				 &zrouter.mlag_info.t_read);
		return 0;
	}
	len = sizeof(struct ucred);
	ucred.pid = getpid();

	set_nonblocking(mlag_socket);
	setsockopt(mlag_socket, SOL_SOCKET, SO_PEERCRED, &ucred, len);

	if (IS_ZEBRA_DEBUG_MLAG)
		zlog_debug("%s: Connection with MLAG is established ",
			   __FUNCTION__);

	thread_add_read(zmlag_master, zebra_mlag_read, NULL, mlag_socket,
			&zrouter.mlag_info.t_read);
	/*
	 * Connection is established with MLAGD, post to clients
	 */
	zebra_mlag_handle_process_state(MLAG_UP);
	return 0;
}

/*
 * Currently we are doing polling later we will look for better options
 */
void zebra_mlag_private_monitor_state(void)
{
	thread_add_event(zmlag_master, zebra_mlag_connect, NULL, 0,
			 &zrouter.mlag_info.t_read);
}

int zebra_mlag_private_open_channel(void)
{
	zmlag_master = zrouter.mlag_info.th_master;

	if (zrouter.mlag_info.connected == true) {
		if (IS_ZEBRA_DEBUG_MLAG)
			zlog_debug("%s: Zebra already connected to MLAGD",
				   __FUNCTION__);
		return 0;
	}

	if (zrouter.mlag_info.timer_running == true) {
		if (IS_ZEBRA_DEBUG_MLAG)
			zlog_debug(
				"%s: Connection retry is in progress for "
				"MLAGD",
				__FUNCTION__);
		return 0;
	}

	if (zrouter.mlag_info.clients_interested_cnt) {
		/*
		 * Connect only if any clients are showing interest
		 */
		thread_add_event(zmlag_master, zebra_mlag_connect, NULL, 0,
				 &zrouter.mlag_info.t_read);
	}
	return 0;
}

int zebra_mlag_private_close_channel(void)
{
	if (zmlag_master == NULL)
		return -1;

	if (zrouter.mlag_info.clients_interested_cnt) {
		if (IS_ZEBRA_DEBUG_MLAG)
			zlog_debug(
				"%s: still %d clients are connected, "
				"can't close",
				__FUNCTION__,
				zrouter.mlag_info.clients_interested_cnt);
		return -1;
	}

	/*
	 * Post the De-register to MLAG, so that it can do necesasry cleanup
	 */
	zebra_mlag_send_deregister();

	return 0;
}

void zebra_mlag_private_cleanup_data(void)
{
	zmlag_master = NULL;
	zrouter.mlag_info.connected = false;
	zrouter.mlag_info.timer_running = false;

	close(mlag_socket);
}

#else  /*HAVE_CUMULUS */

int zebra_mlag_private_write_data(uint8_t *data, uint32_t len)
{
	return 0;
}

void zebra_mlag_private_monitor_state(void)
{
	return;
}

int zebra_mlag_private_open_channel()
{
	return 0;
}

int zebra_mlag_private_close_channel()
{
	return 0;
}

void zebra_mlag_private_cleanup_data(void)
{
	return;
}
#endif /*HAVE_CUMULUS*/
