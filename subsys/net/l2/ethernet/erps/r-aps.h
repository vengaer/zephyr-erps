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

#ifndef ZEPHYR_SUBSYS_NET_L2_ETHERNET_ERPS_R_APS_H_
#define ZEPHYR_SUBSYS_NET_L2_ETHERNET_ERPS_R_APS_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/dt-bindings/ethernet/erps.h>
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

/** R-APS fixed PDU constants as per ITU-T G.8032/Y.1344 */
enum {
	/** R-APS OAM PDU opcode */
	RAPS_OPCODE		= 0x28,

	/** PDU flags */
	RAPS_FLAGS		= 0x00,
};


/** Various constants */
enum {
	/** Request/state shift */
	RAPS_RS_SHIFT		= 4,

	/** BPR shift in flags */
	RAPS_BPR_SHIFT		= 5,

	/** R-APS subcode mask */
	RAPS_SC_MASK		= 0x0f,

	/** MEL shift in first byte of CFM header */
	RAPS_MEL_SHIFT		= 5,

	/** Maximum MEL value */
	RAPS_MEL_MAX		= 0x07,
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
	uint8_t rs_sc;

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
	struct raps_cfm_pdu_hdr cfm_hdr;

	/** R-APS specific information */
	struct raps_spc_info raps_info;

	/* No R-APS specific TLVs */

	/** End of optional PDU TLV */
	uint8_t end_tlv;
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
	RAPS_SC_FLUSH_REQ	= 0,
};


/** R-APS status bits */
enum {

	/** RPL blocked */
	RAPS_RB			= BIT(7),

	/** Do not flush */
	RAPS_DNF		= BIT(6),

	/** Blocked port reference */
	RAPS_BPR		= BIT(5),
};


/** Request/state */
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
	RAPS_EVENT		= 0x0e,
};



/**
 * @brief Request/state and status
 *
 * Lower value has higher priority.
 *
 * See Table 10-1 in the specification
 */
enum raps_request {

	/** Clear */
	RAPS_REQ_CLEAR,

	/** Forced switch */
	RAPS_REQ_FS,

	/** R-APS (forced switch) */
	RAPS_REQ_RAPS_FS,

	/** Local signal fail */
	RAPS_REQ_SF,

	/** Local clear signal fail */
	RAPS_REQ_CLEAR_SF,

	/** R-APS (signal failure) */
	RAPS_REQ_RAPS_SF,

	/** R-APS (manual switch) */
	RAPS_REQ_RAPS_MS,

	/** Manual switch */
	RAPS_REQ_MS,

	/** Wait to restore expires */
	RAPS_REQ_WTR_EXPIRES,

	/** Wait to restore running */
	RAPS_REQ_WTR_RUNNING,

	/** Wait to block expires */
	RAPS_REQ_WTB_EXPIRES,

	/** Wait to block running */
	RAPS_REQ_WTB_RUNNING,

	/** R-APS (no request, RPL blocked) */
	RAPS_REQ_RAPS_NR_RB,

	/** R-APS (no request) */
	RAPS_REQ_RAPS_NR,

	/** Invalid request */
	RAPS_REQ_INVALID,
};

/**
 * @brief Determine whether or not the @c BPR flag is set in @p pdu.
 *
 * @param pdu An R-APS PDU.
 *
 * @retval true  The @c BPR flag is set in @p pdu.
 * @retval false The @c BPR flag it not set in @p pdu.
 */
inline bool raps_pdu_bpr(const struct raps_pdu *pdu)
{
	return pdu->raps_info.status & RAPS_BPR;
}


/**
 * @brief Determine whether or not the @c DNF flag is set in @p pdu.
 *
 * @param pdu An R-APS PDU.
 *
 * @retval true  The @c DNF flag is set in @p pdu.
 * @retval false The @c DNF flag it not set in @p pdu.
 */
inline bool raps_pdu_dnf(const struct raps_pdu *pdu)
{
	return pdu->raps_info.status & RAPS_DNF;
}


/**
 * @brief Determine whether or not the @c RB flag is set in @p pdu.
 *
 * @param pdu An R-APS PDU.
 *
 * @retval true  The @c RB flag is set in @p pdu.
 * @retval false The @c RB flag it not set in @p pdu.
 */
inline bool raps_pdu_rb(const struct raps_pdu *pdu)
{
	return pdu->raps_info.status & RAPS_RB;
}


/**
 * @brief Extract request/state from R-APS part of PDU.
 *
 * @param info The R-APS part of a PDU.
 *
 * @return The request/state bits from the PDU part.
 */
inline enum raps_req_state raps_info_get_req_state(
				const struct raps_spc_info *info)
{
	return info->rs_sc >> RAPS_RS_SHIFT;
}

/**
 * @brief Extract subcode from R-APS part of PDU.
 *
 * @param info THe R-APS part of the PDU.
 *
 * @return The subocde bits from the PDU part.
 */
inline enum raps_subcode raps_info_get_subcode(const struct raps_spc_info *info)
{
	return info->rs_sc & RAPS_SC_MASK;
}


/**
 * @brief Extract request/state from PDU.
 *
 * @param pdu The R-APS PDU.
 *
 * @return The request/state bits from the R-APS part of the PDU.
 */
inline enum raps_req_state raps_pdu_get_req_state(const struct raps_pdu *pdu)
{
	return raps_info_get_req_state(&pdu->raps_info);
}


/**
 * @brief Extract subcode from PDU.
 *
 * @param pdu The R-APS PDU.
 *
 * @return The subcode bits from the R-APS part of the PDU.
 */
inline enum raps_subcode raps_pdu_get_subcode(const struct raps_pdu *pdu)
{
	return raps_info_get_subcode(&pdu->raps_info);
}


/**
 * @brief Get request/state as ASCII string.
 *
 * @param rs The request/state.
 *
 * @return @p rs as an ASCII string, or @c "<invalid>" if @p rs is invalid.
 */
char const *raps_req_state_str(enum raps_req_state rs);


/**
 * @brief Get the status bits from @p pdu.
 *
 * @param pdu The R-APS PDU.
 *
 * @return The status bits from the R-APS-specific part of @p pdu.
 */
inline uint_fast8_t raps_pdu_status(const struct raps_pdu *pdu)
{
	return pdu->raps_info.status;
}

/**
 * @brief Get R-APS ring id
 *
 * @param pdu The PDU whose ring ID is to be extracted
 *
 * @return Ring ID encoded in the PDU's node identifier
 */
inline uint_fast8_t raps_pdu_get_ring_id(const struct raps_pdu *pdu)
{
	const struct net_eth_addr *node_id = &pdu->raps_info.node_id;
	return node_id->addr[sizeof(node_id) - 1u];
}


/** @} */

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_SUBSYS_NET_L2_ETHERNET_ERPS_R_APS_H_ */
