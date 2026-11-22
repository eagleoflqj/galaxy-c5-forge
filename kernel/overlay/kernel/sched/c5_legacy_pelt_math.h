/* SPDX-License-Identifier: GPL-2.0-only */
/* Legacy arithmetic and tables derived from Samsung c5lte kernel/sched/fair.c.
 * This is an unintegrated statistics component, not a scheduling policy.
 */
#ifndef _C5_LEGACY_PELT_MATH_H
#define _C5_LEGACY_PELT_MATH_H

#include <linux/compiler.h>
#include <linux/errno.h>
#include <linux/math64.h>
#include <linux/types.h>

#include <linux/c5_legacy_pelt_types.h>

#define C5_LEGACY_PELT_PERIOD 32
#define C5_LEGACY_PELT_MAX 47742
#define C5_LEGACY_PELT_MAX_N 345

static const u32 c5_legacy_pelt_yN_inv[] = {
	0xffffffff, 0xfa83b2da, 0xf5257d14, 0xefe4b99a, 0xeac0c6e6, 0xe5b906e6,
	0xe0ccdeeb, 0xdbfbb796, 0xd744fcc9, 0xd2a81d91, 0xce248c14, 0xc9b9bd85,
	0xc5672a10, 0xc12c4cc9, 0xbd08a39e, 0xb8fbaf46, 0xb504f333, 0xb123f581,
	0xad583ee9, 0xa9a15ab4, 0xa5fed6a9, 0xa2704302, 0x9ef5325f, 0x9b8d39b9,
	0x9837f050, 0x94f4efa8, 0x91c3d373, 0x8ea4398a, 0x8b95c1e3, 0x88980e80,
	0x85aac367, 0x82cd8698,
};

static const u32 c5_legacy_pelt_yN_sum[] = {
	    0, 1002, 1982, 2941, 3880, 4798, 5697, 6576, 7437, 8279, 9103,
	 9909,10698,11470,12226,12966,13690,14398,15091,15769,16433,17082,
	17718,18340,18949,19545,20128,20698,21256,21802,22336,22859,23371,
};

static __always_inline u64 c5_legacy_pelt_decay(u64 val, u64 n)
{
	unsigned int local_n;

	if (!n)
		return val;
	else if (unlikely(n > C5_LEGACY_PELT_PERIOD * 63))
		return 0;

	/* after bounds checking we can collapse to 32-bit */
	local_n = n;

	/*
	 * As y^PERIOD = 1/2, we can combine
	 *    y^n = 1/2^(n/PERIOD) * k^(n%PERIOD)
	 * With a look-up table which covers k^n (n<PERIOD)
	 *
	 * To achieve constant time c5_legacy_pelt_decay.
	 */
	if (unlikely(local_n >= C5_LEGACY_PELT_PERIOD)) {
		val >>= local_n / C5_LEGACY_PELT_PERIOD;
		local_n %= C5_LEGACY_PELT_PERIOD;
	}

	val *= c5_legacy_pelt_yN_inv[local_n];
	/* We don't use SRR here since we always want to round down. */
	return val >> 32;
}


static inline u32 c5_legacy_pelt_contrib(u64 n)
{
	u32 contrib = 0;

	if (likely(n <= C5_LEGACY_PELT_PERIOD))
		return c5_legacy_pelt_yN_sum[n];
	else if (unlikely(n >= C5_LEGACY_PELT_MAX_N))
		return C5_LEGACY_PELT_MAX;

	/* Compute \Sum k^n combining precomputed values for k^i, \Sum k^j */
	do {
		contrib /= 2; /* y^C5_LEGACY_PELT_PERIOD = 1/2 */
		contrib += c5_legacy_pelt_yN_sum[C5_LEGACY_PELT_PERIOD];

		n -= C5_LEGACY_PELT_PERIOD;
	} while (n > C5_LEGACY_PELT_PERIOD);

	contrib = c5_legacy_pelt_decay(contrib, n);
	return contrib + c5_legacy_pelt_yN_sum[n];
}


static inline void
c5_legacy_pelt_add_scaled(const struct c5_legacy_pelt_scale *scale, struct c5_legacy_pelt_state *sa, u64 delta)
{
	int cur_freq = scale->cur_freq;
	u64 scaled_delta;
	int sf;

	if (!scale->hmp_enabled)
		return;

	if (unlikely(cur_freq > scale->max_freq))
		cur_freq = scale->max_freq;

	scaled_delta = div64_u64(delta * cur_freq, scale->max_freq);
	sf = ((int)scale->efficiency * 1024) / scale->max_efficiency;
	scaled_delta *= sf;
	scaled_delta >>= 10;
	sa->runnable_avg_sum_scaled += scaled_delta;
}


