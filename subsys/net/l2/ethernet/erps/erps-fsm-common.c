/*
 * Copyright (c) 2026 Vilhelm Engström
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include <zephyr/logging/log.h>
#include <zephyr/net/ethernet.h>
#include <zephyr/net/net_log.h>

#include "erps-fsm.h"
#include "erps-internal.h"

LOG_MODULE_REGISTER(erps_fsm_common, CONFIG_ERPS_LOG_LEVEL);

int erps_fsm_fs_common(struct erps_link *lnk);
int erps_fsm_sf_common(struct erps_link *lnk);
int erps_fsm_ms_common(struct erps_link *lnk);
int erps_fsm_raps_fs_common(struct erps_link *lnk);
int erps_fsm_raps_sf_common(struct erps_link *lnk);
int erps_fsm_raps_ms_common(struct erps_link *lnk);

int erps_fsm_fs_sf_ms(struct erps_link *lnk, enum raps_req_state req_state,
		enum erps_node_state next_state)
{
	int ret;
	uint8_t status;
	struct erps_node *node = erps_link_get_node(lnk);
	struct erps_link *oth_lnk = erps_node_other_link(node, lnk);

	ret = 0;
	status = RAPS_DNF;
	if (!erps_link_is_blocked(lnk)) {
		ret = erps_link_block(lnk);
		status = 0u;
	}
	if (!ret) {
		ret = erps_node_sched_tx(node, req_state, 0u, status);
	}
	if (!ret) {
		ret = erps_link_unblock(oth_lnk);
	}
	if (!ret && !status) {
		ret = erps_flush_fdb(lnk);
	}
	if (!ret) {
		erps_fsm_transition(node, next_state);
	}

	return ret;
}

int erps_fsm_unblock_and_stop(struct erps_link *lnk, bool only_non_failed,
					enum erps_node_state next_state)
{
	int ret;
	struct erps_node *node = erps_link_get_node(lnk);

	if (only_non_failed) {
		ret = erps_node_unblock_non_failed(node);
	}
	else {
		ret = erps_node_unblock_all(node);
	}

	if (!ret) {
		erps_node_stop_tx(node);
		erps_fsm_transition(node, next_state);
	}

	return ret;
}

int erps_fsm_maybe_unblock_and_stop(struct erps_link *lnk,
					const struct raps_pdu *pdu)
{
	int ret;
	struct net_eth_addr mac;
	struct erps_node *node = erps_link_get_node(lnk);

	__ASSERT_NO_MSG(pdu);

	ret = erps_link_get_node_id(lnk, &mac);
	if (ret) {
		NET_ERR("Could not get MAC: %d", -ret);
		return ret;
	}

	if (memcmp(&pdu->raps_info.node_id, &mac, sizeof(mac)) <= 0) {
		/* Nothing to do */
		return 0;
	}

	ret = erps_node_unblock_non_failed(node);
	if (!ret) {
		erps_node_stop_tx(node);
	}

	return ret;
}

int erps_fsm_clear_common(struct erps_link *lnk)
{
	int ret;
	bool rpl_owner;
	struct erps_node *node = erps_link_get_node(lnk);

	ret = 0;
	rpl_owner = erps_node_is_rpl_owner(node);

	if (erps_node_any_link_blocked(node)) {
		erps_node_start_guard_timer(node);
		ret = erps_node_sched_tx(node, RAPS_NR, 0u, 0u);
		if (!ret && rpl_owner && erps_node_is_revertive(node)) {
			ret = erps_node_start_wtb(node);
		}
	}

	if (!ret) {
		erps_fsm_transition(node, ERPS_STATE_PENDING);
	}

	return ret;
}
