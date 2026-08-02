/*
 * Copyright (c) 2026 Vilhelm Engström
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_log.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/clock.h>
#include <zephyr/toolchain.h>

#include "erps-fsm.h"
#include "erps-internal.h"
#include "r-aps.h"

/* compatible = "itu-t,erps-node"; */
#define DT_DRV_COMPAT itu_t_erps_node

#ifndef NET_ETH_PTYPE_OAM
#define NET_ETH_PTYPE_OAM 0x8902
#endif

#define ERPS_MCAST_MAC								\
	(struct net_eth_addr) { .addr = { 0x01, 0x19, 0xa7, 0x00, 0x00, 0x01 }, }

LOG_MODULE_REGISTER(erps, CONFIG_ERPS_LOG_LEVEL);

enum {
	/* Minimum ring id */
	ERPS_RING_ID_MIN	= 1,

	/* Maximum ring id */
	ERPS_RING_ID_MAX	= 239,
};

/* Burst consists of 3 PDUs sent in quick succession. Counter is 0-based, hence
 * the value being 2
 */
enum { ERPS_RAPS_BURST = 2 };

enum {
	/* Regular TX period, in us */
	ERPS_TX_PERIOD		= 5000000,

	/* TX burst period, in us */
	ERPS_TX_BURST_PERIOD	= 1,

	/* About continuous WTB/R_RUNNING, the spec. says only
	*
	*   While a delay timer is running, the appropriate WTR or the WTB Running signal is continuously
	*   generated.
	*
	* Nothing about the frequency of said generation. Since the signal is handled by the
	* same state machine as R-APS dittos, assuming the same frequency seems appropriate
	*/
	ERPS_WTX_RUNNING_PERIOD = ERPS_TX_PERIOD,
};


union erps_eth_hdr {
	struct net_eth_hdr eth;
	struct net_eth_vlan_hdr vlan;
};

/* Mutable parts of an R-APS PDU */
struct raps_pdu_mut {
	/* Request/state and subcode */
	uint8_t rs_sc;

	/* Status flags */
	uint8_t status;
};

/* Inter-node link */
struct erps_link {
	/* Whether ro nt the link has failed */
	bool failed;

	/* Whether or not the link is blocked */
	bool blocked;

	/* Blocked port reference, second half of (node ID, BPR) pair, see Section 10.1.10 */
	uint8_t bpr;

	/* Whether or not this link is the RPL */
	const bool rpl;

	/* Link index */
	const uint8_t idx;

	/* Node if of received PDU, first half part of (node ID BPR) pair, Section 10.1.10 */
	struct net_eth_addr last_node_id;

	/* Device for the port/MAC node */
	const struct device *dev;
};

/* ERPS ring node */
struct erps_node {
	/* Whether or not to revert traffic to original path on restoration */
	bool revertive;

	/* Current local command */
	uint8_t lcmd;

	/* Whether or not this node is the RPL owner */
	const bool rpl_owner;

	/* Whether or not this node is the RPL neighbor */
	const bool rpl_nbr;

	/* R-APS version */
	const uint8_t raps_ver;

	/* Ring identifier */
	const uint8_t ring_id;

	/* Maintenance entity group level */
	const uint8_t raps_mel;

	/* Wait-to-restore timer duration, in minutes */
	const uint8_t wtr_duration;

	/* Current ring state */
	uint8_t state;

	/* Number of packets remaining in TX burst */
	uint8_t tx_burst;

	/* Duration of the guard timer, in ms */
	const uint16_t guard_timer_duration;

	/* VLAN identifier */
	const uint16_t vid;

	/* net_pkt allocation timeout */
	const uint32_t net_pkt_alloc_timeout;

	/* Values to use in sent PDUs */
	struct raps_pdu_mut pdu_mut;

	/* Mutex protecting the state machine */
	struct k_mutex fsm_mutex;

	/* Guard timer */
	k_timepoint_t guard_timer_expiry;

	/* WTR timer expiry */
	k_timepoint_t wtr_expiry;

	/* WTB timer expiry */
	k_timepoint_t wtb_expiry;

	/* Ports connected to the node */
	struct erps_link ports[2u];

	/* TX work */
	struct k_work_delayable tx_dwork;

	/* Wait-to-restore work */
	struct k_work_delayable wtr_dwork;

	/* Wait-to-block work */
	struct k_work_delayable wtb_dwork;
};

static inline char const *erps_state_name(enum erps_node_state state)
{
	switch (state) {
	case ERPS_STATE_IDLE:
		return "IDLE";
	case ERPS_STATE_PROTECTION:
		return "PROTECTION";
	case ERPS_STATE_MANUAL_SWITCH:
		return "MANUAL_SWITCH";
	case ERPS_STATE_FORCED_SWITCH:
		return "FORCED_SWITCH";
	case ERPS_STATE_PENDING:
		return "PENDING";
	default:
		break;
	}

	return "<unknown>";
}

struct erps_node *erps_link_get_node(struct erps_link *lnk)
{
	return CONTAINER_OF((void *)(lnk - lnk->idx), struct erps_node, ports);
}

static struct erps_link *erps_link_lookup_by_iface(struct net_if *iface)
{
	struct erps_link *lnk;
	const size_t nports = ARRAY_SIZE((((struct erps_node *)0)->ports));
	const struct device *dev = net_if_get_device(iface);

