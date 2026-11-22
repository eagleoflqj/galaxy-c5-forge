/* SPDX-License-Identifier: GPL-2.0-only */
/* Original C5 MSS regulator contract from the accepted 3.10 driver. */
static inline bool c5_mss_legacy_power(const struct q6v5_data *drv)
{
	const char *model;
	struct device_node *node = drv->desc.dev->of_node;

	return IS_ENABLED(CONFIG_C5_RECOVERY_ONLY) && node &&
		!of_property_read_string(of_root, "model", &model) &&
		!strcmp(model, "Samsung C5LTE CHN OPEN PROJECT Rev09") &&
		of_device_is_compatible(node, "qcom,pil-q6v55-mss") &&
		!of_find_property(node, "vdd_mss-uV", NULL);
}

static inline int c5_mss_legacy_prepare(struct q6v5_data *drv)
{
	int ret;

	if (!c5_mss_legacy_power(drv) || !drv->vreg)
		return 0;
	/* Accepted VDD_MSS_UV and MAX_VDD_MSS_UV, not inferred DT values. */
	ret = regulator_set_voltage(drv->vreg, 1000000, 1150000);
	if (ret)
		return ret;
	ret = regulator_set_load(drv->vreg, 100000);
	return ret < 0 ? ret : 0;
}
