/*
 * Copyright (c) 2026 Vilhelm Engström
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Ethernet ring protection switching
 */

#ifndef ZEPHYR_INCLUDE_NET_ERPS_H_
#define ZEPHYR_INCLUDE_NET_ERPS_H_

#include <stdint.h>

/**
 * @brief Ethernet ring protection switching
 * @defgroup erps Ethernet ring protection switching
 * @{
 */

struct net_if;

/** External events. Not to be confused with the R-APS events */
enum erps_event {
	/** Physical link/node failure */
	ERPS_SIGNAL_FAIL,

	/** Physical link/node restored */
	ERPS_CLEAR_SIGNAL_FAIL,

	/** Administrative clear, reset everything */
	ERPS_ADM_CLEAR,

	/** Force immediate block of port. Override potential ring failures */
	ERPS_ADM_FORCED_SWITCH,

	/** Block port if ring is otherwise intace */
	ERPS_ADM_MANUAL_SWITCH,
};

/**
 * @brief Execute ERPS event on @p iface
 *
 * @param iface The interface on which the event is to be issued
 * @param ev    The event to issue
 *
 * @retval 0       Event successfully processed
 * @retval -EINVAL @p ev is invalid
 * @retval -errno  Error code indicating what went wrong
 */
int net_erps_ctl(struct net_if *iface, enum erps_event ev);


/**
 * @brief Look up interface by ring ID and port index
 *
 * @param ring_id Ring identifier
 * @param port    Port index
 *
 * @retval >0   Address of the interface corresponding to the ring port
 * @retval NULL No ring matches @p ring_id, or @p port is invalid
 */
struct net_if *net_erps_lookup_iface(uint8_t ring_id, uint8_t port);

/** @} */

#endif  /* ZEPHYR_INCLUDE_NET_ERPS_H_ */
