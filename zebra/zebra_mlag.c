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

#include "command.h"
#include "hook.h"

#include "zebra/zebra_mlag.h"
#include "zebra/zebra_router.h"
#include "zebra/zapi_msg.h"
#include "zebra/debug.h"

#ifndef VTYSH_EXTRACT_PL
#include "zebra/zebra_mlag_clippy.c"
#endif

uint8_t mlag_wr_buffer[ZEBRA_MLAG_BUF_LIMIT];
uint8_t mlag_rd_buffer[ZEBRA_MLAG_BUF_LIMIT];
uint32_t mlag_wr_buf_ptr = 0;


/*
 * API to post the Registartion to MLAGD
 * MLAG will not process any messages with out the registration
 */
void zebra_mlag_send_register(void)
{
	return;
}

/*
 * API to post the De-Registartion to MLAGD
 * MLAG will not process any messages after the de-registration
 */
void zebra_mlag_send_deregister(void)
{
	return;
}

void zebra_mlag_process_mlag_data(uint8_t *data, uint32_t len)
{
}

/*
 * API to handle teh process state.
 * In case of Down, Zebra keep monitoring the MLAG state.
 * all the state Notifications will be published to clients
 */
void zebra_mlag_handle_process_state(enum zebra_mlag_state state)
{
}


/***********************Handling ZAPI Requests from clients*******************/

/*
 * API to register zebra client for MLAG Updates
 */
void zebra_mlag_client_register(ZAPI_HANDLER_ARGS)
{
}

/*
 * API to un-register for MLAG Updates
 */
void zebra_mlag_client_unregister(ZAPI_HANDLER_ARGS)
{
}

/*
 * Does following things.
 * 1) allocated new local stream, and copies teh client data and enqueue
 *    to MLAG Thread
 *  2) MLAG Thread after dequeing, encode the client data using protobuf
 *     and write on to MLAG
 */
void zebra_mlag_forward_client_msg(ZAPI_HANDLER_ARGS)
{
}

/***********************End of Handling ZAPI Requests from clients*************/

enum mlag_role zebra_mlag_get_role(void)
{
	return zrouter.mlag_info.role;
}

DEFUN_HIDDEN (show_mlag,
	      show_mlag_cmd,
	      "show zebra mlag",
	      SHOW_STR
	      ZEBRA_STR
	      "The mlag role on this machine\n")
{
	char buf[80];

	vty_out(vty, "MLag is configured to: %s\n",
		mlag_role2str(zrouter.mlag_info.role, buf, sizeof(buf)));

	return CMD_SUCCESS;
}

DEFPY (test_mlag,
       test_mlag_cmd,
       "test zebra mlag <none$none|primary$primary|secondary$secondary>",
       "Test code\n"
       ZEBRA_STR
       "Modify the Mlag state\n"
       "Mlag is not setup on the machine\n"
       "Mlag is setup to be primary\n"
       "Mlag is setup to be the secondary\n")
{
	enum mlag_role orig = zrouter.mlag_info.role;
	char buf1[80], buf2[80];

	if (none)
		zrouter.mlag_info.role = MLAG_ROLE_NONE;
	if (primary)
		zrouter.mlag_info.role = MLAG_ROLE_PRIMARY;
	if (secondary)
		zrouter.mlag_info.role = MLAG_ROLE_SECONDARY;

	if (IS_ZEBRA_DEBUG_MLAG)
		zlog_debug("Test: Changing role from %s to %s",
			   mlag_role2str(orig, buf1, sizeof(buf1)),
			   mlag_role2str(orig, buf2, sizeof(buf2)));

	if (orig != zrouter.mlag_info.role)
		zsend_capabilities_all_clients();

	return CMD_SUCCESS;
}

void zebra_mlag_init(void)
{
	install_element(VIEW_NODE, &show_mlag_cmd);
	install_element(ENABLE_NODE, &test_mlag_cmd);
}

void zebra_mlag_terminate(void)
{
}