	lnk = NULL;
	STRUCT_SECTION_FOREACH(erps_node, node) {
		for (unsigned int i = 0u; !lnk && i < nports; ++i) {
			if (node->ports[i].dev == dev) {
				lnk = &node->ports[i];
			}
		}

		if (lnk) {
			break;
		}
	}

	return lnk;
}

static inline uint32_t erps_wtb_duration(const struct erps_node *node)
{
	/* Guard timer + 5 seconds */
	return node->guard_timer_duration + 5000;
}

static inline void erps_node_dst_mac(const struct erps_node *node,
					struct net_eth_addr *mac)
{
	BUILD_ASSERT(sizeof(*mac) == sizeof(ERPS_MCAST_MAC), "");
	memcpy(mac, &ERPS_MCAST_MAC, sizeof(*mac) - 1u);
	mac->addr[sizeof(mac->addr) - 1u] = node->ring_id;
}

int erps_flush_fdb(struct erps_link *lnk)
{
	struct net_if *iface = net_if_lookup_by_dev(lnk->dev);

	if (unlikely(!iface)) {
		return -ENODEV;
	}

	return net_eth_fdb_mgmt(iface, FDB_MGMT_FLUSH);
}

int erps_link_get_node_id(struct erps_link *lnk, struct net_eth_addr *mac)
{
	struct net_if *iface;
	struct net_linkaddr *link_addr;

	BUILD_ASSERT(
		sizeof(link_addr->addr) >= sizeof(*mac),
		"Link address struct cannot hold a MAC address"
	);

	iface = net_if_lookup_by_dev(lnk->dev);
	if (unlikely(!iface)) {
		return -ENODEV;
	}

	link_addr = net_if_get_link_addr(iface);
	if (unlikely(!link_addr || link_addr->len != sizeof(*mac))) {
		return -ENXIO;
	}

	memcpy(mac, link_addr->addr, link_addr->len);
	return 0;
}

static void erps_link_delete_node_id_bpr(struct erps_link *lnk)
{
	lnk->bpr = 0;
	memset(&lnk->last_node_id, 0, sizeof(lnk->last_node_id));
}

struct erps_link *erps_node_get_rpl(struct erps_node *node)
{
	/* RPL, if present, is always at index 1 */
	return node->ports[1u].rpl ?
		&node->ports[1u] : (struct erps_link *)NULL;
}

struct erps_link *erps_node_other_link(struct erps_node *node,
				const struct erps_link *lnk)
{
	return lnk ? &node->ports[node->ports == lnk] : NULL;
}

bool erps_link_is_blocked(const struct erps_link *lnk)
{
	return lnk->blocked;
}

bool erps_node_any_link_blocked(const struct erps_node *node)
{
	return node->ports[0u].blocked || node->ports[1u].blocked;
}

int erps_link_block(struct erps_link *lnk)
{
	int ret;
	struct net_if *iface;
	struct erps_node *node;
	struct erps_link *oth_lnk;

	if (lnk->blocked) {
		return 0;
	}

	iface = net_if_lookup_by_dev(lnk->dev);
	if (unlikely(!iface)) {
		return -ENODEV;
	}

	ret = net_if_down(iface);
	if (ret == -EALREADY) {
		ret = 0;
	}
	if (!ret) {
		lnk->blocked = true;

		node = erps_link_get_node(lnk);
		oth_lnk = erps_node_other_link(node, lnk);

		/* Section 10.1.10 */
		erps_link_delete_node_id_bpr(lnk);
		erps_link_delete_node_id_bpr(oth_lnk);
	}

	return ret;
}

int erps_link_unblock(struct erps_link *lnk)
{
	int ret;
	struct net_if *iface;

	if (!lnk->blocked) {
		return 0;
	}

	iface = net_if_lookup_by_dev(lnk->dev);
	if (unlikely(!iface)) {
		return -ENODEV;
	}

	ret = net_if_up(iface);
	if (ret == -EALREADY) {
		ret = 0;
	}
	if (!ret) {
		lnk->blocked = false;
	}
	return ret;
}

int erps_node_unblock_all(struct erps_node *node)
{
	int ret;
	struct erps_link *lnk;

	ret = 0;
	for (unsigned int i = 0u; !ret && i < ARRAY_SIZE(node->ports); ++i) {
		lnk = &node->ports[i];
		ret = erps_link_unblock(lnk);
	}

	return ret;
}

int erps_node_unblock_non_failed(struct erps_node *node)
{
	int ret;
	struct erps_link *lnk;

	ret = 0;
	for (unsigned int i = 0u; !ret && i < ARRAY_SIZE(node->ports); ++i) {
		lnk = &node->ports[i];
		if (!lnk->failed) {
			ret = erps_link_unblock(lnk);
		}
	}

	return ret;
}

static inline int erps_node_wtr_start(struct erps_node *node)
{
	int ret = k_work_reschedule(&node->wtr_dwork,
				K_MINUTES(node->wtr_duration));
	return ret < 0 ? ret : 0;
}

