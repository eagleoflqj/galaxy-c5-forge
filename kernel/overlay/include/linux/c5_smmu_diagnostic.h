/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_C5_SMMU_DIAGNOSTIC_H
#define _LINUX_C5_SMMU_DIAGNOSTIC_H
#include <linux/types.h>
struct iommu_domain;
#if defined(CONFIG_C5_RECOVERY_ONLY) && defined(CONFIG_ARM_SMMU)
void c5_smmu_dma_observe(struct iommu_domain *domain, dma_addr_t iova,
		phys_addr_t expected_phys);
#else
static inline void c5_smmu_dma_observe(struct iommu_domain *domain,
		dma_addr_t iova, phys_addr_t expected_phys)
{
	(void)domain; (void)iova; (void)expected_phys;
}
#endif
#endif
