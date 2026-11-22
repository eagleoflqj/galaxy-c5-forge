#ifdef CONFIG_C5_PM_RESUME_SUMMARY
#include <linux/wakeup_reason.h>
#endif
/* Copyright (c) 2013-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_irq.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/spinlock.h>
#include <linux/syscore_ops.h>
#include "pinctrl-msm.h"

#include <linux/pinctrl/sec-pinmux.h>

#if defined(CONFIG_FB_MSM_MDSS_SAMSUNG)
#include <linux/c5-display.h>
#endif

/* config translations */
#define drv_str_to_rval(drv)	((drv >> 1) - 1)
#define rval_to_drv_str(val)	((val + 1) << 1)
#define dir_to_inout_val(dir)	(dir << 1)
#define inout_val_to_dir(val)	(val >> 1)
#define rval_to_pull(val)	((val > 2) ? 1 : val)
#define TLMM_NO_PULL			0
#define TLMM_PULL_DOWN			1
#define TLMM_PULL_UP			3
/* GP PIN TYPE REG MASKS */
#define TLMM_GP_DRV_SHFT		6
#define TLMM_GP_DRV_MASK		0x7
#define TLMM_GP_PULL_SHFT		0
#define TLMM_GP_PULL_MASK		0x3
#define TLMM_GP_DIR_SHFT		9
#define TLMM_GP_DIR_MASK		1
#define TLMM_GP_FUNC_SHFT		2
#define TLMM_GP_FUNC_MASK		0xF
#define GPIO_OUT_BIT			1
#define GPIO_IN_BIT			0
#define GPIO_OE_BIT			9
/* SDC1 PIN TYPE REG MASKS */
#define TLMM_SDC1_CLK_DRV_SHFT		6
#define TLMM_SDC1_CLK_DRV_MASK		0x7
#define TLMM_SDC1_DATA_DRV_SHFT		0
#define TLMM_SDC1_DATA_DRV_MASK		0x7
#define TLMM_SDC1_CMD_DRV_SHFT		3
#define TLMM_SDC1_CMD_DRV_MASK		0x7
#define TLMM_SDC1_CLK_PULL_SHFT		13
#define TLMM_SDC1_CLK_PULL_MASK		0x3
#define TLMM_SDC1_DATA_PULL_SHFT	9
#define TLMM_SDC1_DATA_PULL_MASK	0x3
#define TLMM_SDC1_CMD_PULL_SHFT		11
#define TLMM_SDC1_CMD_PULL_MASK		0x3
#define TLMM_SDC1_RCLK_PULL_SHFT	15
#define TLMM_SDC1_RCLK_PULL_MASK	0x3
/* SDC2 PIN TYPE REG MASKS */
#define TLMM_SDC2_CLK_DRV_SHFT		6
#define TLMM_SDC2_CLK_DRV_MASK		0x7
#define TLMM_SDC2_DATA_DRV_SHFT		0
#define TLMM_SDC2_DATA_DRV_MASK		0x7
#define TLMM_SDC2_CMD_DRV_SHFT		3
#define TLMM_SDC2_CMD_DRV_MASK		0x7
#define TLMM_SDC2_CLK_PULL_SHFT		14
#define TLMM_SDC2_CLK_PULL_MASK		0x3
#define TLMM_SDC2_DATA_PULL_SHFT	9
#define TLMM_SDC2_DATA_PULL_MASK	0x3
#define TLMM_SDC2_CMD_PULL_SHFT		11
#define TLMM_SDC2_CMD_PULL_MASK		0x3
/* SDC 3 PIN TYPE REG MASKS */
#define TLMMV3_SDC3_CLK_DRV_SHFT	6
#define TLMMV3_SDC3_CLK_DRV_MASK	0x7
#define TLMMV3_SDC3_DATA_DRV_SHFT	0
#define TLMMV3_SDC3_DATA_DRV_MASK	0x7
#define TLMMV3_SDC3_CMD_DRV_SHFT	3
#define TLMMV3_SDC3_CMD_DRV_MASK	0x7
#define TLMMV3_SDC3_CLK_PULL_SHFT	14
#define TLMMV3_SDC3_CLK_PULL_MASK	0x3
#define TLMMV3_SDC3_DATA_PULL_SHFT	9
#define TLMMV3_SDC3_DATA_PULL_MASK	0x3
#define TLMMV3_SDC3_CMD_PULL_SHFT	11
#define TLMMV3_SDC3_CMD_PULL_MASK	0x3
/* EBI2 PIN TYPE REG MASKS */
#define TLMM_EBI2_BOOT_SELECT_BIT	0
#define TLMM_EMMC_BOOT_SELECT_BIT	1
#define TLMM_EBI2_CS_PULL_SHFT		2
#define TLMM_EBI2_CS_PULL_MASK		0x3
#define TLMM_EBI2_CS_DRV_SHFT		4
#define TLMM_EBI2_CS_DRV_MASK		0x7
#define TLMM_EBI2_OE_PULL_SHFT		7
#define TLMM_EBI2_OE_PULL_MASK		0x3
#define TLMM_EBI2_OE_DRV_SHFT		9
#define TLMM_EBI2_OE_DRV_MASK		0x7
#define TLMM_EBI2_ALE_PULL_SHFT		12
#define TLMM_EBI2_ALE_PULL_MASK		0x3
#define TLMM_EBI2_ALE_DRV_SHFT		14
#define TLMM_EBI2_ALE_DRV_MASK		0x7
#define TLMM_EBI2_CLE_PULL_SHFT		17
#define TLMM_EBI2_CLE_PULL_MASK		0x3
#define TLMM_EBI2_CLE_DRV_SHFT		19
#define TLMM_EBI2_CLE_DRV_MASK		0x7
#define TLMM_EBI2_WE_PULL_SHFT		22
#define TLMM_EBI2_WE_PULL_MASK		0x3
#define TLMM_EBI2_WE_DRV_SHFT		24
#define TLMM_EBI2_WE_DRV_MASK		0x7
#define TLMM_EBI2_BUSY_PULL_SHFT	27
#define TLMM_EBI2_BUSY_PULL_MASK	0x3
#define TLMM_EBI2_BUSY_DRV_SHFT		29
#define TLMM_EBI2_BUSY_DRV_MASK		0x7
#define TLMM_EBI2_DATA_PULL_SHFT	15
#define TLMM_EBI2_DATA_PULL_MASK	0x3
#define TLMM_EBI2_DATA_DRV_SHFT		17
#define TLMM_EBI2_DATA_DRV_MASK		0x7

/* TLMM IRQ REG fields */
#define INTR_ENABLE_BIT		0
#define	INTR_POL_CTL_BIT	1
#define	INTR_DECT_CTL_BIT	2
#define	INTR_RAW_STATUS_EN_BIT	4
#define	INTR_TARGET_PROC_BIT	5
#define	INTR_DIR_CONN_EN_BIT	8
#define INTR_STATUS_BIT		0
#define DC_POLARITY_BIT		8

/* Target processors for TLMM pin based interrupts */
#define INTR_TARGET_PROC_APPS(core_id)	((core_id) << INTR_TARGET_PROC_BIT)
#define TLMM_APPS_ID_DEFAULT	4
#define INTR_TARGET_PROC_NONE	(7 << INTR_TARGET_PROC_BIT)
/* Interrupt flag bits */
#define DC_POLARITY_HI		BIT(DC_POLARITY_BIT)
#define INTR_POL_CTL_HI		BIT(INTR_POL_CTL_BIT)
#define INTR_DECT_CTL_LEVEL	(0 << INTR_DECT_CTL_BIT)
#define INTR_DECT_CTL_POS_EDGE	(1 << INTR_DECT_CTL_BIT)
#define INTR_DECT_CTL_NEG_EDGE	(2 << INTR_DECT_CTL_BIT)
#define INTR_DECT_CTL_DUAL_EDGE	(3 << INTR_DECT_CTL_BIT)
#define INTR_DECT_CTL_MASK	(3 << INTR_DECT_CTL_BIT)