int erps_node_sched_tx(struct erps_node *node, uint8_t req_state,
		uint8_t subcode, uint8_t status)
{
	int ret;
	uint8_t rs_sc;

	switch (req_state) {
	case RAPS_NR:
	case RAPS_MS:
	case RAPS_SF:
	case RAPS_FS:
	case RAPS_EVENT:
		break;
	default:
		return -EINVAL;
	}

	if (unlikely(subcode && req_state != RAPS_EVENT)) {
		return -EINVAL;
	}

	/* Cancel and wait for completion to avoid racing accesses */
	k_work_cancel_delayable_sync(&node->tx_dwork, &(struct k_work_sync) { 0 });

	node->tx_burst = 0u;

	rs_sc = (req_state << RAPS_RS_SHIFT) | subcode;
	if (node->pdu_mut.rs_sc != rs_sc) {
		node->pdu_mut.rs_sc = rs_sc;
		node->tx_burst = ERPS_RAPS_BURST;
	}
	if (node->pdu_mut.status != status) {
		node->pdu_mut.status = status;
		node->tx_burst = ERPS_RAPS_BURST;
	}

	if (IS_ENABLED(CONFIG_MULTITHREADING)) {
		/* Syncronize with TX work handler */
		atomic_thread_fence(memory_order_release);
	}

	ret = k_work_reschedule(
		&node->tx_dwork,
		K_USEC(node->tx_burst ?  ERPS_TX_BURST_PERIOD : ERPS_TX_PERIOD)
	);

	return ret < 0 ? ret : 0;
}

void erps_link_set_failed(struct erps_link *lnk)
{
	lnk->failed = true;
}

void erps_link_clear_failed(struct erps_link *lnk)
{
	lnk->failed = false;
}

bool erps_node_is_rpl_owner(const struct erps_node *node)
{
	return node->rpl_owner;
}

bool erps_node_is_rpl_nbr(const struct erps_node *node)
{
	return node->rpl_nbr;
}

bool erps_node_is_revertive(const struct erps_node *node)
{
	return node->revertive;
}

static int erps_read_eth_hdr(struct erps_link *lnk, struct net_pkt *pkt, union erps_eth_hdr *hdr)
{
	int ret;
	size_t pkt_len;
	uint16_t eth_type, vid;
	const struct erps_node *node = erps_link_get_node(lnk);

	pkt_len = net_pkt_get_len(pkt);

	if (unlikely(pkt_len < sizeof(hdr->eth))) {
		NET_DBG("Packet does not contain a complete Ethernet header");
		return -ENODATA;
	}

	net_pkt_cursor_init(pkt);
	ret = net_pkt_read(pkt, hdr, sizeof(hdr->eth));
	if (ret) {
		NET_ERR("Error reading Ethernet header: %d", -ret);
		return ret;
	}

	eth_type = hdr->eth.type;

	/* 802.1Q? */
	if (eth_type == NET_ETH_PTYPE_VLAN) {
		if (pkt_len < sizeof(hdr->vlan)) {
			NET_DBG("Dropping VLAN packet, too small");
			return -ENODATA;
		}

		ret = net_pkt_read(pkt, (uint8_t *)hdr + sizeof(hdr->eth),
				sizeof(*hdr) - sizeof(hdr->eth));
		if (ret) {
			NET_ERR("Error reading VLAN tpid/tci: %d", -ret);
			return ret;
		}

		vid = net_eth_vlan_get_vid(hdr->vlan.vlan.tci);
		if (unlikely(node->vid != vid)) {
			NET_DBG("Wrong VLAN 0x%x", (unsigned int)vid);
			return -EINVAL;
		}

		eth_type = hdr->vlan.type;
	}

	return (int)eth_type;
}

static bool erps_is_local_raps_frame(struct erps_link *lnk,
					const union erps_eth_hdr *hdr)
{
	int ret;
	struct net_eth_addr mac;

	ret = erps_link_get_node_id(lnk, &mac);
	if (ret) {
		NET_ERR("Could not get node id: %d", -ret);
		/* Better to keep going than to risk discard PDUs */
		return false;
	}

	return !memcmp(&mac, &hdr->eth.dst, sizeof(mac));
}

void erps_fsm_transition(struct erps_node *node, enum erps_node_state next)
{
	if (unlikely(node->state == next)) {
		return;
	}

	/* N.B. this logs the original state on the first transition
	 * as IDLE which, stictly speaking, is incorrect. Shouldn't
	 * be much of a problem though.
	 */
	NET_DBG("ERPS ring 0x%02x transition: [%s] -> [%s]", (unsigned int)node->ring_id,
		erps_state_name(node->state), erps_state_name(next));
	node->state = next;
}

/* Called with fsm_mutex held */
static inline bool erps_fsm_clear_valid(const struct erps_node *node)
{
	switch (node->lcmd) {
	case RAPS_REQ_FS:
	case RAPS_REQ_MS:
		return true;
	default:
		break;
	}

	return erps_node_is_rpl_owner(node) &&
			node->lcmd != RAPS_REQ_RAPS_FS &&
			node->lcmd != RAPS_REQ_RAPS_MS;
}

static int erps_fsm_resolve_req_prio(struct erps_node *node, enum raps_request req)
{
	if (req == RAPS_REQ_CLEAR && !erps_fsm_clear_valid(node)) {
		/* Clear not allowed in this state */
		return -EINVAL;
	}

	if (req == RAPS_REQ_CLEAR_SF && node->lcmd == RAPS_REQ_SF) {
		node->lcmd = RAPS_REQ_INVALID;
	}

	if (req < node->lcmd) {
		switch (req) {
		case RAPS_REQ_CLEAR:
		case RAPS_REQ_FS:
		case RAPS_REQ_MS:
			NET_DBG("Local command is 0x%x", (unsigned int)req);
			node->lcmd = req;
			break;
		default:
			node->lcmd = RAPS_REQ_INVALID;
			break;
		}
	}
	else {
		NET_DBG("Disregarding request 0x%x, current 0x%x", (unsigned int)req, node->lcmd);
		/* Request not high-enough priority, do not pass to FSM */
		return -ENOMSG;
	}

	/* Top priority request */
	return 0;
}

