/*
 * Copyright (c) 2026 Vilhelm Engström
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
#include <zephyr/net/net_log.h>

#include "erps-fsm.h"
#include "erps-internal.h"

LOG_MODULE_REGISTER(erps_fsm_fs, CONFIG_ERPS_LOG_LEVEL);

static inline int erps_fsm_clear_fs(struct erps_link *lnk)
{
	int ret = erps_fsm_clear_common(lnk);

	if (ret) {
		NET_ERR("Error executing CLEAR while in FORCED SWITCH. %d",
				-ret);
	}
	return ret;
}

static int erps_fsm_fs_fs(struct erps_link *lnk)
{
	int ret;
	struct erps_node *node = erps_link_get_node(lnk);

	ret = erps_link_block(lnk);
	if (!ret) {
		ret = erps_node_sched_tx(node, RAPS_FS, 0u, 0u);
	}
	if (!ret) {
		ret = erps_flush_fdb(lnk);
	}

	if (ret) {
		NET_ERR("Error executing FS while in FORCED SWITCH: %d", -ret);
	}
	return ret;
}

static int erps_fsm_raps_nr_fs(struct erps_link *lnk, bool rb)
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
			"FORCED SWITCH: %d", rb ? ",RB" : "", -ret);
	}

	return ret;
}

int erps_fsm_post_forced_switch(struct erps_link *lnk, enum erps_request req)
{
	bool rb = false;

	switch (req) {
	case ERPS_REQ_CLEAR:		/* Table 10-2, row 44 */
		return erps_fsm_clear_fs(lnk);
	case ERPS_REQ_FS:		/* Table 10-2, row 45 */
		return erps_fsm_fs_fs(lnk);
	case ERPS_REQ_RAPS_FS:		/* Table 10-2, row 46 */
	case ERPS_REQ_SF:		/* Table 10-2, row 47 */
	case ERPS_REQ_CLEAR_SF:		/* Table 10-2, row 48 */
	case ERPS_REQ_RAPS_SF:		/* Table 10-2, row 49 */
	case ERPS_REQ_RAPS_MS:		/* Table 10-2, row 50 */
	case ERPS_REQ_MS:		/* Table 10-2, row 51 */
	case ERPS_REQ_WTR_EXPIRES:	/* Table 10-2, row 52 */
	case ERPS_REQ_WTR_RUNNING:	/* Table 10-2, row 53 */
	case ERPS_REQ_WTB_EXPIRES:	/* Table 10-2, row 54 */
	case ERPS_REQ_WTB_RUNNING:	/* Table 10-2, row 55 */
		/* No action */
		return 0;
	case ERPS_REQ_RAPS_NR_RB:	/* Table 10-2, row 56 */
		rb = true;
		__fallthrough;
	case ERPS_REQ_RAPS_NR:		/* Table 10-2, row 57 */
		return erps_fsm_raps_nr_fs(lnk, rb);
	default:
		break;
	}

	NET_DBG("Invalid request 0x%x while in FORCED SWITCH",
						(unsigned int)req);
	return -EINVAL;
}