#define TLMM_GP_INOUT_BIT		1
#define TLMM_GP_OUT			BIT(TLMM_GP_INOUT_BIT)
#define TLMM_GP_IN			0

#define gc_to_pintype(gc) \
		container_of(gc, struct msm_pintype_info, gc)
#define ic_to_pintype(ic) \
		((struct msm_pintype_info *)ic->pinfo)
#define pintype_get_gc(pinfo)	(&pinfo->gc)
#define pintype_get_ic(pinfo)	(pinfo->irq_chip)

/* GP pin type register offsets */
#define TLMM_GP_CFG(pi, pin)	(pi->reg_base + 0x0 + \
				 pi->pintype_data->gp_reg_size * (pin))
#define TLMM_GP_INOUT(pi, pin)	(pi->reg_base + 0x4 + \
				 pi->pintype_data->gp_reg_size * (pin))
#define TLMM_GP_INTR_CFG(pi, pin)	(pi->reg_base + 0x8 + \
					 pi->pintype_data->gp_reg_size * (pin))
#define TLMM_GP_INTR_STATUS(pi, pin)	(pi->reg_base + 0x0c + \
					 pi->pintype_data->gp_reg_size * (pin))

/* QDSD Pin type register offsets */
#define TLMMV_QDSD_PULL_MASK			0x3
#define TLMMV4_QDSD_PULL_OFFSET			0x3
#define TLMMV4_QDSD_CONFIG_WIDTH		0x5
#define TLMMV4_QDSD_DRV_MASK			0x7

struct msm_sdc_regs {
	unsigned long pull_mask;
	unsigned long pull_shft;
	unsigned long drv_mask;
	unsigned long drv_shft;
};

struct msm_ebi_regs {
	unsigned long pull_mask;
	unsigned long pull_shft;
	unsigned long drv_mask;
	unsigned long drv_shft;
};

static const struct msm_sdc_regs sdc_regs[MSM_PINTYPE_SDC_REGS_MAX] = {
	/* SDC1 CLK */
	{
		.pull_mask = TLMM_SDC1_CLK_PULL_MASK,
		.pull_shft = TLMM_SDC1_CLK_PULL_SHFT,
		.drv_mask = TLMM_SDC1_CLK_DRV_MASK,
		.drv_shft = TLMM_SDC1_CLK_DRV_SHFT,
	},
	/* SDC1 CMD */
	{
		.pull_mask = TLMM_SDC1_CMD_PULL_MASK,
		.pull_shft = TLMM_SDC1_CMD_PULL_SHFT,
		.drv_mask = TLMM_SDC1_CMD_DRV_MASK,
		.drv_shft = TLMM_SDC1_CMD_DRV_SHFT,
	},
	/* SDC1 DATA */
	{
		.pull_mask = TLMM_SDC1_DATA_PULL_MASK,
		.pull_shft = TLMM_SDC1_DATA_PULL_SHFT,
		.drv_mask = TLMM_SDC1_DATA_DRV_MASK,
		.drv_shft = TLMM_SDC1_DATA_DRV_SHFT,
	},
	/* SDC1 RCLK */
	{
		.pull_mask = TLMM_SDC1_RCLK_PULL_MASK,
		.pull_shft = TLMM_SDC1_RCLK_PULL_SHFT,
	},
	/* SDC2 CLK */
	{
		.pull_mask = TLMM_SDC2_CLK_PULL_MASK,
		.pull_shft = TLMM_SDC2_CLK_PULL_SHFT,
		.drv_mask = TLMM_SDC2_CLK_DRV_MASK,
		.drv_shft = TLMM_SDC2_CLK_DRV_SHFT,
	},
	/* SDC2 CMD */
	{
		.pull_mask = TLMM_SDC2_CMD_PULL_MASK,
		.pull_shft = TLMM_SDC2_CMD_PULL_SHFT,
		.drv_mask = TLMM_SDC2_CMD_DRV_MASK,
		.drv_shft = TLMM_SDC2_CMD_DRV_SHFT,
	},
	/* SDC2 DATA */
	{
		.pull_mask = TLMM_SDC2_DATA_PULL_MASK,
		.pull_shft = TLMM_SDC2_DATA_PULL_SHFT,
		.drv_mask = TLMM_SDC2_DATA_DRV_MASK,
		.drv_shft = TLMM_SDC2_DATA_DRV_SHFT,
	},
	/* SDC3 CLK */
	{
		.pull_mask = TLMMV3_SDC3_CLK_PULL_MASK,
		.pull_shft = TLMMV3_SDC3_CLK_PULL_SHFT,
		.drv_mask = TLMMV3_SDC3_CLK_DRV_MASK,
		.drv_shft = TLMMV3_SDC3_CLK_DRV_SHFT,
	},
	/* SDC3 CMD */
	{
		.pull_mask = TLMMV3_SDC3_CMD_PULL_MASK,
		.pull_shft = TLMMV3_SDC3_CMD_PULL_SHFT,
		.drv_mask = TLMMV3_SDC3_CMD_DRV_MASK,
		.drv_shft = TLMMV3_SDC3_CMD_DRV_SHFT,
	},
	/* SDC3 DATA */
	{
		.pull_mask = TLMMV3_SDC3_DATA_PULL_MASK,
		.pull_shft = TLMMV3_SDC3_DATA_PULL_SHFT,
		.drv_mask = TLMMV3_SDC3_DATA_DRV_MASK,
		.drv_shft = TLMMV3_SDC3_DATA_DRV_SHFT,
	},

};

static int msm_tlmm_sdc_cfg(uint pin_no, unsigned long *config,
			    bool write, const struct msm_pintype_info *pinfo)
{
	unsigned int val, id, data;
	u32 mask, shft;
	void __iomem *cfg_reg;
	void __iomem *reg_base = pinfo->reg_base;
	const struct msm_pintype_data *sdc_info = pinfo->pintype_data;
	s32 offset = sdc_info->sdc_reg_offsets[pin_no];

	if (pin_no >= ARRAY_SIZE(sdc_regs))
		return -EINVAL;

	cfg_reg = reg_base + offset;
	id = pinconf_to_config_param(*config);
	val = readl_relaxed(cfg_reg);
	/* Get mask and shft values for this config type */
	switch (id) {
	case PIN_CONFIG_BIAS_DISABLE:
		mask = sdc_regs[pin_no].pull_mask;
		shft = sdc_regs[pin_no].pull_shft;
		data = TLMM_NO_PULL;
		if (!write) {
			val >>= shft;
			val &= mask;
			data = rval_to_pull(val);
		}
		break;
	case PIN_CONFIG_BIAS_PULL_DOWN:
		mask = sdc_regs[pin_no].pull_mask;
		shft = sdc_regs[pin_no].pull_shft;
		data = TLMM_PULL_DOWN;
		if (!write) {
			val >>= shft;
			val &= mask;
			data = rval_to_pull(val);
		}
		break;
	case PIN_CONFIG_BIAS_PULL_UP:
		mask = sdc_regs[pin_no].pull_mask;
		shft = sdc_regs[pin_no].pull_shft;
		data = TLMM_PULL_UP;
		if (!write) {
			val >>= shft;
			val &= mask;
			data = rval_to_pull(val);
		}
		break;
	case PIN_CONFIG_DRIVE_STRENGTH:
		mask = sdc_regs[pin_no].drv_mask;
		shft = sdc_regs[pin_no].drv_shft;
		if (write) {
			data = pinconf_to_config_argument(*config);
			data = drv_str_to_rval(data);
		} else {
			val >>= shft;
			val &= mask;
			data = rval_to_drv_str(val);
		}
		break;
	default:
		return -EINVAL;
	};

	if (write) {
		val &= ~(mask << shft);
		val |= (data << shft);
		writel_relaxed(val, cfg_reg);
	} else
		*config = pinconf_to_config_packed(id, data);
	return 0;
}

