/* SPDX-License-Identifier: GPL-2.0-only */
/* Read-only powered nonsecure MDSS context observations, not ATOS or a fix. */
static atomic_t c5_dsi_map_capture_count = ATOMIC_INIT(0);

void c5_smmu_dma_observe(struct iommu_domain *domain, dma_addr_t iova,
		phys_addr_t expected_phys)
{
	static const u16 offsets[] = {
		0x0000, 0x0010, 0x0020, 0x0024, 0x0030, 0x0038, 0x0058,
	};
	struct arm_smmu_domain *sd;
	struct arm_smmu_device *smmu;
	struct arm_smmu_cfg *cfg;
	struct arm_smmu_cb *cb;
	void __iomem *base;
	phys_addr_t software;
	u32 value[ARRAY_SIZE(offsets)];
	unsigned int n;

	if (!IS_ENABLED(CONFIG_C5_RECOVERY_ONLY) || !domain ||
		domain->ops != &arm_smmu_ops)
		return;
	sd = to_smmu_domain(domain);
	smmu = sd->smmu;
	if (!smmu || !sd->pgtbl_ops ||
		READ_ONCE(sd->secure_pgtable_error) ||
		!arm_smmu_c5_context_scope(smmu) ||
		smmu->sec_id != TZ_DEVICE_APPS ||
		smmu->phys_addr != 0x01e00000 || smmu->size != 0x40000 ||
		smmu->pgshift != 12 || smmu->num_context_banks <= 23 ||
		smmu->num_context_banks > 32 || !smmu->cbs ||
		sd->stage != ARM_SMMU_DOMAIN_S1 || sd->cfg.cbndx != 23 ||
		test_bit(23, smmu->secure_context_map) || !sd->dev ||
		!sd->dev->of_node ||
		!of_device_is_compatible(sd->dev->of_node,
				"qcom,smmu_mdp_unsec") ||
		iova < 0x20000 || iova >= 0x40000000)
		return;
	cfg = &sd->cfg;
	cb = &smmu->cbs[23];
	if (cb->cfg != cfg ||
		!atomic_add_unless(&c5_dsi_map_capture_count, 1, 12))
		return;

	/* Caller is only the DSI DMA mapper after native attach/power-on.
	 * No arbitrary user address, global registers, secure CB, ATOS,
	 * fault acknowledgment, reset, map change or power vote is added.
	 */
	software = arm_smmu_iova_to_phys(domain, iova);
	base = ARM_SMMU_CB_BASE(smmu) + ARM_SMMU_CB(smmu, 23);
	for (n = 0; n < ARRAY_SIZE(offsets); n++)
		value[n] = readl_relaxed(base + offsets[n]);
	pr_warn("C5DSIMAP iova=%016llx expected=%016llx software=%016llx fmt=%d\n",
		(unsigned long long)iova, (unsigned long long)expected_phys,
		(unsigned long long)software, cfg->fmt);
	pr_warn("C5DSIMAP shadow ttbr=%016llx tcr=%08x tcr2=%08x skip_sctlr=%d skip_ttbr=%d\n",
		(unsigned long long)cb->ttbr[0], cb->tcr[0], cb->tcr[1],
		arm_smmu_skip_write(base), arm_smmu_skip_write(base + 0x20));
	pr_warn("C5DSIMAP live sctlr=%08x tcr2=%08x ttbr_lo=%08x ttbr_hi=%08x\n",
		value[0], value[1], value[2], value[3]);
	pr_warn("C5DSIMAP live tcr=%08x mair=%08x fsr=%08x;sequential powered snapshot\n",
		value[4], value[5], value[6]);
}
