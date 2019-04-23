/* PIM Mlag Code.
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

#include "pimd.h"
#include "pim_mlag.h"
#include "pim_zebra.h"
#include "pim_oil.h"

extern struct zclient *zclient;

#define PIM_MLAG_METADATA_LEN 4

/*********************ACtual Data processing *****************************/
/* TBD: There can be duplicate updates to FIB***/
#define PIM_MLAG_ADD_OIF_TO_OIL(ch, ch_oil)                                    \
	do {                                                                   \
		if (PIM_DEBUG_MLAG)                                            \
			zlog_debug(                                            \
				"%s: add Dual-active Interface to %s "         \
				"to oil:%s",                                   \
				__FUNCTION__, ch->interface->name,             \
				ch->sg_str);                                   \
		pim_channel_add_oif(ch_oil, ch->interface,                     \
				    PIM_OIF_FLAG_PROTO_IGMP);                  \
		ch->mlag_am_i_df = true;                                       \
	} while (0)

#define PIM_MLAG_DEL_OIF_TO_OIL(ch, ch_oil)                                    \
	do {                                                                   \
		if (PIM_DEBUG_MLAG)                                            \
			zlog_debug(                                            \
				"%s: del Dual-active Interface to %s "         \
				"to oil:%s",                                   \
				__FUNCTION__, ch->interface->name,             \
				ch->sg_str);                                   \
		pim_channel_del_oif(ch_oil, ch->interface,                     \
				    PIM_OIF_FLAG_PROTO_IGMP);                  \
		ch->mlag_am_i_df = false;                                      \
	} while (0)


#define PIM_MLAG_UPDATE_OIL_BASED_ON_DR(pim_ifp, ch, ch_oil)                   \
	do {                                                                   \
		if (PIM_I_am_DR(pim_ifp))                                      \
			PIM_MLAG_ADD_OIF_TO_OIL(ch, ch_oil);                   \
		else                                                           \
			PIM_MLAG_DEL_OIF_TO_OIL(ch, ch_oil);                   \
	} while (0)


static void pim_mlag_calculate_df_for_ifchannel(struct pim_ifchannel *ch)
{
	struct pim_interface *pim_ifp = NULL;
	struct pim_upstream *upstream = ch->upstream;
	struct channel_oil *ch_oil = NULL;

	pim_ifp = (ch->interface) ? ch->interface->info : NULL;
	ch_oil = (upstream) ? upstream->channel_oil : NULL;

	if (!pim_ifp || !upstream || !ch_oil)
		return;

	if (PIM_DEBUG_MLAG)
		zlog_debug("%s: Calculating DF for Dual active if-channel%s",
			   __FUNCTION__, ch->sg_str);

	/* Local Interface is not configured with Dual active */
	if (!PIM_I_am_DualActive(pim_ifp)
	    || ch->mlag_peer_is_dual_active == false) {
		if (PIM_DEBUG_MLAG)
			zlog_debug(
				"%s: Dual -active is not configured on "
				"both sides local:%d, peer:%d",
				__FUNCTION__, PIM_I_am_DualActive(pim_ifp),
				ch->mlag_peer_is_dual_active);
		PIM_MLAG_UPDATE_OIL_BASED_ON_DR(pim_ifp, ch, ch_oil);
	}

	if (ch->mlag_local_cost_to_rp != ch->mlag_peer_cost_to_rp) {
		if (PIM_DEBUG_MLAG)
			zlog_debug(
				"%s: Cost_to_rp  is not same local:%u, "
				"peer:%u",
				__FUNCTION__, ch->mlag_local_cost_to_rp,
				ch->mlag_peer_cost_to_rp);
		if (ch->mlag_local_cost_to_rp < ch->mlag_peer_cost_to_rp)
			/* My cost to RP is better then peer */
			PIM_MLAG_ADD_OIF_TO_OIL(ch, ch_oil);
		else
			PIM_MLAG_DEL_OIF_TO_OIL(ch, ch_oil);
	} else {
		/* Cost is same, Tie break is DR */
		PIM_MLAG_UPDATE_OIL_BASED_ON_DR(pim_ifp, ch, ch_oil);
	}
}


/******************POsting Local data to peer****************************/