static int msm_tlmm_qdsd_cfg(uint pin_no, unsigned long *config,
			     bool write, const struct msm_pintype_info *pinfo)
{
	unsigned int val, id, data;
	u32 mask, shft;
	void __iomem *cfg_reg;

	cfg_reg = pinfo->reg_base;
	id = pinconf_to_config_param(*config);
	val = readl_relaxed(cfg_reg);
	/* Get mask and shft values for this config type */
	switch (id) {
	case PIN_CONFIG_BIAS_DISABLE:
		mask = TLMMV_QDSD_PULL_MASK;
		shft = pin_no * TLMMV4_QDSD_CONFIG_WIDTH
					+ TLMMV4_QDSD_PULL_OFFSET;
		data = TLMM_NO_PULL;
		if (!write) {
			val >>= shft;
			data = val & mask;
		}
		break;
	case PIN_CONFIG_BIAS_PULL_DOWN:
		mask = TLMMV_QDSD_PULL_MASK;
		shft = pin_no * TLMMV4_QDSD_CONFIG_WIDTH
					+ TLMMV4_QDSD_PULL_OFFSET;
		data = TLMM_PULL_DOWN;
		if (!write) {
			val >>= shft;
			data = val & mask;
		}
		break;
	case PIN_CONFIG_BIAS_PULL_UP:
		mask = TLMMV_QDSD_PULL_MASK;
		shft = pin_no * TLMMV4_QDSD_CONFIG_WIDTH
					+ TLMMV4_QDSD_PULL_OFFSET;
		data = TLMM_PULL_UP;
		if (!write) {
			val >>= shft;
			data = val & mask;
		}
		break;
	case PIN_CONFIG_DRIVE_STRENGTH:
		mask = TLMMV4_QDSD_DRV_MASK;
		shft = pin_no * TLMMV4_QDSD_CONFIG_WIDTH;
		if (write) {
			data = pinconf_to_config_argument(*config);
		} else {
			val >>= shft;
			data = val & mask;
		}
		break;
	default:
		return -EINVAL;
	};

	if (write) {
		val &= ~(mask << shft);
		/* QDSD software override bit */
		val |= ((data << shft) | BIT(31));
		writel_relaxed(val, cfg_reg);
	} else {
		*config = pinconf_to_config_packed(id, data);
	}
	return 0;
}

static int msm_tlmm_gp_cfg(uint pin_no, unsigned long *config,
			   bool write, const struct msm_pintype_info *pinfo)
{
	unsigned int val, id, data, inout_val;
	u32 mask = 0, shft = 0;
	void __iomem *inout_reg = NULL;
	void __iomem *cfg_reg = TLMM_GP_CFG(pinfo, pin_no);

#ifdef ENABLE_SENSORS_FPRINT_SECURE
	if (pin_no >= CONFIG_SENSORS_FP_SPI_GPIO_START
		&& pin_no <= CONFIG_SENSORS_FP_SPI_GPIO_END)
		return 0;
#endif
#if defined(CONFIG_MST_LDO) || defined(CONFIG_C5_MST_PIN_GUARD)
		if (pin_no == MST_GPIO_D_MINUS || pin_no == MST_GPIO_D_PLUS)
			return 0;
#endif

	id = pinconf_to_config_param(*config);
	val = readl_relaxed(cfg_reg);
	/* Get mask and shft values for this config type */
	switch (id) {
	case PIN_CONFIG_BIAS_DISABLE:
		mask = TLMM_GP_PULL_MASK;
		shft = TLMM_GP_PULL_SHFT;
		data = TLMM_NO_PULL;
		if (!write) {
			val >>= shft;
			val &= mask;
			data = rval_to_pull(val);
		}
		break;
	case PIN_CONFIG_BIAS_PULL_DOWN:
		mask = TLMM_GP_PULL_MASK;
		shft = TLMM_GP_PULL_SHFT;
		data = TLMM_PULL_DOWN;
		if (!write) {
			val >>= shft;
			val &= mask;
			data = rval_to_pull(val);
		}
		break;
	case PIN_CONFIG_BIAS_PULL_UP:
		mask = TLMM_GP_PULL_MASK;
		shft = TLMM_GP_PULL_SHFT;
		data = TLMM_PULL_UP;
		if (!write) {
			val >>= shft;
			val &= mask;
			data = rval_to_pull(val);
		}
		break;
	case PIN_CONFIG_DRIVE_STRENGTH:
		mask = TLMM_GP_DRV_MASK;
		shft = TLMM_GP_DRV_SHFT;
		if (write) {
			data = pinconf_to_config_argument(*config);
			data = drv_str_to_rval(data);
		} else {
			val >>= shft;
			val &= mask;
			data = rval_to_drv_str(val);
		}
		break;
	case PIN_CONFIG_OUTPUT:
		mask = TLMM_GP_DIR_MASK;
		shft = TLMM_GP_DIR_SHFT;
		inout_reg = TLMM_GP_INOUT(pinfo, pin_no);
		if (write) {
			data = pinconf_to_config_argument(*config);
			inout_val = dir_to_inout_val(data);
			writel_relaxed(inout_val, inout_reg);
			data = mask;
		} else {
			inout_val = readl_relaxed(inout_reg);
			data = inout_val_to_dir(inout_val);
		}
		break;
	case PIN_CONFIG_INPUT_ENABLE:
		mask = TLMM_GP_DIR_MASK;
		shft = TLMM_GP_DIR_SHFT;
		inout_reg = TLMM_GP_INOUT(pinfo, pin_no);
		if (write) {
			/* GPIO_IN (b0) of TLMM_GPIO_IN_OUT is read-only */
			data = 0;
		} else {
			inout_val = readl_relaxed(inout_reg);
			data = inout_val;
		}
		break;

	default:
		return -EINVAL;
	};

	if (write) {
		val &= ~(mask << shft);
		val |= (data << shft);
		writel_relaxed(val, cfg_reg);
	} else
		*config = pinconf_to_config_packed(id, data);
	return 0;
}

static const struct msm_ebi_regs ebi_regs[MSM_PINTYPE_EBI_REGS_MAX] = {
	/* EBI2 CS*/
	{
		.pull_mask = TLMM_EBI2_CS_PULL_MASK,
		.pull_shft = TLMM_EBI2_CS_PULL_SHFT,
		.drv_mask = TLMM_EBI2_CS_DRV_MASK,
		.drv_shft = TLMM_EBI2_CS_DRV_SHFT,
	},
	/* EBI2 OE */
	{
		.pull_mask = TLMM_EBI2_OE_PULL_MASK,
		.pull_shft = TLMM_EBI2_OE_PULL_SHFT,
		.drv_mask = TLMM_EBI2_OE_DRV_MASK,
		.drv_shft = TLMM_EBI2_OE_DRV_SHFT,
	},
	/* EBI2 ALE*/
	{
		.pull_mask = TLMM_EBI2_ALE_PULL_MASK,
		.pull_shft = TLMM_EBI2_ALE_PULL_SHFT,
		.drv_mask = TLMM_EBI2_ALE_DRV_MASK,
		.drv_shft = TLMM_EBI2_ALE_DRV_SHFT,
	},
	/* EBI2 CLE */
	{
		.pull_mask = TLMM_EBI2_CLE_PULL_MASK,
		.pull_shft = TLMM_EBI2_CLE_PULL_SHFT,
		.drv_mask = TLMM_EBI2_CLE_DRV_MASK,
		.drv_shft = TLMM_EBI2_CLE_DRV_SHFT,
	},
	/* EBI2 WE*/
	{
		.pull_mask = TLMM_EBI2_WE_PULL_MASK,
		.pull_shft = TLMM_EBI2_WE_PULL_SHFT,
		.drv_mask = TLMM_EBI2_WE_DRV_MASK,
		.drv_shft = TLMM_EBI2_WE_DRV_SHFT,
	},
	/* EBI2 BUSY */
	{
		.pull_mask = TLMM_EBI2_BUSY_PULL_MASK,
		.pull_shft = TLMM_EBI2_BUSY_PULL_SHFT,
		.drv_mask = TLMM_EBI2_BUSY_DRV_MASK,
		.drv_shft = TLMM_EBI2_BUSY_DRV_SHFT,
	},
	/* EBI2 DATA */
	{
		.pull_mask = TLMM_EBI2_DATA_PULL_MASK,
		.pull_shft = TLMM_EBI2_DATA_PULL_SHFT,
		.drv_mask = TLMM_EBI2_DATA_DRV_MASK,
		.drv_shft = TLMM_EBI2_DATA_DRV_SHFT,
	},
};

