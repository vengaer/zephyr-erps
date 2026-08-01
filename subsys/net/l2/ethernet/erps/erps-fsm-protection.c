/*
 * Copyright (c) 2026 Vilhelm Engström
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>

#include <zephyr/logging/log.h>
#include <zephyr/net/net_log.h>

#include "erps-fsm.h"
#include "erps-internal.h"

LOG_MODULE_REGISTER(erps_fsm_protection, CONFIG_ERPS_LOG_LEVEL);

static inline int erps_fsm_fs_protection(struct erps_link *lnk)
{
	int ret = erps_fsm_fs_common(lnk);

	if (ret) {
		NET_ERR("Error executing FS while in PROTECTION: %d", -ret);
	}
	return ret;
}

static inline int erps_fsm_raps_fs_protection(struct erps_link *lnk)
{
	int ret = erps_fsm_raps_fs_common(lnk);

	if (ret) {
		NET_ERR("Error executing R-APS(FS) while in PROTECTION: %d",
									-ret);
	}
	return ret;
}

static inline int erps_fsm_sf_protection(struct erps_link *lnk)
{
	int ret = erps_fsm_sf_common(lnk);

	if (ret) {
		NET_ERR("Error executing SF while in PROTECTION: %d", -ret);
	}
	return ret;
}

static int erps_fsm_clear_sf_protection(struct erps_link *lnk)
{
	int ret;
	bool rpl_owner;
	struct erps_node *node = erps_link_get_node(lnk);

	erps_node_start_guard_timer(node);
	ret = erps_node_sched_tx(node, RAPS_NR, 0u, 0u);
	rpl_owner = erps_node_is_rpl_owner(node);
	if (!ret && rpl_owner && erps_node_is_revertive(node)) {
		ret = erps_node_start_wtr(node);
	}

	if (!ret) {
		erps_fsm_transition(node, ERPS_STATE_PENDING);
	}
	else {
		NET_ERR("Error clearing SF while in PROTECTION: %d", -ret);
	}
	return ret;
}

static int erps_fsm_raps_nr_protection(struct erps_link *lnk, bool rb)
{
	int ret;
	bool rpl_owner;
	struct erps_node *node = erps_link_get_node(lnk);

	ret = 0;
	rpl_owner = erps_node_is_rpl_owner(node);
	if (!rb && rpl_owner && erps_node_is_revertive(node)) {
		ret = erps_node_start_wtr(node);
	}
	if (!ret) {
		erps_fsm_transition(node, ERPS_STATE_PENDING);
	}
	else {
		NET_ERR("Error handling R-APS(NR%s) while in PROTECTION: %d",
			rb ? ",RB" : "", -ret);
	}

	return ret;
}

int erps_fsm_post_protection(struct erps_link *lnk, enum raps_request req)
{
	bool rb = false;

	switch (req) {
	case RAPS_REQ_CLEAR:		/* Table 10-2, row 16 */
		/* No action */
		return 0;
	case RAPS_REQ_FS:		/* Table 10-2, row 17 */
		return erps_fsm_fs_protection(lnk);
	case RAPS_REQ_RAPS_FS:		/* Table 10-2, row 18 */
		return erps_fsm_raps_fs_protection(lnk);
	case RAPS_REQ_SF:		/* Table 10-2, row 19 */
		return erps_fsm_sf_protection(lnk);
	case RAPS_REQ_CLEAR_SF:		/* Table 10-2, row 20 */
		return erps_fsm_clear_sf_protection(lnk);
	case RAPS_REQ_RAPS_SF:		/* Table 10-2, row 21 */
	case RAPS_REQ_RAPS_MS:		/* Table 10-2, row 22 */
	case RAPS_REQ_MS:		/* Table 10-2, row 23 */
	case RAPS_REQ_WTR_EXPIRES:	/* Table 10-2, row 24 */
	case RAPS_REQ_WTR_RUNNING:	/* Table 10-2, row 25 */
	case RAPS_REQ_WTB_EXPIRES:	/* Table 10-2, row 26 */
	case RAPS_REQ_WTB_RUNNING:	/* Table 10-2, row 27 */
		/* No action */
		return 0;
	case RAPS_REQ_RAPS_NR_RB:	/* Table 10-2, row 28 */
		rb = true;
		__fallthrough;
	case RAPS_REQ_RAPS_NR:		/* Table 10-2, row 29 */
		return erps_fsm_raps_nr_protection(lnk, rb);
	default:
		break;
	}

	NET_DBG("Invalid request 0x%x while in PROTECTION", (unsigned int)req);
	return -EINVAL;
}
