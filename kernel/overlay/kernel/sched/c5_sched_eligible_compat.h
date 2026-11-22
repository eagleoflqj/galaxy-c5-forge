/* SPDX-License-Identifier: GPL-2.0-only */
/* Included by sched_avg.c after nr_lock is defined. All class snapshots use
 * the existing per-CPU nr_lock; the publisher already owns the primary rq.
 * Readers never take an rq lock. Publication does not sample a clock, flush
 * an integral or touch the native WALT classifier/controller.
 */
#ifndef _C5_SCHED_ELIGIBLE_COMPAT_H
#define _C5_SCHED_ELIGIBLE_COMPAT_H
#include <linux/moduleparam.h>

struct c5_avg_eligible_snapshot {
	int big, small, error;
	bool maximum;
	u64 generation, refusals;
};

/* Old init_cluster and global max_possible_capacity both start at 1024. */
static DEFINE_PER_CPU(struct c5_avg_eligible_snapshot, c5_avg_eligible) = {
	.maximum = true,
};

int c5_sched_avg_publish_eligible(int cpu, int big, int small,
				 bool maximum, u64 generation)
{
	struct c5_avg_eligible_snapshot *snapshot;
	unsigned long flags;

	if (cpu < 0 || cpu >= nr_cpu_ids || !cpu_possible(cpu) ||
	    big < 0 || small < 0)
		return -EINVAL;
	lockdep_assert_held(&cpu_rq(cpu)->lock);
	spin_lock_irqsave(&per_cpu(nr_lock, cpu), flags);
	snapshot = &per_cpu(c5_avg_eligible, cpu);
	snapshot->big = big;
	snapshot->small = small;
	snapshot->maximum = maximum;
	snapshot->generation = generation;
	spin_unlock_irqrestore(&per_cpu(nr_lock, cpu), flags);
	return 0;
}

static unsigned int c5_avg_eligible_locked(int cpu, bool *bad)
{
	struct c5_avg_eligible_snapshot *snapshot = &per_cpu(c5_avg_eligible, cpu);
	unsigned int running;
	int big, error;

	lockdep_assert_held(&per_cpu(nr_lock, cpu));
	error = snapshot->error;
	if (!error)
		error = c5_legacy_window_eligible_error(cpu);
	if (error)
		goto refuse;
	if (!snapshot->maximum)
		return snapshot->big;
	/* Preserve the old pre-delta rq phase, not the collector's new nr.
	 * The old signed subtraction is defined for the positive int domain;
	 * quarantine an impossible/corrupt larger rq count instead of UB.
	 * A remote reader does not own the rq, as in the old implementation.
	 */
	running = READ_ONCE(cpu_rq(cpu)->nr_running);
	if (running > 0x7fffffffU) {
		snapshot->error = -EOVERFLOW;
		goto refuse;
	}
	big = (int)running - snapshot->small;
	if (big < 0)
		big = 0;
	return big;
refuse:
	if (snapshot->refusals != (u64)-1)
		snapshot->refusals++;
	*bad = true;
	return 0;
}

unsigned int nr_eligible_big_tasks(int cpu)
{
	unsigned long flags;
	unsigned int big;
	bool bad = false;

	if (cpu < 0 || cpu >= nr_cpu_ids || !cpu_possible(cpu))
		return 0;
	spin_lock_irqsave(&per_cpu(nr_lock, cpu), flags);
	big = c5_avg_eligible_locked(cpu, &bad);
	spin_unlock_irqrestore(&per_cpu(nr_lock, cpu), flags);
	/* The unsigned legacy ABI has no errno channel. A source fault has an
	 * explicit cached ERROR/refusal witness; zero is never accepted as a
	 * successful restored statistic when that witness is nonzero.
	 */
	return big;
}

static unsigned int c5_avg_legacy_eligible(int cpu)
{
	bool bad = false;
	unsigned int big = c5_avg_eligible_locked(cpu, &bad);

	if (bad)
		per_cpu(c5_legacy_bank, cpu).overflow |= 4;
	return big;
}

static int c5_avg_eligible_state_get(char *buffer,
				   const struct kernel_param *parameter)
{
	unsigned long flags;
	int cpu, length = 0;

	(void)parameter;
	length += scnprintf(buffer + length, PAGE_SIZE - length,
			    "C5_AVG_ELIGIBLE1\n");
	for_each_possible_cpu(cpu) {
		struct c5_avg_eligible_snapshot snapshot;
		int error;

		spin_lock_irqsave(&per_cpu(nr_lock, cpu), flags);
		snapshot = per_cpu(c5_avg_eligible, cpu);
		error = snapshot.error ? snapshot.error :
			c5_legacy_window_eligible_error(cpu);
		spin_unlock_irqrestore(&per_cpu(nr_lock, cpu), flags);
		length += scnprintf(buffer + length, PAGE_SIZE - length,
			"CPU=%d BIG=%d SMALL=%d MAX=%u GEN=%llu ERROR=%d REFUSALS=%llu\n",
			cpu, snapshot.big, snapshot.small, snapshot.maximum,
			(unsigned long long)snapshot.generation, error,
			(unsigned long long)snapshot.refusals);
	}
	length += scnprintf(buffer + length, PAGE_SIZE - length,
			    "C5_AVG_ELIGIBLE_COMPLETE\n");
	return length;
}

static const struct kernel_param_ops c5_avg_eligible_state_ops = {
	.get = c5_avg_eligible_state_get,
};
module_param_cb(c5_eligibility_state, &c5_avg_eligible_state_ops, NULL, 0400);
#endif