static int msm_tlmm_ebi_cfg(uint pin_no, unsigned long *config,
			    bool write, const struct msm_pintype_info *pinfo)
{
	unsigned int val, id, data;
	u32 mask, shft;
	void __iomem *cfg_reg;
	void __iomem *reg_base = pinfo->reg_base;
	const struct msm_pintype_data *ebi_info = pinfo->pintype_data;
	s32 offset = ebi_info->ebi_reg_offsets[pin_no];

	if (pin_no >= ARRAY_SIZE(ebi_regs))
		return -EINVAL;

	cfg_reg = reg_base + offset;
	id = pinconf_to_config_param(*config);
	val = readl_relaxed(cfg_reg);
	/* Get mask and shft values for this config type */
	switch (id) {
	case PIN_CONFIG_BIAS_DISABLE:
		mask = ebi_regs[pin_no].pull_mask;
		shft = ebi_regs[pin_no].pull_shft;
		data = TLMM_NO_PULL;
		if (!write) {
			val >>= shft;
			val &= mask;
			data = rval_to_pull(val);
		}
		break;
	case PIN_CONFIG_BIAS_PULL_DOWN:
		mask = ebi_regs[pin_no].pull_mask;
		shft = ebi_regs[pin_no].pull_shft;
		data = TLMM_PULL_DOWN;
		if (!write) {
			val >>= shft;
			val &= mask;
			data = rval_to_pull(val);
		}
		break;
	case PIN_CONFIG_BIAS_PULL_UP:
		mask = ebi_regs[pin_no].pull_mask;
		shft = ebi_regs[pin_no].pull_shft;
		data = TLMM_PULL_UP;
		if (!write) {
			val >>= shft;
			val &= mask;
			data = rval_to_pull(val);
		}
		break;
	case PIN_CONFIG_DRIVE_STRENGTH:
		mask = ebi_regs[pin_no].drv_mask;
		shft = ebi_regs[pin_no].drv_shft;
		if (write) {
			data = pinconf_to_config_argument(*config);
			data = drv_str_to_rval(data);
		} else {
			val >>= shft;
			val &= mask;
			data = rval_to_drv_str(val);
		}
		break;
	default:
		return -EINVAL;
	};

	if (write) {
		val &= ~(mask << shft);
		val |= (data << shft);
		writel_relaxed(val, cfg_reg);
	} else
		*config = pinconf_to_config_packed(id, data);
	return 0;
}

static void msm_tlmm_set_reg_base(void __iomem *tlmm_base,
				  struct msm_pintype_info *pinfo)
{
	pinfo->reg_base = tlmm_base + pinfo->pintype_data->reg_base_offset;
}

static void msm_tlmm_gp_fn(uint pin_no, u32 func, bool enable,
			   const struct msm_pintype_info *pinfo)
{
	unsigned int val;
	void __iomem *cfg_reg = TLMM_GP_CFG(pinfo, pin_no);
	
#ifdef ENABLE_SENSORS_FPRINT_SECURE
	if (pin_no >= CONFIG_SENSORS_FP_SPI_GPIO_START
		&& pin_no <= CONFIG_SENSORS_FP_SPI_GPIO_END)
		return;
#endif

	val = readl_relaxed(cfg_reg);
	val &= ~(TLMM_GP_FUNC_MASK << TLMM_GP_FUNC_SHFT);
	if (enable)
		val |= (func << TLMM_GP_FUNC_SHFT);
	writel_relaxed(val, cfg_reg);
}

/* GPIO CHIP */
static int msm_tlmm_gp_get(struct gpio_chip *gc, unsigned offset)
{
	struct msm_pintype_info *pinfo = gc_to_pintype(gc);
	void __iomem *inout_reg = TLMM_GP_INOUT(pinfo, offset);

	return readl_relaxed(inout_reg) & BIT(GPIO_IN_BIT);
}

static void msm_tlmm_gp_set(struct gpio_chip *gc, unsigned offset, int val)
{
	struct msm_pintype_info *pinfo = gc_to_pintype(gc);
	void __iomem *inout_reg = TLMM_GP_INOUT(pinfo, offset);

	writel_relaxed(val ? BIT(GPIO_OUT_BIT) : 0, inout_reg);
}

static int msm_tlmm_gp_dir_in(struct gpio_chip *gc, unsigned offset)
{
	unsigned int val;
	struct msm_pintype_info *pinfo = gc_to_pintype(gc);
	void __iomem *cfg_reg = TLMM_GP_CFG(pinfo, offset);

	val = readl_relaxed(cfg_reg);
	val &= ~BIT(GPIO_OE_BIT);
	writel_relaxed(val, cfg_reg);
	return 0;
}

static int msm_tlmm_gp_dir_out(struct gpio_chip *gc, unsigned offset, int val)
{
	struct msm_pintype_info *pinfo = gc_to_pintype(gc);
	void __iomem *cfg_reg = TLMM_GP_CFG(pinfo, offset);

	msm_tlmm_gp_set(gc, offset, val);
	val = readl_relaxed(cfg_reg);
	val |= BIT(GPIO_OE_BIT);
	writel_relaxed(val, cfg_reg);
	return 0;
}

static int msm_tlmm_gp_to_irq(struct gpio_chip *gc, unsigned offset)
{
	struct msm_pintype_info *pinfo = gc_to_pintype(gc);
	struct msm_tlmm_irq_chip *ic = pintype_get_ic(pinfo);
	struct irq_fwspec fwspec;
	unsigned int virq;

	if (!ic->domain || offset >= ic->num_irqs)
		return -EINVAL;
	virq = irq_find_mapping(ic->domain, offset);
	if (virq)
		return virq;
	if (ic->domain->parent) {
		fwspec.fwnode = ic->domain->fwnode;
		fwspec.param_count = 2;
		fwspec.param[0] = offset;
		fwspec.param[1] = IRQ_TYPE_NONE;
		virq = irq_create_fwspec_mapping(&fwspec);
		if (!virq)
			return -ENXIO;
		return virq;
	}
	virq = irq_create_mapping(ic->domain, offset);
	if (!virq)
		return -ENXIO;
	return virq;
}

/* Irq reg ops */
static void msm_tlmm_set_intr_status(struct msm_tlmm_irq_chip *ic, unsigned pin)
{
	const struct msm_pintype_info *pinfo = ic->pinfo;
	void __iomem *status_reg = TLMM_GP_INTR_STATUS(pinfo, pin);

	writel_relaxed(0, status_reg);
}

static int msm_tlmm_get_intr_status(struct msm_tlmm_irq_chip *ic, unsigned pin)
{
	const struct msm_pintype_info *pinfo = ic->pinfo;
	void __iomem *status_reg = TLMM_GP_INTR_STATUS(pinfo, pin);
	return readl_relaxed(status_reg) & BIT(INTR_STATUS_BIT);
}

static void msm_tlmm_set_intr_cfg_enable(struct msm_tlmm_irq_chip *ic,
					 unsigned pin, int enable)
{
	unsigned int val;
	const struct msm_pintype_info *pinfo = ic->pinfo;
	void __iomem *cfg_reg = TLMM_GP_INTR_CFG(pinfo, pin);

	val = readl_relaxed(cfg_reg);
	if (enable) {
		val &= ~BIT(INTR_DIR_CONN_EN_BIT);
		val |= BIT(INTR_ENABLE_BIT);
	} else
		val &= ~BIT(INTR_ENABLE_BIT);
	writel_relaxed(val, cfg_reg);
}

