/* SPDX-License-Identifier: GPL-2.0-only */
/* Native WALT input for C5 interactive; no legacy window reset or policy alias. */
#include <linux/atomic.h>
#include <linux/cpumask.h>
#include <linux/errno.h>
#ifdef CONFIG_C5_SCHED_BUSY_API
#include <linux/hardirq.h>
#ifdef CONFIG_C5_SCHED_BUSY_QUERY_DIAG
#include <linux/mutex.h>
#endif
#endif
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/c5-sched-input.h>

#include "sched.h"
#include "walt.h"
#ifdef CONFIG_C5_SCHED_WINDOW_TRACE_COMPAT
#include <trace/events/sched.h>
#endif

static atomic64_t window_checks = ATOMIC64_INIT(0);
static atomic64_t window_rejected = ATOMIC64_INIT(0);
static atomic64_t sample_calls = ATOMIC64_INIT(0);
static atomic64_t sample_failed = ATOMIC64_INIT(0);
static atomic64_t native_load_calls = ATOMIC64_INIT(0);

static bool input_ready(unsigned long period_us)
{
	unsigned int ns = READ_ONCE(sched_ravg_window);

	/* Permit only the existing window, rounded to the governor's us unit. */
	return ns && period_us &&
		DIV_ROUND_CLOSEST_ULL(ns, NSEC_PER_USEC) == period_us &&
		!READ_ONCE(walt_disabled) &&
		READ_ONCE(sysctl_sched_use_walt_cpu_util) &&
		READ_ONCE(walt_cpu_util_freq_divisor);
}

int c5_sched_input_window(unsigned long period_us)
{
	atomic64_inc(&window_checks);
	if (!input_ready(period_us)) {
		atomic64_inc(&window_rejected);
		return -EOPNOTSUPP;
	}

	/* This checks the native period; it does not claim to align its phase. */
	return 0;
}

static unsigned long load_us(unsigned long util, unsigned long capacity,
			     unsigned long period_us)
{
	util = min(util, capacity);
	/* Validated native capacity <= 1024 and period <= 1,000,000 us. */
	return DIV_ROUND_UP_ULL((u64)util * period_us, capacity);
}

int c5_sched_input_load(struct sched_load *busy,
		const struct cpumask *query_cpus, unsigned long period_us)
{
	unsigned int i = 0, sampled = 0;
	unsigned long flags;
	int cpu;

	atomic64_inc(&sample_calls);
	if (!busy || !query_cpus || cpumask_empty(query_cpus) ||
	    !cpumask_subset(query_cpus, cpu_possible_mask) ||
	    !input_ready(period_us) || period_us > USEC_PER_SEC)
		goto failed;

	for_each_cpu(cpu, query_cpus) {
		struct sched_walt_cpu_load native = { 0 };
		struct rq *rq = cpu_rq(cpu);
		unsigned long capacity, util;
#ifdef CONFIG_C5_SCHED_WINDOW_TRACE_COMPAT
		u64 legacy_busy_us;
#endif

		busy[i].prev_load = 0;
		busy[i].new_task_load = 0;
		busy[i].predicted_load = 0;
		raw_spin_lock_irqsave(&rq->lock, flags);
		if (!cpu_online(cpu)) {
			raw_spin_unlock_irqrestore(&rq->lock, flags);
			i++;
			continue;
		}
		capacity = capacity_orig_of(cpu);
		if (!capacity || capacity > SCHED_CAPACITY_SCALE ||
		    !input_ready(period_us)) {
			raw_spin_unlock_irqrestore(&rq->lock, flags);
			goto failed;
		}
		/* Same native load provider and rq-lock context as schedutil. */
		util = cpu_util_freq_walt(cpu, &native);
		busy[i].prev_load = load_us(util, capacity, period_us);
		busy[i].new_task_load = load_us(native.nl, capacity, period_us);
		busy[i].predicted_load = load_us(native.pl, capacity, period_us);
#ifdef CONFIG_C5_SCHED_WINDOW_TRACE_COMPAT
		legacy_busy_us = busy[i].prev_load;
#endif
		raw_spin_unlock_irqrestore(&rq->lock, flags);
		atomic64_inc(&native_load_calls);
#ifdef CONFIG_C5_SCHED_WINDOW_TRACE_COMPAT
		/* Real completed per-CPU native sample, in accepted governor us units.
		 * Snapshot before unlock; this trace never changes WALT accounting. */
		trace_sched_get_busy(cpu, legacy_busy_us);
#endif
		sampled++;
		i++;
	}
	if (sampled)
		return 0;
failed:
	atomic64_inc(&sample_failed);
	return -EAGAIN;
}


