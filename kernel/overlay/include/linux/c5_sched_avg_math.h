/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_C5_SCHED_AVG_MATH_H
#define _LINUX_C5_SCHED_AVG_MATH_H

#include <linux/types.h>
#include <linux/math64.h>

/* The public outputs are signed 32-bit int on this target. */
#define C5_AVG_INT_MAX 2147483647U
#define C5_AVG_U64_MAX (~(u64)0)

static inline void c5_avg_sum(u64 *area, u64 add, bool *overflow)
{
	if (*overflow)
		return;
	if (add > C5_AVG_U64_MAX - *area) {
		*area = C5_AVG_U64_MAX;
		*overflow = true;
	} else {
		*area += add;
	}
}

static inline void c5_avg_product(u64 *area, u64 count, u64 elapsed,
				  bool *overflow)
{
	if (*overflow)
		return;
	/* The normal scheduler interval takes only the comparison fast path. */
	if (!(count <= C5_AVG_INT_MAX &&
	      elapsed <= C5_AVG_U64_MAX / C5_AVG_INT_MAX) &&
	    count && elapsed > div64_u64(C5_AVG_U64_MAX, count)) {
		*area = C5_AVG_U64_MAX;
		*overflow = true;
		return;
	}
	c5_avg_sum(area, count * elapsed, overflow);
}

/* Exact floor(remainder * 100 / denominator), without a wide multiply. */
static inline unsigned int c5_avg_fraction100(u64 remainder, u64 denominator)
{
	u64 partial = 0;
	unsigned int quotient = 0;
	int bit;

	for (bit = 6; bit >= 0; bit--) {
		quotient <<= 1;
		if (partial >= denominator - partial) {
			partial -= denominator - partial;
			quotient++;
		} else {
			partial += partial;
		}
		if ((100U >> bit) & 1U) {
			if (partial >= denominator - remainder) {
				partial -= denominator - remainder;
				quotient++;
			} else {
				partial += remainder;
			}
		}
	}
	return quotient;
}

static inline int c5_avg_percent(u64 area, u64 elapsed, bool overflow)
{
	u64 whole, remainder;
	unsigned int base, fraction;

	if (overflow || !elapsed)
		return C5_AVG_INT_MAX;
	whole = div64_u64(area, elapsed);
	if (whole > C5_AVG_INT_MAX / 100U)
		return C5_AVG_INT_MAX;
	remainder = area - whole * elapsed;
	base = (unsigned int)whole * 100U;
	fraction = c5_avg_fraction100(remainder, elapsed);
	if (fraction > C5_AVG_INT_MAX - base)
		return C5_AVG_INT_MAX;
	return base + fraction;
}

#endif
