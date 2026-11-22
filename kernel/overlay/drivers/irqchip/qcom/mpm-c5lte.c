/* SPDX-License-Identifier: GPL-2.0-only */
/* C5-only wake mappings, not a generic MSM8952 table.
 * Derived from frozen CSA1 C5 DTB msm8952-sec-c5lte-chn-hk-r09.dtb,
 * SHA-256: 83e02661e33cc35a25c6fee66abb0e2f492e2dc854b79a396a8eb90817869713.
 * Legacy 0xff bypass entries are NOT pins; preserve their separate class.
 * Classification alone is not a connected XO-clock voting policy.
 * Table presence/compilation does not establish suspend/wakeup functionality.
 */
#include <linux/kernel.h>
#include "mpm.h"

const struct mpm_pin mpm_c5lte_gic_chip_data[] = {
	{2, 216},
	{49, 172},
	{53, 104},
	{58, 166},
	{62, 222},
	{-1},
};

const struct mpm_pin mpm_c5lte_gpio_chip_data[] = {
	{3, 38},
	{4, 1},
	{5, 5},
	{6, 9},
	{8, 37},
	{9, 36},
	{10, 13},
	{11, 35},
	{12, 17},
	{13, 21},
	{14, 54},
	{15, 34},
	{17, 58},
	{18, 28},
	{19, 42},
	{20, 25},
	{21, 12},
	{22, 43},
	{23, 44},
	{24, 45},
	{25, 46},
	{26, 48},
	{27, 65},
	{28, 93},
	{29, 97},
	{30, 63},
	{31, 70},
	{32, 71},
	{33, 72},
	{34, 81},
	{35, 85},
	{36, 90},
	{50, 67},
	{51, 73},
	{52, 74},
	{53, 62},
	{-1},
};

const irq_hw_number_t mpm_c5lte_gic_bypass[] = {
	18, 19, 20, 35, 39, 40, 47, 56,
	57, 58, 59, 60, 61, 65, 73, 74,
	75, 76, 78, 79, 85, 86, 90, 92,
	93, 97, 102, 108, 109, 112, 114, 128,
	129, 131, 137, 138, 140, 142, 143, 144,
	145, 146, 147, 148, 149, 150, 151, 152,
	153, 155, 157, 167, 170, 173, 174, 175,
	176, 177, 178, 179, 181, 188, 189, 190,
	191, 192, 193, 194, 195, 196, 197, 198,
	200, 201, 202, 203, 204, 205, 206, 207,
	215, 224, 239, 240, 253, 260, 262, 269,
	270, 271, 272, 273, 274, 275, 276, 277,
	280, 321, 322, 323, 325, 331,
};

const irq_hw_number_t mpm_c5lte_gpio_bypass[] = {
	86, 87, 88,
};

int mpm_c5lte_classify_irq(bool gpio, irq_hw_number_t hwirq)
{
	const struct mpm_pin *mapped = gpio ? mpm_c5lte_gpio_chip_data :
					   mpm_c5lte_gic_chip_data;
	const irq_hw_number_t *bypass = gpio ? mpm_c5lte_gpio_bypass :
						mpm_c5lte_gic_bypass;
	unsigned int count = gpio ? ARRAY_SIZE(mpm_c5lte_gpio_bypass) :
					  ARRAY_SIZE(mpm_c5lte_gic_bypass);
	unsigned int i;

	for (i = 0; mapped[i].pin >= 0; i++)
		if (mapped[i].hwirq == hwirq)
			return MPM_C5LTE_IRQ_MAPPED;
	for (i = 0; i < count; i++)
		if (bypass[i] == hwirq)
			return MPM_C5LTE_IRQ_BYPASS;
	/* Unlisted IRQs need separate enabled/wake tracking and an XO vote. */
	return MPM_C5LTE_IRQ_UNLISTED;
}
