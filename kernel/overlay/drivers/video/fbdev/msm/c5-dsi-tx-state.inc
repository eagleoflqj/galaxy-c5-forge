/* SPDX-License-Identifier: GPL-2.0-only */
/* Observe the native DMA boundary; never change transfer/error policy. */
static atomic_t c5_dsi_tx_capture_count = ATOMIC_INIT(0);
static atomic_t c5_dsi_irq_capture_count = ATOMIC_INIT(0);
static u32 c5_dsi_tx_last_isr;

static bool c5_dsi_tx_guard(struct mdss_dsi_ctrl_pdata *ctrl)
{
	return IS_ENABLED(CONFIG_C5_RECOVERY_ONLY) && ctrl &&
		ctrl->shared_data && ctrl->ctrl_base &&
		ctrl->reg_size >= 0x01d8 && ctrl->ndx == DSI_CTRL_0 &&
		ctrl->shared_data->hw_rev == MDSS_DSI_HW_REV_103_1 &&
		ctrl->shared_data->phy_rev == DSI_PHY_REV_10 &&
		mdss_dsi_is_hw_config_single(ctrl->shared_data);
}

static void c5_dsi_tx_irq_observe(struct mdss_dsi_ctrl_pdata *ctrl, u32 isr)
{
	/* Value already read by the native ISR, before its acknowledgment.
	 * No extra MMIO or printk in IRQ context; at most 64 RAM updates.
	 */
	if (c5_dsi_tx_guard(ctrl) &&
		atomic_add_unless(&c5_dsi_irq_capture_count, 1, 64))
		WRITE_ONCE(c5_dsi_tx_last_isr, isr);
}

static void c5_dsi_tx_registers(struct mdss_dsi_ctrl_pdata *ctrl,
		const char *phase)
{
	static const u16 offsets[] = {
		0x0004, 0x003c, 0x0048, 0x004c,
		0x000c, 0x00b4, 0x0120, 0x01d4,
	};
	u32 value[ARRAY_SIZE(offsets)];
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(offsets); i++)
		value[i] = MIPI_INP(ctrl->ctrl_base + offsets[i]);
	pr_warn("C5DSITX %s 004=%08x 03c=%08x 048=%08x 04c=%08x\n",
		phase, value[0], value[1], value[2], value[3]);
	pr_warn("C5DSITX %s 00c=%08x 0b4=%08x 120=%08x 1d4=%08x\n",
		phase, value[4], value[5], value[6], value[7]);
}

static bool c5_dsi_tx_begin(struct mdss_dsi_ctrl_pdata *ctrl,
		struct dsi_buf *tp)
{
	u32 header;

	if (!c5_dsi_tx_guard(ctrl) || !tp || !tp->start ||
		tp->data != tp->start || tp->len < 4 || tp->len > 4096 ||
		tp->size < tp->len ||
		!atomic_add_unless(&c5_dsi_tx_capture_count, 1, 12))
		return false;

	/* memcpy permits unaligned headers; no payload/calibration dump. */
	memcpy(&header, tp->data, sizeof(header));
	pr_warn("C5DSITX pre header=%08x len=%d phys=%016llx dma=%016llx mapped=%d\n",
		header, tp->len, (unsigned long long)tp->dmap,
		(unsigned long long)ctrl->dma_addr, ctrl->dmap_iommu_map);
	c5_dsi_tx_registers(ctrl, "pre");
	return true;
}

static void c5_dsi_tx_end(struct mdss_dsi_ctrl_pdata *ctrl, bool traced,
		int native_result)
{
	if (!traced || !c5_dsi_tx_guard(ctrl))
		return;
	pr_warn("C5DSITX post result=%d last_isr=%08x;not atomic or unique IRQ correlation\n",
		native_result, READ_ONCE(c5_dsi_tx_last_isr));
	c5_dsi_tx_registers(ctrl, "post");
}
