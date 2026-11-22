/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef C5_VIDC_LAYOUT_H
#define C5_VIDC_LAYOUT_H

/* Frozen C5 Venus nonsecure policy, not a union of secure/nonsecure ranges. */
#define C5_VIDC_DATA_START 0x5dc00000UL
#define C5_VIDC_DATA_SIZE  0x7f000000UL
#define C5_VIDC_QUEUE_START 0xdcc00000UL
#define C5_VIDC_QUEUE_SIZE  0x01000000UL
#define C5_VIDC_DATA_MASK  0x7ffU
#define C5_VIDC_QUEUE_MASK 0x800U
#define C5_VIDC_PAGE_SIZE 4096UL

/* No dependency on kernel allocation or hardware: also compiled in HOST tests. */
static inline int c5_vidc_pool_for(unsigned int type, int secure)
{
	if (secure)
		return -EOPNOTSUPP;
	/* A HAL buffer type is one bit, not a mask of different allocation roles. */
	if (!type || (type & (type - 1)))
		return -EINVAL;
	if (type & C5_VIDC_DATA_MASK)
		return 0;
	if (type == C5_VIDC_QUEUE_MASK)
		return 1;
	return -EINVAL;
}

static inline int c5_vidc_extent_valid(unsigned long start, unsigned long size,
	unsigned long pool_start, unsigned long pool_size)
{
	return size && !(start & (C5_VIDC_PAGE_SIZE - 1)) &&
		!(size & (C5_VIDC_PAGE_SIZE - 1)) && start >= pool_start &&
		size <= pool_size && start - pool_start <= pool_size - size;
}

static inline int c5_vidc_align_valid(unsigned long align)
{
	return align >= C5_VIDC_PAGE_SIZE && align <= 0x100000UL &&
		!(align & (align - 1));
}

#endif
