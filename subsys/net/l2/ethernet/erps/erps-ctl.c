/*
 * Copyright (c) 2026 Vilhelm Engström
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/net/erps.h>

#include "erps-fsm.h"
#include "erps-internal.h"
#include "r-aps.h"

int net_erps_ctl(struct net_if *iface, enum erps_event ev)
{
	enum erps_request req;

	switch (ev) {
	case ERPS_SIGNAL_FAIL:
		req = ERPS_REQ_SF;
		break;
	case ERPS_CLEAR_SIGNAL_FAIL:
		req = ERPS_REQ_CLEAR_SF;
		break;
	case ERPS_ADM_CLEAR:
		req = ERPS_REQ_CLEAR;
		break;
	case ERPS_ADM_FORCED_SWITCH:
		req = ERPS_REQ_FS;
		break;
	case ERPS_ADM_MANUAL_SWITCH:
		req = ERPS_REQ_MS;
		break;
	default:
		return -EINVAL;
	}

	return net_erps_fsm_post(iface, req);
}
