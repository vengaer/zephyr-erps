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

/** Ring information */
struct erps_ring_info {
	/** Ring identifier */
	uint8_t ring_id;

	/** Control VLAN identifier */
	uint16_t ctrl_vid;

	/** Traffic VLAN identifier */
	uint16_t traffic_vid;
};

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
#if defined(CONFIG_NET_ERPS_SHELL) || __DOXYGEN__
struct net_if *net_erps_lookup_iface(uint8_t ring_id, uint8_t port);
#else
static inline net_if *net_erps_lookup_iface(uint8_t ring_id, uint8_t port)
{
	ARG_UNUSED(ring_id);
	ARG_UNUSED(port);

	return NULL;
}
#endif

/**
 * @brief Look up information about the ring containing @p iface
 *
 * @param iface Network interface
 * @param info  Ring information struct to fill in
 *
 * @retval 0       @p info populated
 * @retval -EINVAL @p iface is not a ring link
 * @retval -ENODEV Could not get device associated with @p iface
 */
int net_erps_ring_info_by_iface(struct net_if *iface, struct erps_ring_info *info);


/** @} */

#endif  /* ZEPHYR_INCLUDE_NET_ERPS_H_ */