static int msm_tlmm_get_intr_cfg_enable(struct msm_tlmm_irq_chip *ic,
					unsigned pin)
{
	const struct msm_pintype_info *pinfo = ic->pinfo;
	void __iomem *cfg_reg = TLMM_GP_INTR_CFG(pinfo, pin);

	return readl_relaxed(cfg_reg) & BIT(INTR_ENABLE_BIT);
}

static void msm_tlmm_set_intr_cfg_type(struct msm_tlmm_irq_chip *ic,
				       struct irq_data *d, unsigned int type)
{
	unsigned cfg;
	const struct msm_pintype_info *pinfo = ic->pinfo;
	void __iomem *cfg_reg = TLMM_GP_INTR_CFG(pinfo, (irqd_to_hwirq(d)));

	/*
	 * RAW_STATUS_EN is left on for all gpio irqs. Due to the
	 * internal circuitry of TLMM, toggling the RAW_STATUS
	 * could cause the INTR_STATUS to be set for EDGE interrupts.
	 */
	cfg = BIT(INTR_RAW_STATUS_EN_BIT) | INTR_TARGET_PROC_APPS(ic->apps_id);
	writel_relaxed(cfg, cfg_reg);
	cfg &= ~INTR_DECT_CTL_MASK;
	if (type == IRQ_TYPE_EDGE_RISING)
		cfg |= INTR_DECT_CTL_POS_EDGE;
	else if (type == IRQ_TYPE_EDGE_FALLING)
		cfg |= INTR_DECT_CTL_NEG_EDGE;
	else if (type == IRQ_TYPE_EDGE_BOTH)
		cfg |= INTR_DECT_CTL_DUAL_EDGE;
	else
		cfg |= INTR_DECT_CTL_LEVEL;

	if (type & IRQ_TYPE_LEVEL_LOW)
		cfg &= ~INTR_POL_CTL_HI;
	else
		cfg |= INTR_POL_CTL_HI;

	writel_relaxed(cfg, cfg_reg);
	/*
	 * Sometimes it might take a little while to update
	 * the interrupt status after the RAW_STATUS is enabled
	 * We clear the interrupt status before enabling the
	 * interrupt in the unmask call-back.
	 */
	udelay(5);
}



static irqreturn_t msm_tlmm_gp_handle_irq(int irq, struct msm_tlmm_irq_chip *ic)
{
	unsigned long i;
	unsigned int virq = 0;
	struct irq_chip *chip;
	struct irq_desc *desc = irq_to_desc(irq);



#if defined(CONFIG_FB_MSM_MDSS_SAMSUNG)
	struct irq_desc *descriptor;
#endif

	if (unlikely(!desc))
		return IRQ_HANDLED;

	chip = irq_desc_get_chip(desc);
	chained_irq_enter(chip, desc);
	for_each_set_bit(i, ic->enabled_irqs, ic->num_irqs)
	 {
		dev_dbg(ic->dev, "hwirq in bit mask %d\n", (unsigned int)i);
		if (msm_tlmm_get_intr_status(ic, i)) {
			dev_dbg(ic->dev, "hwirw %d fired\n", (unsigned int)i);
			/* Mappings must be allocated before entering interrupt context. */
			virq = irq_find_mapping(ic->domain, i);
			if (!virq) {
				dev_dbg(ic->dev, "invalid virq\n");
				continue;
			}

#ifdef CONFIG_C5_PM_RESUME_SUMMARY
			c5_resume_summary_record_gpio(virq, i);
#endif

#if defined(CONFIG_FB_MSM_MDSS_SAMSUNG)
			descriptor = irq_to_desc(virq);
			if (!IS_ERR_OR_NULL(descriptor))
				mdss_samsung_resume_event(descriptor->irq_data.irq);
#endif
			generic_handle_irq(virq);
		}

	}
	chained_irq_exit(chip, desc);
	return IRQ_HANDLED;
}

static void msm_tlmm_irq_ack(struct irq_data *d)
{
	struct msm_tlmm_irq_chip *ic = irq_data_get_irq_chip_data(d);

	msm_tlmm_set_intr_status(ic, irqd_to_hwirq(d));
	mb();
}

static void msm_tlmm_irq_mask(struct irq_data *d)
{
	unsigned long irq_flags;
	struct msm_tlmm_irq_chip *ic = irq_data_get_irq_chip_data(d);

	spin_lock_irqsave(&ic->irq_lock, irq_flags);
	msm_tlmm_set_intr_cfg_enable(ic, irqd_to_hwirq(d), 0);
	__clear_bit(irqd_to_hwirq(d), ic->enabled_irqs);
	mb();
	spin_unlock_irqrestore(&ic->irq_lock, irq_flags);
	if (d->parent_data)
		irq_chip_mask_parent(d);
	else if (ic->irq_chip_extn->irq_mask)
		ic->irq_chip_extn->irq_mask(d);
}

static void msm_tlmm_irq_unmask(struct irq_data *d)
{
	unsigned long irq_flags;
	struct msm_tlmm_irq_chip *ic = irq_data_get_irq_chip_data(d);

	spin_lock_irqsave(&ic->irq_lock, irq_flags);
	__set_bit(irqd_to_hwirq(d), ic->enabled_irqs);
	if (!msm_tlmm_get_intr_cfg_enable(ic, irqd_to_hwirq(d))) {
		msm_tlmm_set_intr_status(ic, irqd_to_hwirq(d));
		msm_tlmm_set_intr_cfg_enable(ic, irqd_to_hwirq(d), 1);
		mb();
	}
	spin_unlock_irqrestore(&ic->irq_lock, irq_flags);
	if (d->parent_data)
		irq_chip_unmask_parent(d);
	else if (ic->irq_chip_extn->irq_unmask)
		ic->irq_chip_extn->irq_unmask(d);
}

static void msm_tlmm_irq_disable(struct irq_data *d)
{
	unsigned long flags;
	struct msm_tlmm_irq_chip *ic = irq_data_get_irq_chip_data(d);

	/* IRQ core treats irq_disable as a completed hardware mask. */
	spin_lock_irqsave(&ic->irq_lock, flags);
	msm_tlmm_set_intr_cfg_enable(ic, irqd_to_hwirq(d), 0);
	__clear_bit(irqd_to_hwirq(d), ic->enabled_irqs);
	mb();
	spin_unlock_irqrestore(&ic->irq_lock, flags);
	if (d->parent_data)
		irq_chip_disable_parent(d);
	else if (ic->irq_chip_extn->irq_disable)
		ic->irq_chip_extn->irq_disable(d);
}

static int msm_tlmm_irq_set_type(struct irq_data *d, unsigned int flow_type)
{
	unsigned long irq_flags;
	unsigned int pin = irqd_to_hwirq(d);
	struct msm_tlmm_irq_chip *ic = irq_data_get_irq_chip_data(d);
	int ret;

	/* Reject a failed parent update before changing TLMM registers. */
	if (d->parent_data) {
		ret = irq_chip_set_type_parent(d, flow_type);
		if (ret)
			return ret;
	} else if (ic->irq_chip_extn->irq_set_type) {
		ret = ic->irq_chip_extn->irq_set_type(d, flow_type);
		if (ret)
			return ret;
	}
	spin_lock_irqsave(&ic->irq_lock, irq_flags);

	if (flow_type & IRQ_TYPE_EDGE_BOTH) {
		irq_set_handler_locked(d, handle_edge_irq);
		if ((flow_type & IRQ_TYPE_EDGE_BOTH) == IRQ_TYPE_EDGE_BOTH)
			__set_bit(pin, ic->dual_edge_irqs);
		else
			__clear_bit(pin, ic->dual_edge_irqs);
	} else {
		irq_set_handler_locked(d, handle_level_irq);
		__clear_bit(pin, ic->dual_edge_irqs);
	}

	msm_tlmm_set_intr_cfg_type(ic, d, flow_type);

	mb();
	spin_unlock_irqrestore(&ic->irq_lock, irq_flags);

	return 0;
}

