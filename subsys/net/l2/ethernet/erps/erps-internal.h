/*
 * Copyright (c) 2026 Vilhelm Engström
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SUBSYS_NET_L2_ETHERNET_ERPS_ERPS_INTERNAL_H_
#define ZEPHYR_SUBSYS_NET_L2_ETHERNET_ERPS_ERPS_INTERNAL_H_

#include <stdbool.h>
#include <stdint.h>

#include "r-aps.h"

struct erps_link;
struct erps_node;

/**
 * @brief Request/state and status
 *
 * Lower value has higher priority.
 *
 * See Table 10-1 in the specification
 */
enum erps_request {

	/** Clear */
	ERPS_REQ_CLEAR,

	/** Forced switch */
	ERPS_REQ_FS,

	/** R-APS (forced switch) */
	ERPS_REQ_RAPS_FS,

	/** Local signal fail */
	ERPS_REQ_SF,

	/** Local clear signal fail */
	ERPS_REQ_CLEAR_SF,

	/** R-APS (signal failure) */
	ERPS_REQ_RAPS_SF,

	/** R-APS (manual switch) */
	ERPS_REQ_RAPS_MS,

	/** Manual switch */
	ERPS_REQ_MS,

	/** Wait to restore expires */
	ERPS_REQ_WTR_EXPIRES,

	/** Wait to restore running */
	ERPS_REQ_WTR_RUNNING,

	/** Wait to block expires */
	ERPS_REQ_WTB_EXPIRES,

	/** Wait to block running */
	ERPS_REQ_WTB_RUNNING,

	/** R-APS (no request, RPL blocked) */
	ERPS_REQ_RAPS_NR_RB,

	/** R-APS (no request) */
	ERPS_REQ_RAPS_NR,

	/** Invalid request */
	ERPS_REQ_INVALID,
};


/*
 * Get ERPS node associated with provided link.
 *
 * param lnk: The link whose node is to be looked up
 */
struct erps_node *erps_link_get_node(struct erps_link *lnk);

/*
 * Given an ERPS link and node, get the other link
 *
 * param node: Node associated with the two links
 * param lnk:  The link NOT to be looked up
 */
struct erps_link *erps_node_other_link(struct erps_node *node,
				const struct erps_link *lnk);

/*
 * Get RPL, or NULL if neither link is the RPL
 *
 * param node: The node whose links are to be searched for the RPL
 */
struct erps_link *erps_node_get_rpl(struct erps_node *node);

/*
 * Block the provided link
 *
 * param lnk: Link to be blocked
 */
int erps_link_block(struct erps_link *lnk);

/*
 * Unblock provided link
 *
 * param lnk: Link to be unblocked
 */
int erps_link_unblock(struct erps_link *lnk);

/*
 * Unblock both of the provided node's links
 *
 * param node: Node whose links are to be unblocked
 */
int erps_node_unblock_all(struct erps_node *node);

/*
 * Unblock all non-failed links of the provided node
 *
 * param node: Node whose links are to be unblocked
 */
int erps_node_unblock_non_failed(struct erps_node *node);

/*
 * Determine whether or not the provided link is blocked
 *
 * param lnk: The link whose state is to be queried
 */
bool erps_link_is_blocked(const struct erps_link *lnk);

/*
 * Determine whether at least one link connected to the given node is blocked
 *
 * param node: The node whose links are to be checked
 */
bool erps_node_any_link_blocked(const struct erps_node *node);

/*
 * Schedule R-APS transmission
 *
 * param node:      ERPS node for which TX is to be scheduled
 * param req_state: Request/state to transmit
 * param subcode:   Event subcode
 * param status:    Status byte to include in the R-APS PDU
 */
int erps_node_sched_tx(struct erps_node *node, uint8_t req_state,
		uint8_t subcode, uint8_t status);

/*
 * Stop periodic transmission for the provided node
 *
 * param node: The node for which transmission is to be stopped
 */
void erps_node_stop_tx(struct erps_node *node);

/*
 * Flush forwarding database
 *
 * param lnk: Link on which the flush is to be performed
 */
int erps_flush_fdb(struct erps_link *lnk);

/*
 * Determine whether or not provided node is the RPL owner
 *
 * param node: The node to check
 */
bool erps_node_is_rpl_owner(const struct erps_node *node);

/*
 * Determine whether or not provided node is the RPL neighbor
 *
 * param node: The node to check
 */
bool erps_node_is_rpl_nbr(const struct erps_node *node);

/*
 * Determine whether or not provided node is in revertive mode
 *
 * param node: The node to check
 */
bool erps_node_is_revertive(const struct erps_node *node);

/*
 * Get ERPS node ID (a MAC address)
 *
 * param lnk: One of the links of the node for which the ID is to be gotten
 * param mac: Address to write the ID to
 */
int erps_link_get_node_id(struct erps_link *lnk, struct net_eth_addr *mac);

/*
 * Arm guard timer
 *
 * param node: The node whose guard timer is to be started
 */
void erps_node_start_guard_timer(struct erps_node *node);

/*
 * Start wait-to-restore timer for the provided node
 *
 * param node: The node whose timer is to be started
 */
int erps_node_start_wtr(struct erps_node *node);

/*
 * Start wait-to-block timer for the provided node
 *
 * param node: The node whose timer is to be started
 */
int erps_node_start_wtb(struct erps_node *node);

/*
 * Stop WTR timer
 *
 * param node:  The node whose timer is to be stopped
 */
void erps_node_stop_wtr(struct erps_node *node);

/*
 * Stop WTB timer
 *
 * param node: The node whose timer is to be stopped
 */
void erps_node_stop_wtb(struct erps_node *node);

#endif /* ZEPHYR_SUBSYS_NET_L2_ETHERNET_ERPS_ERPS_INTERNAL_H_ */
