/* SPDX-License-Identifier: GPL-2.0 */
/* Legacy C5 APSS statistics layout, isolated from the native CPU header. */
#ifndef _LINUX_C5_APSS_CORE_H
#define _LINUX_C5_APSS_CORE_H
#include <linux/types.h>
#include <linux/cpumask.h>
#define C5_APSS_DEBUG_MAX_PSTATES 20
struct cpu_pstate_pwr {
	unsigned int freq;
	uint32_t power;
};

struct cpu_pwr_stats {
	int cpu;
	long temp;
	bool throttling;
	struct cpu_pstate_pwr *ptable;
	int len;
};
struct cpu_pwr_stats *get_cpu_pwr_stats(void);
void trigger_cpu_pwr_stats_calc(void);
void set_cpu_throttled(struct cpumask *mask, bool throttling);
int c5_apss_debug_override(unsigned int cpu,
		const struct cpu_pstate_pwr *table, unsigned int len, bool enable);
int c5_apss_debug_snapshot(unsigned int cpu, struct cpu_pwr_stats *snapshot,
		struct cpu_pstate_pwr *table, unsigned int capacity);
#endif
