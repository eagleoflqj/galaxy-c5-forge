/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_CPUFREQ_LIMIT_H
#define _LINUX_CPUFREQ_LIMIT_H
#include <linux/types.h>
struct cpufreq_frequency_table;
struct cpufreq_limit_handle;

#ifdef CONFIG_CPU_FREQ_LIMIT
struct cpufreq_limit_handle *cpufreq_limit_get(unsigned long min_freq,
		unsigned long max_freq, char *label);
int cpufreq_limit_put(struct cpufreq_limit_handle *handle, int release);
void cpufreq_limit_set_table(int cpu, struct cpufreq_frequency_table *table);
ssize_t cpufreq_limit_get_table(char *buf);
static inline struct cpufreq_limit_handle *cpufreq_limit_min_freq(
		unsigned long min_freq, char *label)
{
	return cpufreq_limit_get(min_freq, 0, label);
}
static inline struct cpufreq_limit_handle *cpufreq_limit_max_freq(
		unsigned long max_freq, char *label)
{
	return cpufreq_limit_get(0, max_freq, label);
}
#endif

#ifdef CONFIG_CPU_FREQ_LIMIT_USERSPACE
enum {
	DVFS_NO_ID = 0,
	DVFS_TOUCH_ID = 0x00000001,
	DVFS_APPS_MIN_ID = 0x00000002,
	DVFS_APPS_MAX_ID = 0x00000004,
	DVFS_UNICPU_ID = 0x00000008,
	DVFS_LTETP_ID = 0x00000010,
	DVFS_CAMERA_ID = 0x00000012,
	DVFS_FINGER_ID = 0x00000014,
	DVFS_THERMALD_ID = 0x00000100,
	DVFS_MAX_ID,
};
int set_freq_limit(unsigned long id, unsigned int freq);
#endif
#endif
