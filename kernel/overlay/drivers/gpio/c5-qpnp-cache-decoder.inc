/* Exact namespaced native pure cache checks; no new use of existing GPIO helpers. */
static bool c5_qpnp_is_gpio_lv_mv(struct qpnp_pin_spec *q_spec)
{
	if ((q_spec->type == Q_GPIO_TYPE) &&
		(q_spec->subtype == Q_GPIO_SUBTYPE_GPIO_LV ||
		q_spec->subtype == Q_GPIO_SUBTYPE_GPIO_MV))
		return true;

	return false;
}
static int c5_qpnp_check_config(enum qpnp_pin_param_type idx,
				 struct qpnp_pin_spec *q_spec, uint32_t val)
{
	u8 subtype = q_spec->subtype;

	switch (idx) {
	case Q_PIN_CFG_MODE:
		if (q_spec->type == Q_GPIO_TYPE) {
			if (c5_qpnp_is_gpio_lv_mv(q_spec)) {
				if (val >= QPNP_PIN_GPIO_LV_MV_MODE_INVALID)
					return -EINVAL;
			} else if (val >= QPNP_PIN_GPIO_MODE_INVALID) {
				return -EINVAL;
			}
		} else if (q_spec->type == Q_MPP_TYPE) {
			if (val >= QPNP_PIN_MPP_MODE_INVALID)
				return -EINVAL;
			if ((subtype == Q_MPP_SUBTYPE_ULT_4CH_NO_ANA_OUT ||
			     subtype == Q_MPP_SUBTYPE_ULT_4CH_NO_SINK) &&
			     (val == QPNP_PIN_MODE_BIDIR))
				return -ENXIO;
		}
		break;
	case Q_PIN_CFG_OUTPUT_TYPE:
		if (q_spec->type != Q_GPIO_TYPE)
			return -ENXIO;
		if ((val == QPNP_PIN_OUT_BUF_OPEN_DRAIN_NMOS ||
		    val == QPNP_PIN_OUT_BUF_OPEN_DRAIN_PMOS) &&
		    (subtype == Q_GPIO_SUBTYPE_GPIOC_4CH ||
		    (subtype == Q_GPIO_SUBTYPE_GPIOC_8CH)))
			return -EINVAL;
		else if (c5_qpnp_is_gpio_lv_mv(q_spec) &&
			val >= QPNP_PIN_GPIO_LV_MV_OUT_BUF_INVALID)
			return -EINVAL;
		else if (val >= QPNP_PIN_OUT_BUF_INVALID)
			return -EINVAL;
		break;
	case Q_PIN_CFG_INVERT:
		if (val >= QPNP_PIN_INVERT_INVALID)
			return -EINVAL;
		break;
	case Q_PIN_CFG_PULL:
		if (q_spec->type == Q_GPIO_TYPE &&
		    val >= QPNP_PIN_GPIO_PULL_INVALID)
			return -EINVAL;
		if (q_spec->type == Q_MPP_TYPE) {
			if (val >= QPNP_PIN_MPP_PULL_INVALID)
				return -EINVAL;
			if (subtype == Q_MPP_SUBTYPE_ULT_4CH_NO_ANA_OUT ||
			    subtype == Q_MPP_SUBTYPE_ULT_4CH_NO_SINK)
				return -ENXIO;
		}
		break;
	case Q_PIN_CFG_VIN_SEL:
		if (c5_qpnp_is_gpio_lv_mv(q_spec)) {
			if (subtype == Q_GPIO_SUBTYPE_GPIO_LV) {
				if (val >= QPNP_PIN_GPIO_LV_VIN_INVALID)
					return -EINVAL;
			} else {
				if (val >= QPNP_PIN_GPIO_MV_VIN_INVALID)
					return -EINVAL;
			}
		} else if (val >= QPNP_PIN_VIN_8CH_INVALID) {
			return -EINVAL;
		} else if (val >= QPNP_PIN_VIN_4CH_INVALID) {
			if (q_spec->type == Q_GPIO_TYPE &&
			   (subtype == Q_GPIO_SUBTYPE_GPIO_4CH ||
			    subtype == Q_GPIO_SUBTYPE_GPIOC_4CH))
				return -EINVAL;
			if (q_spec->type == Q_MPP_TYPE &&
			   (subtype == Q_MPP_SUBTYPE_4CH_NO_ANA_OUT ||
			    subtype == Q_MPP_SUBTYPE_4CH_NO_SINK ||
			    subtype == Q_MPP_SUBTYPE_4CH_FULL_FUNC ||
			    subtype == Q_MPP_SUBTYPE_ULT_4CH_NO_ANA_OUT ||
			    subtype == Q_MPP_SUBTYPE_ULT_4CH_NO_SINK))
				return -EINVAL;
		}
		break;
	case Q_PIN_CFG_OUT_STRENGTH:
		if (q_spec->type != Q_GPIO_TYPE)
			return -ENXIO;
		if (val >= QPNP_PIN_OUT_STRENGTH_INVALID ||
		    val == 0)
			return -EINVAL;
		break;
	case Q_PIN_CFG_SRC_SEL:
		if (q_spec->type == Q_MPP_TYPE &&
		    (val == QPNP_PIN_SEL_FUNC_1 ||
		     val == QPNP_PIN_SEL_FUNC_2))
			return -EINVAL;
		if (c5_qpnp_is_gpio_lv_mv(q_spec)) {
			if (val >= QPNP_PIN_GPIO_LV_MV_SRC_INVALID)
				return -EINVAL;
		} else if (val >= QPNP_PIN_SRC_INVALID) {
			return -EINVAL;
		}
		break;
	case Q_PIN_CFG_MASTER_EN:
		if (val >= QPNP_PIN_MASTER_INVALID)
			return -EINVAL;
		break;
	case Q_PIN_CFG_AOUT_REF:
		if (q_spec->type != Q_MPP_TYPE)
			return -ENXIO;
		if (subtype == Q_MPP_SUBTYPE_4CH_NO_ANA_OUT ||
		    subtype == Q_MPP_SUBTYPE_ULT_4CH_NO_ANA_OUT)
			return -ENXIO;
		if (val >= QPNP_PIN_AOUT_REF_INVALID)
			return -EINVAL;
		break;
	case Q_PIN_CFG_AIN_ROUTE:
		if (q_spec->type != Q_MPP_TYPE)
			return -ENXIO;
		if (val >= QPNP_PIN_AIN_ROUTE_INVALID)
			return -EINVAL;
		break;
	case Q_PIN_CFG_CS_OUT:
		if (q_spec->type != Q_MPP_TYPE)
			return -ENXIO;
		if (subtype == Q_MPP_SUBTYPE_4CH_NO_SINK ||
		    subtype == Q_MPP_SUBTYPE_ULT_4CH_NO_SINK)
			return -ENXIO;
		if (val >= QPNP_PIN_CS_OUT_INVALID)
			return -EINVAL;
		break;
	case Q_PIN_CFG_APASS_SEL:
		if (!c5_qpnp_is_gpio_lv_mv(q_spec))
			return -ENXIO;
		if (val >= QPNP_PIN_APASS_SEL_INVALID)
			return -EINVAL;
		break;
	case Q_PIN_CFG_DTEST_SEL:
		if (val > QPNP_PIN_DTEST_SEL_INVALID)
			return -EINVAL;
		break;
	default:
		pr_err("invalid param type %u specified\n", idx);
		return -EINVAL;
	}
	return 0;
}
static inline u8 c5_qpnp_reg_get(u8 *reg, int shift, int mask)
{
	return (*reg & mask) >> shift;
}
/* SPDX-License-Identifier: GPL-2.0-only */
/* Selected 4.9 register decoder; literal legacy display tables. */
struct c5_qpnp_reg { u32 addr, idx, shift, mask; };
static int c5_qpnp_reg_attr(enum qpnp_pin_param_type type,
			     struct c5_qpnp_reg *cfg,
			struct qpnp_pin_spec *q_spec)
{
	switch (type) {
	case Q_PIN_CFG_MODE:
		if (c5_qpnp_is_gpio_lv_mv(q_spec)) {
			cfg->shift = Q_REG_LV_MV_MODE_SEL_SHIFT;
			cfg->mask = Q_REG_LV_MV_MODE_SEL_MASK;
		} else {
			cfg->shift = Q_REG_MODE_SEL_SHIFT;
			cfg->mask = Q_REG_MODE_SEL_MASK;
		}
		cfg->addr = Q_REG_MODE_CTL;
		cfg->idx = Q_REG_I_MODE_CTL;
		break;
	case Q_PIN_CFG_OUTPUT_TYPE:
		cfg->addr = Q_REG_DIG_OUT_CTL;
		cfg->idx = Q_REG_I_DIG_OUT_CTL;
		cfg->shift = Q_REG_OUT_TYPE_SHIFT;
		cfg->mask = Q_REG_OUT_TYPE_MASK;
		break;
	case Q_PIN_CFG_INVERT:
		if (c5_qpnp_is_gpio_lv_mv(q_spec)) {
			cfg->addr = Q_REG_DIG_OUT_SRC_CTL;
			cfg->idx = Q_REG_I_DIG_OUT_SRC_CTL;
			cfg->shift = Q_REG_DIG_OUT_SRC_INVERT_SHIFT;
			cfg->mask = Q_REG_DIG_OUT_SRC_INVERT_MASK;
		} else {
			cfg->addr = Q_REG_MODE_CTL;
			cfg->idx = Q_REG_I_MODE_CTL;
			cfg->shift = Q_REG_OUT_INVERT_SHIFT;
			cfg->mask = Q_REG_OUT_INVERT_MASK;
		}
		break;
	case Q_PIN_CFG_PULL:
		cfg->addr = Q_REG_DIG_PULL_CTL;
		cfg->idx = Q_REG_I_DIG_PULL_CTL;
		cfg->shift = Q_REG_PULL_SHIFT;
		cfg->mask = Q_REG_PULL_MASK;
		break;
	case Q_PIN_CFG_VIN_SEL:
		cfg->addr = Q_REG_DIG_VIN_CTL;
		cfg->idx = Q_REG_I_DIG_VIN_CTL;
		cfg->shift = Q_REG_VIN_SHIFT;
		cfg->mask = Q_REG_VIN_MASK;
		break;
	case Q_PIN_CFG_OUT_STRENGTH:
		cfg->addr = Q_REG_DIG_OUT_CTL;
		cfg->idx = Q_REG_I_DIG_OUT_CTL;
		cfg->shift = Q_REG_OUT_STRENGTH_SHIFT;
		cfg->mask = Q_REG_OUT_STRENGTH_MASK;
		break;
	case Q_PIN_CFG_SRC_SEL:
		if (c5_qpnp_is_gpio_lv_mv(q_spec)) {
			cfg->addr = Q_REG_DIG_OUT_SRC_CTL;
			cfg->idx = Q_REG_I_DIG_OUT_SRC_CTL;
			cfg->shift = Q_REG_DIG_OUT_SRC_SRC_SEL_SHIFT;
			cfg->mask = Q_REG_DIG_OUT_SRC_SRC_SEL_MASK;
		} else {
			cfg->addr = Q_REG_MODE_CTL;
			cfg->idx = Q_REG_I_MODE_CTL;
			cfg->shift = Q_REG_SRC_SEL_SHIFT;
			cfg->mask = Q_REG_SRC_SEL_MASK;
		}
		break;
	case Q_PIN_CFG_MASTER_EN:
		cfg->addr = Q_REG_EN_CTL;
		cfg->idx = Q_REG_I_EN_CTL;
		cfg->shift = Q_REG_MASTER_EN_SHIFT;
		cfg->mask = Q_REG_MASTER_EN_MASK;
		break;
	case Q_PIN_CFG_AOUT_REF:
		cfg->addr = Q_REG_AOUT_CTL;
		cfg->idx = Q_REG_I_AOUT_CTL;
		cfg->shift = Q_REG_AOUT_REF_SHIFT;
		cfg->mask = Q_REG_AOUT_REF_MASK;
		break;
	case Q_PIN_CFG_AIN_ROUTE:
		cfg->addr = Q_REG_AIN_CTL;
		cfg->idx = Q_REG_I_AIN_CTL;
		cfg->shift = Q_REG_AIN_ROUTE_SHIFT;
		cfg->mask = Q_REG_AIN_ROUTE_MASK;
		break;
	case Q_PIN_CFG_CS_OUT:
		cfg->addr = Q_REG_SINK_CTL;
		cfg->idx = Q_REG_I_SINK_CTL;
		cfg->shift = Q_REG_CS_OUT_SHIFT;
		cfg->mask = Q_REG_CS_OUT_MASK;
		break;
	case Q_PIN_CFG_APASS_SEL:
		cfg->addr = Q_REG_APASS_SEL_CTL;
		cfg->idx = Q_REG_I_APASS_SEL_CTL;
		cfg->shift = Q_REG_APASS_SEL_SHIFT;
		cfg->mask = Q_REG_APASS_SEL_MASK;
		break;
	case Q_PIN_CFG_DTEST_SEL:
		if (c5_qpnp_is_gpio_lv_mv(q_spec)) {
			cfg->shift = Q_REG_LV_MV_DTEST_SEL_CFG_SHIFT;
			cfg->mask = Q_REG_LV_MV_DTEST_SEL_CFG_MASK;
		} else {
			cfg->shift = Q_REG_DTEST_SEL_SHIFT;
			cfg->mask = Q_REG_DTEST_SEL_MASK;
		}
		cfg->addr = Q_REG_DIG_IN_CTL;
		cfg->idx = Q_REG_I_DIG_IN_CTL;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}
static const char * const qpnp_mode_str[] = {
        "D_IN",
        "D_OUT",
        "D_INOUT",
        "D_BI",
        "A_IN",
        "A_OUT",
        "C_SINK",
        "NA",
};
static const char * const qpnp_output_type_str[] = {
        "CMOS",
        "nMOS",
        "pMOS",
        "NA",
};
static const char * const qpnp_invert_str[] = {
        "NORMAL",
        "INVERT",
};
static const char * const qpnp_pull_str[] = {
        "P_UP30",
        "P_UP15",
        "P_UP31",
        "P_UPMAX",
        "P_DOWN",
        "NO_PULL",
        "NA",
        "NA",
};
static const char * const qpnp_vin_sel_str[] = {
        "VIN0",
        "VIN1",
        "VIN2",
        "VIN3",
        "VIN4",
        "VIN5",
        "VIN6",
        "VIN7",
};
static const char * const qpnp_out_strength_str[] = {
        "NA",
        "STR_LOW",
        "STR_MID",
        "STR_HIGH",
};
static const char * const qpnp_src_sel_str[] = {
        "GND",
        "PAIRED",
        "FUNC1",
        "FUNC2",
        "DTEST1",
        "DTEST2",
        "DTEST3",
        "DTEST4",
};
static const char * const qpnp_master_en_str[] = {
        "HIGH-Z",
        "GPIO",
};
static const char * const qpnp_aout_ref_str[] = {
        "VREF1V25",
        "NA", "NA", "NA", "NA", "NA", "NA", "NA",
};
static const char * const qpnp_ain_route_str[] = {
        "HKADC5",
        "HKADC6",
        "HKADC7",
        "HKADC8",
        "NA", "NA", "NA", "NA",
};
static const char * const qpnp_cs_out_str[] = {
        "SINK5",
        "SINK10",
        "SINK15",
        "SINK20",
        "SINK25",
        "SINK30",
        "SINK35",
        "SINK40",
};