void pim_mlag_add_entry_to_peer(struct pim_ifchannel *ch)
{
	struct stream *s = NULL;
	struct pim_interface *pim_ifp = ch->interface->info;
	struct vrf *vrf = vrf_lookup_by_id(ch->interface->vrf_id);

	if (router->connected_to_mlag == false) {
		/* Not connected to peer, update FIB based on DR role*/
		pim_mlag_calculate_df_for_ifchannel(ch);
		return;
	}

	s = stream_new(MLAG_MROUTE_ADD_MSGSIZE + PIM_MLAG_METADATA_LEN);
	if (!s)
		return;

	stream_putl(s, MLAG_MROUTE_ADD);
	stream_put(s, vrf->name, VRF_NAMSIZ);
	stream_putl(s, htonl(ch->sg.src.s_addr));
	stream_putl(s, htonl(ch->sg.grp.s_addr));
	stream_putl(s, ch->mlag_local_cost_to_rp);
	stream_putl(s, 0xa5a5);
	stream_putc(s, PIM_I_am_DR(pim_ifp));
	stream_putc(s, PIM_I_am_DualActive(pim_ifp));
	stream_putl(s, ch->interface->vrf_id);
	stream_put(s, ch->interface->name, INTERFACE_NAMSIZ);

	stream_fifo_push_safe(router->mlag_fifo, s);
	pim_mlag_signal_zpthread();

	pim_mlag_calculate_df_for_ifchannel(ch);
	if (PIM_DEBUG_MLAG)
		zlog_debug("%s: Enqueued MLAG Route add for %s", __FUNCTION__,
			   ch->sg_str);
}

/*
 * The iNtention of posting Delete is to clean teh DB at MLAGD
 */
void pim_mlag_del_entry_to_peer(struct pim_ifchannel *ch)
{
	struct stream *s = NULL;
	struct vrf *vrf = vrf_lookup_by_id(ch->interface->vrf_id);

	s = stream_new(MLAG_MROUTE_DEL_MSGSIZE + PIM_MLAG_METADATA_LEN);
	if (!s)
		return;

	stream_putl(s, MLAG_MROUTE_DEL);
	stream_put(s, vrf->name, VRF_NAMSIZ);
	stream_putl(s, htonl(ch->sg.src.s_addr));
	stream_putl(s, htonl(ch->sg.grp.s_addr));
	stream_putl(s, 0xa5a5);
	stream_putl(s, ch->interface->vrf_id);
	stream_put(s, ch->interface->name, INTERFACE_NAMSIZ);

	stream_fifo_push_safe(router->mlag_fifo, s);
	pim_mlag_signal_zpthread();

	if (PIM_DEBUG_MLAG)
		zlog_debug("%s: Enqueued MLAG Route del for %s", __FUNCTION__,
			   ch->sg_str);
}

/******************End of posting local data to peer ********************/

/*****************PIM Actions for MLAG state chnages**********************/

void pim_mlag_update_dr_state_to_peer(struct interface *ifp)
{
	struct pim_interface *pim_ifp = ifp->info;
	struct pim_instance *pim;
	struct pim_upstream *up;
	struct pim_ifchannel *ch;
	struct listnode *node;


	if (!pim_ifp && PIM_I_am_DualActive(pim_ifp))
		return;

	pim = pim_ifp->pim;
	if (PIM_DEBUG_MLAG)
		zlog_debug(
			"%s: DR Role of an Interface-%s changed, "
			"updating to peer",
			__FUNCTION__, ifp->name);

	for (ALL_LIST_ELEMENTS_RO(pim->upstream_list, node, up)) {
		ch = pim_ifchannel_find(ifp, &up->sg);
		if (ch)
			pim_mlag_add_entry_to_peer(ch);
	}
}

void pim_mlag_update_cost_to_rp_to_peer(struct pim_upstream *up)
{
	struct listnode *chnode;
	struct listnode *chnextnode;
	struct pim_ifchannel *ch = NULL;
	struct pim_interface *pim_ifp = NULL;

	if (PIM_DEBUG_MLAG)
		zlog_debug(
			"%s: RP cost of upstream-%s changed, "
			"updating to peer",
			__FUNCTION__, up->sg_str);

	for (ALL_LIST_ELEMENTS(up->ifchannels, chnode, chnextnode, ch)) {
		pim_ifp = (ch->interface) ? ch->interface->info : NULL;
		if (!pim_ifp)
			continue;

		if (PIM_I_am_DualActive(pim_ifp)) {
			ch->mlag_local_cost_to_rp =
				up->rpf.source_nexthop.mrib_route_metric;
			pim_mlag_add_entry_to_peer(ch);
		}
	}
}

static void pim_mlag_handle_state_change_for_ifp(struct pim_instance *pim,
						 struct interface *ifp,
						 bool is_up)
{
	struct pim_upstream *up;
	struct pim_ifchannel *ch;
	struct listnode *node;

	for (ALL_LIST_ELEMENTS_RO(pim->upstream_list, node, up)) {
		ch = pim_ifchannel_find(ifp, &up->sg);
		if (ch) {
			if (is_up)
				pim_mlag_add_entry_to_peer(ch);
			else {
				/* Reset peer data */
				ch->mlag_peer_cost_to_rp =
					PIM_ASSERT_ROUTE_METRIC_MAX;
				pim_mlag_calculate_df_for_ifchannel(ch);
			}
		}
	}
}

