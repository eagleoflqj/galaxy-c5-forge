/*
 * C5 bootloader charging-mode identity, shared by panel/battery consumers.
 * Copyright (C) 2012 Samsung Electronics
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2.
 *
 * Derived from the accepted CONFIG_BATTERY_SAMSUNG_V2 boot-mode parser in
 * drivers/battery/sec_battery_v2.c, frozen SHA-256
 * 141ae59cfdc5a4e2f063ba724baa69da0206a6b969316633bb6f5f38568b03d5.
 * Preserve its prefix comparison, latched flag and __setup return value.
 * Future battery integration must reuse this single provider, not duplicate it.
 * A parsed boot mode is not proof of charging hardware or charger UI operation.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/string.h>

int poweroff_charging;

static int sec_bat_is_lpm_check(char *str)
{
	if (strncmp(str, "charger", 7) == 0)
		poweroff_charging = 1;

	pr_info("%s: Low power charging mode: %d\n", __func__, poweroff_charging);

	return poweroff_charging;
}
__setup("androidboot.mode=", sec_bat_is_lpm_check);
