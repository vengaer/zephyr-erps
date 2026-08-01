# Copyright (c) 2026 Vilhelm Engström
#
# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources(erps-fsm-common.c)
zephyr_library_sources(erps-fsm-forced-switch.c)
zephyr_library_sources(erps-fsm-idle.c)
zephyr_library_sources(erps-fsm-manual-switch.c)
zephyr_library_sources(erps-fsm-pending.c)
zephyr_library_sources(erps-fsm-protection.c)
zephyr_library_sources(erps.c)
zephyr_library_sources(r-aps.c)

zephyr_iterable_section(NAME erps_node GROUP DATA_REGION ${XIP_ALIGN_WITH_INPUT})
zephyr_linker_sources(DATA_SECTIONS erps.ld)
