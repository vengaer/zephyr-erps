/*
 * Copyright (c) 2026 Vilhelm Engström
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @ingroup raps
 * @brief R-APS protocol implementation
 */

#include <zephyr/net/erps/r-aps.h>

bool raps_pdu_bpr(const struct raps_pdu *pdu);
bool raps_pdu_dnf(const struct raps_pdu *pdu);
bool raps_pdu_rb(const struct raps_pdu *pdu);

enum raps_req_state raps_info_get_req_state(const struct raps_spc_info *info);
enum raps_subcode raps_info_get_subcode(const struct raps_spc_info *info);

enum raps_req_state raps_pdu_get_req_state(const struct raps_pdu *pdu);
enum raps_subcode raps_pdu_get_subcode(const struct raps_pdu *pdu);

uint_fast8_t raps_pdu_status(const struct raps_pdu *pdu);

char const *raps_req_state_str(enum raps_req_state rs)
{
	switch (rs) {
	case RAPS_NR:
		return "NR";
	case RAPS_MS:
		return "MS";
	case RAPS_SF:
		return "SF";
	case RAPS_FS:
		return "FS";
	case RAPS_EVENT:
		return "EV";
	default:
		break;
	};

	return "<invalid>";
}