static int msm_tlmm_irq_set_wake(struct irq_data *d, unsigned int on)
{
	unsigned int pin = irqd_to_hwirq(d);
	struct msm_tlmm_irq_chip *ic = irq_data_get_irq_chip_data(d);
	unsigned long flags;
	bool change_summary;
	int ret = 0;

	if (d->parent_data)
		ret = irq_chip_set_wake_parent(d, on);
	else if (ic->irq_chip_extn->irq_set_wake)
		ret = ic->irq_chip_extn->irq_set_wake(d, on);
	if (ret)
		return ret;

	/* Serialize first/last wake-source transitions across GPIO IRQs. */
	spin_lock_irqsave(&ic->irq_lock, flags);
	if (!!on == test_bit(pin, ic->wake_irqs)) {
		spin_unlock_irqrestore(&ic->irq_lock, flags);
		return 0;
	}
	if (on) {
		change_summary = bitmap_empty(ic->wake_irqs, ic->num_irqs);
	} else {
		__clear_bit(pin, ic->wake_irqs);
		change_summary = bitmap_empty(ic->wake_irqs, ic->num_irqs);
		__set_bit(pin, ic->wake_irqs);
	}
	if (change_summary)
		ret = irq_set_irq_wake(ic->irq, !!on);
	if (!ret) {
		if (on)
			__set_bit(pin, ic->wake_irqs);
		else
			__clear_bit(pin, ic->wake_irqs);
	}
	spin_unlock_irqrestore(&ic->irq_lock, flags);

	if (ret) {
		/* Native MPM skips set_wake; retain rollback for extension chips. */
		if (d->parent_data)
			irq_chip_set_wake_parent(d, !on);
		else if (ic->irq_chip_extn->irq_set_wake)
			ic->irq_chip_extn->irq_set_wake(d, !on);
	}
	return ret;
}

static struct lock_class_key msm_tlmm_irq_lock_class;

static int msm_tlmm_irq_map(struct irq_domain *h, unsigned int virq,
			    irq_hw_number_t hw)
{
	struct msm_tlmm_irq_chip *ic = h->host_data;

	if (hw >= ic->num_irqs)
		return -EINVAL;
	irq_set_lockdep_class(virq, &msm_tlmm_irq_lock_class);
	irq_set_chip_data(virq, ic);
	irq_set_chip_and_handler(virq, &ic->chip,
					handle_level_irq);
	/* set_irq_flags was an empty macro in the ARM64 3.10 baseline. */
	return 0;
}

/*
 * irq domain callbacks for interrupt controller.
 */
static const struct irq_domain_ops msm_tlmm_gp_irqd_ops = {
	.map	= msm_tlmm_irq_map,
	.xlate	= irq_domain_xlate_twocell,
};

static int msm_tlmm_domain_translate(struct irq_domain *domain,
		struct irq_fwspec *fwspec, irq_hw_number_t *hwirq,
		unsigned int *type)
{
	struct msm_tlmm_irq_chip *ic = domain->host_data;

	if (!is_of_node(fwspec->fwnode) || fwspec->param_count != 2 ||
	    fwspec->param[0] >= ic->num_irqs ||
	    (fwspec->param[1] & ~IRQ_TYPE_SENSE_MASK))
		return -EINVAL;
	*hwirq = fwspec->param[0];
	*type = fwspec->param[1];
	return 0;
}

static int msm_tlmm_domain_alloc(struct irq_domain *domain, unsigned int virq,
		unsigned int nr_irqs, void *arg)
{
	struct msm_tlmm_irq_chip *ic = domain->host_data;
	struct irq_fwspec parent_fwspec = *(struct irq_fwspec *)arg;
	irq_hw_number_t hwirq;
	unsigned int type;
	int ret;

	/* The native GPIO-MPM allocator supports one GPIO per request. */
	if (!domain->parent || nr_irqs != 1)
		return -EINVAL;
	ret = msm_tlmm_domain_translate(domain, arg, &hwirq, &type);
	if (ret)
		return ret;
	parent_fwspec.fwnode = domain->parent->fwnode;
	ret = irq_domain_alloc_irqs_parent(domain, virq, nr_irqs,
					 &parent_fwspec);
	if (ret)
		return ret;
	irq_domain_set_info(domain, virq, hwirq, &ic->chip, ic,
			    handle_level_irq, NULL, NULL);
	irq_set_lockdep_class(virq, &msm_tlmm_irq_lock_class);
	return 0;
}

static const struct irq_domain_ops msm_tlmm_gp_hierarchy_ops = {
	.translate = msm_tlmm_domain_translate,
	.alloc = msm_tlmm_domain_alloc,
	.free = irq_domain_free_irqs_top,
};

static struct irq_chip mpm_tlmm_irq_extn;

static struct msm_tlmm_irq_chip msm_tlmm_gp_irq = {
	.irq_chip_extn = &mpm_tlmm_irq_extn,
	.chip = {
		.name		= "msm_tlmm_irq",
		.irq_mask	= msm_tlmm_irq_mask,
		.irq_unmask	= msm_tlmm_irq_unmask,
		.irq_ack	= msm_tlmm_irq_ack,
		.irq_set_type	= msm_tlmm_irq_set_type,
		.irq_set_wake	= msm_tlmm_irq_set_wake,
		.irq_disable	= msm_tlmm_irq_disable,
	},
	.apps_id = TLMM_APPS_ID_DEFAULT,
	.domain_ops = &msm_tlmm_gp_irqd_ops,
	.handler = msm_tlmm_gp_handle_irq,
};

/* Power management core operations */

static int msm_tlmm_gp_irq_suspend(void)
{
	unsigned long irq_flags;
	unsigned long i;
	struct msm_tlmm_irq_chip *ic = &msm_tlmm_gp_irq;
	int num_irqs = ic->num_irqs;

	spin_lock_irqsave(&ic->irq_lock, irq_flags);
	for_each_set_bit(i, ic->enabled_irqs, num_irqs)
		msm_tlmm_set_intr_cfg_enable(ic, i, 0);

	for_each_set_bit(i, ic->wake_irqs, num_irqs)
		msm_tlmm_set_intr_cfg_enable(ic, i, 1);
	mb();
	spin_unlock_irqrestore(&ic->irq_lock, irq_flags);
	return 0;
}

static void msm_tlmm_gp_irq_resume(void)
{
	unsigned long irq_flags;
	unsigned long i;
	struct msm_tlmm_irq_chip *ic = &msm_tlmm_gp_irq;
	int num_irqs = ic->num_irqs;

	spin_lock_irqsave(&ic->irq_lock, irq_flags);
	for_each_set_bit(i, ic->wake_irqs, num_irqs)
		msm_tlmm_set_intr_cfg_enable(ic, i, 0);

	for_each_set_bit(i, ic->enabled_irqs, num_irqs)
		msm_tlmm_set_intr_cfg_enable(ic, i, 1);
	mb();
	spin_unlock_irqrestore(&ic->irq_lock, irq_flags);
}

static struct syscore_ops msm_tlmm_irq_syscore_ops = {
	.suspend = msm_tlmm_gp_irq_suspend,
	.resume = msm_tlmm_gp_irq_resume,
};

#ifdef CONFIG_USE_PINCTRL_IRQ
int msm_tlmm_of_gp_irq_init(struct device_node *controller,
			    struct irq_chip *chip_extn)
{
	int ret;
	u32 num_irqs, num_pins, apps_id;
	struct device_node *pin_node, *parent_node;
	struct irq_domain *domain, *parent_domain;
	struct msm_tlmm_irq_chip *ic = &msm_tlmm_gp_irq;

