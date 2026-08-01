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

LOG_MODULE_REGISTER(erps_fsm_ms, CONFIG_ERPS_LOG_LEVEL);

static inline int erps_fsm_clear_ms(struct erps_link *lnk)
{
	int ret = erps_fsm_clear_common(lnk);

	if (ret) {
		NET_ERR("Error executing CLEAR while in MANUAL SWITCH: %d",
									-ret);
	}
	return ret;
}

static inline int erps_fsm_fs_ms(struct erps_link *lnk)
{
	int ret = erps_fsm_fs_common(lnk);

	if (ret) {
		NET_ERR("Error executing FS while in MANUAL SWITCH: %d", -ret);
	}
	return ret;
}

static inline int erps_fsm_raps_fs_ms(struct erps_link *lnk)
{
	int ret = erps_fsm_raps_fs_common(lnk);

	if (ret) {
		NET_ERR("Error executing R-APS(FS) while in MANUAL SWITCH: %d",
			 -ret);
	}
	return ret;
}

static inline int erps_fsm_sf_ms(struct erps_link *lnk)
{
	int ret = erps_fsm_sf_common(lnk);

	if (ret) {
		NET_ERR("Error executing local SF while in MANUAL SWITCH: %d",
				-ret);
	}
	return ret;
}

static int erps_fsm_raps_sf_ms(struct erps_link *lnk)
{
	int ret = erps_fsm_raps_sf_common(lnk);

	if (ret) {
		NET_ERR("Error executing R-APS(FS) while in MANUAL SWITCH: %d",
					-ret);
	}
	return ret;
}

static int erps_fsm_raps_ms_ms(struct erps_link *lnk)
{
	int ret;
	bool rpl_owner;
	enum erps_node_state next;
	struct erps_node *node = erps_link_get_node(lnk);

	ret = 0;
	next = ERPS_STATE_MANUAL_SWITCH;
	if (erps_node_any_link_blocked(node)) {
		erps_node_start_guard_timer(node);

		ret = erps_node_sched_tx(node, RAPS_NR, 0u, 0u);
		rpl_owner = erps_node_is_rpl_owner(node);

		if (!ret && rpl_owner && erps_node_is_revertive(node)) {
			ret = erps_node_start_wtb(node);
			next = ERPS_STATE_PENDING;
		}
	}

	if (!ret) {
		erps_fsm_transition(node, next);
	}
	else {
		NET_ERR("Error executing R-APS(MS) while in MANUAL SWITCH: %d",
				-ret);
	}

	return ret;
}

static int erps_fsm_raps_nr_ms(struct erps_link *lnk, bool rb)
{
	int ret;
	bool rpl_owner;
	struct erps_node *node = erps_link_get_node(lnk);

	ret = 0;
	rpl_owner = erps_node_is_rpl_owner(node);
	if (!rb && rpl_owner && erps_node_is_revertive(node)) {
		ret = erps_node_start_wtb(node);
	}
	if (!ret) {
		erps_fsm_transition(node, ERPS_STATE_PENDING);
	}
	else {
		NET_ERR("Error executing R-APS(NR%s) while in "
			"MANUAL SWITCH: %d", rb ? ",RB": "", -ret);
	}

	return ret;
}

int erps_fsm_post_manual_switch(struct erps_link *lnk, enum raps_request req)
{
	bool rb = false;

	switch (req) {
	case RAPS_REQ_CLEAR:		/* Table 10-2, row 30 */
		return erps_fsm_clear_ms(lnk);
	case RAPS_REQ_FS:		/* Table 10-2, row 31 */
		return erps_fsm_fs_ms(lnk);
	case RAPS_REQ_RAPS_FS:		/* Table 10-2, row 32 */
		return erps_fsm_raps_fs_ms(lnk);
	case RAPS_REQ_SF:		/* Table 10-2, row 33 */
		return erps_fsm_sf_ms(lnk);
	case RAPS_REQ_CLEAR_SF:		/* Table 10-2, row 34 */
		/* No action */
		return 0;
	case RAPS_REQ_RAPS_SF:		/* Table 10-2, row 35 */
		return erps_fsm_raps_sf_ms(lnk);
	case RAPS_REQ_RAPS_MS:		/* Table 10-2, row 36 */
		return erps_fsm_raps_ms_ms(lnk);
	case RAPS_REQ_MS:		/* Table 10-2, row 37 */
	case RAPS_REQ_WTR_EXPIRES:	/* Table 10-2, row 38 */
	case RAPS_REQ_WTR_RUNNING:	/* Table 10-2, row 39 */
	case RAPS_REQ_WTB_EXPIRES:	/* Table 10-2, row 40 */
	case RAPS_REQ_WTB_RUNNING:	/* Table 10-2, row 41 */
		/* No action */
		return 0;
	case RAPS_REQ_RAPS_NR_RB:	/* Table 10-2, row 42 */
		rb = true;
		__fallthrough;
	case RAPS_REQ_RAPS_NR:		/* Table 10-2, row 43 */
		return erps_fsm_raps_nr_ms(lnk, rb);
	default:
		break;
	}

	NET_DBG("Invalid request 0x%x while in MANUAL SWITCH",
						(unsigned int)req);
	return -EINVAL;
}
