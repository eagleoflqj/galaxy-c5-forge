/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * C5 builtin legacy valid-MMIO entrypoints on native ARM64 primitives.
 * The caller owns a valid native MMIO mapping. A high pointer alone does
 * not establish mapping validity or ownership. Legacy low cookies and
 * ioport_map/ioport_unmap are outside the owner-accepted contract.
 */
#define C5_IOMAP_PROVIDER
#include <linux/io.h>
#include <linux/msm_rtb.h>
#include <linux/c5-iomap-compat.h>

#if !defined(CONFIG_ARM64) || !defined(CONFIG_MMU) || \
	defined(CONFIG_MODULES) || defined(CONFIG_PCI) || \
	defined(CONFIG_GENERIC_IOMAP)
#error "C5 MMIO compatibility requires the reviewed builtin ARM64/MMU profile"
#endif

static bool c5_iomap_is_mmio(const void __iomem *addr)
{
	/* Reject the entire old reserved low-cookie range without bus access. */
	return (unsigned long)addr >= 0x40000UL;
}

unsigned int c5_legacy_ioread8(void __iomem *addr)
{
	uncached_logk_pc(LOGK_READL, __builtin_return_address(0), addr);
	return c5_iomap_is_mmio(addr) ? readb_no_log(addr) : 0xffU;
}

unsigned int c5_legacy_ioread16(void __iomem *addr)
{
	uncached_logk_pc(LOGK_READL, __builtin_return_address(0), addr);
	return c5_iomap_is_mmio(addr) ? readw_no_log(addr) : 0xffffU;
}

unsigned int c5_legacy_ioread16be(void __iomem *addr)
{
	uncached_logk_pc(LOGK_READL, __builtin_return_address(0), addr);
	return c5_iomap_is_mmio(addr) ? ioread16be(addr) : 0xffffU;
}

unsigned int c5_legacy_ioread32(void __iomem *addr)
{
	uncached_logk_pc(LOGK_READL, __builtin_return_address(0), addr);
	return c5_iomap_is_mmio(addr) ? readl_no_log(addr) : 0xffffffffU;
}

unsigned int c5_legacy_ioread32be(void __iomem *addr)
{
	uncached_logk_pc(LOGK_READL, __builtin_return_address(0), addr);
	return c5_iomap_is_mmio(addr) ? ioread32be(addr) : 0xffffffffU;
}

void c5_legacy_iowrite8(u8 value, void __iomem *addr)
{
	uncached_logk_pc(LOGK_WRITEL, __builtin_return_address(0), addr);
	if (c5_iomap_is_mmio(addr))
		writeb_no_log(value, addr);
}

void c5_legacy_iowrite16(u16 value, void __iomem *addr)
{
	uncached_logk_pc(LOGK_WRITEL, __builtin_return_address(0), addr);
	if (c5_iomap_is_mmio(addr))
		writew_no_log(value, addr);
}

void c5_legacy_iowrite16be(u16 value, void __iomem *addr)
{
	uncached_logk_pc(LOGK_WRITEL, __builtin_return_address(0), addr);
	if (c5_iomap_is_mmio(addr))
		iowrite16be(value, addr);
}

void c5_legacy_iowrite32(u32 value, void __iomem *addr)
{
	uncached_logk_pc(LOGK_WRITEL, __builtin_return_address(0), addr);
	if (c5_iomap_is_mmio(addr))
		writel_no_log(value, addr);
}

void c5_legacy_iowrite32be(u32 value, void __iomem *addr)
{
	uncached_logk_pc(LOGK_WRITEL, __builtin_return_address(0), addr);
	if (c5_iomap_is_mmio(addr))
		iowrite32be(value, addr);
}

/* Raw fixed-address repetitions: preserve byte order and the ulong ABI. */
#define C5_MMIO_READ_REP(bits, type, primitive) \
void c5_legacy_ioread##bits##_rep(void __iomem *addr, void *dst, \
				unsigned long count) \
{ \
	type *out = dst; \
	if (!c5_iomap_is_mmio(addr)) \
		return; \
	while (count--) \
		*out++ = primitive(addr); \
}

C5_MMIO_READ_REP(8, u8, __raw_readb)
C5_MMIO_READ_REP(16, u16, __raw_readw)
C5_MMIO_READ_REP(32, u32, __raw_readl)

#define C5_MMIO_WRITE_REP(bits, type, primitive) \
void c5_legacy_iowrite##bits##_rep(void __iomem *addr, const void *src, \
				unsigned long count) \
{ \
	const type *in = src; \
	if (!c5_iomap_is_mmio(addr)) \
		return; \
	while (count--) \
		primitive(*in++, addr); \
}

C5_MMIO_WRITE_REP(8, u8, __raw_writeb)
C5_MMIO_WRITE_REP(16, u16, __raw_writew)
C5_MMIO_WRITE_REP(32, u32, __raw_writel)

void c5_legacy_pci_iounmap(struct pci_dev *dev, void __iomem *addr)
{
	(void)dev;
	if (c5_iomap_is_mmio(addr))
		iounmap(addr);
}