static int erps_fsm_post(struct erps_link *lnk, enum raps_request req,
		const struct raps_pdu *pdu)
{
	int ret;
	struct erps_node *node = erps_link_get_node(lnk);

	ret = k_mutex_lock(&node->fsm_mutex, K_MSEC(250));
	if (ret) {
		NET_ERR("Could not ERPS FSM: %d", -ret);
		return ret;
	}

	ret = erps_fsm_resolve_req_prio(node, req);
	if (!ret) {
		switch (node->state) {
		case ERPS_STATE_IDLE:		/* Table 10-2, rows 2-15 */
			ret = erps_fsm_post_idle(lnk, req, pdu);
			break;
		case ERPS_STATE_PROTECTION:	/* Table 10-2, rows 16-29 */
			ret = erps_fsm_post_protection(lnk, req);
			break;
		case ERPS_STATE_MANUAL_SWITCH:	/* Table 10-2, rows 30-43 */
			ret = erps_fsm_post_manual_switch(lnk, req);
			break;
		case ERPS_STATE_FORCED_SWITCH:	/* Table 10-2, rows 44-57 */
			ret = erps_fsm_post_forced_switch(lnk, req);
			break;
		case ERPS_STATE_PENDING:	/* Table 10-2, rows 58-73 */
			ret = erps_fsm_post_pending(lnk, req, pdu);
			break;
		default:
			NET_ERR("Invalid ERPS state 0x%02x",
					(unsigned int)node->state);
			break;
		}
	}

	k_mutex_unlock(&node->fsm_mutex);
	return ret;
}

int net_erps_fsm_post(struct net_if *iface, enum raps_request req)
{
	struct erps_link *lnk;

	lnk = erps_link_lookup_by_iface(iface);
	if (!lnk) {
		return -ENODEV;
	}

	return erps_fsm_post(lnk, req, NULL);
}

void erps_node_start_guard_timer(struct erps_node *node)
{
	node->guard_timer_expiry =
		sys_timepoint_calc(K_MSEC(node->guard_timer_duration));
}

static int erps_node_start_timer(struct erps_node *node,
		struct k_work_delayable *dwork)
{
	int ret;
	enum raps_request req;

	if (k_work_delayable_is_pending(dwork)) {
		NET_DBG("Timer already running");
		return 0;
	}

	if (dwork == &node->wtr_dwork) {
		req = RAPS_REQ_WTR_RUNNING;
		node->wtr_expiry = sys_timepoint_calc(K_MINUTES(node->wtr_duration));
	}
	else {
		req = RAPS_REQ_WTB_RUNNING;
		node->wtb_expiry = sys_timepoint_calc(K_MSEC(erps_wtb_duration(node)));
	}

	/* Doesn't matter on which link the event is triggered */
	ret = erps_fsm_post(&node->ports[0u], req, NULL);
	if (ret) {
		NET_ERR("Error posting running signal: %d", -ret);

		/* Don't care, the signal would have been ignored by the FSM either way */
	}

	ret = k_work_reschedule(dwork, K_MSEC(ERPS_WTX_RUNNING_PERIOD));

	return ret < 0 ? ret : 0;
}

int erps_node_start_wtr(struct erps_node *node)
{
	return erps_node_start_timer(node, &node->wtr_dwork);
}

int erps_node_start_wtb(struct erps_node *node)
{
	return erps_node_start_timer(node, &node->wtb_dwork);
}


void erps_node_stop_tx(struct erps_node *node)
{
	k_work_cancel_delayable(&node->tx_dwork);
}

void erps_node_stop_wtr(struct erps_node *node)
{
	node->wtr_expiry = sys_timepoint_calc(K_NO_WAIT);
	k_work_cancel_delayable(&node->wtr_dwork);
}

void erps_node_stop_wtb(struct erps_node *node)
{
	node->wtb_expiry = sys_timepoint_calc(K_NO_WAIT);
	k_work_cancel_delayable(&node->wtb_dwork);
}