	if (!chip_extn)
		return -EINVAL;
	if (ic->domain)
		return -EBUSY;
	ret = of_property_read_u32(controller, "num_irqs", &num_irqs);
	if (ret) {
		WARN(1, "Cannot get numirqs from device tree\n");
		return ret;
	}
	pin_node = of_get_parent(controller);
	if (!pin_node)
		return -EINVAL;
	ret = of_property_read_u32(pin_node, "qcom,num-pins", &num_pins);
	of_node_put(pin_node);
	if (ret)
		return ret;
	if (!num_irqs || num_irqs != num_pins)
		return -EINVAL;
	parent_node = of_irq_find_parent(controller);
	if (of_device_is_compatible(parent_node, "qcom,mpm-gpio")) {
		parent_domain = irq_find_host(parent_node);
		of_node_put(parent_node);
		if (!parent_domain)
			return -ENXIO;
		domain = irq_domain_add_hierarchy(parent_domain, 0, num_irqs,
				controller, &msm_tlmm_gp_hierarchy_ops, ic);
	} else {
		of_node_put(parent_node);
		domain = irq_domain_add_linear(controller, num_irqs,
						ic->domain_ops, ic);
	}
	if (!domain)
		return -ENOMEM;
	ret = of_property_read_u32(controller, "apps_id", &apps_id);
	if (!ret) {
		pr_info("processor id specified, in device tree %d\n", apps_id);
		ic->apps_id = apps_id;
	}
	ic->num_irqs = num_irqs;
	ic->domain = domain;
	ic->irq_chip_extn = chip_extn;
	return 0;
}
#endif

static void msm_tlmm_gp_irq_cleanup(void *data)
{
	struct msm_tlmm_irq_chip *ic = data;

	/* The summary action was requested before these managed allocations. */
	disable_irq(ic->irq);
	unregister_syscore_ops(&msm_tlmm_irq_syscore_ops);
	ic->enabled_irqs = NULL;
	ic->dual_edge_irqs = NULL;
	ic->wake_irqs = NULL;
	ic->chip_base = NULL;
	ic->dev = NULL;
	ic->pinfo = NULL;
	ic->irq = 0;
}

static int msm_tlmm_gp_irq_init(int irq, struct msm_pintype_info *pinfo,
				struct device *tlmm_dev)
{
	int num_irqs;
	unsigned long *enabled, *dual_edge, *wake;
	struct msm_tlmm_irq_chip *ic = pinfo->irq_chip;

	if (!ic->domain)
		return -ENXIO;
	if (irq <= 0 || ic->num_irqs != pinfo->num_pins)
		return -EINVAL;
	if (ic->enabled_irqs)
		return -EBUSY;

	num_irqs = ic->num_irqs;
	enabled = devm_kzalloc(tlmm_dev, sizeof(unsigned long)
					* BITS_TO_LONGS(num_irqs), GFP_KERNEL);
	if (!enabled) {
		dev_err(tlmm_dev, "Unable to allocate enabled irqs bitmap\n");
		return -ENOMEM;
	}
	dual_edge = devm_kzalloc(tlmm_dev, sizeof(unsigned long)
					* BITS_TO_LONGS(num_irqs), GFP_KERNEL);
	if (!dual_edge) {
		dev_err(tlmm_dev, "Unable to allocate dual edge irqs bitmap\n");
		return -ENOMEM;
	}
	wake = devm_kzalloc(tlmm_dev, sizeof(unsigned long)
					* BITS_TO_LONGS(num_irqs), GFP_KERNEL);
	if (!wake) {
		dev_err(tlmm_dev, "Unable to allocate wake irqs bitmap\n");
		return -ENOMEM;
	}
	ic->enabled_irqs = enabled;
	ic->dual_edge_irqs = dual_edge;
	ic->wake_irqs = wake;
	spin_lock_init(&ic->irq_lock);
	ic->chip_base = pinfo->reg_base;
	ic->irq = irq;
	ic->dev = tlmm_dev;
	ic->pinfo = pinfo;
	register_syscore_ops(&msm_tlmm_irq_syscore_ops);
	return devm_add_action_or_reset(tlmm_dev, msm_tlmm_gp_irq_cleanup, ic);
}

static irqreturn_t msm_tlmm_handle_irq(int irq, void *data)
{
	int i, num_pintypes;
	struct msm_pintype_info *pintypes, *pintype;
	struct msm_tlmm_irq_chip *ic;
	struct msm_tlmm_desc *tlmm_desc = (struct msm_tlmm_desc *)data;
	irqreturn_t ret = IRQ_NONE;

	pintypes = tlmm_desc->pintypes;
	num_pintypes = tlmm_desc->num_pintypes;
	for (i = 0; i < num_pintypes; i++) {
		pintype = &pintypes[i];
		if (!pintype->irq_chip)
			continue;
		ic = pintype->irq_chip;
		if (!ic->node)
			continue;
		ret = ic->handler(irq, ic);
		if (ret != IRQ_HANDLED)
			break;
	}
	return ret;
}

static struct msm_pintype_info tlmm_pininfo[] = {
	{
		.prg_cfg = msm_tlmm_gp_cfg,
		.prg_func = msm_tlmm_gp_fn,
		.set_reg_base = msm_tlmm_set_reg_base,
		.gc = {
			.label		  = "msm_tlmm_gpio",
			.direction_input  = msm_tlmm_gp_dir_in,
			.direction_output = msm_tlmm_gp_dir_out,
			.get              = msm_tlmm_gp_get,
			.set              = msm_tlmm_gp_set,
			.to_irq           = msm_tlmm_gp_to_irq,
		},
		.init_irq = msm_tlmm_gp_irq_init,
		.irq_chip = &msm_tlmm_gp_irq,
		.name = "gp",
	},
	{
		.prg_cfg = msm_tlmm_sdc_cfg,
		.set_reg_base = msm_tlmm_set_reg_base,
		.name = "sdc",
	},
	{
		.prg_cfg = msm_tlmm_qdsd_cfg,
		.set_reg_base = msm_tlmm_set_reg_base,
		.name = "qdsd",
	},
	{
		.prg_cfg = msm_tlmm_ebi_cfg,
		.set_reg_base = msm_tlmm_set_reg_base,
		.name = "ebi",
	}
};

#define DECLARE_PINTYPE_DATA_GP(name, offset, regsize)	\
static const struct msm_pintype_data name = {		\
	.reg_base_offset = offset,			\
	.gp_reg_size = regsize,				\
}

#define DECLARE_PINTYPE_DATA_SDC(name, offset, offsets)	\
static const struct msm_pintype_data name = {		\
	.reg_base_offset = offset,			\
	.sdc_reg_offsets = offsets,			\
}

#define DECLARE_PINTYPE_DATA_QDSD(name, offset)		\
static const struct msm_pintype_data name = {		\
	.reg_base_offset = offset,			\
}

#define DECLARE_PINTYPE_DATA_EBI(name, offset, offsets)	\
static const struct msm_pintype_data name = {		\
	.reg_base_offset = offset,			\
	.ebi_reg_offsets = offsets,			\
}

#define ARG_PROTECT(...) __VA_ARGS__
DECLARE_PINTYPE_DATA_GP(gp_data_8974, 0x1000, 0x10);
DECLARE_PINTYPE_DATA_GP(gp_data_8916, 0x0, 0x1000);
DECLARE_PINTYPE_DATA_SDC(sdc_data_8974, 0x2044,
			 ARG_PROTECT({0, 0, 0, 0, 0x4, 0x4, 0x4}));
DECLARE_PINTYPE_DATA_SDC(sdc_data_8994, 0x2044,
		ARG_PROTECT({0, 0, 0, 0, 0x4, 0x4, 0x4, 0x28, 0x28, 0x28}));
DECLARE_PINTYPE_DATA_SDC(sdc_data_8916, 0x109000,
			 ARG_PROTECT({0x1000, 0x1000, 0x1000, 0x1000, 0, 0, 0})
			);
DECLARE_PINTYPE_DATA_QDSD(qdsd_data, 0x19C000);
DECLARE_PINTYPE_DATA_EBI(ebi_data, 0x10A000,
			 ARG_PROTECT({0x7000, 0x7000, 0x7000, 0x7000,
					 0x7000, 0x7000, 0}));
#undef ARG_PROTECT

static const struct msm_pintype_data *pintype_data_8974[MSM_PINTYPE_MAX] = {
	&gp_data_8974, &sdc_data_8974, &qdsd_data,
};

static const struct msm_pintype_data *pintype_data_8916[MSM_PINTYPE_MAX] = {
	&gp_data_8916, &sdc_data_8916, &qdsd_data, &ebi_data,
};

