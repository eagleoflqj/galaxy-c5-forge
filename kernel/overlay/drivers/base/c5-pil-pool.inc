/* SPDX-License-Identifier: GPL-2.0-only */
/* Caller holds the same native pool spinlock as the ordinary allocator.
 * Both retained firmware MDTs require 1 MiB relocation alignment.  Count
 * pages exactly, retaining one head length so the order-only free ABI can
 * release the actual allocation rather than a power-of-two neighbour.
 */
static int c5_dma_exact_alloc(struct dma_coherent_mem *mem, ssize_t size)
{
	unsigned int pages;
	unsigned long pos;
	unsigned long mask;

	if (mem->size <= 0 || size <= 0 ||
	    (size_t)size > ((size_t)mem->size << PAGE_SHIFT))
		return -ENOMEM;
	/* Preserve natural order alignment below the firmware's 1 MiB cap. */
	mask = min_t(unsigned long, 1UL << get_order(size),
			C5_PIL_DMA_ALIGN_PAGES) - 1;
	pages = DIV_ROUND_UP(size, PAGE_SIZE);
	pos = bitmap_find_next_zero_area_off(mem->bitmap, mem->size, 0,
			pages, mask, mem->pfn_base & mask);
	if (pos >= (unsigned int)mem->size || pages > mem->size - pos)
		return -ENOMEM;
	bitmap_set(mem->bitmap, pos, pages);
	mem->c5_alloc_pages[pos] = pages;
	return pos;
}

static bool c5_dma_exact_release(struct dma_coherent_mem *mem,
				unsigned int page, int order)
{
	unsigned int pages;

	if (mem->size <= 0 || page >= (unsigned int)mem->size || order < 0)
		return false;
	pages = mem->c5_alloc_pages[page];
	if (!pages || pages > mem->size - page ||
	    get_order((size_t)pages << PAGE_SHIFT) != (unsigned int)order)
		return false;
	bitmap_clear(mem->bitmap, page, pages);
	mem->c5_alloc_pages[page] = 0;
	return true;
}
