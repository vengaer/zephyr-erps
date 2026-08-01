/*
 * Copyright (c) 2026 Vilhelm Engström
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include <zephyr/logging/log.h>
#include <zephyr/net/net_log.h>

#include "erps-fsm.h"
#include "erps-internal.h"

LOG_MODULE_REGISTER(erps_fsm_pending, CONFIG_ERPS_LOG_LEVEL);

static int erps_fsm_clear_pending(struct erps_link *lnk)
{
	int ret;
	uint8_t status;
	struct erps_link *rpl, *non_rpl;
	struct erps_node *node = erps_link_get_node(lnk);

	rpl = erps_node_get_rpl(node);
	non_rpl = erps_node_other_link(node, rpl);

	if (erps_node_is_rpl_owner(node)) {
		erps_node_stop_wtr(node);
		erps_node_stop_wtb(node);
	}

	ret = 0;
	if (rpl) {
		__ASSERT_NO_MSG(non_rpl);

		status = RAPS_RB | RAPS_DNF;
		if (!erps_link_is_blocked(rpl)) {
			ret = erps_link_block(rpl);
			status &= ~RAPS_DNF;
		}
		if (!ret) {
			ret = erps_node_sched_tx(node, RAPS_NR, 0u, status);
		}
		if (!ret) {
			ret = erps_link_unblock(non_rpl);
		}
		if (!ret && !(status & RAPS_DNF)) {
			ret = erps_flush_fdb();
		}
	}

	if (!ret) {
		erps_fsm_transition(node, ERPS_STATE_IDLE);
	}
	else {
		NET_ERR("Error executing CLEAR while PENDING: %d", -ret);
	}

	return ret;
}

static int erps_fsm_fs_pending(struct erps_link *lnk)
{
	int ret;
	struct erps_node *node = erps_link_get_node(lnk);

	ret = erps_fsm_fs_common(lnk);
	if (!ret) {
		if (erps_node_is_rpl_owner(node)) {
			erps_node_stop_wtr(node);
			erps_node_stop_wtb(node);
		}
	}
	else {
		NET_ERR("Error executing FS while PENDING: %d", -ret);
	}

	return ret;
}

static int erps_fsm_raps_fs_pending(struct erps_link *lnk)
{
	int ret;
	struct erps_node *node = erps_link_get_node(lnk);

	ret = erps_fsm_raps_fs_common(lnk);
	if (!ret) {
		if (erps_node_is_rpl_owner(node)) {
			erps_node_stop_wtr(node);
			erps_node_stop_wtb(node);
		}
	}
	else {
		NET_ERR("Error executing R-APS(FS) while PENDING: %d", -ret);
	}

	return ret;
}

static int erps_fsm_sf_pending(struct erps_link *lnk)
{
	int ret;
	struct erps_node *node = erps_link_get_node(lnk);

	ret = erps_fsm_sf_common(lnk);
	if (!ret) {
		if (erps_node_is_rpl_owner(node)) {
			erps_node_stop_wtr(node);
			erps_node_stop_wtb(node);
		}
	}
	else {
		NET_ERR("Error executing SF while PENDING: %d", -ret);
	}

	return ret;
}

static int erps_fsm_raps_sf_pending(struct erps_link *lnk)
{
	int ret;
	struct erps_node *node = erps_link_get_node(lnk);

	ret = erps_fsm_raps_sf_common(lnk);
	if (!ret) {
		if (erps_node_is_rpl_owner(node)) {
			erps_node_stop_wtr(node);
			erps_node_stop_wtb(node);
		}
	}
	else {
		NET_ERR("Error executing R-APS(SF) while PENDING: %d", -ret);
	}

	return ret;
}

static int erps_fsm_raps_ms_pending(struct erps_link *lnk)
{
	int ret;
	struct erps_node *node = erps_link_get_node(lnk);

	ret = erps_fsm_raps_ms_common(lnk);
	if (!ret) {
		if (erps_node_is_rpl_owner(node)) {
			erps_node_stop_wtr(node);
			erps_node_stop_wtb(node);
		}
	}
	else {
		NET_ERR("Error executing R-APS(MS) while PENDING: %d", -ret);
	}

	return ret;
}

static int erps_fsm_ms_pending(struct erps_link *lnk)
{
	int ret;
	struct erps_node *node = erps_link_get_node(lnk);

	if (erps_node_is_rpl_owner(node)) {
		erps_node_stop_wtr(node);
		erps_node_stop_wtb(node);
	}

	ret = erps_fsm_ms_common(lnk);
	if (ret) {
		NET_ERR("Error executing MS while PENDING: %d", -ret);
	}

	return ret;
}

static int erps_fsm_timer_expires_pending(struct erps_link *lnk)
{
	int ret;
	uint8_t status;
	struct erps_link *rpl, *non_rpl;
	struct erps_node *node = erps_link_get_node(lnk);

	rpl = erps_node_get_rpl(node);
	non_rpl = erps_node_other_link(node, rpl);

	__ASSERT_NO_MSG(rpl);
	__ASSERT_NO_MSG(non_rpl);

	ret = 0;
	status = RAPS_RB | RAPS_DNF;
	if (!erps_link_is_blocked(rpl)) {
		ret = erps_link_block(rpl);
		status &= ~RAPS_DNF;
	}
	if (!ret) {
		ret = erps_node_sched_tx(node, RAPS_NR, 0u, status);
	}
	if (!ret) {
		ret = erps_link_unblock(non_rpl);
	}
	if (!ret && !(status & RAPS_DNF)) {
		ret = erps_flush_fdb();
	}
	if (!ret) {
		erps_fsm_transition(node, ERPS_STATE_IDLE);
	}

	return ret;
}

static int erps_fsm_wtr_expires_pending(struct erps_link *lnk)
{
	int ret;
	struct erps_node *node = erps_link_get_node(lnk);

	ret = 0;
	if (erps_node_is_rpl_owner(node)) {
		erps_node_stop_wtb(node);
		ret = erps_fsm_timer_expires_pending(lnk);
	}

	if (ret) {
		NET_ERR("Error executing WTR_EXPIRES while PENDING %d", -ret);
	}

	return ret;
}

static int erps_fsm_wtb_expires_pending(struct erps_link *lnk)
{
	int ret;
	struct erps_node *node = erps_link_get_node(lnk);

	ret = 0;
	if (erps_node_is_rpl_owner(node)) {
		erps_node_stop_wtr(node);
		ret = erps_fsm_timer_expires_pending(lnk);
	}

	if (ret) {
		NET_ERR("Error executing WTR_EXPIRES while PENDING: %d", -ret);
	}

	return ret;
}

static int erps_fsm_raps_nr_rb_pending(struct erps_link *lnk)
{
	int ret;
	bool rpl_owner, rpl_nbr;
	struct erps_link *rpl, *non_rpl;
	struct erps_node *node = erps_link_get_node(lnk);

	ret = 0;
	rpl_nbr = erps_node_is_rpl_nbr(node);
	rpl_owner = erps_node_is_rpl_owner(node);

	switch ((rpl_owner << 1) | rpl_nbr) {
	case (true << 1) | false: /* RPL owner */
		erps_node_stop_wtr(node);
		erps_node_stop_wtb(node);
		break;
	case (false << 1) | false: /* Neither RPL owner nor nbr */
		ret = erps_node_unblock_all(node);
		if (!ret) {
			erps_node_stop_tx(node);
		}
		break;
	case (false << 1) | true: /* RPL nbr */
		rpl = erps_node_get_rpl(node);
		non_rpl = erps_node_other_link(node, rpl);

		__ASSERT_NO_MSG(rpl);
		__ASSERT_NO_MSG(non_rpl);

		ret = erps_link_block(rpl);
		if (!ret) {
			ret = erps_link_unblock(non_rpl);
		}
		if (!ret) {
			erps_node_stop_tx(node);
		}
		break;
	default:
		CODE_UNREACHABLE;
		break;
	}

	if (!ret) {
		erps_fsm_transition(node, ERPS_STATE_IDLE);
	}
	else {
		NET_ERR("Error executing R-APS(NR,RB) while PENDING: %d", -ret);
	}

	return ret;
}