static int pim_mlag_down_handler(struct thread *thread)
{
	struct vrf *vrf;
	struct interface *ifp;
	struct pim_interface *pim_ifp;

	RB_FOREACH (vrf, vrf_name_head, &vrfs_by_name) {
		if (!vrf->info)
			continue;

		FOR_ALL_INTERFACES (vrf, ifp) {
			if (!ifp->info)
				continue;
			pim_ifp = ifp->info;
			if (!ifp->info || !PIM_I_am_DualActive(pim_ifp))
				continue;
			pim_mlag_handle_state_change_for_ifp(vrf->info, ifp,
							     false);
		}
	}
	return (0);
}

static int pim_mlag_local_up_handler(struct thread *thread)
{
	struct vrf *vrf;
	struct interface *ifp;
	struct pim_interface *pim_ifp;

	RB_FOREACH (vrf, vrf_name_head, &vrfs_by_name) {
		if (!vrf->info)
			continue;

		FOR_ALL_INTERFACES (vrf, ifp) {
			if (!ifp->info)
				continue;
			pim_ifp = ifp->info;
			if (!ifp->info || !PIM_I_am_DualActive(pim_ifp))
				continue;
			pim_mlag_handle_state_change_for_ifp(vrf->info, ifp,
							     true);
		}
	}
	return (0);
}

/**************End of PIM Actions for MLAG State changes******************/


/********************API to process PIM MLAG Data ************************/

static void pim_mlag_process_mlagd_state_change(struct mlag_status msg)
{
	if (PIM_DEBUG_MLAG)
		zlog_debug("%s: msg dump: my_role:%d, peer_state:%d",
			   __FUNCTION__, msg.my_role, msg.peer_state);

	/*
	 * This can be receieed when Peer clag down/peerlink failure.
	 */
	if (msg.peer_state == MLAG_STATE_DOWN) {
		router->connected_to_mlag = false;
		thread_add_event(router->master, pim_mlag_down_handler, NULL, 0,
				 NULL);
	}

	/*
	 * In below case local clag is running, but connection with
	 * peer is restored
	 * Ideally in this case we no need to do anything, peer MLAG will
	 * start replay the new data, just act on it.
	 */
	if (msg.peer_state == MLAG_STATE_RUNNING)
		router->connected_to_mlag = true;
}

static void pim_mlag_process_mroute_add(struct mlag_mroute_add msg)
{
	struct vrf *vrf = NULL;
	struct interface *ifp = NULL;
	struct pim_ifchannel *ch = NULL;
	struct prefix_sg sg;

	if (PIM_DEBUG_MLAG)
		zlog_debug(
			"%s: msg dump: vrf_name:%s, s.ip:0x%x, g.ip:0x%x "
			"cost:%u, vni_id:%d, DR:%d, Dual active:%d, vrf_id:0x%x"
			"intf_name:%s",
			__FUNCTION__, msg.vrf_name, msg.source_ip, msg.group_ip,
			msg.cost_to_rp, msg.vni_id, msg.am_i_dr,
			msg.am_i_dual_active, msg.vrf_id, msg.intf_name);

	vrf = vrf_lookup_by_name(msg.vrf_name);
	if (vrf)
		ifp = if_lookup_by_name(msg.intf_name, vrf->vrf_id);

	if (!vrf || !ifp || !ifp->info) {
		if (PIM_DEBUG_MLAG)
			zlog_debug(
				"%s: Invalid params...vrf:%p, ifp,%p, "
				"pim_ifp:%p",
				__FUNCTION__, vrf, ifp, ifp->info);
		return;
	}

	memset(&sg, 0, sizeof(struct prefix_sg));
	sg.src.s_addr = ntohl(msg.source_ip);
	sg.grp.s_addr = ntohl(msg.group_ip);

	ch = pim_ifchannel_find(ifp, &sg);
	if (ch) {
		if (PIM_DEBUG_MLAG)
			zlog_debug("%s: Updating ifchannel-%s peer mlag params",
				   __FUNCTION__, ch->sg_str);
		ch->mlag_peer_cost_to_rp = msg.cost_to_rp;
		ch->mlag_peer_is_dr = msg.am_i_dr;
		ch->mlag_peer_is_dual_active = msg.am_i_dual_active;
		pim_mlag_calculate_df_for_ifchannel(ch);
	} else {
		if (PIM_DEBUG_MLAG)
			zlog_debug("%s: failed to find if-channel...",
				   __FUNCTION__);
	}
}

