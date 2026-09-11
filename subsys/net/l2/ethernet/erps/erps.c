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
#include <zephyr/net/dsa_core.h>
#include <zephyr/net/erps.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/ethernet_vlan.h>
#include <zephyr/net/net_log.h>
#include <zephyr/net/net_ip.h>
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

LOG_MODULE_REGISTER(erps, CONFIG_NET_ERPS_LOG_LEVEL);

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

	/* Local top priority request */
	uint8_t local_topreq;

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

	/* Control VLAN identifier */
	const uint16_t ctrl_vid;

	/* Traffic VLAN identifier */
	const uint16_t traffic_vid;

	/* net_pkt allocation timeout */
	const uint32_t net_pkt_alloc_timeout;

	/* Values to use in sent PDUs */
	struct raps_pdu_mut pdu_mut;

	/* Mutex protecting the state machine */
	struct k_mutex fsm_mutex;

	/* Guard timer */
	k_timepoint_t guard_timer_expiry;

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

static inline char const *erps_request_name(enum erps_request req)
{
	switch (req) {
	case ERPS_REQ_CLEAR:
		return "CLEAR";
	case ERPS_REQ_FS:
		return "FS";
	case ERPS_REQ_RAPS_FS:
		return "R-APS(FS)";
	case ERPS_REQ_SF:
		return "SF";
	case ERPS_REQ_CLEAR_SF:
		return "Local clear SF";
	case ERPS_REQ_RAPS_SF:
		return "R-APS(SF)";
	case ERPS_REQ_RAPS_MS:
		return "R-APS(MS)";
	case ERPS_REQ_MS:
		return "MS";
	case ERPS_REQ_WTR_EXPIRES:
		return "WTR Expires";
	case ERPS_REQ_WTR_RUNNING:
		return "WTR Running";
	case ERPS_REQ_WTB_EXPIRES:
		return "WTB Expires";
	case ERPS_REQ_WTB_RUNNING:
		return "WTB Running";
	case ERPS_REQ_RAPS_NR_RB:
		return "R-APS(NR,RB)";
	case ERPS_REQ_RAPS_NR:
		return "R-APS(NR)";
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

	return net_eth_mac_flush(iface, ETHERNET_FILTER_TYPE_DST_MAC_ADDRESS,
							ETHERNET_MAC_TABLE_DYNAMIC);
}

int erps_link_get_node_id(struct erps_link *lnk, struct net_eth_addr *mac)
{
	struct net_if *iface;
	struct net_linkaddr *link_addr;
	struct erps_node *node = erps_link_get_node(lnk);
	struct erps_link *first_lnk = &node->ports[0u];

	BUILD_ASSERT(
		sizeof(link_addr->addr) >= sizeof(*mac),
		"Link address struct cannot hold a MAC address"
	);

	iface = net_if_lookup_by_dev(first_lnk->dev);
	if (unlikely(!iface)) {
		return -ENODEV;
	}

	/* Use addres of link 0 as node id */
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
	struct erps_node *node;
	struct erps_link *oth_lnk;
	struct net_if *iface, *vlan_iface;

	if (lnk->blocked) {
		return 0;
	}

	iface = net_if_lookup_by_dev(lnk->dev);
	if (unlikely(!iface)) {
		return -ENODEV;
	}

	NET_DBG("Blocking interface %d", net_if_get_by_iface(iface));

	ret = net_eth_port_set_enabled(iface, false);
	if (ret) {
		return ret;
	}
	lnk->blocked = true;

	node = erps_link_get_node(lnk);
	oth_lnk = erps_node_other_link(node, lnk);

	/* Section 10.1.10 */
	erps_link_delete_node_id_bpr(lnk);
	erps_link_delete_node_id_bpr(oth_lnk);

	vlan_iface = net_eth_get_vlan_iface(iface, node->traffic_vid);
	if (!vlan_iface) {
		NET_ERR("Could not get VLAN interface");
		return -ENODEV;
	}

	NET_DBG("Bringing VLAN interface %d down", net_if_get_by_iface(vlan_iface));
	net_if_down(vlan_iface);

	return 0;
}

int erps_link_unblock(struct erps_link *lnk)
{
	int ret;
	struct erps_node *node;
	struct net_if *iface, *vlan_iface;

	lnk->failed = false;
	if (!lnk->blocked) {
		return 0;
	}

	iface = net_if_lookup_by_dev(lnk->dev);
	if (unlikely(!iface)) {
		return -ENODEV;
	}

	NET_DBG("Unblocking interface %d", net_if_get_by_iface(iface));

	ret = net_eth_port_set_enabled(iface, true);
	if (ret) {
		return ret;
	}
	lnk->blocked = false;
	node = erps_link_get_node(lnk);

	vlan_iface = net_eth_get_vlan_iface(iface, node->traffic_vid);

	if (!vlan_iface) {
		NET_ERR("Could not get VLAN interface");
		return -ENODEV;
	}

	NET_DBG("Bringing VLAN interface %d up", net_if_get_by_iface(vlan_iface));
	net_if_up(vlan_iface);

	return 0;
}

static inline int erps_link_unblock_force(struct erps_link *lnk)
{
	lnk->blocked = true;
	return erps_link_unblock(lnk);
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

	NET_DBG("Scheduling R-APS(%s%s%s) TX%s, status 0x%02x",
		raps_req_state_str(req_state),
		status & RAPS_RB ? ",RB" : "",
		status & RAPS_DNF ? ",DNF" : "",
		node->tx_burst == ERPS_RAPS_BURST ? " (burst)" : "",
		(unsigned int)status);

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

static bool erps_is_local_raps_frame(struct erps_link *lnk,
					const struct raps_pdu *pdu)
{
	int ret;
	struct net_eth_addr mac;

	ret = erps_link_get_node_id(lnk, &mac);
	if (ret) {
		NET_ERR("Could not get node id: %d", -ret);
		/* Better to keep going than to risk discard PDUs */
		return false;
	}

	return !memcmp(&mac, &pdu->raps_info.node_id, sizeof(mac));
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

/* Section 10.1.9 */
static bool erps_local_clear_valid(struct erps_node *node, enum erps_request req)
{
	/* Always if local FS or MS in effect */
	switch (node->local_topreq) {
	case ERPS_REQ_FS:
	case ERPS_REQ_MS:
		return true;
	default:
		break;
	}

	/* Never if R-APS(MS) or R-APS(FS) is top request. */
	switch (MIN(req, node->local_topreq)) {
	case ERPS_REQ_RAPS_FS:
	case ERPS_REQ_RAPS_MS:
		return false;
	default:
		break;
	}

	/* Allow if RPL owner */
	return erps_node_is_rpl_owner(node);
}

/* Sections 10.1.1 and  10.1.9 */
static int erps_fsm_resolve_req_prio(struct erps_node *node, enum erps_request req)
{
	if (req == ERPS_REQ_CLEAR && !erps_local_clear_valid(node, req)) {
		return -EBUSY;
	}

	/* Allow CLEAR_SF if SF is active */
	if (req == ERPS_REQ_CLEAR_SF && node->local_topreq == ERPS_REQ_SF) {
		node->local_topreq = ERPS_REQ_INVALID;
	}

	/* Update local top priority request on higher priority request (10.1.9) */
	if (req <= node->local_topreq) {
		switch (req) {
		case ERPS_REQ_SF:
		case ERPS_REQ_FS:
		case ERPS_REQ_MS:
		case ERPS_REQ_WTR_RUNNING:
		case ERPS_REQ_WTB_RUNNING:
			/* Local request is top priority */
			node->local_topreq = req;
			break;
		default:
			/* If local command is overridden ... that command it forgotten (10.1.9) */
			node->local_topreq = ERPS_REQ_INVALID;
			break;
		}
	}
	else {
		NET_DBG("Request '%s' is lower priority than local top request '%s'",
			erps_request_name(req), erps_request_name(node->local_topreq));
		return -EBUSY;
	}

	/* req is top priority, pass it to FSM */
	return 0;
}

static int erps_fsm_post_locked(struct erps_link *lnk, enum erps_request req,
		const struct raps_pdu *pdu)
{
	int ret;
	struct erps_node *node = erps_link_get_node(lnk);

	NET_DBG("FSM request '%s' on interface %d", erps_request_name(req),
			net_if_get_by_iface(net_if_lookup_by_dev(lnk->dev)));

	ret = erps_fsm_resolve_req_prio(node, req);
	if (ret == -EBUSY) {
		NET_DBG("Request '%s' ignored by priority logic", erps_request_name(req));
		return 0;
	}

	if (req == ERPS_REQ_SF) {
		NET_DBG("Marking link as failed");
		lnk->failed = true;
	}

	NET_DBG("State is [%s]", erps_state_name(node->state));
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

	return ret;
}

static inline int erps_fsm_post(struct erps_link *lnk, enum erps_request req,
		const struct raps_pdu *pdu)
{
	int ret;
	struct erps_node *node = erps_link_get_node(lnk);

	/* The network stack posts an SF whenever an interface is brought down.
	 * This includes when ports are blocked.
	 */
	if (lnk->blocked && req == ERPS_REQ_SF) {
		NET_DBG("Ignoring SF on blocked port");
		return 0;
	}

	ret = k_mutex_lock(&node->fsm_mutex, K_MSEC(250));
	if (ret) {
		NET_ERR("Could not lock ERPS FSM mutex: %d", -ret);
		return ret;
	}

	ret = erps_fsm_post_locked(lnk, req, pdu);
	k_mutex_unlock(&node->fsm_mutex);

	NET_DBG("FSM request processed, status %d", ret);
	return ret;
}

int net_erps_fsm_post(struct net_if *iface, enum erps_request req)
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
	NET_DBG("Starting guard timer");
	node->guard_timer_expiry =
		sys_timepoint_calc(K_MSEC(node->guard_timer_duration));
}

static int erps_node_start_timer(struct erps_node *node,
		struct k_work_delayable *dwork)
{
	int ret;
	uint32_t duration;
	k_timeout_t expiry;
	enum erps_request req;

	if (k_work_delayable_is_pending(dwork)) {
		NET_DBG("Timer already running");
		return 0;
	}

	if (dwork == &node->wtr_dwork) {
		req = ERPS_REQ_WTR_RUNNING;
		NET_DBG("Expires in %u mins", node->wtr_duration);
		expiry = K_MINUTES(node->wtr_duration);
	}
	else {
		req = ERPS_REQ_WTB_RUNNING;
		duration = erps_wtb_duration(node);

		NET_DBG("Expires in %u ms", duration);
		expiry = K_MSEC(duration);
	}

	/* WTR/WTB timers are managed entirely by the FSM. This means that the
	 * fsm_mutex is already held whenever this function is called.
	 *
	 * The event can be issued on either link as WTR/WTB timer requests operate
	 * on the entire node rather than individual links.
	 */
	ret = erps_fsm_post_locked(&node->ports[0u], req, NULL);
	if (ret) {
		NET_ERR("Error posting running signal %d: %d", (int)req, -ret);

		/* Don't care, the signal would have been ignored by the FSM either way */
	}

	ret = k_work_reschedule(dwork, expiry);

	return ret < 0 ? ret : 0;
}

int erps_node_start_wtr(struct erps_node *node)
{
	NET_DBG("Starting WTR");
	return erps_node_start_timer(node, &node->wtr_dwork);
}

int erps_node_start_wtb(struct erps_node *node)
{
	NET_DBG("Starting WTB");
	return erps_node_start_timer(node, &node->wtb_dwork);
}


void erps_node_stop_tx(struct erps_node *node)
{
	NET_DBG("Stopping TX");
	k_work_cancel_delayable(&node->tx_dwork);
}

void erps_node_stop_wtr(struct erps_node *node)
{
	NET_DBG("Stop WTR");
	k_work_cancel_delayable(&node->wtr_dwork);
}

void erps_node_stop_wtb(struct erps_node *node)
{
	NET_DBG("Stop WTB");
	k_work_cancel_delayable(&node->wtb_dwork);
}

/* Conditional FDB flush, Section 10.1.10 */
static int erps_raps_node_id_bpr_flush(struct erps_link *lnk, enum erps_request req,
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
	if (req == ERPS_REQ_RAPS_NR) {
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

static inline bool erps_is_stray_raps_pdu(const struct erps_node *node,
					  const struct net_eth_vlan_hdr *hdr)
{
	const struct net_eth_addr *dst = &hdr->dst;

	/* Section 10.1.6, last paragraph */
	return node->ring_id != dst->addr[sizeof(dst->addr) - 1u];

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
	enum erps_request req;
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

	BUILD_ASSERT(ERPS_REQ_RAPS_NR_RB == ERPS_REQ_RAPS_NR - 1, "");

	switch (req_state) {
	case RAPS_NR:
		req = ERPS_REQ_RAPS_NR - raps_pdu_rb(pdu);
		break;
	case RAPS_MS:
		if (node->raps_ver == ERPS_RAPS_VER_1) {
			NET_DBG("MS not supported in version 1, dropping");
			return NET_DROP;
		}
		req = ERPS_REQ_RAPS_MS;
		break;
	case RAPS_SF:
		req = ERPS_REQ_RAPS_SF;
		break;
	case RAPS_FS:
		if (node->raps_ver == ERPS_RAPS_VER_1) {
			NET_DBG("FS not supported in version 1, dropping");
			return NET_DROP;
		}
		req = ERPS_REQ_RAPS_FS;
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

static int erps_read_vlan_hdr(struct erps_link *lnk, struct net_pkt *pkt,
						struct net_eth_vlan_hdr *hdr)
{
	int ret;
	uint16_t vid, tpid, type;
	struct erps_node *node = erps_link_get_node(lnk);

	if (unlikely(net_pkt_remaining_data(pkt) < sizeof(*hdr))) {
		return -ENODATA;
	}

	ret = net_pkt_read(pkt, hdr, sizeof(*hdr));
	if (ret) {
		return ret;
	}

	tpid = net_ntohs(hdr->vlan.tpid);
	if (unlikely(tpid != NET_ETH_PTYPE_VLAN)) {
		NET_DBG("Not a VLAN frame, protocol type 0x%x", (unsigned int)tpid);
		return -EINVAL;
	}

	vid = net_eth_vlan_get_vid(net_ntohs(hdr->vlan.tci));
	if (vid != node->ctrl_vid) {
		NET_DBG("Unexpected VLAN. Got 0x%x, expected 0x%x", (unsigned int)vid,
				(unsigned int)node->ctrl_vid);
		return -EINVAL;
	}

	type = net_ntohs(hdr->type);
	if (unlikely(type != NET_ETH_PTYPE_OAM)) {
		NET_DBG("Not an OAM frame, type 0x%x", (unsigned int)type);
	}

	return 0;
}

static enum net_verdict erps_eth_recv(struct erps_link *lnk,
				struct net_if *iface, struct net_pkt *pkt)
{
	int ret;
	size_t psize;
	struct raps_pdu pdu;
	struct net_eth_vlan_hdr hdr;
	struct erps_node *node = erps_link_get_node(lnk);

	psize = net_pkt_remaining_data(pkt);

	NET_DBG("Incoming frame on interface %d", net_if_get_by_iface(iface));

	ret = erps_read_vlan_hdr(lnk, pkt, &hdr);
	if (ret) {
		NET_DBG("Drop: Invalid VLAN header (%d)", ret);
		return NET_DROP;
	}

	if (unlikely(erps_is_stray_raps_pdu(node, &hdr))) {
		NET_DBG("Stray PDU");
		return NET_DROP;
	}

	LOG_HEXDUMP_DBG(&hdr, sizeof(hdr), "VLAN header: ");

	ret = net_pkt_read(pkt, &pdu, sizeof(pdu));
	if (unlikely(ret)) {
		NET_ERR("Error reading R-APS PDU: %d", -ret);
		return NET_DROP;
	}

	LOG_HEXDUMP_DBG(&pdu, sizeof(pdu), "R-APS PDU: ");

	if (unlikely(erps_is_local_raps_frame(lnk, &pdu))) {
		NET_DBG("Drop: local R-APS frame");
		return NET_DROP;
	}

	return erps_raps_recv(lnk, iface, &pdu);
}

static enum net_verdict erps_recv(struct net_if *iface, uint16_t ptype,
							struct net_pkt *pkt)
{
	enum net_verdict vdct;
	struct erps_link *lnk;
	struct net_pkt_cursor backup;

	NET_DBG("Incoming R-APS PDU");

	lnk = erps_link_lookup_by_iface(iface);
	if (unlikely(!lnk)) {
		NET_ERR("Could not find ERPS link");
		return NET_DROP;
	}

	net_pkt_cursor_backup(pkt, &backup);
	if (likely(net_pkt_get_len(pkt) >= sizeof(struct raps_pdu))) {
		vdct = erps_eth_recv(lnk, iface, pkt);
	}
	else {
		NET_DBG("R-APS packet too small. Expected %zu, have %zu",
			sizeof(struct raps_pdu), net_pkt_get_len(pkt));
		vdct = NET_CONTINUE;
	}

	if (vdct == NET_OK) {
		net_pkt_unref(pkt);
	}
	else {
		net_pkt_cursor_restore(pkt, &backup);
	}

	return vdct;
}

static int erps_raps_create(struct erps_link *lnk, struct net_pkt *pkt)
{
	int ret;
	struct raps_pdu *pdu;
	struct erps_node *node = erps_link_get_node(lnk);
	NET_PKT_DATA_ACCESS_DEFINE(raps_access, struct raps_pdu);

	pdu = net_pkt_get_data(pkt, &raps_access);
	if (!pdu) {
		return -ENOBUFS;
	}

	pdu->cfm_hdr.mel_ver = node->raps_mel << RAPS_MEL_SHIFT;
	pdu->cfm_hdr.mel_ver |= node->raps_ver;
	pdu->cfm_hdr.opcode = RAPS_OPCODE;
	pdu->cfm_hdr.flags = RAPS_FLAGS,
	pdu->cfm_hdr.tlv_off = sizeof(pdu->raps_info);

	pdu->raps_info.rs_sc = node->pdu_mut.rs_sc;
	pdu->raps_info.status = node->pdu_mut.status;

	ret = erps_link_get_node_id(lnk, &pdu->raps_info.node_id);
	if (ret) {
		return ret;
	}
	memset(pdu->raps_info.rfu, 0, sizeof(pdu->raps_info.rfu));

	ret = net_pkt_set_data(pkt, &raps_access);
	if (ret) {
		return -ENOBUFS;
	}

	if (IS_ENABLED(CONFIG_NET_ERPS_PAD_RAPS_PDUS)) {
		struct net_eth_vlan_hdr *hdr;
		uint8_t pad[NET_ETH_MINIMAL_FRAME_SIZE - sizeof(*hdr) - sizeof(*pdu)] = { 0 };

		NET_DBG("Padding R-APS PDU with %zu bytes", sizeof(pad));
		ret = net_pkt_write(pkt, pad, sizeof(pad));
	}

	return ret;
}

static int erps_link_send_pdu(struct erps_link *lnk, struct net_if *iface)
{
	int ret;
	size_t frame_size;
	struct net_pkt *pkt;
	enum net_verdict vdct;
	struct net_eth_addr mac;
	struct erps_node *node = erps_link_get_node(lnk);

	frame_size = sizeof(struct raps_pdu);

	if (IS_ENABLED(CONFIG_NET_ERPS_PAD_RAPS_PDUS)) {
		BUILD_ASSERT(sizeof(struct raps_pdu) < NET_ETH_MINIMAL_FRAME_SIZE);
		frame_size = NET_ETH_MINIMAL_FRAME_SIZE;
	}

	pkt = net_pkt_alloc_with_buffer(iface, frame_size, NET_AF_UNSPEC, 0,
			K_MSEC(node->net_pkt_alloc_timeout));
	if (!pkt) {
		return -ENOMEM;
	}

	net_pkt_set_ll_proto_type(pkt, NET_ETH_PTYPE_OAM);
	ret = net_linkaddr_copy(net_pkt_lladdr_src(pkt), net_if_get_link_addr(iface));

	if (!ret) {
		erps_node_dst_mac(node, &mac);
		ret = net_linkaddr_set(net_pkt_lladdr_dst(pkt), mac.addr, sizeof(mac));
	}

	if (!ret) {
		ret = erps_raps_create(lnk, pkt);
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

static void erps_node_tx_single_pdu(struct erps_node *node)
{
	int ret;
	struct erps_link *lnk;
	struct net_if *iface, *vlan_iface;

	for (unsigned int i = 0u; i < ARRAY_SIZE(node->ports); ++i) {
		lnk = &node->ports[i];

		iface = net_if_lookup_by_dev(lnk->dev);
		if (unlikely(!iface)) {
			NET_ERR("Error looking up interface");
			continue;
		}

		if (!net_if_is_up(iface)) {
			NET_DBG("Interface %d is down (%sRPL)", net_if_get_by_iface(iface),
									lnk->rpl ? "" : "not ");
			continue;
		}

		vlan_iface = net_eth_get_vlan_iface(iface, node->ctrl_vid);
		if (!vlan_iface) {
			NET_ERR("Found no VLAN interface for %d",
				net_if_get_by_iface(iface));
		}

		ret = erps_link_send_pdu(lnk, vlan_iface);
		if (ret) {
			NET_ERR("Error sending R-APS PDU: %d (iface %d)", -ret,
					net_if_get_by_iface(vlan_iface));
		}
		else {
			NET_DBG("R-APS(%s%s%s) - status 0x%02x - on interface %d",
				raps_req_state_str(node->pdu_mut.rs_sc >> RAPS_RS_SHIFT),
				node->pdu_mut.status & RAPS_RB ? ",RB" : "",
				node->pdu_mut.status & RAPS_DNF ? ",DNF" : "",
				(unsigned int)node->pdu_mut.status,
				net_if_get_by_iface(vlan_iface));
		}
	}
}

static void erps_tx_work(struct k_work *work)
{
	int ret;
	k_timeout_t delay;
	struct erps_node *node;
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);

	node = CONTAINER_OF(dwork, struct erps_node, tx_dwork);

	/* Syncronize with TX scheduler */
	if (IS_ENABLED(CONFIG_MULTITHREADING)) {
		atomic_thread_fence(memory_order_acquire);
	}

	erps_node_tx_single_pdu(node);

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
	struct erps_node *node;
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);

	NET_DBG("WTR timer expired");

	node = CONTAINER_OF(dwork, struct erps_node, wtr_dwork);

	/* Doesn't matter on which link the event is triggered */
	ret = erps_fsm_post(&node->ports[0u], ERPS_REQ_WTR_EXPIRES, NULL);
	if (ret) {
		NET_ERR("Error handling WTR expiry: %d", -ret);
	}
}

static void erps_wtb_work(struct k_work *work)
{
	int ret;
	struct erps_node *node;
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);

	NET_DBG("WTB timer expired");

	node = CONTAINER_OF(dwork, struct erps_node, wtb_dwork);

	/* Doesn't matter on which link the event is triggered */
	ret = erps_fsm_post(&node->ports[0u], ERPS_REQ_WTB_EXPIRES, NULL);
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

int net_erps_ring_info_by_iface(struct net_if *iface, struct erps_ring_info *info)
{
	struct erps_link *rpl;
	struct device const *dev = net_if_get_device(iface);

	if (!dev) {
		return -ENODEV;
	}

	STRUCT_SECTION_FOREACH(erps_node, node) {
		for (unsigned int i = 0u; i < ARRAY_SIZE(node->ports); ++i) {
			if (node->ports[i].dev != dev) {
				continue;
			}

			info->ring_id = node->ring_id;
			info->reverting = k_work_delayable_is_pending(&node->wtr_dwork);
			info->ctrl_vid = node->ctrl_vid;
			info->traffic_vid = node->traffic_vid;
			info->requester_index = (uint8_t)i;

			info->blocked[0u] = node->ports[0u].blocked;
			info->blocked[1u] = node->ports[1u].blocked;

			info->iface_port[0u] = net_if_lookup_by_dev(node->ports[0u].dev);
			if (!info->iface_port[0u]) {
				return -ENODEV;
			}

			info->iface_port[1u] = net_if_lookup_by_dev(node->ports[1u].dev);
			if (!info->iface_port[1u]) {
				return -ENODEV;
			}

			rpl = erps_node_get_rpl(node);
			if (rpl) {
				info->rpl_index = 1;
			}
			else {
				info->rpl_index = -1;
			}

			return 0;
		}
	}

	return -EINVAL;
}

void net_erps_ring_mcast_addr(unsigned int ring_id, struct net_eth_addr *mac)
{
	memcpy(mac, &ERPS_MCAST_MAC, sizeof(*mac) - 1u);
	mac->addr[sizeof(mac->addr) - 1u] = (uint8_t)ring_id;
}

static int erps_fsm_init(struct erps_node *node)
{
	int ret;
	bool rpl_owner;
	struct erps_link *rpl = erps_node_get_rpl(node);
	struct erps_link *non_rpl = erps_node_other_link(node, rpl);

	node->guard_timer_expiry = sys_timepoint_calc(K_NO_WAIT);

	rpl_owner = erps_node_is_rpl_owner(node);
	if (rpl_owner || erps_node_is_rpl_nbr(node)) {
		__ASSERT_NO_MSG(rpl);
		__ASSERT_NO_MSG(non_rpl);

		ret = erps_link_block(rpl);
		if (!ret) {
			ret = erps_link_unblock_force(non_rpl);
		}
		if (!ret) {
			ret = erps_node_sched_tx(node, RAPS_NR, 0u, 0u);
		}
		if (!ret && rpl_owner && erps_node_is_revertive(node)) {
			ret = erps_node_start_wtr(node);
		}
	}
	else {
		ret = erps_link_block(&node->ports[0u]);
		if (!ret) {
			ret = erps_link_unblock_force(&node->ports[1u]);
		}
		if (!ret) {
			ret = erps_node_sched_tx(node, RAPS_NR, 0u, 0u);
		}
	}

	erps_fsm_transition(node, ERPS_STATE_PENDING);
	return ret;
}

static int erps_link_pass_ctrl_frames(struct erps_link *lnk)
{
	int ret;
	struct ethernet_context const *eth_ctx;
	struct net_if *iface = net_if_lookup_by_dev(lnk->dev);

	while (iface) {
		NET_DBG("Control frame configuration for interface %d",
			net_if_get_by_iface(iface));

		if (net_if_l2(iface) != &NET_L2_GET_NAME(ETHERNET)) {
			return -EINVAL;
		}

		eth_ctx = net_if_l2_data(iface);
		if (!eth_ctx) {
			return -ENODEV;
		}

		ret = net_eth_pass_ctrl_frames(iface, true);
		switch (ret) {
		case 0:
			NET_DBG("Control frames pass interface %d", net_if_get_by_iface(iface));
			break;
		case -ENOTSUP:
			NET_DBG("Interface %d does not support control frame management",
				net_if_get_by_iface(iface));
			ret = 0;
			break;
		default:
			NET_ERR("Control frame management failure: %d", -ret);
			return ret;
		}

		switch (eth_ctx->dsa_port) {
		case DSA_CONDUIT_PORT:
		case NON_DSA_PORT:
			return 0;
		default:
			break;
		}

		iface = dsa_get_conduit_iface(iface);
	}

	return -ENODEV;
}

static int erps_link_configure_vlan(struct erps_link *lnk)
{
	int ret;
	struct net_eth_addr mac;
	struct net_if *vlan_iface;
	struct erps_node *node = erps_link_get_node(lnk);
	struct net_if *iface = net_if_lookup_by_dev(lnk->dev);

	if (!iface) {
		return -ENODEV;
	}

	erps_node_dst_mac(node, &mac);

	ret = net_eth_vlan_enable(iface, node->ctrl_vid);
	if (ret) {
		NET_ERR("Error enabling VLAN 0x%x: %d", (unsigned int)node->ctrl_vid, -ret);
		return ret;
	}

	ret = net_eth_vlan_enable(iface, node->traffic_vid);
	if (ret) {
		NET_ERR("Error enabling VLAN 0x%x: %d", (unsigned int)node->traffic_vid, -ret);
		return ret;
	}

	vlan_iface = net_eth_get_vlan_iface(iface, node->ctrl_vid);
	if (!vlan_iface) {
		return -ENODEV;
	}

	NET_DBG("Bringing up iface %d", net_if_get_by_iface(vlan_iface));
	net_if_up(vlan_iface);

	return 0;
}

static int erps_node_init(struct erps_node *node)
{
	int ret;

	k_work_init_delayable(&node->tx_dwork, erps_tx_work);
	k_work_init_delayable(&node->wtr_dwork, erps_wtr_work);
	k_work_init_delayable(&node->wtb_dwork, erps_wtb_work);

	node->local_topreq = ERPS_REQ_INVALID;

	ret = k_mutex_init(&node->fsm_mutex);
	for (unsigned int i = 0u; !ret && i < ARRAY_SIZE(node->ports); ++i) {
		ret = erps_link_pass_ctrl_frames(&node->ports[i]);
		if (!ret)
			ret = erps_link_configure_vlan(&node->ports[i]);
	}
	if (!ret) {
		ret = erps_fsm_init(node);
	}

	if (!ret) {
		LOG_DBG("Ring %u node initialized", node->ring_id);
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
		!DT_INST_PROP(n, itu_t_rpl_owner) ||                                           \
			DT_INST_NODE_HAS_PROP(n, itu_t_ring_protection_link),                  \
		"itu-t,rpl-owner needs itu-t,ring-protection-link"                             \
	);                                                                                     \
										               \
	BUILD_ASSERT(                                                                          \
		DT_INST_PROP_LEN(n, itu_t_ring_links) +                                        \
			DT_INST_NODE_HAS_PROP(n, itu_t_ring_protection_link) == 2u,            \
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
		.ctrl_vid = DT_INST_PROP(n, itu_t_control_vlan_identifier),                    \
		.traffic_vid = DT_INST_PROP(n, itu_t_traffic_vlan_identifier),                 \
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

BUILD_ASSERT(CONFIG_NET_ERPS_INIT_PRIORITY > CONFIG_NET_INIT_PRIO);
SYS_INIT(erps_init, POST_KERNEL, CONFIG_NET_ERPS_INIT_PRIORITY);

ETH_NET_L3_REGISTER(ERPS, NET_ETH_PTYPE_OAM, erps_recv);
