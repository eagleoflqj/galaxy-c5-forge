/* SPDX-License-Identifier: GPL-2.0-only */
/* Failure-context reads only;do not acknowledge/clear any native status. */
static atomic_t c5_dsi_rx_capture_count = ATOMIC_INIT(0);

static void c5_dsi_rx_failure_state(struct mdss_dsi_ctrl_pdata *ctrl,
		int rx_byte, int read_count)
{
	static const u16 offsets[] = {
		0x0004, 0x0008, 0x000c, 0x003c,
		0x0068, 0x00ac, 0x00b4, 0x00c0,
		0x010c, 0x0110, 0x0120, 0x01d4,
	};
	u32 value[ARRAY_SIZE(offsets)];
	unsigned int i;

	if (!IS_ENABLED(CONFIG_C5_RECOVERY_ONLY) || !ctrl ||
		!ctrl->shared_data || !ctrl->ctrl_base ||
		ctrl->reg_size < 0x01d8 || ctrl->ndx != DSI_CTRL_0 ||
		ctrl->shared_data->hw_rev != MDSS_DSI_HW_REV_103_1 ||
		ctrl->shared_data->phy_rev != DSI_PHY_REV_10 ||
		!mdss_dsi_is_hw_config_single(ctrl->shared_data))
		return;
	if (!atomic_add_unless(&c5_dsi_rx_capture_count, 1, 3))
		return;

	/* Caller is the existing DMA-RX path under its native clock votes.
	 * These native status/control offsets are read elsewhere by this driver.
	 * Sequential snapshot,not atomic;ISR may already have cleared status.
	 */
	for (i = 0; i < ARRAY_SIZE(offsets); i++)
		value[i] = MIPI_INP(ctrl->ctrl_base + offsets[i]);

	pr_warn("C5DSIRX rx=%d read=%d;sequential status,not atomic\n",
		rx_byte, read_count);
	pr_warn("C5DSIRX 004=%08x 008=%08x 00c=%08x 03c=%08x\n",
		value[0], value[1], value[2], value[3]);
	pr_warn("C5DSIRX 068=%08x 0ac=%08x 0b4=%08x 0c0=%08x\n",
		value[4], value[5], value[6], value[7]);
	pr_warn("C5DSIRX 10c=%08x 110=%08x 120=%08x 1d4=%08x\n",
		value[8], value[9], value[10], value[11]);
}
