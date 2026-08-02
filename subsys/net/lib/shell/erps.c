/*
 * Copyright (c) 2026 Vilhelm Engström
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>

#include <zephyr/logging/log.h>
#include <zephyr/net/erps.h>
#include <zephyr/shell/shell.h>

LOG_MODULE_DECLARE(erps_sh);


static int erps_sh_parse_u8(const char *str)
{
	char *endp;
	unsigned long ul;

	if (!str) {
		return -EINVAL;
	}

	errno = 0;
	ul = strtoul(str, &endp, 10);
	if (*endp || errno == ERANGE || ul > UINT8_MAX) {
		return errno ? -errno : -EINVAL;
	}

	return (uint8_t)ul;
}

static int erps_sh_ctl(enum erps_event ev, size_t argc, char *argv[])
{
	int ring_id, port;
	struct net_if *iface;

	/* Need ring ID and port index */
	if (argc != 3u) {
		return -EINVAL;
	}

	ring_id = erps_sh_parse_u8(argv[1u]);
	if (ring_id < 0) {
		return ring_id;
	}

	port = erps_sh_parse_u8(argv[2u]);
	if (port < 0) {
		return port;
	}

	iface = net_erps_lookup_iface((uint8_t)ring_id, (uint8_t)port);
	if (!iface) {
		return -ENODEV;
	}

	return net_erps_ctl(iface, ev);
}


static int erps_sh_clear(const struct shell *sh, size_t argc, char *argv[])
{
	return erps_sh_ctl(ERPS_ADM_CLEAR, argc, argv);
}

static int erps_sh_manual_switch(const struct shell *sh, size_t argc, char *argv[])
{
	return erps_sh_ctl(ERPS_ADM_MANUAL_SWITCH, argc, argv);
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	net_cmd_erps,

	SHELL_CMD(
		clear, NULL,
		SHELL_HELP("Issue administrative clear", "<ring_id> <port_idx>"),
		erps_sh_clear
	),

	SHELL_CMD(
		manual_switch, NULL,
		SHELL_HELP("Engage manual switch on port", "<ring_id> <port_idx>"),
		erps_sh_manual_switch
	),

	SHELL_SUBCMD_SET_END
);


SHELL_SUBCMD_ADD((net), erps, &net_cmd_erps, "Manage ERPS rings.", NULL, 1, 1);
