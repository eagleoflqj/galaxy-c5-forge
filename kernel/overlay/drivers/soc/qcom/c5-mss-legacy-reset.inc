/* SPDX-License-Identifier: GPL-2.0-only */
/* C5's accepted MSS reset order, with the native combined MBA/DP buffer. */
static int c5_mss_legacy_reset(struct pil_desc *pil)
{
	struct q6v5_data *drv = container_of(pil, struct q6v5_data, desc);
	phys_addr_t start_addr = pil_get_entry_addr(pil);
	int ret;

	if (drv->mba_dp_phys)
		start_addr = drv->mba_dp_phys;
	ret = pil_mss_power_up(drv);
	if (ret)
		goto err_power;

	/* Original C5: release subsystem reset before enabling its clocks. */
	ret = pil_mss_restart_reg(drv, 0);
	if (ret)
		goto err_restart;
	ret = pil_mss_enable_clks(drv);
	if (ret)
		goto err_clks;

	if (modem_dbg_cfg)
		writel_relaxed(modem_dbg_cfg, drv->reg_base + QDSP6SS_DBG_CFG);
	if (drv->self_auth) {
		writel_relaxed(start_addr, drv->rmb_base + RMB_MBA_IMAGE);
		mb();
	} else {
		writel_relaxed((start_addr >> 4) & 0x0FFFFFF0,
				drv->reg_base + QDSP6SS_RST_EVB);
	}
	/* Keep the native combined allocation and its exact DP placement. */
	if (drv->dp_size) {
		writel_relaxed(start_addr + SZ_1M,
				drv->rmb_base + RMB_PMI_CODE_START);
		writel_relaxed(drv->dp_size,
				drv->rmb_base + RMB_PMI_CODE_LENGTH);
	} else {
		writel_relaxed(0, drv->rmb_base + RMB_PMI_CODE_START);
		writel_relaxed(0, drv->rmb_base + RMB_PMI_CODE_LENGTH);
	}
	mb();
	dev_info(pil->dev, "C5 MSS legacy reset: clocks ready\n");
	ret = pil_q6v5_reset(pil);
	if (ret)
		goto err_q6v5_reset;
	if (drv->self_auth) {
		ret = pil_msa_wait_for_mba_ready(drv);
		if (ret)
			goto err_q6v5_reset;
	}
	dev_info(pil->dev, "MBA boot done (C5 legacy reset)\n");
	drv->is_booted = true;
	return 0;

err_q6v5_reset:
	modem_log_rmb_regs(drv->rmb_base);
	pil_mss_disable_clks(drv);
	if (drv->ahb_clk_vote)
		clk_disable_unprepare(drv->ahb_clk);
err_clks:
	pil_mss_restart_reg(drv, 1);
err_restart:
	pil_mss_power_down(drv);
err_power:
	dev_err(pil->dev, "C5 MSS legacy reset failed(rc:%d)\n", ret);
	return ret;
}
