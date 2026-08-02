/*
 * Copyright (c) 2026 Vilhelm Engström
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_SUBSYS_NET_L2_ETHERNET_ERPS_ERPS_FSM_H_
#define ZEPHYR_SUBSYS_NET_L2_ETHERNET_ERPS_ERPS_FSM_H_

#include "r-aps.h"

struct erps_link;
struct erps_node;

/* ERPS FSM state */
enum erps_node_state {
	/* State A */
	ERPS_STATE_IDLE,

	/* State B */
	ERPS_STATE_PROTECTION,

	/* State C */
	ERPS_STATE_MANUAL_SWITCH,

	/* State D */
	ERPS_STATE_FORCED_SWITCH,

	/* State E */
	ERPS_STATE_PENDING,
};

/**
 * @brief Post request to the FSM via interface
 *
 * @param iface Interface on which the event is to be posted
 * @param req   Request to post
 *
 * @retval 0      Event successfully processed
 * @retval -errno Error code indicating what went wrong
 */
int net_erps_fsm_post(struct net_if *iface, enum raps_request req);

/*
 * Process request while in idle state
 *
 * param lnk: Link on which the request is to be processed
 * param req: The request
 * param pdu: R-APS PDU that triggered the request, if any
 */
int erps_fsm_post_idle(struct erps_link *lnk, enum raps_request req,
	const struct raps_pdu *pdu);

/*
 * Process request while in protection state
 *
 * param lnk: Link on which the request is to be processed
 * param req: The request
 */
int erps_fsm_post_protection(struct erps_link *lnk, enum raps_request req);

/*
 * Process request while in manual switch state
 *
 * param lnk: Link on which the request is to be processed
 * param req: The request
 */
int erps_fsm_post_manual_switch(struct erps_link *lnk, enum raps_request req);
/*
 * Process request while in forced switch state
 *
 * param lnk: Link on which the request is to be processed
 * param req: The request
 */
int erps_fsm_post_forced_switch(struct erps_link *lnk, enum raps_request req);
/*
 * Process request while in pending state
 *
 * param lnk: Link on which the request is to be processed
 * param req: The request
 * param pdu: R-APS PDU that triggered the request, if any
 */
int erps_fsm_post_pending(struct erps_link *lnk, enum raps_request req,
	const struct raps_pdu *pdu);

/*
 * Transition FSM of provided node to new state
 *
 * param node: Node whose FSM is to be transitioned
 * param next: New state
 */
void erps_fsm_transition(struct erps_node *node, enum erps_node_state next);

/*
 * (Part of) common FS, SF and MS handling
 *
 * param lnk:        Link on which the request is to be processed
 * param req_state:  Request/state for the R-APS PDU to send
 * param next_state: State to transition to
 */
int erps_fsm_fs_sf_ms(struct erps_link *lnk, enum raps_req_state req_state,
					enum erps_node_state next_state);

/*
 * (Part of) FS handling for idle, protection, manual switch and pending states
 *
 * param lnk: Link on which the forced switch is to be processed
 */
inline int erps_fsm_fs_common(struct erps_link *lnk)
{
	return erps_fsm_fs_sf_ms(lnk, RAPS_FS, ERPS_STATE_FORCED_SWITCH);
}

/*
 * (Part of) SF handling for idle, protection, manual switch and pending states
 *
 * param lnk: Link on which the signal fail is to be processed
 */
inline int erps_fsm_sf_common(struct erps_link *lnk)
{
	return erps_fsm_fs_sf_ms(lnk, RAPS_SF, ERPS_STATE_PROTECTION);
}

/*
 * (Part of) MS handling for idle and pending states
 *
 * param lnk: Link on which the manual switch occurs
 */
inline int erps_fsm_ms_common(struct erps_link *lnk)
{
	return erps_fsm_fs_sf_ms(lnk, RAPS_MS, ERPS_STATE_MANUAL_SWITCH);
}

/*
 * Unblock links and stop TX.
 *
 * param lnk:             Link on which to operate
 * param only_non_failed: Unblock only non-failed
 * param next_state:      State to transition to
 */
int erps_fsm_unblock_and_stop(struct erps_link *lnk, bool only_non_failed,
					enum erps_node_state next_state);

/*
 * (Part of) R-APS(FS) handling for idle, protection, manual switch and pending
 *
 * param lnk: Link on which the R-APS(FS) request was received
 */
inline int erps_fsm_raps_fs_common(struct erps_link *lnk)
{
	return erps_fsm_unblock_and_stop(lnk, false, ERPS_STATE_FORCED_SWITCH);
}

/*
 * (Part of) R-APS(SF) handling for idle, manual switch and pending states
 *
 * param lnk: Link on which the R-APS(SF) was received
 */
inline int erps_fsm_raps_sf_common(struct erps_link *lnk)
{
	return erps_fsm_unblock_and_stop(lnk, true, ERPS_STATE_PROTECTION);
}

/*
 * (Part of) R-APS(S) handling for idle and penidng states
 *
 * param lnk: Link on which the R-APS(MS) was received
 */
inline int erps_fsm_raps_ms_common(struct erps_link *lnk)
{
	return erps_fsm_unblock_and_stop(lnk, true, ERPS_STATE_MANUAL_SWITCH);
}

/*
 * If remote node ID is higher than own counterpart, unblock non-failed port and
 * stop TX
 *
 * param lnk: Link connected to own node
 * param pdu: PDU received on the link
 */
int erps_fsm_maybe_unblock_and_stop(struct erps_link *lnk,
					const struct raps_pdu *pdu);

/*
 * Clear processing for manual switch and forced switch states
 *
 * param lnk: Link on which the clear occurred
 */
int erps_fsm_clear_common(struct erps_link *lnk);

#endif /* ZEPHYR_SUBSYS_NET_L2_ETHERNET_ERPS_ERPS_FSM_H_ */