/* Conditional FDB flush, Section 10.1.10 */
static int erps_raps_node_id_bpr_flush(struct erps_link *lnk, enum raps_request req,
								const struct raps_pdu *pdu)
{
	int ret;
	uint_fast8_t bpr;
	bool node_id_chgd;
	struct erps_node *node;
	struct erps_link *oth_lnk;
	const struct raps_spc_info *raps_info;
	struct net_eth_addr node_id, *last_node_id;
	const struct net_eth_addr *pdu_node_id, *oth_last_node_id;

	node = erps_link_get_node(lnk);
	oth_lnk = erps_node_other_link(node, lnk);
	raps_info = &pdu->raps_info;

	bpr = raps_pdu_bpr(pdu);
	pdu_node_id = &raps_info->node_id;
	last_node_id = &lnk->last_node_id;
	oth_last_node_id = &oth_lnk->last_node_id;

	/* R-APS(NR) does nothing but delete the (node ID,BPR) pair */
	if (req == RAPS_REQ_RAPS_NR) {
		erps_link_delete_node_id_bpr(lnk);
		return 0;
	}

	node_id_chgd = !!memcmp(last_node_id, pdu_node_id, sizeof(*pdu_node_id));
	if (bpr == lnk->bpr || !node_id_chgd) {
		/* Pair unchanged, nothing to do */
		return 0;
	}

	/* Replace (node ID, BPR) pair */
	lnk->bpr = bpr;
	if (node_id_chgd) {
		memcpy(last_node_id, pdu_node_id, sizeof(*pdu_node_id));
	}

	ret = erps_link_get_node_id(lnk, &node_id);
	if (ret) {
		return ret;
	}

	if (!(raps_info->status & RAPS_DNF) || !memcmp(&node_id, pdu_node_id, sizeof(node_id))) {
		/* DNF being set or the PDU containing this node's ID should not trigger a flush */
		return 0;
	}

	if (bpr != oth_lnk->bpr || memcmp(oth_last_node_id, pdu_node_id, sizeof(*pdu_node_id))) {
		/* Neither link has seen this (node ID, BPR) pair last, DNF is not set, the PDU does
		 * not contain this node's ID and the request is not R-APS(NR). Flush */
		ret = erps_flush_fdb(lnk);
	}

	return ret;
}

static inline bool erps_is_stray_raps_pdu(const struct erps_node *node, const struct raps_pdu *pdu)
{
	/* Section 10.1.6, last paragraph */
	return node->ring_id != raps_pdu_get_ring_id(pdu);

}

static int erps_handle_raps_event(struct erps_link *lnk, const struct raps_pdu *pdu)
{
	uint_fast8_t subcode;

	subcode = raps_pdu_get_subcode(pdu);
	if (subcode != RAPS_SC_FLUSH_REQ) {
		NET_DBG("Invalid event subcode 0x%x",
					(unsigned int)subcode);
		return -EINVAL;
	}
	if (raps_pdu_status(pdu)) {
		NET_DBG("Event with invalid status 0x%x",
				(unsigned int)raps_pdu_status(pdu));
		return -EINVAL;
	}

	/* Note: the specification seems to treat the ring and the flush logic as
	 * distinct entities, expecting the ring to signal the flush logic for 10ms
	 * here rather than simply carrying out the flush itself.
	 */
	return erps_flush_fdb(lnk);
}

static enum net_verdict erps_raps_recv(struct erps_link *lnk, struct net_if *iface,
								const struct raps_pdu *pdu)
{
	int ret;
	enum raps_request req;
	uint_fast8_t req_state;
	struct erps_node *node = erps_link_get_node(lnk);

	req_state = raps_pdu_get_req_state(pdu);
	NET_DBG("R-APS(%s%s%s), status 0x%02x",
		raps_req_state_str(req_state),
		raps_pdu_rb(pdu) ? ",RB" : "",
		raps_pdu_dnf(pdu) ? ",DNF" : "",
		(unsigned int)raps_pdu_status(pdu));

	if (unlikely(pdu->cfm_hdr.opcode != RAPS_OPCODE)) {
		NET_DBG("Discarding CFM frame, wrong opcode 0x%02x",
			(unsigned int)pdu->cfm_hdr.opcode);
		return NET_DROP;
	}

	if (unlikely(erps_is_stray_raps_pdu(node, pdu))) {
		return NET_DROP;
	}

	BUILD_ASSERT(RAPS_REQ_RAPS_NR_RB == RAPS_REQ_RAPS_NR - 1, "");

	switch (req_state) {
	case RAPS_NR:
		req = RAPS_REQ_RAPS_NR - raps_pdu_rb(pdu);
		break;
	case RAPS_MS:
		if (node->raps_ver == ERPS_RAPS_VER_1) {
			NET_DBG("MS not supported in version 1, dropping");
			return NET_DROP;
		}
		req = RAPS_REQ_RAPS_MS;
		break;
	case RAPS_SF:
		req = RAPS_REQ_RAPS_SF;
		break;
	case RAPS_FS:
		if (node->raps_ver == ERPS_RAPS_VER_1) {
			NET_DBG("FS not supported in version 1, dropping");
			return NET_DROP;
		}
		req = RAPS_REQ_RAPS_FS;
		break;
	case RAPS_EVENT:
		ret = erps_handle_raps_event(lnk, pdu);
		return ret ? NET_DROP : NET_OK;
	default:
		NET_ERR("Unsupported R-APS request/state 0x%02x",
						(unsigned int)req_state);
		return NET_DROP;
	}

	if (!sys_timepoint_expired(node->guard_timer_expiry)) {
		NET_DBG("R-APS packet dropped due to guard timer");
		return NET_DROP;
	}

	/* Section 10.1.10 */
	ret = erps_raps_node_id_bpr_flush(lnk, req, pdu);
	if (ret) {
		NET_ERR("Error handling (node ID, BPR)-based flush: %d", -ret);
	}

	ret = erps_fsm_post(lnk, req, pdu);
	return ret ? NET_DROP : NET_OK;
}

static enum net_verdict erps_eth_recv(struct erps_link *lnk,
				struct net_if *iface, struct net_pkt *pkt)
{
	size_t psize;
	int ret, eth_type;
	struct raps_pdu pdu;
	union erps_eth_hdr hdr;