static inline int erps_fsm_raps_nr_pending(struct erps_link *lnk,
					const struct raps_pdu *pdu)
{
	int ret = erps_fsm_maybe_unblock_and_stop(lnk, pdu);

	if (ret) {
		NET_ERR("Error executing R-APS(NR) while PENDING: %d", -ret);
	}

	return ret;
}

int erps_fsm_post_pending(struct erps_link *lnk, enum raps_request req,
	const struct raps_pdu *pdu)
{
	switch (req) {
	case RAPS_REQ_CLEAR:		/* Table 10-2, row 58 */
		return erps_fsm_clear_pending(lnk);
	case RAPS_REQ_FS:		/* Table 10-2, row 59 */
		return erps_fsm_fs_pending(lnk);
	case RAPS_REQ_RAPS_FS:		/* Table 10-2, row 60 */
		return erps_fsm_raps_fs_pending(lnk);
	case RAPS_REQ_SF:		/* Table 10-2, row 61 */
		return erps_fsm_sf_pending(lnk);
	case RAPS_REQ_CLEAR_SF:		/* Table 10-2, row 62 */
		/* No action */
		return 0;
	case RAPS_REQ_RAPS_SF:		/* Table 10-2, row 63 */
		return erps_fsm_raps_sf_pending(lnk);
	case RAPS_REQ_RAPS_MS:		/* Table 10-2, row 64 */
		return erps_fsm_raps_ms_pending(lnk);
	case RAPS_REQ_MS:		/* Table 10-2, row 65 */
		return erps_fsm_ms_pending(lnk);
	case RAPS_REQ_WTR_EXPIRES:	/* Table 10-2, row 66 */
		return erps_fsm_wtr_expires_pending(lnk);
	case RAPS_REQ_WTR_RUNNING:	/* Table 10-2, row 67 */
		/* No action */
		return 0;
	case RAPS_REQ_WTB_EXPIRES:	/* Table 10-2, row 68 */
		return erps_fsm_wtb_expires_pending(lnk);
	case RAPS_REQ_WTB_RUNNING:	/* Table 10-2, row 69 */
		/* No action */
		return 0;
	case RAPS_REQ_RAPS_NR_RB:	/* Table 10-2, row 70 */
		return erps_fsm_raps_nr_rb_pending(lnk);
	case RAPS_REQ_RAPS_NR:		/* Table 10-2, row 71 */
		return erps_fsm_raps_nr_pending(lnk, pdu);
	default:
		break;
	}

	NET_DBG("Invalid request 0x%x while in PENDING", (unsigned int)req);
	return -EINVAL;
}
