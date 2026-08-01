/*
 * Copyright (c) 2026 Vilhelm Engström
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
#include <zephyr/net/net_log.h>

#include "erps-fsm.h"
#include "erps-internal.h"

LOG_MODULE_REGISTER(erps_fsm_idle, CONFIG_ERPS_LOG_LEVEL);

static inline int erps_fsm_fs_idle(struct erps_link *lnk)
{
	int ret = erps_fsm_fs_common(lnk);

	if (ret) {
		NET_ERR("Error executing FS while IDLE: %d", -ret);
	}
	return ret;
}

static inline int erps_fsm_raps_fs_idle(struct erps_link *lnk)
{
	int ret = erps_fsm_raps_fs_common(lnk);

	if (ret) {
		NET_ERR("Error executing R-APS(FS) while IDLE: %d", -ret);
	}
	return ret;
}

static inline int erps_fsm_sf_idle(struct erps_link *lnk)
{
	int ret = erps_fsm_sf_common(lnk);

	if (ret) {
		NET_ERR("Error executing SF while IDLE: %d", -ret);
	}
	return ret;
}

static int erps_fsm_raps_sf_idle(struct erps_link *lnk)
{
	int ret = erps_fsm_raps_sf_common(lnk);

	if (ret) {
		NET_ERR("Error handling R-APS(SF) while IDLE: %d", -ret);
	}
	return ret;
}

static inline int erps_fsm_raps_ms_idle(struct erps_link *lnk)
{
	int ret = erps_fsm_raps_ms_common(lnk);

	if (ret) {
		NET_ERR("Error handling R-APS(MS) while IDLE: %d", -ret);
	}
	return ret;
}

static inline int erps_fsm_ms_idle(struct erps_link *lnk)
{
	int ret = erps_fsm_ms_common(lnk);

	if (ret) {
		NET_ERR("Error handling MS while IDLE: %d", -ret);
	}
	return ret;
}

static int erps_fsm_raps_nr_rb_idle(struct erps_link *lnk)
{
	int ret;
	struct erps_link *rpl, *non_rpl;
	struct erps_node *node = erps_link_get_node(lnk);

	ret = 0;
	rpl = erps_node_get_rpl(node);
	non_rpl = erps_node_other_link(node, rpl);
	if (non_rpl) {
		ret = erps_link_unblock(non_rpl);
	}
	if (!ret) {
		if (!erps_node_is_rpl_owner(node)) {
			erps_node_stop_tx(node);
		}
	}
	else {
		NET_ERR("Error handling R-APS(NR,RB) while IDLE: %d", -ret);
	}

	return ret;
}

static int erps_fsm_raps_nr_idle(struct erps_link *lnk,
					const struct raps_pdu *pdu)
{
	int ret;
	struct erps_node *node = erps_link_get_node(lnk);

	if (erps_node_is_rpl_owner(node) || erps_node_is_rpl_nbr(node)) {
		/* Nothing to do */
		return 0;
	}

	ret = erps_fsm_maybe_unblock_and_stop(lnk, pdu);
	if (ret) {
		NET_ERR("Error process R-APS(NR) while IDLE: %d", -ret);
	}

	return ret;
}

int erps_fsm_post_idle(struct erps_link *lnk, enum raps_request req,
	const struct raps_pdu *pdu)
{
	switch (req) {
	case RAPS_REQ_CLEAR:		/* Table 10-2, row 2 */
		/* No action */
		return 0;
	case RAPS_REQ_FS:		/* Table 10-2, row 3 */
		return erps_fsm_fs_idle(lnk);
	case RAPS_REQ_RAPS_FS:		/* Table 10-2, row 4 */
		return erps_fsm_raps_fs_idle(lnk);
	case RAPS_REQ_SF:		/* Table 10-2, row 5 */
		return erps_fsm_sf_idle(lnk);
	case RAPS_REQ_CLEAR_SF:		/* Table 10-2, row 6 */
		/* No action */
		return 0;
	case RAPS_REQ_RAPS_SF:		/* Table 10-2, row 7 */
		return erps_fsm_raps_sf_idle(lnk);
	case RAPS_REQ_RAPS_MS:		/* Table 10-2, row 8 */
		return erps_fsm_raps_ms_idle(lnk);
	case RAPS_REQ_MS:		/* Table 10-2, row 9 */
		return erps_fsm_ms_idle(lnk);
	case RAPS_REQ_WTR_EXPIRES:	/* Table 10-2, row 10 */
	case RAPS_REQ_WTR_RUNNING:	/* Table 10-2, row 11 */
	case RAPS_REQ_WTB_EXPIRES:	/* Table 10-2, row 12 */
	case RAPS_REQ_WTB_RUNNING:	/* Table 10-2, row 13 */
		/* No action */
		return 0;
	case RAPS_REQ_RAPS_NR_RB:	/* Table 10-2, row 14 */
		return erps_fsm_raps_nr_rb_idle(lnk);
	case RAPS_REQ_RAPS_NR:		/* Table 10-2, row 15 */
		return erps_fsm_raps_nr_idle(lnk, pdu);
	default:
		break;
	}

	NET_DBG("Invalid request 0x%x while IDLE", (unsigned int)req);
	return -EINVAL;
}