	psize = net_pkt_remaining_data(pkt);

	eth_type = erps_read_eth_hdr(lnk, pkt, &hdr);
	if (unlikely(eth_type < 0)) {
		return NET_DROP;
	}

	LOG_HEXDUMP_DBG((void *)&hdr, psize - net_pkt_remaining_data(pkt),
		"Ethernet header: ");

	if (unlikely(eth_type != NET_ETH_PTYPE_OAM)) {
		return NET_DROP;
	}

	if (unlikely(erps_is_local_raps_frame(lnk, &hdr))) {
		return NET_DROP;
	}

	ret = net_pkt_read(pkt, &pdu, sizeof(pdu));
	if (unlikely(ret)) {
		NET_ERR("Error reading R-APS PDU: %d", -ret);
		return NET_DROP;
	}

	return erps_raps_recv(lnk, iface, &pdu);
}

static enum net_verdict erps_recv(struct net_if *iface, uint16_t ptype,
							struct net_pkt *pkt)
{
	struct erps_link *lnk;

	NET_DBG("Incoming R-APS PDU");

	lnk = erps_link_lookup_by_iface(iface);
	if (unlikely(!lnk)) {
		NET_ERR("Could not find ERPS link");
		return NET_DROP;
	}

	if (unlikely(net_pkt_get_len(pkt) < sizeof(struct raps_pdu))) {
		NET_DBG("R-APS packet too small. Expected %zu, have %zu",
			sizeof(struct raps_pdu), net_pkt_get_len(pkt));
		return NET_DROP;
	}

	return erps_eth_recv(lnk, iface, pkt);
}

static inline int erps_link_put_cfm_hdr(struct erps_link *lnk,
						struct net_pkt *pkt)
{
	struct erps_node *node = erps_link_get_node(lnk);
	struct raps_cfm_pdu_hdr cfm_hdr = {
		.mel_ver = (node->raps_mel << RAPS_MEL_SHIFT) | node->raps_ver,
		.opcode = RAPS_OPCODE,
		.flags = RAPS_FLAGS,
		.tlv_off = sizeof(struct raps_spc_info),
	};

	return net_pkt_write(pkt, &cfm_hdr, sizeof(cfm_hdr));
}

static int erps_link_put_raps_spc_info(struct erps_link *lnk,
			struct net_if *iface, struct net_pkt *pkt)
{
	int ret;
	struct net_eth_addr mac;
	struct erps_node *node = erps_link_get_node(lnk);

	ret = erps_link_get_node_id(lnk, &mac);
	if (!ret) {
		ret = net_pkt_write_u8(pkt, node->pdu_mut.rs_sc);
	}
	if (!ret) {
		ret = net_pkt_write_u8(pkt, node->pdu_mut.status | (lnk->bpr << RAPS_BPR_SHIFT));
	}
	if (!ret) {
		ret = net_pkt_write(pkt, mac.addr, sizeof(mac.addr));
	}
	if (!ret) {
		ret = net_pkt_memset(pkt, 0,
				sizeof(((struct raps_spc_info *)0)->rfu));
	}

	return ret;
}

static int erps_link_send_pdu(struct erps_link *lnk, struct net_if *iface)
{
	int ret;
	struct net_iface;
	struct net_pkt *pkt;
	enum net_verdict vdct;
	struct net_eth_addr mac;
	struct erps_node *node = erps_link_get_node(lnk);

	pkt = net_pkt_alloc_with_buffer(iface, sizeof(struct raps_pdu),
		NET_AF_UNSPEC, 0, K_MSEC(node->net_pkt_alloc_timeout));
	if (!pkt) {
		return -ENOMEM;
	}

	net_pkt_set_ll_proto_type(pkt, NET_ETH_PTYPE_OAM);
	net_pkt_set_vlan_tag(pkt, node->vid);
	ret = net_linkaddr_copy(net_pkt_lladdr_src(pkt),
		net_if_get_link_addr(iface));
	if (!ret) {
		erps_node_dst_mac(node, &mac);
		ret = net_linkaddr_set(net_pkt_lladdr_dst(pkt), mac.addr,
								sizeof(mac));
	}
	if (!ret) {
		ret = erps_link_put_cfm_hdr(lnk, pkt);
	}
	if (!ret) {
		ret = erps_link_put_raps_spc_info(lnk, iface, pkt);
	}
	if (!ret) {
		vdct = net_if_try_send_data(iface, pkt, K_NO_WAIT);
		if (vdct == NET_DROP) {
			ret = -EIO;
		}
	}
	if (ret) {
		net_pkt_unref(pkt);
	}

	return ret;
}

static void erps_tx_work(struct k_work *work)
{
	int ret;
	k_timeout_t delay;
	struct net_if *iface;
	struct erps_link *lnk;
	struct erps_node *node;
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);

	node = CONTAINER_OF(dwork, struct erps_node, tx_dwork);

	if (IS_ENABLED(CONFIG_MULTITHREADING)) {
		/* Syncronize with TX scheduler */
		atomic_thread_fence(memory_order_acquire);
	}

	for (unsigned int i = 0u; i < ARRAY_SIZE(node->ports); ++i) {
		lnk = &node->ports[i];

		iface = net_if_lookup_by_dev(lnk->dev);
		if (unlikely(!iface)) {
			NET_ERR("Error looking up interface");
			continue;
		}

		ret = erps_link_send_pdu(lnk, iface);
		if (ret) {
			NET_ERR("Error sending R-APS PDU: %d", -ret);
		}
	}

	delay = K_USEC(ERPS_TX_PERIOD);
	if (node->tx_burst) {
		delay = K_USEC(ERPS_TX_BURST_PERIOD);
		--node->tx_burst;
	}

	ret = k_work_reschedule(dwork, delay);
	if (ret < 0) {
		NET_ERR("Error rescheduling TX: %d", -ret);
	}
}

