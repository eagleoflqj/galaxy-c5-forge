/* SPDX-License-Identifier: GPL-2.0-only */
/* Capped outer CPUFreq observations only; no lock-internal clock MMIO or RAM
 * journal, no remote IPI, no rate/voltage/timeout change. */
#ifdef CONFIG_C5_RECOVERY_ONLY
#include <linux/atomic.h>
#include <linux/irqflags.h>
static atomic_t c5_dvfs_transition_logs = ATOMIC_INIT(0);

static bool c5_dvfs_log_begin(struct cpufreq_policy *policy,
			     unsigned int requested)
{
	if (irqs_disabled() || atomic_read(&c5_dvfs_transition_logs) >= 16)
		return false;
	if (atomic_inc_return(&c5_dvfs_transition_logs) > 16)
		return false;
	pr_info("C5DVFS transition begin cpu=%u old=%u requested=%u\n",
		policy->cpu, policy->cur, requested);
	return true;
}

static void c5_dvfs_log_rate(bool logged, unsigned int cpu,
			    unsigned long rate)
{
	if (logged && !irqs_disabled())
		pr_info("C5DVFS transition rounded cpu=%u Hz=%lu\n", cpu, rate);
}

static void c5_dvfs_log_end(bool logged, unsigned int cpu, int result)
{
	if (logged && !irqs_disabled())
		pr_info("C5DVFS transition returned cpu=%u result=%d\n", cpu, result);
}
#else
static bool c5_dvfs_log_begin(struct cpufreq_policy *policy,
			     unsigned int requested)
{
	(void)policy; (void)requested;
	return false;
}
static void c5_dvfs_log_rate(bool logged, unsigned int cpu,
			    unsigned long rate)
{
	(void)logged; (void)cpu; (void)rate;
}
static void c5_dvfs_log_end(bool logged, unsigned int cpu, int result)
{
	(void)logged; (void)cpu; (void)result;
}
#endif
