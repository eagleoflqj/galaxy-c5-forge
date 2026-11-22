/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_C5_IOMAP_COMPAT_H
#define _LINUX_C5_IOMAP_COMPAT_H

/* Builtin ARM64 C5 legacy valid-MMIO ABI; no port-cookie constructors. */
struct pci_dev;

#define C5_IOMAP_DECLARE(result, name, args) \
	result c5_legacy_##name args __asm__(#name)

C5_IOMAP_DECLARE(unsigned int, ioread8, (void __iomem *addr));
C5_IOMAP_DECLARE(unsigned int, ioread16, (void __iomem *addr));
C5_IOMAP_DECLARE(unsigned int, ioread16be, (void __iomem *addr));
C5_IOMAP_DECLARE(unsigned int, ioread32, (void __iomem *addr));
C5_IOMAP_DECLARE(unsigned int, ioread32be, (void __iomem *addr));
C5_IOMAP_DECLARE(void, iowrite8, (u8 value, void __iomem *addr));
C5_IOMAP_DECLARE(void, iowrite16, (u16 value, void __iomem *addr));
C5_IOMAP_DECLARE(void, iowrite16be, (u16 value, void __iomem *addr));
C5_IOMAP_DECLARE(void, iowrite32, (u32 value, void __iomem *addr));
C5_IOMAP_DECLARE(void, iowrite32be, (u32 value, void __iomem *addr));
C5_IOMAP_DECLARE(void, ioread8_rep,
		(void __iomem *addr, void *dst, unsigned long count));
C5_IOMAP_DECLARE(void, ioread16_rep,
		(void __iomem *addr, void *dst, unsigned long count));
C5_IOMAP_DECLARE(void, ioread32_rep,
		(void __iomem *addr, void *dst, unsigned long count));
C5_IOMAP_DECLARE(void, iowrite8_rep,
		(void __iomem *addr, const void *src, unsigned long count));
C5_IOMAP_DECLARE(void, iowrite16_rep,
		(void __iomem *addr, const void *src, unsigned long count));
C5_IOMAP_DECLARE(void, iowrite32_rep,
		(void __iomem *addr, const void *src, unsigned long count));
C5_IOMAP_DECLARE(void, pci_iounmap,
		(struct pci_dev *dev, void __iomem *addr));

#undef C5_IOMAP_DECLARE

/* Native inlines have already been parsed. Keep the native 64-bit names. */
#ifndef C5_IOMAP_PROVIDER
#undef ioread8
#define ioread8 c5_legacy_ioread8
#undef ioread16
#define ioread16 c5_legacy_ioread16
#undef ioread16be
#define ioread16be c5_legacy_ioread16be
#undef ioread32
#define ioread32 c5_legacy_ioread32
#undef ioread32be
#define ioread32be c5_legacy_ioread32be
#undef iowrite8
#define iowrite8 c5_legacy_iowrite8
#undef iowrite16
#define iowrite16 c5_legacy_iowrite16
#undef iowrite16be
#define iowrite16be c5_legacy_iowrite16be
#undef iowrite32
#define iowrite32 c5_legacy_iowrite32
#undef iowrite32be
#define iowrite32be c5_legacy_iowrite32be
#undef ioread8_rep
#define ioread8_rep c5_legacy_ioread8_rep
#undef ioread16_rep
#define ioread16_rep c5_legacy_ioread16_rep
#undef ioread32_rep
#define ioread32_rep c5_legacy_ioread32_rep
#undef iowrite8_rep
#define iowrite8_rep c5_legacy_iowrite8_rep
#undef iowrite16_rep
#define iowrite16_rep c5_legacy_iowrite16_rep
#undef iowrite32_rep
#define iowrite32_rep c5_legacy_iowrite32_rep
#undef pci_iounmap
#define pci_iounmap c5_legacy_pci_iounmap
#endif /* !C5_IOMAP_PROVIDER */

#endif /* _LINUX_C5_IOMAP_COMPAT_H */