static void pim_mlag_process_mroute_del(struct mlag_mroute_del msg)
{
	if (PIM_DEBUG_MLAG)
		zlog_debug(
			"%s: msg dump: vrf_name:%s, s.ip:0x%x, g.ip:0x%x "
			"vni_id:%d, vrf_id:0x%x intf_name:%s",
			__FUNCTION__, msg.vrf_name, msg.source_ip, msg.group_ip,
			msg.vni_id, msg.vrf_id, msg.intf_name);
}

static void pim_mlag_process_peer_status_update(struct mlag_pim_status msg)
{
	if (PIM_DEBUG_MLAG)
		zlog_debug("%s: msg dump: switchd_state:%d, svi_state:%d",
			   __FUNCTION__, msg.switchd_state, msg.svi_state);
}

int pim_zebra_mlag_handle_msg(struct stream *s, int len)
{
	struct mlag_msg mlag_msg;
	char buf[80];
	int rc = 0;

	rc = zebra_mlag_lib_decode_mlag_hdr(s, &mlag_msg);
	if (rc)
		return (rc);

	if (PIM_DEBUG_MLAG)
		zlog_debug(
			"%s: Received msg type:%s length:%d, bulk_cnt:%d",
			__FUNCTION__,
			zebra_mlag_lib_msgid_to_str(mlag_msg.msg_type, buf, 80),
			mlag_msg.data_len, mlag_msg.msg_cnt);

	switch (mlag_msg.msg_type) {
	case MLAG_STATUS_UPDATE: {
		struct mlag_status msg;
		rc = zebra_mlag_lib_decode_mlag_status(s, &msg);
		if (rc)
			return (rc);
		pim_mlag_process_mlagd_state_change(msg);
	} break;
	case MLAG_MROUTE_ADD: {
		struct mlag_mroute_add msg;
		rc = zebra_mlag_lib_decode_mroute_add(s, &msg);
		if (rc)
			return (rc);
		pim_mlag_process_mroute_add(msg);
	} break;
	case MLAG_MROUTE_DEL: {
		struct mlag_mroute_del msg;
		rc = zebra_mlag_lib_decode_mroute_del(s, &msg);
		if (rc)
			return (rc);
		pim_mlag_process_mroute_del(msg);
	} break;
	case MLAG_MROUTE_ADD_BULK: {
		struct mlag_mroute_add msg;
		int i = 0;
		for (i = 0; i < mlag_msg.msg_cnt; i++) {

			rc = zebra_mlag_lib_decode_mroute_add(s, &msg);
			if (rc)
				return (rc);
			pim_mlag_process_mroute_add(msg);
		}
	} break;
	case MLAG_MROUTE_DEL_BULK: {
		struct mlag_mroute_del msg;
		int i = 0;
		for (i = 0; i < mlag_msg.msg_cnt; i++) {

			rc = zebra_mlag_lib_decode_mroute_del(s, &msg);
			if (rc)
				return (rc);
			pim_mlag_process_mroute_del(msg);
		}
	} break;
	case MLAG_PIM_STATUS_UPDATE: {
		struct mlag_pim_status msg;
		rc = zebra_mlag_lib_decode_pim_status(s, &msg);
		if (rc)
			return (rc);
		pim_mlag_process_peer_status_update(msg);

	} break;
	default:
		break;
	}
	return (0);
}

/****************End of PIM Mesasge processing handler********************/

int pim_zebra_mlag_process_up(void)
{
	if (PIM_DEBUG_MLAG)
		zlog_debug("%s: Received Process-Up from Mlag", __FUNCTION__);

	/*
	 * Incase of local MLAG restyrat, PIM needs to replay all the dat
	 * since MLAG is empty.
	 */
	router->connected_to_mlag = true;
	thread_add_event(router->master, pim_mlag_local_up_handler, NULL, 0,
			 NULL);
	return (0);
}

int pim_zebra_mlag_process_down(void)
{
	if (PIM_DEBUG_MLAG)
		zlog_debug("%s: Received Process-Down from Mlag", __FUNCTION__);

	/*
	 * Local CLAG is down, reset peer data
	 * and forward teh traffic if we are DR
	 */
	router->connected_to_mlag = false;
	thread_add_event(router->master, pim_mlag_down_handler, NULL, 0, NULL);
	return (0);
}

