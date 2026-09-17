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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Ethernet ring protection switching
 * @defgroup erps Ethernet ring protection switching
 * @{
 */

struct net_if;
struct net_eth_addr;

/** Ring information */
struct erps_ring_info {
	/** Ring identifier */
	uint8_t ring_id;

	/** Whether or not reversion is in progress */
	bool reverting;

	/** Whether or not each port is blocked */
	bool blocked[2u];

	/** Index of the requesting interface */
	uint8_t requester_index;

	/**
	  * Index of the RPL interface in the @c iface_port, -1 if the node is
	  * neither the RPL owner nor the RPL neighbor
	  */
	int8_t rpl_index;

	/** Control VLAN identifier */
	uint16_t ctrl_vid;

	/** Traffic VLAN identifier */
	uint16_t traffic_vid;

	/** Port interfaces */
	struct net_if *iface_port[2u];
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
struct net_if *net_erps_lookup_iface(uint8_t ring_id, uint8_t port);

/**
 * @brief Look up information about the ring containing @p iface
 *
 * @param iface Network interface
 * @param info  Ring information struct to fill in
 *
 * @retval 0       @p info populated
 * @retval -ENODEV @p iface could not be identified as a ring link
 */
int net_erps_ring_info_by_iface(struct net_if *iface, struct erps_ring_info *info);

/**
 * @brief Write ERPS multicast address to @p mac
 *
 * @param ring_id Ring identifier obtained via net_erps_ring_info_by_iface()
 * @param mac     The address to store the multicast address in
 */
void net_erps_ring_mcast_addr(unsigned int ring_id, struct net_eth_addr *mac);

/**
 * @brief Get the control VLAN identifier of the ring containing @p iface
 *
 * @param iface Network interface
 * @param vid   VID to fill in
 *
 * @return See net_erps_ring_info_by_iface().
 */
static inline int net_erps_ring_get_ctrl_vid_by_iface(struct net_if *iface, uint16_t *vid)
{
	int ret;
	struct erps_ring_info info;

	ret = net_erps_ring_info_by_iface(iface, &info);
	if (ret) {
		return ret;
	}

	*vid = info.ctrl_vid;
	return 0;
}

/**
 * @brief Get the traffic VLAN identifier of the ring containing @p iface
 *
 * @param iface Network interface
 * @param vid   VID to fill in
 *
 * @return See net_erps_ring_info_by_iface().
 */
static inline int net_erps_ring_get_traffic_vid_by_iface(struct net_if *iface, uint16_t *vid)
{
	int ret;
	struct erps_ring_info info;

	ret = net_erps_ring_info_by_iface(iface, &info);
	if (ret) {
		return ret;
	}

	*vid = info.traffic_vid;
	return 0;
}

/**
 * @brief Get the other network interface in the ring containing @p iface.
 *
 * @param iface Network interface
 *
 * @retval >0   Address of the other ring interface
 * @retval NULL @p iface is either not in a ring, or the lookup failed for some other reason
 */
static inline struct net_if *net_erps_ring_get_other_link(struct net_if *iface)
{
	int ret;
	struct erps_ring_info info;

	ret = net_erps_ring_info_by_iface(iface, &info);
	if (ret) {
		return NULL;
	}

	return info.iface_port[info.iface_port[0u] == iface];
}

/**
 * @brief Mark the link corresponding to @p iface  as RPL
 *
 * @note Must be called before ERPS initialization.
 *
 * @param iface    Network interface
 * @param is_owner Set if the node corresponding to @p iface is to be the RPL owner
 *
 * @retval 0       Link marked as RPL
 * @retval -ENODEV @p iface could not be identified as a ring link
 * @retval -EPERM  Configuration not allowed at this state
 * @retval -EBUSY  The other link connected to the corresponding node is marked as RPL
 */
int net_erps_set_as_rpl(struct net_if *iface, bool is_owner);

/**
 * @brief Mark the link as non-RPL
 *
 * @note Must be called before ERPS initialization.
 *
 * @details If the link was set as RPL, the RPL neighbor or RPL owner
 * state is cleared from the corresponding node
 *
 * @param iface Network interface
 *
 * @retval 0       Link marked as RPL
 * @retval -EPERM  Configuration not allowed at this state
 * @retval -ENODEV @p iface could not be identified as a ring link
 */
int net_erps_unset_rpl(struct net_if *iface);

/**
 * @brief Set ID of the ring associated with @p iface
 *
 * @param iface   Network interface
 * @param ring_id The identifier to set
 *
 * @retval 0       Ring ID set
 * @retval -ENODEV Ring node could not be identified
 * @retval -EINVAL @p ring_id is invalid
 */
int net_erps_set_ring_id(struct net_if *iface, uint8_t ring_id);


/** @} */

#endif  /* ZEPHYR_INCLUDE_NET_ERPS_H_ */
