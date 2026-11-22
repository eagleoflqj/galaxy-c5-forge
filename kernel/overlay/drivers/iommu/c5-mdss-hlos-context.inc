/* SPDX-License-Identifier: GPL-2.0-only */
/* The accepted legacy ordinary-MDSS path restores firmware then programs CB23.
 * This is distinct from secure-buffer/secure-page-table ownership.
 */
static int arm_smmu_c5_mdss_hlos_prepare(struct iommu_domain *domain,
		struct arm_smmu_device *smmu, struct device *dev)
{
	struct arm_smmu_domain *sd = to_smmu_domain(domain);
	struct iommu_fwspec *fw;
	u32 fixed;
	int ret;

	if (!IS_ENABLED(CONFIG_C5_RECOVERY_ONLY) ||
		!arm_smmu_c5_context_scope(smmu) ||
		smmu->sec_id != TZ_DEVICE_APPS || sd->cfg.cbndx != 23)
		return 0;
	if (is_dynamic_domain(domain) || arm_smmu_has_secure_vmid(sd) ||
		sd->stage != ARM_SMMU_DOMAIN_S1 || smmu->pgshift != 12 ||
		smmu->num_context_banks <= 23 || smmu->num_context_banks > 32 ||
		smmu->phys_addr != 0x01e00000 || smmu->size != 0x40000 ||
		!dev || !dev->of_node ||
		!of_device_is_compatible(dev->of_node, "qcom,smmu_mdp_unsec") ||
		of_property_count_u32_elems(dev->of_node,
					"qcom,fixed-context-bank") != 1 ||
		of_property_read_u32(dev->of_node,
				"qcom,fixed-context-bank", &fixed) || fixed != 23)
		return -EINVAL;
	fw = dev->iommu_fwspec;
	if (!fw || fw->ops != &arm_smmu_ops || fw->num_ids != 1 ||
		fw->ids[0] != 0xc00 ||
		!test_bit(23, smmu->secure_context_map))
		return -EINVAL;
	if (sd->c5_mdss_hlos_owned)
		return test_bit(23, smmu->c5_hlos_context_map) ? 0 : -EINVAL;
	if (test_bit(23, smmu->c5_hlos_context_map))
		return -EBUSY;

	/* Firmware failure must never enable a HLOS context write. */
	ret = arm_smmu_restore_sec_cfg(smmu, 23);
	if (ret)
		return ret;
	set_bit(23, smmu->c5_hlos_context_map);
	sd->c5_mdss_hlos_owned = true;
	dev_warn(smmu->dev,
		"C5SMMU CB23_HLOS_ALLOW exact ordinary MDSS after firmware restore\n");
	return 0;
}

static void arm_smmu_c5_mdss_hlos_release(struct arm_smmu_domain *sd)
{
	if (!sd->c5_mdss_hlos_owned || !sd->smmu)
		return;
	/* Caller retains the allowance through native bank disable first. */
	clear_bit(23, sd->smmu->c5_hlos_context_map);
	sd->c5_mdss_hlos_owned = false;
}