static inline void c5_legacy_pelt_decay_scaled(const struct c5_legacy_pelt_scale *scale, struct c5_legacy_pelt_state *sa, u64 periods)
{
	if (!scale->hmp_enabled)
		return;

	sa->runnable_avg_sum_scaled =
		c5_legacy_pelt_decay(sa->runnable_avg_sum_scaled,
			   periods);
}


static __always_inline int c5_legacy_pelt_update_valid(const struct c5_legacy_pelt_scale *scale, u64 now,
							struct c5_legacy_pelt_state *sa,
							int runnable)
{
	u64 delta, periods;
	u32 runnable_contrib;
	int delta_w, decayed = 0;

	delta = now - sa->last_runnable_update;
	/*
	 * This should only happen when time goes backwards, which it
	 * unfortunately does during sched clock init when we swap over to TSC.
	 */
	if ((s64)delta < 0) {
		sa->last_runnable_update = now;
		return 0;
	}

	/*
	 * Use 1024ns as the unit of measurement since it's a reasonable
	 * approximation of 1us and fast to compute.
	 */
	delta >>= 10;
	if (!delta)
		return 0;
	sa->last_runnable_update = now;

	/* delta_w is the amount already accumulated against our next period */
	delta_w = sa->runnable_avg_period % 1024;
	if (delta + delta_w >= 1024) {
		/* period roll-over */
		decayed = 1;

		/*
		 * Now that we know we're crossing a period boundary, figure
		 * out how much from delta we need to complete the current
		 * period and accrue it.
		 */
		delta_w = 1024 - delta_w;
		if (runnable) {
			sa->runnable_avg_sum += delta_w;
			c5_legacy_pelt_add_scaled(scale, sa, delta_w);
		}
		sa->runnable_avg_period += delta_w;

		delta -= delta_w;

		/* Figure out how many additional periods this update spans */
		periods = delta / 1024;
		delta %= 1024;

		sa->runnable_avg_sum = c5_legacy_pelt_decay(sa->runnable_avg_sum,
						  periods + 1);
		c5_legacy_pelt_decay_scaled(scale, sa, periods + 1);
		sa->runnable_avg_period = c5_legacy_pelt_decay(sa->runnable_avg_period,
						     periods + 1);

		/* Efficiently calculate \sum (1..n_period) 1024*y^i */
		runnable_contrib = c5_legacy_pelt_contrib(periods);
		if (runnable) {
			sa->runnable_avg_sum += runnable_contrib;
			c5_legacy_pelt_add_scaled(scale, sa, runnable_contrib);
		}
		sa->runnable_avg_period += runnable_contrib;
	}

	/* Remainder of delta accrued against u_0` */
	if (runnable) {
		sa->runnable_avg_sum += delta;
		c5_legacy_pelt_add_scaled(scale, sa, delta);
	}
	sa->runnable_avg_period += delta;

	return decayed;
}


/* Same three sample assignments as the old HMP task initializer.
 * Clock ownership and migration state remain the caller's responsibility;
 * in particular this helper deliberately leaves last_runnable_update alone.
 */
static inline int c5_legacy_pelt_reset_samples(struct c5_legacy_pelt_state *sa,
                                               u32 initial_pelt)
{
    if (!sa)
        return -EINVAL;
    sa->runnable_avg_period = initial_pelt ? C5_LEGACY_PELT_MAX : 0;
    sa->runnable_avg_sum = initial_pelt;
    sa->runnable_avg_sum_scaled = initial_pelt;
    return 0;
}

/* Validate a scaling sample only where the reference consumes it. Sleeping,
 * backwards-clock and subunit updates still follow the old rules without
 * consulting unused scale values. Reject a consumed invalid sample before
 * changing state; signed-int limits preserve the reference C promotions.
 */
static inline int c5_legacy_pelt_update(struct c5_legacy_pelt_state *sa, u64 now,
                                       int runnable,
                                       const struct c5_legacy_pelt_scale *scale)
{
    u64 delta;

    if (!sa || !scale)
        return -EINVAL;
    delta = now - sa->last_runnable_update;
    if (scale->hmp_enabled && runnable && (s64)delta >= 0 && (delta >> 10) &&
        (!scale->max_freq || !scale->max_efficiency ||
         scale->cur_freq > 0x7fffffffU ||
         scale->efficiency > 0x7fffffffU / 1024U))
        return -EINVAL;
    return c5_legacy_pelt_update_valid(scale, now, sa, runnable);
}

#endif
