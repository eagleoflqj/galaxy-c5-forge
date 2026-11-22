/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_C5_SCHED_INPUT_H
#define _LINUX_C5_SCHED_INPUT_H

#ifdef CONFIG_C5_SCHED_INPUT
struct cpumask;
struct sched_load;

int c5_sched_input_window(unsigned long period_us);
int c5_sched_input_load(struct sched_load *busy,
		const struct cpumask *query_cpus, unsigned long period_us);
#endif

#endif
