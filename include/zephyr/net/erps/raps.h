/*
 * Copyright (c) 2026 Vilhelm Engström
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @ingroup raps
 * @brief R-APS protocol header
 */

#ifndef ZEPHYR_INCLUDE_NET_ERPS_RAPS_H_
#define ZEPHYR_INCLUDE_NET_ERPS_RAPS_H_

#include <stdint.h>

#include <zephyr/net/ethernet.h>
#include <zephyr/sys/util_macro.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief R-APS protocol.
 * @defgroup raps R-APS protocol
 * @ingroup erps
 * @{
 */

/** RAPS fixed PDU constants as per ITU-T G.8032/Y.1344 */
enum {

	/** RAPS version */
	RAPS_VERSION		= 1,

	/** RAPS OAM PDU opcode */
	RAPS_OPCODE		= 0x28,

	/** PDU flagsh */
	RAPS_FLAGS		= 0x00,
};


/** CFM PDU common header */
struct raps_cfm_pdu_hdr {
	/**
	 * Bits 7:5 MEL
	 * Bits 4:0 Version
	 */
	uint8_t mel_ver;

	/** Ethernet OAM opcode */
	uint8_t opcode;

	/** PDU flags */
	uint8_t flags;

	/** TLV offset */
	uint8_t tlv_off;
};


/** R-APS specific information */
struct raps_spc_info {
	/**
	 * Bits 7:4 Request/state
	 * Bits 3:0 Sub-code
	 */
	uint8_t rc_sc;

	/**
	 * Bit 7    RPL Blocked
	 * Bit 6    Do not flush
	 * Bit 5    Blocked port reference
	 * Bits 4:0 RFU
	 */
	uint8_t status;

	/** Node Identifier (MAC address) */
	struct net_eth_addr node_id;

	/** Reserved bytes */
	uint8_t rfu[24u];
};


/** R-APS protocol data unit */
struct raps_pdu {

	/** CFG PDU common header */
	struct raps_cfm_pdu_hdr chdr;

	/** R-APS specific information */
	struct raps_spc_info spc_info;

	/* No R-APS specific TLVs */

	/** End of optional PDU TLV */
	uint8_t end_tlv;
};


/**
 * @brief R-APS request/state values
 *
 * Remaining values reserved.
 */
enum raps_req_state {

	/** No request */
	RAPS_NR			= 0x00,

	/** Manual switch */
	RAPS_MS			= 0x07,

	/** Signal fail */
	RAPS_SF			= 0x0b,

	/** Forced switch */
	RAPS_FS			= 0x0d,

	/** Event */
	RAPS_EV			= 0x0e,
};


/**
 * @brief R-APS subcodes.
 *
 * Remaining values reserved
 */
enum raps_subcode {

	/**
	 * @brief Flush request
	 *
	 * Valid only for @c RAPS_EV
	 */
	RAPS_SC_FLUSH_REQ
};


/** R-APS status bits */
enum {

	/** RPL blocked */
	RAPS_RB			= BIT(7),

	/** Do not flush */
	RAPS_DNF		= BIT(6),

	/** Blocked port reference */
	RAPS_BRP		= BIT(5),
};


/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_NET_ERPS_RAPS_H_ */