#ifdef CONFIG_C5_SCHED_BUSY_API
static atomic64_t scalar_calls = ATOMIC64_INIT(0);
static atomic64_t scalar_completed = ATOMIC64_INIT(0);
static atomic64_t scalar_failed = ATOMIC64_INIT(0);

/*
 * Legacy scalar ABI, with the accepted native WALT numerical policy.
 * Timer/IRQ callers must hold no rq lock. No allocation or sleeping lock.
 * Zero is also the legacy ABI's unavailable result; counters distinguish it
 * from a successful zero-load sample. NMI and offline queries are rejected.
 */
unsigned long sched_get_busy(int cpu)
{
	struct sched_walt_cpu_load native = { 0 };
	unsigned long flags, period_us, capacity, util, busy;
	u64 wallclock;
	struct rq *rq;

	atomic64_inc(&scalar_calls);
	/* Validate before either a cpumask lookup or cpu_rq(). */
	if (in_nmi() || cpu < 0 || cpu >= nr_cpu_ids || !cpu_possible(cpu))
		goto failed;
	period_us = DIV_ROUND_CLOSEST_ULL(READ_ONCE(sched_ravg_window),
					NSEC_PER_USEC);
	if (!input_ready(period_us) || period_us > USEC_PER_SEC)
		goto failed;

	rq = cpu_rq(cpu);
	raw_spin_lock_irqsave(&rq->lock, flags);
	capacity = capacity_orig_of(cpu);
	if (!cpu_online(cpu) || !rq->curr || !rq->window_start ||
	    !capacity || capacity > SCHED_CAPACITY_SCALE ||
	    !input_ready(period_us))
		goto unlock_failed;
	/* Both independent accounting paths observe this query at one time.
	 * The returned load still uses the accepted native WALT policy. */
	wallclock = sched_ktime_clock();
#ifdef CONFIG_C5_SCHED_WINDOW_TRACE_COMPAT
	if (c5_legacy_window_native_update(rq->curr, rq, TASK_UPDATE,
					   wallclock, 0))
		goto unlock_failed;
#endif
	update_task_ravg(rq->curr, rq, TASK_UPDATE, wallclock, 0);
	if (!input_ready(period_us))
		goto unlock_failed;
	util = cpu_util_freq_walt(cpu, &native);
	busy = load_us(util, capacity, period_us);
	raw_spin_unlock_irqrestore(&rq->lock, flags);
	atomic64_inc(&scalar_completed);
#ifdef CONFIG_C5_SCHED_WINDOW_TRACE_COMPAT
	/* One completed scalar sample, after releasing its own rq lock. */
	trace_sched_get_busy(cpu, (u64)busy);
#endif
	return busy;

unlock_failed:
	raw_spin_unlock_irqrestore(&rq->lock, flags);
failed:
	atomic64_inc(&scalar_failed);
	return 0;
}


#ifdef CONFIG_C5_SCHED_BUSY_QUERY_DIAG
/* Deliberate active collection: first privileged read only, <=10 scalar
 * calls, no setting writes. Later reads return this boot's cached result. */
static DEFINE_MUTEX(busy_query_lock);
static bool busy_query_done;
static unsigned int busy_query_calls, busy_query_valid, busy_query_invalid;
static unsigned int busy_query_errors;
static int busy_query_max_load_rc;
static unsigned long busy_query_load[8];

static void busy_query_one(int cpu, bool valid, unsigned long period_us)
{
	u64 calls = (u64)atomic64_read(&scalar_calls);
	u64 done = (u64)atomic64_read(&scalar_completed);
	u64 failed = (u64)atomic64_read(&scalar_failed);
	unsigned long load = sched_get_busy(cpu);
	bool ok;

	busy_query_calls++;
	ok = (u64)atomic64_read(&scalar_calls) == calls + 1;
	if (valid) {
		busy_query_load[cpu] = load;
		ok = ok && load <= period_us &&
			(u64)atomic64_read(&scalar_completed) == done + 1 &&
			(u64)atomic64_read(&scalar_failed) == failed;
		if (ok)
			busy_query_valid++;
	} else {
		ok = ok && !load &&
			(u64)atomic64_read(&scalar_completed) == done &&
			(u64)atomic64_read(&scalar_failed) == failed + 1;
		if (ok)
			busy_query_invalid++;
	}
	if (!ok)
		busy_query_errors++;
}