static int pim_mlag_register(struct thread *thread)
{
	uint32_t bit_mask = 0;

	if (!zclient)
		return (-1);
	SET_FLAG(bit_mask, (1 << MLAG_STATUS_UPDATE));
	SET_FLAG(bit_mask, (1 << MLAG_MROUTE_ADD));
	SET_FLAG(bit_mask, (1 << MLAG_MROUTE_DEL));
	SET_FLAG(bit_mask, (1 << MLAG_DUMP));
	SET_FLAG(bit_mask, (1 << MLAG_MROUTE_ADD_BULK));
	SET_FLAG(bit_mask, (1 << MLAG_MROUTE_DEL_BULK));
	SET_FLAG(bit_mask, (1 << MLAG_PIM_STATUS_UPDATE));
	SET_FLAG(bit_mask, (1 << MLAG_PIM_CFG_DUMP));

	if (PIM_DEBUG_MLAG)
		zlog_debug(
			"%s: Posting Client Register to MLAG from PIM, "
			"mask:0x%x",
			__FUNCTION__, bit_mask);

	zclient_send_mlag_register(zclient, bit_mask);
	return (0);
}

static int pim_mlag_deregister(struct thread *thread)
{
	if (!zclient)
		return (-1);

	if (PIM_DEBUG_MLAG)
		zlog_debug("%s: Posting Client De-Register to MLAG from PIM",
			   __FUNCTION__);
	router->connected_to_mlag = false;
	zclient_send_mlag_deregister(zclient);
	return (0);
}

void pim_if_configure_mlag_dualactive(struct pim_interface *pim_ifp)
{
	if (!pim_ifp || pim_ifp->activeactive == true)
		return;

	if (PIM_DEBUG_MLAG)
		zlog_debug("%s: Configuring active-active on Interface: %s",
			   __FUNCTION__, "NULL");

	pim_ifp->activeactive = true;
	if (pim_ifp->pim)
		pim_ifp->pim->inst_mlag_intf_cnt++;

	router->pim_mlag_intf_cnt++;
	if (PIM_DEBUG_MLAG)
		zlog_debug(
			"%s: Total active-active configured Interfaces on "
			"router: %d, Inst:%d",
			__FUNCTION__, pim_ifp->pim->inst_mlag_intf_cnt,
			router->pim_mlag_intf_cnt);

	if (router->pim_mlag_intf_cnt == 1) {
		/*
		 * atleast one Interface is configured for MLAG, send register
		 * to Zebra for receiving MLAG Updates
		 */
		thread_add_event(router->master, pim_mlag_register, NULL, 0,
				 NULL);
	}
}

void pim_if_unconfigure_mlag_dualactive(struct pim_interface *pim_ifp)
{
	if (!pim_ifp || pim_ifp->activeactive == false)
		return;

	if (PIM_DEBUG_MLAG)
		zlog_debug("%s: UnConfiguring active-active on Interface: %s",
			   __FUNCTION__, "NULL");

	pim_ifp->activeactive = false;
	if (pim_ifp->pim)
		pim_ifp->pim->inst_mlag_intf_cnt--;

	router->pim_mlag_intf_cnt--;
	if (PIM_DEBUG_MLAG)
		zlog_debug(
			"%s: Total active-active configured Interfaces on "
			"router: %d, Inst:%d",
			__FUNCTION__, pim_ifp->pim->inst_mlag_intf_cnt,
			router->pim_mlag_intf_cnt);

	if (router->pim_mlag_intf_cnt == 0) {
		/*
		 * all the Interfaces are MLAG un-configured, post MLAG
		 * De-register to Zebra
		 */
		thread_add_event(router->master, pim_mlag_deregister, NULL, 0,
				 NULL);
	}
}


void pim_instance_mlag_init(struct pim_instance *pim)
{
	if (!pim)
		return;

	pim->inst_mlag_intf_cnt = 0;
}


void pim_instance_mlag_terminate(struct pim_instance *pim)
{
	struct interface *ifp;

	if (!pim)
		return;

	FOR_ALL_INTERFACES (pim->vrf, ifp) {
		struct pim_interface *pim_ifp = ifp->info;

		if (!pim_ifp || pim_ifp->activeactive == false)
			continue;

		pim_if_unconfigure_mlag_dualactive(pim_ifp);
	}
	pim->inst_mlag_intf_cnt = 0;
	return;
}

void pim_mlag_init(void)
{
	router->mlag_role = MLAG_ROLE_NONE;
	router->pim_mlag_intf_cnt = 0;
	router->connected_to_mlag = false;
	router->mlag_fifo = stream_fifo_new();
	router->zpthread_mlag_write = NULL;
	router->mlag_stream = stream_new(MLAG_BUF_LIMIT);
}
