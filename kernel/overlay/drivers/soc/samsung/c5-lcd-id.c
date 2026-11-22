/*
 * C5 bootloader LCD identity, split from Samsung display common code.
 * Copyright (C) 2015, Samsung Electronics. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * Derived from frozen ss_dsi_panel_common.c SHA-256
 * d2bb2dd3393ecc428ba22880630a50487b366c5e3d7e40b96c2bfa81296681a6.
 * This preserves actual lcd_id parsing; it is not a panel-present stub.
 * The future Samsung display port must reuse this single shared provider.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/string.h>

static int mdss_samsung_parse_panel_id(char *panel_id)
{
	char *pt;
	int lcd_id = 0;

	if (IS_ERR_OR_NULL(panel_id))
		return lcd_id;

	for (pt = panel_id; *pt != 0; pt++) {
		lcd_id <<= 4;
		switch (*pt) {
		case '0' ... '9':
			lcd_id += *pt - '0';
			break;
		case 'a' ... 'f':
			lcd_id += 10 + *pt - 'a';
			break;
		case 'A' ... 'F':
			lcd_id += 10 + *pt - 'A';
			break;
		}
	}
	pr_info("LCD_ID = 0x%X\n", lcd_id);
	return lcd_id;
}

int get_lcd_attached(char *mode)
{
	static int lcd_id;

	if (mode == NULL)
		return true;
	if (strncmp(mode, "GET", 3))
		lcd_id = mdss_samsung_parse_panel_id(mode);
	return lcd_id;
}
EXPORT_SYMBOL(get_lcd_attached);
__setup("lcd_id=0x", get_lcd_attached);
