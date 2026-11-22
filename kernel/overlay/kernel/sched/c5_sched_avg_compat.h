/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _C5_SCHED_AVG_COMPAT_H
#define _C5_SCHED_AVG_COMPAT_H

#include <linux/c5_sched_avg_math.h>

/* Existing nr_lock protects each bank. Query lock serializes legacy readers.
 * Neither path writes the native collector's integrals, timestamps or maxima.
 */
struct c5_avg_bank {
	u64 area[3];
	u64 stamp;
	unsigned int overflow;
};

static DEFINE_PER_CPU(struct c5_avg_bank, c5_legacy_bank);
static DEFINE_RAW_SPINLOCK(c5_legacy_query_lock);
static u64 c5_legacy_get_time;

#ifdef CONFIG_C5_SCHED_ELIGIBLE_COMPAT
#include "c5_sched_eligible_compat.h"
#endif

static void c5_avg_legacy_account(int cpu, u64 now, u64 running,
				  u64 iowait, u64 big)
{
	struct c5_avg_bank *bank = &per_cpu(c5_legacy_bank, cpu);
	u64 values[3] = { running, iowait, big };
	u64 elapsed = now - bank->stamp;
	bool overflow;
	int i;

	if ((s64)elapsed < 0) {
		bank->overflow = 7;
		bank->stamp = now;
		return;
	}
	for (i = 0; i < 3; i++) {
		overflow = !!(bank->overflow & (1U << i));
		c5_avg_product(&bank->area[i], values[i], elapsed, &overflow);
		if (overflow)
			bank->overflow |= 1U << i;
	}
	bank->stamp = now;
}

void sched_get_nr_running_avg(int *avg, int *iowait_avg, int *big_avg)
{
	unsigned long query_flags, cpu_flags;
	u64 areas[3] = { 0, 0, 0 };
	u64 now, elapsed;
	unsigned int overflow = 0;
	bool bad;
	int cpu, i;
#ifdef CONFIG_C5_SCHED_AVG_TRACE_COMPAT
	int trace_avg, trace_iowait, trace_big;
#endif

	*avg = *iowait_avg = *big_avg = 0;
	raw_spin_lock_irqsave(&c5_legacy_query_lock, query_flags);
	now = sched_clock();
	if (now == c5_legacy_get_time) {
		raw_spin_unlock_irqrestore(&c5_legacy_query_lock, query_flags);
		return;
	}
	if ((s64)(now - c5_legacy_get_time) < 0)
		overflow = 7;
	for_each_possible_cpu(cpu) {
		struct c5_avg_bank *bank = &per_cpu(c5_legacy_bank, cpu);

		spin_lock_irqsave(&per_cpu(nr_lock, cpu), cpu_flags);
		now = sched_clock();
		c5_avg_legacy_account(cpu, now, per_cpu(nr, cpu),
				      nr_iowait_cpu(cpu),
#ifdef CONFIG_C5_SCHED_ELIGIBLE_COMPAT
				      c5_avg_legacy_eligible(cpu));
#else
				      nr_eligible_big_tasks(cpu));
#endif
		overflow |= bank->overflow;
		for (i = 0; i < 3; i++) {
			bad = !!(overflow & (1U << i));
			c5_avg_sum(&areas[i], bank->area[i], &bad);
			if (bad)
				overflow |= 1U << i;
			bank->area[i] = 0;
		}
		bank->overflow = 0;
		spin_unlock_irqrestore(&per_cpu(nr_lock, cpu), cpu_flags);
	}
	elapsed = now - c5_legacy_get_time;
	if ((s64)elapsed < 0)
		overflow = 7;
	c5_legacy_get_time = now;
	*avg = c5_avg_percent(areas[0], elapsed, !!(overflow & 1));
	*iowait_avg = c5_avg_percent(areas[1], elapsed, !!(overflow & 2));
	*big_avg = c5_avg_percent(areas[2], elapsed, !!(overflow & 4));
#ifdef CONFIG_C5_SCHED_AVG_TRACE_COMPAT
	/* Snapshot published outputs while serialized; never trace under the lock. */
	trace_avg = *avg;
	trace_iowait = *iowait_avg;
	trace_big = *big_avg;
#endif
	raw_spin_unlock_irqrestore(&c5_legacy_query_lock, query_flags);
#ifdef CONFIG_C5_SCHED_AVG_TRACE_COMPAT
	trace_sched_get_nr_running_avg(trace_avg, trace_big, trace_iowait);
#else
	/* Legacy trace formatting remains a separate, explicitly open contract. */
#endif
}
EXPORT_SYMBOL(sched_get_nr_running_avg);

#endif