static void erps_wtr_work(struct k_work *work)
{
	int ret;
	enum raps_request req;
	struct erps_node *node;
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);

	node = CONTAINER_OF(dwork, struct erps_node, wtr_dwork);

	req = RAPS_REQ_WTR_EXPIRES;
	if (!sys_timepoint_expired(node->wtr_expiry)) {
		req = RAPS_REQ_WTR_RUNNING;
		ret = k_work_reschedule(dwork, K_MSEC(5000));
		if (ret < 0) {
			NET_ERR("Could not reschedule WTR: %d", -ret);
		}
	}

	/* Doesn't matter on which link the event is triggered */
	ret = erps_fsm_post(&node->ports[0u], req, NULL);
	if (ret) {
		NET_ERR("Error handling WTR expiry: %d", -ret);
	}
}

static void erps_wtb_work(struct k_work *work)
{
	int ret;
	enum raps_request req;
	struct erps_node *node;
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);

	node = CONTAINER_OF(dwork, struct erps_node, wtb_dwork);

	req = RAPS_REQ_WTB_EXPIRES;
	if (!sys_timepoint_expired(node->wtb_expiry)) {
		req = RAPS_REQ_WTB_RUNNING;
		ret = k_work_reschedule(dwork, K_MSEC(5000));
		if (ret < 0) {
			NET_ERR("Could not reschedule WTB: %d", -ret);
		}
	}

	/* Doesn't matter on which link the event is triggered */
	ret = erps_fsm_post(&node->ports[0u], req, NULL);
	if (ret) {
		NET_ERR("Error handling WTB expiry: %d", -ret);
	}
}

#if defined(CONFIG_ERPS_SHELL)
struct net_if *net_erps_lookup_iface(uint8_t ring_id, uint8_t port)
{
	if (port >= ARRAY_SIZE((((struct erps_node *)0)->ports))) {
		return NULL;
	}

	STRUCT_SECTION_FOREACH(erps_node, node) {
		if (node->ring_id != ring_id) {
			continue;
		}

		return net_if_lookup_by_dev(node->ports[port].dev);
	}

	return NULL;
}
#endif /* CONFIG_ERPS_SHELL */

static int erps_enable_vlan(struct erps_node *node)
{
	int ret;
	struct net_if *iface;
	struct erps_link *lnk;

	ret = 0;
	for (unsigned int i = 0u; !ret && i < ARRAY_SIZE(node->ports); ++i) {
		lnk = &node->ports[i];

		iface = net_if_lookup_by_dev(lnk->dev);
		if (unlikely(!iface)) {
			ret = -ENODEV;
		}

		if (!ret) {
			ret = net_eth_vlan_enable(iface, node->vid);
		}
	}

	return ret;
}

static int erps_fsm_init(struct erps_node *node)
{
	int ret;
	bool rpl_owner;
	struct erps_link *rpl = erps_node_get_rpl(node);
	struct erps_link *non_rpl = erps_node_other_link(node, rpl);

	node->guard_timer_expiry = sys_timepoint_calc(K_NO_WAIT);
	node->wtr_expiry = node->guard_timer_expiry;
	node->wtb_expiry = node->guard_timer_expiry;

	rpl_owner = erps_node_is_rpl_owner(node);
	if (rpl_owner || erps_node_is_rpl_nbr(node)) {
		__ASSERT_NO_MSG(rpl);
		__ASSERT_NO_MSG(non_rpl);

		ret = erps_link_block(rpl);
		if (!ret) {
			ret = erps_link_unblock(non_rpl);
		}
		if (!ret) {
			ret = erps_node_sched_tx(node, RAPS_NR, 0u, 0u);
		}
		if (!ret && rpl_owner && erps_node_is_revertive(node)) {
			ret = erps_node_wtr_start(node);
		}
	}
	else {
		ret = erps_link_block(&node->ports[0u]);
		if (!ret) {
			ret = erps_link_unblock(&node->ports[1u]);
		}
		if (!ret) {
			ret = erps_node_sched_tx(node, RAPS_NR, 0u, 0u);
		}
	}

	erps_fsm_transition(node, ERPS_STATE_PENDING);
	return ret;
}

static int erps_node_init(struct erps_node *node)
{
	int ret;

	k_work_init_delayable(&node->tx_dwork, erps_tx_work);
	k_work_init_delayable(&node->wtr_dwork, erps_wtr_work);
	k_work_init_delayable(&node->wtb_dwork, erps_wtb_work);

	ret = k_mutex_init(&node->fsm_mutex);
	if (!ret) {
		ret = erps_enable_vlan(node);
	}
	if (!ret) {
		ret = erps_fsm_init(node);
	}

	return ret;
}