static const struct msm_pintype_data *pintype_data_8994[MSM_PINTYPE_MAX] = {
	&gp_data_8974, &sdc_data_8994, &qdsd_data,
};

static const struct of_device_id msm_tlmm_dt_match[] = {
	{ .compatible = "qcom,msm-tlmm-8994", .data = &pintype_data_8994, },
	{ .compatible = "qcom,msm-tlmm-8974", .data = &pintype_data_8974, },
	{ .compatible = "qcom,msm-tlmm-8916", .data = &pintype_data_8916, },
	{ },
};
MODULE_DEVICE_TABLE(of, msm_tlmm_dt_match);

int msm_tlmm_v4_set_gp_cfg(uint pin_no, uint id, bool level)
{
        unsigned int val, data, inout_val;
        u32 mask = 0, shft = 0;
	struct msm_pintype_info *pinfo = &tlmm_pininfo[0];
        void __iomem *inout_reg = NULL;
        void __iomem *cfg_reg = TLMM_GP_CFG(pinfo, pin_no);
	pr_info("[%s] pin_no = %d, id = %d, level = %d\n",
				__func__,pin_no,id,level);

        val = readl_relaxed(cfg_reg);
        /* Get mask and shft values for this config type */
        switch (id) {
        case PIN_CONFIG_BIAS_PULL_DOWN:
                mask = TLMM_GP_PULL_MASK;
                shft = TLMM_GP_PULL_SHFT;
                data = TLMM_PULL_DOWN;
                break;
        case PIN_CONFIG_BIAS_PULL_UP:
                mask = TLMM_GP_PULL_MASK;
                shft = TLMM_GP_PULL_SHFT;
                data = TLMM_PULL_UP;
                break;
	case PIN_CONFIG_BIAS_DISABLE:
                mask = TLMM_GP_PULL_MASK;
                shft = TLMM_GP_PULL_SHFT;
                data = TLMM_NO_PULL;
                break;
        case PIN_CONFIG_OUTPUT:
                mask = TLMM_GP_DIR_MASK;
                shft = TLMM_GP_DIR_SHFT;
                inout_reg = TLMM_GP_INOUT(pinfo, pin_no);
                data = level;
                inout_val = dir_to_inout_val(data);
                writel_relaxed(inout_val, inout_reg);
                data = mask;
                break;
        default:
                return -EINVAL;
        };

        val &= ~(mask << shft);
        val |= (data << shft);
		if(id == PIN_CONFIG_OUTPUT)
			val |= BIT(GPIO_OE_BIT);
		else
			val &= ~BIT(GPIO_OE_BIT);
        writel_relaxed(val, cfg_reg);
		return 0;
}

int msm_tlmm_get_gp_num(void)
{
        struct msm_pintype_info pinfo = tlmm_pininfo[0];

        return pinfo.num_pins;
}

void msm_tlmm_v4_get_gp_cfg(uint pin_no, struct gpiomux_setting *val)
{
        unsigned int cfg_val, inout_val;
        struct msm_pintype_info *pinfo = &tlmm_pininfo[0];

        void __iomem *cfg_reg = TLMM_GP_CFG(pinfo, pin_no);
        void __iomem *inout_reg = TLMM_GP_INOUT(pinfo, pin_no);

        cfg_val = readl_relaxed(cfg_reg);
        inout_val = readl_relaxed(inout_reg);

        val->pull = cfg_val & 0x3;
        val->func = (cfg_val >> 2) & 0xf;
        val->drv = (cfg_val >> 6) & 0x7;
        val->dir = cfg_val& BIT_MASK(9) ? 1 : GPIOMUX_IN;

        if ((val->func == GPIOMUX_FUNC_GPIO) && (val->dir))
                val->dir = inout_val & BIT_MASK(1) ?
                GPIOMUX_OUT_HIGH : GPIOMUX_OUT_LOW;

}

int msm_tlmm_v4_get_gp_value(uint pin_no)
{
        struct msm_pintype_info *pinfo = &tlmm_pininfo[0];

        void __iomem *inout_reg = TLMM_GP_INOUT(pinfo, pin_no);

        return readl_relaxed(inout_reg) & BIT(GPIO_OUT_BIT);
}


int msm_tlmm_v4_get_gp_input_value(uint pin_no)
{
        struct msm_pintype_info *pinfo = &tlmm_pininfo[0];

        void __iomem *inout_reg = TLMM_GP_INOUT(pinfo, pin_no);

        return readl_relaxed(inout_reg) & BIT(GPIO_IN_BIT);
}


#ifdef CONFIG_C5_PM_GPIO_DEBUG
#include "c5-gpio-debug.inc"
#endif

static int msm_tlmm_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	struct msm_tlmm_desc *tlmm_desc;
	int irq, ret;
	struct resource *res;
	int i;
	const struct msm_pintype_data **pintype_data;
	struct device_node *node = pdev->dev.of_node;

	match = of_match_node(msm_tlmm_dt_match, node);
	if (!match)
		return -ENODEV;
	tlmm_desc = devm_kzalloc(&pdev->dev, sizeof(*tlmm_desc), GFP_KERNEL);
	if (!tlmm_desc) {
		dev_err(&pdev->dev, "Alloction failed for tlmm desc\n");
		return -ENOMEM;
	}
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev, "cannot find IO resource\n");
		return -ENOENT;
	}
	tlmm_desc->base = devm_ioremap(&pdev->dev, res->start,
							resource_size(res));
	if (!tlmm_desc->base)
		return -ENOMEM;
	irq = platform_get_irq(pdev, 0);
	if (irq <= 0)
		return irq ? irq : -EINVAL;
	tlmm_desc->irq = irq;
	pintype_data = (const struct msm_pintype_data **)match->data;
	for (i = 0; i < MSM_PINTYPE_MAX; i++)
		tlmm_pininfo[i].pintype_data = pintype_data[i];
	tlmm_desc->pintypes = tlmm_pininfo;
	tlmm_desc->num_pintypes = ARRAY_SIZE(tlmm_pininfo);
	/* Request a disabled action: no callback before pin/bitmap setup. */
	irq_set_status_flags(irq, IRQ_NOAUTOEN);
	ret = devm_request_irq(&pdev->dev, irq, msm_tlmm_handle_irq,
			      IRQF_TRIGGER_HIGH, dev_name(&pdev->dev), tlmm_desc);
	irq_clear_status_flags(irq, IRQ_NOAUTOEN);
	if (ret)
		return ret;
	ret = msm_pinctrl_probe(pdev, tlmm_desc);
	if (ret)
		return ret;
	enable_irq(irq);
	return 0;
}

#ifdef CONFIG_C5_PM_GPIO_DEBUG
static int c5_gpio_debug_probe(struct platform_device *pdev)
{
	int rc;

	rc = c5_gpio_debug_claim(pdev);
	if (rc)
		return rc;
	rc = msm_tlmm_probe(pdev);
	if (rc)
		c5_gpio_debug_release(pdev);
	else
		c5_gpio_debug_publish(pdev);
	return rc;
}

static int c5_gpio_debug_remove(struct platform_device *pdev)
{
	c5_gpio_debug_release(pdev);
	return 0;
}
#endif

static struct platform_driver msm_tlmm_drv = {
#ifdef CONFIG_C5_PM_GPIO_DEBUG
	.probe		= c5_gpio_debug_probe,
	.remove		= c5_gpio_debug_remove,
#else
	.probe		= msm_tlmm_probe,
#endif
	.driver = {
		.name	= "msm-tlmm-pinctrl",
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(msm_tlmm_dt_match),
		.suppress_bind_attrs = true,
	},
};

static int __init msm_tlmm_drv_register(void)
{
	return platform_driver_register(&msm_tlmm_drv);
}
postcore_initcall(msm_tlmm_drv_register);

static void __exit msm_tlmm_drv_unregister(void)
{
	platform_driver_unregister(&msm_tlmm_drv);
}
module_exit(msm_tlmm_drv_unregister);

MODULE_LICENSE("GPL v2");