static int busy_query_get(char *buffer, const struct kernel_param *unused)
{
	unsigned long period_us;
	int cpu, n;

	if (!mutex_trylock(&busy_query_lock))
		return -EBUSY;
	if (!busy_query_done) {
		period_us = DIV_ROUND_CLOSEST_ULL(READ_ONCE(sched_ravg_window),
						NSEC_PER_USEC);
		if (nr_cpu_ids < 1 || nr_cpu_ids > 8 ||
		    !input_ready(period_us) || period_us > USEC_PER_SEC) {
			mutex_unlock(&busy_query_lock);
			return -EAGAIN;
		}
		/* Consume before any query; no retry or background worker. */
		busy_query_done = true;
		for_each_possible_cpu(cpu) {
			if (cpu_online(cpu))
				busy_query_one(cpu, true, period_us);
		}
		busy_query_one(-1, false, period_us);
		busy_query_one(nr_cpu_ids, false, period_us);
		busy_query_max_load_rc =
			sched_update_freq_max_load(cpu_possible_mask);
		if (busy_query_max_load_rc != -EOPNOTSUPP)
			busy_query_errors++;
	}
	n = scnprintf(buffer, PAGE_SIZE,
		"C5_SCALAR_QUERY1\nDONE=%u\nQUERIES=%u\nVALID=%u\nINVALID=%u\n"
		"ERRORS=%u\nMAX_LOAD_RC=%d\n",
		busy_query_done, busy_query_calls, busy_query_valid,
		busy_query_invalid, busy_query_errors, busy_query_max_load_rc);
	for (cpu = 0; cpu < 8; cpu++)
		n += scnprintf(buffer + n, PAGE_SIZE - n,
			       "CPU%d_BUSY_US=%lu\n", cpu, busy_query_load[cpu]);
	n += scnprintf(buffer + n, PAGE_SIZE - n, "C5_SCALAR_QUERY_COMPLETE\n");
	mutex_unlock(&busy_query_lock);
	return n;
}

static const struct kernel_param_ops busy_query_ops = { .get = busy_query_get };
module_param_cb(c5_busy_query_once, &busy_query_ops, NULL, 0400);
#endif

static int busy_state_get(char *buffer, const struct kernel_param *unused)
{
	return scnprintf(buffer, PAGE_SIZE,
		"C5_SCHED_BUSY_API1\nCALLS=%llu\nCOMPLETED=%llu\nFAILED=%llu\n"
		"C5_SCHED_BUSY_API_COMPLETE\n",
		(unsigned long long)atomic64_read(&scalar_calls),
		(unsigned long long)atomic64_read(&scalar_completed),
		(unsigned long long)atomic64_read(&scalar_failed));
}

static const struct kernel_param_ops busy_state_ops = { .get = busy_state_get };
module_param_cb(c5_busy_state, &busy_state_ops, NULL, 0400);
#endif

static int state_get(char *buffer, const struct kernel_param *unused)
{
	return scnprintf(buffer, PAGE_SIZE,
		"C5_SCHED_INPUT1\nWINDOW_NS=%u\nWALT_DISABLED=%u\n"
		"WALT_CPU_UTIL=%u\nDIVISOR=%u\n"
		"WINDOW_CHECKS=%llu\nWINDOW_REJECTED=%llu\n"
		"SAMPLES=%llu\nSAMPLE_FAILED=%llu\nNATIVE_LOAD_CALLS=%llu\n"
		"C5_SCHED_INPUT_COMPLETE\n",
		READ_ONCE(sched_ravg_window), READ_ONCE(walt_disabled),
		READ_ONCE(sysctl_sched_use_walt_cpu_util),
		READ_ONCE(walt_cpu_util_freq_divisor),
		(unsigned long long)atomic64_read(&window_checks),
		(unsigned long long)atomic64_read(&window_rejected),
		(unsigned long long)atomic64_read(&sample_calls),
		(unsigned long long)atomic64_read(&sample_failed),
		(unsigned long long)atomic64_read(&native_load_calls));
}

static const struct kernel_param_ops state_ops = { .get = state_get };
module_param_cb(c5_state, &state_ops, NULL, 0400);