#define ERPS_LINK_DEVICE_GET(link_idx, n)				                       \
	DEVICE_DT_GET(                                                                         \
		COND_CODE_0(link_idx,                                                          \
			(DT_INST_PHANDLE(n, itu_t_ring_links)),                                \
			(COND_CODE_1(DT_INST_NODE_HAS_PROP(n, itu_t_ring_protection_link),     \
				(DT_INST_PHANDLE(n, itu_t_ring_protection_link)),              \
				(DT_INST_PHANDLE_BY_IDX(n, itu_t_ring_links, 1))               \
			))                                                                     \
		)                                                                              \
	)

#define ERPS_LINK_INIT(link_idx, n, is_rpl)                                                    \
	[link_idx] = {                                                                         \
		.rpl = is_rpl,                                                                 \
		.idx = link_idx,                                                               \
		.dev = ERPS_LINK_DEVICE_GET(link_idx, n),                                      \
	}

#define ERPS_NODE_LINKS(n)                                                                     \
	{                                                                                      \
		LISTIFY(                                                                       \
			DT_INST_PROP_LEN(n, itu_t_ring_links),                                 \
			ERPS_LINK_INIT,                                                        \
			(,),                                                                   \
			n,                                                                     \
			false                                                                  \
		),                                                                             \
		COND_CODE_1(                                                                   \
			DT_INST_NODE_HAS_PROP(                                                 \
				n, itu_t_ring_protection_link                                  \
			),                                                                     \
			(ERPS_LINK_INIT(1 , n, true),),                                        \
			(EMPTY)                                                                \
		)                                                                              \
	}


#define ERPS_NODE_INIT(n)                                                                      \
	BUILD_ASSERT(                                                                          \
		!DT_INST_NODE_HAS_PROP(n, itu_t_rpl_owner) ||                                  \
			DT_INST_NODE_HAS_PROP(n, itu_t_ring_protection_link),                  \
		"itu-t,rpl-owner needs itu-t,ring-protection-link"                             \
	);                                                                                     \
										               \
	BUILD_ASSERT(                                                                          \
		DT_INST_PROP_LEN(n, itu_t_ring_links) +                                        \
			DT_INST_NODE_HAS_PROP(n, itu_t_ring_protection_link) ==                \
		2u,								               \
		"Each node requires exactly two links"                                         \
	);                                                                                     \
										               \
	BUILD_ASSERT(                                                                          \
		DT_INST_PROP(n, itu_t_ring_id) >= ERPS_RING_ID_MIN,                            \
		"itu-t,ring-id is too small"                                                   \
	);                                                                                     \
										               \
	BUILD_ASSERT(                                                                          \
		DT_INST_PROP(n, itu_t_ring_id) <= ERPS_RING_ID_MAX,                            \
		"itu-t,ring-id is too large"                                                   \
	);                                                                                     \
										               \
	BUILD_ASSERT(                                                                          \
		DT_INST_PROP(n, itu_t_guard_timer_duration) >= 10,                             \
		"Guard timer duration must be at least 10ms"                                   \
	);                                                                                     \
										               \
	BUILD_ASSERT(                                                                          \
		DT_INST_PROP(n, itu_t_guard_timer_duration) <= 2000,                           \
		"Guard timer duration must be at most 2000ms"                                  \
	);                                                                                     \
										               \
	BUILD_ASSERT(                                                                          \
		!(DT_INST_PROP(n, itu_t_guard_timer_duration) % 10),                           \
		"Guard timer duration must be a multiple of 10"                                \
	);                                                                                     \
										               \
	STRUCT_SECTION_ITERABLE(erps_node, erps_node_ ## n) = {                                \
		.revertive = !DT_INST_PROP(n, itu_t_non_revertive),                            \
		.rpl_owner = DT_INST_PROP(n, itu_t_rpl_owner),                                 \
		.rpl_nbr = !DT_INST_PROP(n, itu_t_rpl_owner) &&                                \
			DT_INST_NODE_HAS_PROP(n, itu_t_ring_protection_link),                  \
		.raps_ver = DT_INST_PROP(n, itu_t_raps_version),                               \
		.ring_id = DT_INST_PROP(n, itu_t_ring_id),                                     \
		.raps_mel = DT_INST_PROP(n, itu_t_raps_mel),                                   \
		.wtr_duration = DT_INST_PROP(n, itu_t_wtr_timer_duration),                     \
		.guard_timer_duration = DT_INST_PROP(                                          \
			n, itu_t_guard_timer_duration                                          \
		),                                                                             \
		.vid = DT_INST_PROP(n, itu_t_vlan_identifier),                                 \
		.net_pkt_alloc_timeout = DT_INST_PROP(                                         \
			n, itu_t_net_pkt_alloc_timeout                                         \
		),                                                                             \
		.ports = ERPS_NODE_LINKS(n),                                                   \
	};                                                                                     \

DT_INST_FOREACH_STATUS_OKAY(ERPS_NODE_INIT)

static int erps_init(void)
{
	int ret;

	ret = 0;
	STRUCT_SECTION_FOREACH(erps_node, node) {
		ret = erps_node_init(node);

		if (ret) {
			LOG_ERR("Error initializing ERPS ring 0x%x", (unsigned int)node->ring_id);

			/* Other rings may still be okay */
		}
	}

	return 0;
}

SYS_INIT(erps_init, POST_KERNEL, CONFIG_ERPS_INIT_PRIORITY);

ETH_NET_L3_REGISTER(ERPS, NET_ETH_PTYPE_OAM, erps_recv);
