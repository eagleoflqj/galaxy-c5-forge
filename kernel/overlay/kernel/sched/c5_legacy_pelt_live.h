/* SPDX-License-Identifier: GPL-2.0-only */
/* Unselected live legacy arithmetic: resample actual CPU inputs at each
 * old scaled-contribution point. A caller still owes full task/rq lifecycle
 * integration and truthful handling of rejected input before trace emission.
 */
#ifndef _C5_LEGACY_PELT_LIVE_H
#define _C5_LEGACY_PELT_LIVE_H
#include "c5_legacy_pelt_backend.h"

static __always_inline int c5_legacy_pelt_live_valid(int cpu, bool enabled,
                    u64 now, struct c5_legacy_pelt_state *sa, int runnable)
{
	u64 delta, periods;
	u32 runnable_contrib;
	int delta_w, decayed = 0;
	int err;
	const struct c5_legacy_pelt_scale mode = {0, 0, 0, 0, enabled};

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
			err = c5_legacy_pelt_add_cpu(cpu, enabled, sa, delta_w);
			if (err < 0)
				return err;
		}
		sa->runnable_avg_period += delta_w;

		delta -= delta_w;

		/* Figure out how many additional periods this update spans */
		periods = delta / 1024;
		delta %= 1024;

		sa->runnable_avg_sum = c5_legacy_pelt_decay(sa->runnable_avg_sum,
						  periods + 1);
		c5_legacy_pelt_decay_scaled(&mode, sa, periods + 1);
		sa->runnable_avg_period = c5_legacy_pelt_decay(sa->runnable_avg_period,
						     periods + 1);

		/* Efficiently calculate \sum (1..n_period) 1024*y^i */
		runnable_contrib = c5_legacy_pelt_contrib(periods);
		if (runnable) {
			sa->runnable_avg_sum += runnable_contrib;
			err = c5_legacy_pelt_add_cpu(cpu, enabled, sa, runnable_contrib);
			if (err < 0)
				return err;
		}
		sa->runnable_avg_period += runnable_contrib;
	}

	/* Remainder of delta accrued against u_0` */
	if (runnable) {
		sa->runnable_avg_sum += delta;
		err = c5_legacy_pelt_add_cpu(cpu, enabled, sa, delta);
		if (err < 0)
			return err;
	}
	sa->runnable_avg_period += delta;

	return decayed;
}

static inline int c5_legacy_pelt_update_live(struct c5_legacy_pelt_state *sa,
                      u64 now, int runnable, int cpu, bool enabled)
{
    struct c5_legacy_pelt_state next;
    int ret;

    if (!sa)
        return -EINVAL;
    next = *sa;
    ret = c5_legacy_pelt_live_valid(cpu, enabled, now, &next, runnable);
    if (ret >= 0)
        *sa = next;
    return ret;
}
#endif
