/* Copyright (c) 2014-2017, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/atomic.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/msm-core-interface.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pm_opp.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/slab.h>
#include <linux/suspend.h>
#include <linux/thermal.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/uio_driver.h>
#include <asm/smp_plat.h>
#include <linux/c5-thermal-sensor.h>
#include <linux/c5-apss-core.h>
#define CREATE_TRACE_POINTS
#include <trace/events/trace_msm_core.h>

#define TEMP_BASE_POINT 35
#define TEMP_MAX_POINT 95
#define CPU_HOTPLUG_LIMIT 80
#define CPU_BIT_MASK(cpu) BIT(cpu)
#define DEFAULT_TEMP 40
#define DEFAULT_LOW_HYST_TEMP 10
#define DEFAULT_HIGH_HYST_TEMP 5
#define CLUSTER_OFFSET_FOR_MPIDR 8
#define MAX_CORES_PER_CLUSTER 4
#define MAX_NUM_OF_CLUSTERS 2
#define NUM_OF_CORNERS 10

#define ALLOCATE_2D_ARRAY(type)\
static type **allocate_2d_array_##type(int idx)\
{\
	int i;\
	type **ptr = NULL;\
	if (!idx) \
		return ERR_PTR(-EINVAL);\
	ptr = kzalloc(sizeof(*ptr) * TEMP_DATA_POINTS, \
				GFP_KERNEL);\
	if (!ptr) { \
		return ERR_PTR(-ENOMEM); \
	} \
	for (i = 0; i < TEMP_DATA_POINTS; i++) { \
		ptr[i] = kzalloc(sizeof(*ptr[i]) * \
					idx, GFP_KERNEL);\
		if (!ptr[i]) {\
			goto done;\
		} \
	} \
	return ptr;\
done:\
	for (i = 0; i < TEMP_DATA_POINTS; i++) \
		kfree(ptr[i]);\
	kfree(ptr);\
	return ERR_PTR(-ENOMEM);\
}

struct cpu_activity_info {
	int cpu;
	int mpidr;
	long temp;
	int sensor_id;
	struct sensor_threshold hi_threshold;
	struct sensor_threshold low_threshold;
	struct cpu_static_info *sp;
};

struct cpu_static_info {
	uint32_t **power;
	cpumask_t mask;
	struct cpufreq_frequency_table *table;
	uint32_t *voltage;
	uint32_t num_of_freqs;
};

static DEFINE_MUTEX(policy_update_mutex);
static DEFINE_MUTEX(kthread_update_mutex);
static DEFINE_SPINLOCK(update_lock);
static struct delayed_work sampling_work;
static DECLARE_WAIT_QUEUE_HEAD(sampling_waitq);
static atomic_t sampling_pending = ATOMIC_INIT(0);
/* Protected with kthread_update_mutex once sampling is active. */
static bool sampling_rearm[NR_CPUS];
static struct task_struct *sampling_task;
static int low_hyst_temp;
static int high_hyst_temp;
static struct platform_device *msm_core_pdev;
static struct cpu_activity_info activity[NR_CPUS];
DEFINE_PER_CPU(struct cpu_pstate_pwr *, ptable);
static struct cpu_pwr_stats cpu_stats[NR_CPUS];
ALLOCATE_2D_ARRAY(uint32_t);

static int poll_ms;
module_param_named(polling_interval, poll_ms, int,
		S_IRUGO | S_IWUSR | S_IWGRP);

static int disabled;
module_param_named(disabled, disabled, int,
		S_IRUGO | S_IWUSR | S_IWGRP);
static bool in_suspend;
static bool activate_power_table;
/* Serialized by policy_update_mutex; never publish a partial provider to ioctl. */
static bool c5_apss_ready;
static DEFINE_MUTEX(c5_apss_probe_mutex);
static bool c5_apss_work_initialized, c5_apss_uio_registered;
static bool c5_apss_misc_registered, c5_apss_freq_registered;
static bool c5_apss_pm_registered;

struct c5_apss_debug_state {
	bool enabled;
	unsigned int len;
	struct cpu_pstate_pwr entries[C5_APSS_DEBUG_MAX_PSTATES + 1];
};

static struct c5_apss_debug_state c5_apss_debug_state[NR_CPUS];
static unsigned int c5_apss_canonical_len[NR_CPUS];

/* All canonical/override publication is serialized by update_lock. */
static void c5_apss_refresh_locked(unsigned int cpu)
{
	struct c5_apss_debug_state *state = &c5_apss_debug_state[cpu];

	cpu_stats[cpu].ptable = state->enabled ? state->entries : per_cpu(ptable, cpu);
	cpu_stats[cpu].len = state->enabled ? state->len : c5_apss_canonical_len[cpu];
}

int c5_apss_debug_override(unsigned int cpu,
		const struct cpu_pstate_pwr *table, unsigned int len, bool enable)
{
	struct c5_apss_debug_state *state;
	unsigned long flags;

	if (cpu >= nr_cpu_ids || !cpu_possible(cpu) ||
	    (enable && (!table || !len || len > C5_APSS_DEBUG_MAX_PSTATES)))
		return -EINVAL;
	spin_lock_irqsave(&update_lock, flags);
	state = &c5_apss_debug_state[cpu];
	memset(state->entries, 0, sizeof(state->entries));
	if (enable)
		memcpy(state->entries, table, sizeof(*table) * len);
	state->len = enable ? len : 0;
	state->enabled = enable;
	c5_apss_refresh_locked(cpu);
	spin_unlock_irqrestore(&update_lock, flags);
	return 0;
}

int c5_apss_debug_snapshot(unsigned int cpu, struct cpu_pwr_stats *snapshot,
		struct cpu_pstate_pwr *table, unsigned int capacity)
{
	unsigned long flags;
	int len, ret = 0;

	if (cpu >= nr_cpu_ids || !cpu_possible(cpu) || !snapshot)
		return -EINVAL;
	spin_lock_irqsave(&update_lock, flags);
	len = cpu_stats[cpu].len;
	if (len < 0 || (unsigned int)len > capacity) {
		ret = -EOVERFLOW;
		goto unlock;
	}
	if (len && (!table || !cpu_stats[cpu].ptable)) {
		ret = -EIO;
		goto unlock;
	}
	*snapshot = cpu_stats[cpu];
	if (len)
		memcpy(table, cpu_stats[cpu].ptable, sizeof(*table) * len);
	snapshot->ptable = len ? table : NULL;
 unlock:
	spin_unlock_irqrestore(&update_lock, flags);
	return ret;
}


static void c5_apss_request_sample(void)
{
	if (!READ_ONCE(c5_apss_ready))
		return;
	atomic_set(&sampling_pending, 1);
	wake_up_interruptible(&sampling_waitq);
}

/*
 * Cannot be called from an interrupt context
 */
static int set_and_activate_threshold(uint32_t sensor_id,
		struct sensor_threshold *threshold)
{
	int ret = sensor_set_trip(sensor_id, threshold);

	if (ret)
		return ret;
	ret = sensor_activate_trip(sensor_id, threshold, true);
	if (ret)
		sensor_cancel_trip(sensor_id, threshold);
	return ret;
}

static int set_threshold(struct cpu_activity_info *node)
{
	int ret;

	if (node->sensor_id < 0)
		return 0;
	ret = sensor_activate_trip(node->sensor_id, &node->hi_threshold, false);
	if (ret && ret != -ENODEV)
		return ret;
	ret = sensor_activate_trip(node->sensor_id, &node->low_threshold, false);
	if (ret && ret != -ENODEV)
		return ret;
	node->hi_threshold.temp = node->temp + high_hyst_temp;
	node->low_threshold.temp = node->temp - low_hyst_temp;
	if (node->hi_threshold.temp < CPU_HOTPLUG_LIMIT) {
		ret = set_and_activate_threshold(node->sensor_id, &node->hi_threshold);
		if (ret)
			return ret;
	}
	ret = set_and_activate_threshold(node->sensor_id, &node->low_threshold);
	if (ret)
		sensor_cancel_trip(node->sensor_id, &node->hi_threshold);
	return ret;
}

static void samplequeue_handle(struct work_struct *work)
{
	c5_apss_request_sample();
}

/* May be called from an interrupt context */
static int core_temp_notify(enum thermal_trip_type type, int temp, void *data)
{
	struct cpu_activity_info *node = data;
	unsigned long flags;
	long previous;

	if (!node || !READ_ONCE(c5_apss_ready))
		return 0;
	spin_lock_irqsave(&update_lock, flags);
	previous = node->temp;
	node->temp = temp;
	spin_unlock_irqrestore(&update_lock, flags);
	trace_temp_notification(node->sensor_id, type, temp, previous);
	c5_apss_request_sample();
	return 0;
}

static void repopulate_stats(int cpu)
{
	int i;
	struct cpu_activity_info *cpu_node = &activity[cpu];
	int temp_point;
	struct cpu_pstate_pwr *pt =  per_cpu(ptable, cpu);

	if (!pt)
		return;

	if (cpu_node->temp < TEMP_BASE_POINT)
		temp_point = 0;
	else if (cpu_node->temp > TEMP_MAX_POINT)
		temp_point = TEMP_DATA_POINTS - 1;
	else
		temp_point = (cpu_node->temp - TEMP_BASE_POINT) / 5;

	cpu_stats[cpu].temp = cpu_node->temp;
	for (i = 0; i < cpu_node->sp->num_of_freqs; i++)
		pt[i].power = cpu_node->sp->power[temp_point][i];

	trace_cpu_stats(cpu, cpu_stats[cpu].temp, pt[0].power,
			pt[cpu_node->sp->num_of_freqs-1].power);
};

void trigger_cpu_pwr_stats_calc(void)
{
	static long prev_temp[NR_CPUS];
	struct cpu_activity_info *node;
	unsigned long flags;
	long before, sample;
	int cpu;

	if (READ_ONCE(disabled))
		return;
	mutex_lock(&policy_update_mutex);
	if (!c5_apss_ready)
		goto unlock;
	for_each_online_cpu(cpu) {
		node = &activity[cpu];
		if (node->sensor_id < 0)
			continue;
		before = READ_ONCE(node->temp);
		sample = before;
		if (before == prev_temp[cpu] &&
		    sensor_get_temp(node->sensor_id, &sample))
			continue;
		spin_lock_irqsave(&update_lock, flags);
		/* Do not overwrite a callback update received during the sleeping read. */
		if (node->temp == before)
			node->temp = sample;
		prev_temp[cpu] = node->temp;
		if (activate_power_table && per_cpu(ptable, cpu) && node->sp &&
		    node->sp->table && node->sp->power)
			repopulate_stats(cpu);
		spin_unlock_irqrestore(&update_lock, flags);
	}
 unlock:
	mutex_unlock(&policy_update_mutex);
}
EXPORT_SYMBOL(trigger_cpu_pwr_stats_calc);

void set_cpu_throttled(cpumask_t *mask, bool throttling)
{
	unsigned long flags;
	int cpu;

	if (!mask)
		return;

	spin_lock_irqsave(&update_lock, flags);
	for_each_cpu(cpu, mask)
		cpu_stats[cpu].throttling = throttling;
	spin_unlock_irqrestore(&update_lock, flags);
}
EXPORT_SYMBOL(set_cpu_throttled);

static void update_related_freq_table(struct cpufreq_policy *policy)
{
	int cpu, num_of_freqs;
	struct cpufreq_frequency_table *table;

	table = policy->freq_table;
	if (!table) {
		pr_err("Couldn't get freq table for cpu%d\n",
				policy->cpu);
		return;
	}

	for (num_of_freqs = 0; table[num_of_freqs].frequency !=
			CPUFREQ_TABLE_END;)
		num_of_freqs++;

	/*
	 * Synchronous cores within cluster have the same
	 * policy. Since these cores do not have the cpufreq
	 * table initialized for all of them, copy the same
	 * table to all the related cpus.
	 */
	for_each_cpu(cpu, policy->related_cpus) {
		activity[cpu].sp->table = table;
		activity[cpu].sp->num_of_freqs = num_of_freqs;
	}
}

static __ref int do_sampling(void *data)
{
	static long prev_temp[NR_CPUS];
	struct cpu_activity_info *node;
	int cpu, ret;

	while (!kthread_should_stop()) {
		ret = wait_event_interruptible(sampling_waitq,
			kthread_should_stop() || atomic_read(&sampling_pending));
		if (kthread_should_stop())
			break;
		if (ret)
			continue;
		atomic_xchg(&sampling_pending, 0);
		mutex_lock(&kthread_update_mutex);
		if (in_suspend || !READ_ONCE(c5_apss_ready))
			goto unlock;
		trigger_cpu_pwr_stats_calc();
		if (!READ_ONCE(c5_apss_ready))
			goto unlock;
		for_each_online_cpu(cpu) {
			node = &activity[cpu];
			if (!sampling_rearm[cpu] && prev_temp[cpu] == node->temp)
				continue;
			ret = set_threshold(node);
			if (!ret) {
				prev_temp[cpu] = node->temp;
				sampling_rearm[cpu] = false;
			} else
				pr_err_ratelimited("APSS threshold update failed: %d\n", ret);
		}
		if (READ_ONCE(poll_ms) > 0 && READ_ONCE(c5_apss_ready))
			schedule_delayed_work(&sampling_work, msecs_to_jiffies(READ_ONCE(poll_ms)));
 unlock:
		mutex_unlock(&kthread_update_mutex);
	}
	return 0;
}

static void clear_static_power(struct cpu_static_info *sp)
{
	int i;

	if (!sp || !cpumask_empty(&sp->mask))
		return;
	if (sp->power) {
		for (i = 0; i < TEMP_DATA_POINTS; i++)
			kfree(sp->power[i]);
		kfree(sp->power);
	}
	kfree(sp);
}

static int c5_apss_resolve_mask(u32 cluster, u32 mask, cpumask_t *cpus)
{
	unsigned int bit, cpu;
	u64 mpidr;

	if (cluster >= MAX_NUM_OF_CLUSTERS || !mask ||
	    (mask & ~((1U << MAX_CORES_PER_CLUSTER) - 1)))
		return -EINVAL;
	cpumask_clear(cpus);
	for (bit = 0; bit < MAX_CORES_PER_CLUSTER; bit++) {
		if (!(mask & BIT(bit)))
			continue;
		mpidr = ((u64)cluster << CLUSTER_OFFSET_FOR_MPIDR) | bit;
		for_each_possible_cpu(cpu)
			if (cpu_logical_map(cpu) == mpidr)
				break;
		if (cpu >= nr_cpu_ids)
			return -EINVAL;
		cpumask_set_cpu(cpu, cpus);
	}
	return 0;
}

static int update_userspace_power(const struct sched_params *args)
{
	unsigned long flags;
	struct cpu_static_info *sp = NULL, *base, *previous;
	cpumask_t cpus;
	unsigned int cpu, first;
	int i, ret;

	mutex_lock(&policy_update_mutex);
	if (!c5_apss_ready) {
		ret = -ENODEV;
		goto unlock;
	}
	ret = c5_apss_resolve_mask(args->cluster, args->cpumask, &cpus);
	if (ret)
		goto unlock;
	first = cpumask_first(&cpus);
	base = activity[first].sp;
	if (!base || !base->table || !base->voltage ||
	    !base->num_of_freqs || base->num_of_freqs > MAX_NUM_FREQ) {
		ret = -ENODATA;
		goto unlock;
	}
	for_each_cpu(cpu, &cpus) {
		struct cpu_static_info *entry = activity[cpu].sp;

		if (!entry || entry->table != base->table ||
		    entry->num_of_freqs != base->num_of_freqs ||
		    (!per_cpu(ptable, cpu) ||
		     c5_apss_canonical_len[cpu] != base->num_of_freqs)) {
			ret = -EINVAL;
			goto unlock;
		}
	}
	sp = kzalloc(sizeof(*sp), GFP_KERNEL);
	if (!sp) {
		ret = -ENOMEM;
		goto unlock;
	}
	sp->power = allocate_2d_array_uint32_t(base->num_of_freqs);
	if (IS_ERR_OR_NULL(sp->power)) {
		ret = sp->power ? PTR_ERR(sp->power) : -ENOMEM;
		kfree(sp);
		goto unlock;
	}
	sp->num_of_freqs = base->num_of_freqs;
	sp->voltage = base->voltage;
	sp->table = base->table;
	for (i = 0; i < TEMP_DATA_POINTS; i++)
		memcpy(sp->power[i], args->power[i],
		       sizeof(args->power[i][0]) * sp->num_of_freqs);
	spin_lock_irqsave(&update_lock, flags);
	for_each_cpu(cpu, &cpus) {
		previous = activity[cpu].sp;
		activity[cpu].sp = sp;
		cpumask_set_cpu(cpu, &sp->mask);
		cpumask_clear_cpu(cpu, &previous->mask);
		clear_static_power(previous);
		repopulate_stats(cpu);
		c5_apss_refresh_locked(cpu);
	}
	activate_power_table = true;
	spin_unlock_irqrestore(&update_lock, flags);
	ret = 0;
 unlock:
	mutex_unlock(&policy_update_mutex);
	return ret;
}

static long msm_core_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg)
{
	struct sched_params __user *argp = (struct sched_params __user *)arg;
	struct sched_params *params;
	struct cpu_static_info *sp;
	cpumask_t cpus;
	unsigned int cpu, chosen = nr_cpu_ids;
	int i, ret;

	if (cmd != EA_LEAKAGE && cmd != EA_VOLT)
		return -ENOTTY;
	if (!argp)
		return -EINVAL;
	if (cmd == EA_LEAKAGE) {
		params = memdup_user(argp, sizeof(*params));
		if (IS_ERR(params))
			return PTR_ERR(params);
		ret = update_userspace_power(params);
		kfree(params);
		return ret;
	}
	params = kzalloc(sizeof(*params), GFP_KERNEL);
	if (!params)
		return -ENOMEM;
	if (copy_from_user(params, argp, offsetof(struct sched_params, power))) {
		ret = -EFAULT;
		goto free_params;
	}
	mutex_lock(&policy_update_mutex);
	if (!c5_apss_ready) {
		ret = -ENODEV;
		goto unlock;
	}
	ret = c5_apss_resolve_mask(params->cluster, params->cpumask, &cpus);
	if (ret)
		goto unlock;
	for_each_cpu(cpu, &cpus)
		chosen = cpu;
	sp = activity[chosen].sp;
	if (!sp || !sp->table || !sp->voltage || !sp->num_of_freqs ||
	    sp->num_of_freqs > MAX_NUM_FREQ) {
		ret = -ENODATA;
		goto unlock;
	}
	memcpy(params->voltage, sp->voltage, sizeof(params->voltage[0]) * sp->num_of_freqs);
	for (i = 0; i < sp->num_of_freqs; i++)
		params->freq[i] = sp->table[i].frequency;
	ret = 0;
	if (copy_to_user(argp->voltage, params->voltage,
			sizeof(params->voltage[0]) * sp->num_of_freqs) ||
	    copy_to_user(argp->freq, params->freq,
			sizeof(params->freq[0]) * sp->num_of_freqs))
		ret = -EFAULT;
 unlock:
	mutex_unlock(&policy_update_mutex);
 free_params:
	kfree(params);
	return ret;
}

#ifdef CONFIG_COMPAT
static long msm_core_compat_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg)
{
	arg = (unsigned long)compat_ptr(arg);
	return msm_core_ioctl(file, cmd, arg);
}
#endif

static int msm_core_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int msm_core_release(struct inode *inode, struct file *file)
{
	return 0;
}

static inline void init_sens_threshold(struct sensor_threshold *threshold,
		enum thermal_trip_type trip, long temp,
		void *data)
{
	threshold->trip = trip;
	threshold->temp = temp;
	threshold->data = data;
	threshold->notify = core_temp_notify;
}

static int msm_core_stats_init(struct device *dev, int cpu)
{
	struct cpu_activity_info *cpu_node = &activity[cpu];
	struct cpu_pstate_pwr *pstate;
	unsigned long flags;
	unsigned int i, count;

	if (!cpu_node->sp || !cpu_node->sp->table)
		return -ENODATA;
	count = cpu_node->sp->num_of_freqs;
	if (!count || count > MAX_NUM_FREQ)
		return -EINVAL;
	pstate = devm_kcalloc(dev, count + 1, sizeof(*pstate), GFP_KERNEL);
	if (!pstate)
		return -ENOMEM;
	for (i = 0; i < count; i++)
		pstate[i].freq = cpu_node->sp->table[i].frequency;
	spin_lock_irqsave(&update_lock, flags);
	per_cpu(ptable, cpu) = pstate;
	c5_apss_canonical_len[cpu] = count;
	cpu_stats[cpu].cpu = cpu;
	cpu_stats[cpu].temp = cpu_node->temp;
	c5_apss_refresh_locked(cpu);
	spin_unlock_irqrestore(&update_lock, flags);
	return 0;
}

static int msm_core_task_init(struct device *dev)
{
	struct task_struct *task;

	atomic_set(&sampling_pending, 0);
	task = kthread_run(do_sampling, NULL, "msm-core:sampling");
	if (IS_ERR(task))
		return PTR_ERR(task);
	sampling_task = task;
	return 0;
}

struct cpu_pwr_stats *get_cpu_pwr_stats(void)
{
	return cpu_stats;
}
EXPORT_SYMBOL(get_cpu_pwr_stats);

static int msm_get_power_values(int cpu, struct cpu_static_info *sp)
{
	int i = 0, j;
	int ret = 0;
	uint64_t power;
	uint32_t **new_power;

	if (!sp->num_of_freqs || sp->num_of_freqs > MAX_NUM_FREQ)
		return -EINVAL;

	/* Calculate dynamic power spent for every frequency using formula:
	 * Power = V * V * f
	 * where V = voltage for frequency
	 *       f = frequency
	 * */
	new_power = allocate_2d_array_uint32_t(sp->num_of_freqs);
	if (IS_ERR_OR_NULL(new_power))
		return new_power ? PTR_ERR(new_power) : -ENOMEM;
	sp->power = new_power;

	for (i = 0; i < TEMP_DATA_POINTS; i++) {
		for (j = 0; j < sp->num_of_freqs; j++) {
			power = (uint64_t)sp->voltage[j] *
						sp->table[j].frequency;
			do_div(power, 1000);
			do_div(power, 1000);
			power *= sp->voltage[j];
			do_div(power, 1000);
			sp->power[i][j] = power;
		}
	}
	return ret;
}

static int msm_get_voltage_levels(struct device *dev, int cpu,
		struct cpu_static_info *sp)
{
	unsigned int *voltage;
	int i;
	unsigned long corner;
	int ret = 0;
	struct dev_pm_opp *opp;
	struct device *cpu_dev = get_cpu_device(cpu);
	/*
	 * Convert cpr corner voltage to average voltage of both
	 * a53 and a57 votlage value
	 */
	int average_voltage[NUM_OF_CORNERS] = {0, 746, 841, 843, 940, 953, 976,
			1024, 1090, 1100};

	if (!cpu_dev)
		return -ENODEV;

	voltage = devm_kzalloc(dev,
			sizeof(*voltage) * sp->num_of_freqs, GFP_KERNEL);

	if (!voltage)
		return -ENOMEM;

	rcu_read_lock();
	for (i = 0; i < sp->num_of_freqs; i++) {
		opp = dev_pm_opp_find_freq_exact(cpu_dev,
				(unsigned long)sp->table[i].frequency * 1000, true);
		if (IS_ERR(opp)) {
			ret = PTR_ERR(opp);
			break;
		}
		corner = dev_pm_opp_get_voltage(opp);

		if (corner > 400000)
			voltage[i] = corner / 1000;
		else if (corner > 0 && corner < ARRAY_SIZE(average_voltage))
			voltage[i] = average_voltage[corner];
		else
			voltage[i]
			     = average_voltage[ARRAY_SIZE(average_voltage) - 1];
	}
	rcu_read_unlock();
	if (ret)
		return ret;

	sp->voltage = voltage;
	return 0;
}

static int msm_core_dyn_pwr_init(struct platform_device *pdev,
				int cpu)
{
	int ret = 0;

	if (!activity[cpu].sp->table)
		return 0;

	ret = msm_get_voltage_levels(&pdev->dev, cpu, activity[cpu].sp);
	if (ret)
		return ret;

	ret = msm_get_power_values(cpu, activity[cpu].sp);

	return ret;
}

static int msm_core_tsens_init(struct device_node *node, int cpu)
{
	struct cpu_activity_info *entry = &activity[cpu];
	struct device_node *sensor;
	const char *name;
	int ret;

	if (!node)
		return -ENODEV;
	sensor = of_parse_phandle(node, "qcom,sensor", 0);
	if (!sensor) {
		entry->sensor_id = -1;
		entry->temp = DEFAULT_TEMP;
		return 0;
	}
	ret = of_property_read_string(sensor, "qcom,sensor-name", &name);
	if (ret)
		goto put_sensor;
	entry->sensor_id = sensor_get_id((char *)name);
	if (entry->sensor_id < 0) {
		ret = entry->sensor_id == -ENODEV ? -EPROBE_DEFER : entry->sensor_id;
		goto put_sensor;
	}
	ret = sensor_get_temp(entry->sensor_id, &entry->temp);
	if (ret)
		goto put_sensor;
	init_sens_threshold(&entry->hi_threshold, THERMAL_TRIP_CONFIGURABLE_HI,
		entry->temp + high_hyst_temp, entry);
	init_sens_threshold(&entry->low_threshold, THERMAL_TRIP_CONFIGURABLE_LOW,
		entry->temp - low_hyst_temp, entry);
 put_sensor:
	of_node_put(sensor);
	return ret;
}

static int c5_apss_cpu_from_node(struct device_node *node)
{
	struct device_node *target, *cpu_node;
	unsigned int cpu;
	bool matches;
	int result = -ENODEV;

	target = of_parse_phandle(node, "qcom,cpu-name", 0);
	if (!target)
		return -ENODEV;
	for_each_possible_cpu(cpu) {
		cpu_node = of_get_cpu_node(cpu, NULL);
		matches = cpu_node == target;
		of_node_put(cpu_node);
		if (matches) {
			result = cpu;
			break;
		}
	}
	of_node_put(target);
	return result;
}

static int msm_core_cpu_policy_handler(struct notifier_block *nb,
		unsigned long val, void *data)
{
	struct cpufreq_policy *policy = data;
	int cpu, ret;

	if (val != CPUFREQ_CREATE_POLICY || !policy || policy->cpu >= nr_cpu_ids)
		return NOTIFY_DONE;
	mutex_lock(&policy_update_mutex);
	if (!c5_apss_ready || !msm_core_pdev || !activity[policy->cpu].sp ||
	    activity[policy->cpu].sp->table)
		goto unlock;
	update_related_freq_table(policy);
	for_each_cpu(cpu, policy->related_cpus) {
		ret = msm_core_dyn_pwr_init(msm_core_pdev, cpu);
		if (!ret)
			ret = msm_core_stats_init(&msm_core_pdev->dev, cpu);
		if (ret)
			pr_err_ratelimited("APSS late frequency statistics unavailable: %d\n", ret);
	}
 unlock:
	mutex_unlock(&policy_update_mutex);
	/* A statistics failure must not veto the CPU frequency policy itself. */
	return NOTIFY_OK;
}

static struct notifier_block cpu_policy = {
	.notifier_call = msm_core_cpu_policy_handler
};

static int system_suspend_handler(struct notifier_block *nb,
				unsigned long val, void *data)
{
	int cpu;

	if (!READ_ONCE(c5_apss_ready))
		return NOTIFY_DONE;

	mutex_lock(&kthread_update_mutex);
	switch (val) {
	case PM_POST_HIBERNATION:
	case PM_POST_SUSPEND:
	case PM_POST_RESTORE:
		/*
		 * Set completion event to read temperature and repopulate
		 * stats
		 */
		in_suspend = 0;
		c5_apss_request_sample();
		break;
	case PM_HIBERNATION_PREPARE:
	case PM_SUSPEND_PREPARE:
		/*
		 * cancel delayed work to be able to restart immediately
		 * after system resume
		 */
		in_suspend = 1;
		cancel_delayed_work(&sampling_work);
		/*
		 * cancel TSENS interrupts as we do not want to wake up from
		 * suspend to take care of repopulate stats while the system is
		 * in suspend
		 */
		for_each_possible_cpu(cpu) {
			if (activity[cpu].sensor_id < 0)
				continue;

			sampling_rearm[cpu] = true;
			sensor_activate_trip(activity[cpu].sensor_id,
				&activity[cpu].hi_threshold, false);
			sensor_activate_trip(activity[cpu].sensor_id,
				&activity[cpu].low_threshold, false);
		}
		break;
	default:
		break;
	}
	mutex_unlock(&kthread_update_mutex);

	return NOTIFY_OK;
}

static struct notifier_block c5_apss_pm_notifier = {
	.notifier_call = system_suspend_handler,
};

static int msm_core_freq_init(void)
{
	int cpu;
	struct cpufreq_policy *policy;

	for_each_possible_cpu(cpu) {
		activity[cpu].sp = kzalloc(sizeof(*(activity[cpu].sp)),
				GFP_KERNEL);
		if (!activity[cpu].sp)
			return -ENOMEM;
		cpumask_set_cpu(cpu, &activity[cpu].sp->mask);
	}

	for_each_online_cpu(cpu) {
		if (activity[cpu].sp->table)
			continue;

		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;

		update_related_freq_table(policy);
		cpufreq_cpu_put(policy);
	}

	return 0;
}

static int msm_core_params_init(struct platform_device *pdev)
{
	struct device_node *mapping, *child;
	cpumask_t seen;
	int cpu, ret = 0;

	mapping = of_get_child_by_name(pdev->dev.of_node, "qcom,core-mapping");
	if (!mapping)
		return -ENODEV;
	cpumask_clear(&seen);
	for_each_child_of_node(mapping, child) {
		cpu = c5_apss_cpu_from_node(child);
		if (cpu < 0) {
			ret = cpu;
			goto put_child;
		}
		if (cpumask_test_cpu(cpu, &seen)) {
			ret = -EINVAL;
			goto put_child;
		}
		cpumask_set_cpu(cpu, &seen);
		activity[cpu].mpidr = cpu_logical_map(cpu);
		ret = msm_core_tsens_init(child, cpu);
		if (ret)
			goto put_child;
		if (!activity[cpu].sp->table)
			continue;
		ret = msm_core_dyn_pwr_init(pdev, cpu);
		if (ret)
			goto put_child;
		ret = msm_core_stats_init(&pdev->dev, cpu);
		if (ret)
			goto put_child;
	}
	if (!cpumask_equal(&seen, cpu_possible_mask))
		ret = -EINVAL;
	of_node_put(mapping);
	return ret;
 put_child:
	of_node_put(child);
	of_node_put(mapping);
	return ret;
}

static const struct file_operations msm_core_ops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = msm_core_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = msm_core_compat_ioctl,
#endif
	.open = msm_core_open,
	.release = msm_core_release,
};

static struct miscdevice msm_core_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "pta",
	.fops = &msm_core_ops
};

static void free_dyn_memory(void)
{
	struct cpu_static_info *sp;
	unsigned long flags;
	int cpu;

	mutex_lock(&policy_update_mutex);
	for_each_possible_cpu(cpu) {
		spin_lock_irqsave(&update_lock, flags);
		sp = activity[cpu].sp;
		activity[cpu].sp = NULL;
		per_cpu(ptable, cpu) = NULL;
		c5_apss_canonical_len[cpu] = 0;
		c5_apss_refresh_locked(cpu);
		spin_unlock_irqrestore(&update_lock, flags);
		if (sp) {
			cpumask_clear_cpu(cpu, &sp->mask);
			clear_static_power(sp);
		}
	}
	mutex_unlock(&policy_update_mutex);
}

static int uio_init(struct platform_device *pdev)
{
	int ret = 0;
	struct uio_info *info = NULL;
	struct resource *clnt_res = NULL;
	u32 ea_mem_size = 0;
	phys_addr_t ea_mem_pyhsical = 0;

	clnt_res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!clnt_res) {
		pr_err("resource not found\n");
		return -ENODEV;
	}

	info = devm_kzalloc(&pdev->dev, sizeof(struct uio_info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	ea_mem_size = resource_size(clnt_res);
	ea_mem_pyhsical = clnt_res->start;

	if (ea_mem_size == 0) {
		pr_err("msm-core: memory size is zero");
		return -EINVAL;
	}

	/* Setup device */
	info->name = clnt_res->name;
	info->version = "1.0";
	info->mem[0].addr = ea_mem_pyhsical;
	info->mem[0].size = ea_mem_size;
	info->mem[0].memtype = UIO_MEM_PHYS;

	ret = uio_register_device(&pdev->dev, info);
	if (ret) {
		pr_err("uio register failed ret=%d", ret);
		return ret;
	}
	dev_set_drvdata(&pdev->dev, info);

	return 0;
}

static void c5_apss_cleanup(struct platform_device *pdev)
{
	struct uio_info *info;
	int cpu;

	mutex_lock(&policy_update_mutex);
	WRITE_ONCE(c5_apss_ready, false);
	mutex_unlock(&policy_update_mutex);
	if (c5_apss_misc_registered) {
		misc_deregister(&msm_core_device);
		c5_apss_misc_registered = false;
	}
	if (c5_apss_pm_registered) {
		unregister_pm_notifier(&c5_apss_pm_notifier);
		c5_apss_pm_registered = false;
	}
	if (c5_apss_freq_registered) {
		cpufreq_unregister_notifier(&cpu_policy, CPUFREQ_POLICY_NOTIFIER);
		c5_apss_freq_registered = false;
	}
	/* Stop the only rearming worker before cancelling timers and thresholds. */
	if (sampling_task) {
		kthread_stop(sampling_task);
		sampling_task = NULL;
	}
	if (c5_apss_work_initialized) {
		cancel_delayed_work_sync(&sampling_work);
		c5_apss_work_initialized = false;
	}
	for_each_possible_cpu(cpu) {
		if (activity[cpu].sensor_id < 0)
			continue;
		sensor_cancel_trip(activity[cpu].sensor_id, &activity[cpu].hi_threshold);
		sensor_cancel_trip(activity[cpu].sensor_id, &activity[cpu].low_threshold);
	}
	atomic_set(&sampling_pending, 0);
	if (c5_apss_uio_registered) {
		info = dev_get_drvdata(&pdev->dev);
		if (info)
			uio_unregister_device(info);
		dev_set_drvdata(&pdev->dev, NULL);
		c5_apss_uio_registered = false;
	}
	free_dyn_memory();
	msm_core_pdev = NULL;
}

static int msm_core_dev_probe(struct platform_device *pdev)
{
	struct device_node *node;
	u32 value;
	int cpu, ret;

	if (!pdev || !pdev->dev.of_node)
		return -ENODEV;
	mutex_lock(&c5_apss_probe_mutex);
	if (msm_core_pdev) {
		ret = -EBUSY;
		goto unlock;
	}
	msm_core_pdev = pdev;
	node = pdev->dev.of_node;
	WRITE_ONCE(c5_apss_ready, false);
	in_suspend = false;
	for_each_possible_cpu(cpu) {
		activity[cpu].cpu = cpu;
		activity[cpu].sensor_id = -1;
		sampling_rearm[cpu] = true;
	}
	low_hyst_temp = DEFAULT_LOW_HYST_TEMP;
	high_hyst_temp = DEFAULT_HIGH_HYST_TEMP;
	if (of_find_property(node, "qcom,low-hyst-temp", NULL)) {
		ret = of_property_read_u32(node, "qcom,low-hyst-temp", &value);
		if (ret || value > INT_MAX / 1000) {
			ret = ret ? ret : -ERANGE;
			goto failed;
		}
		low_hyst_temp = value;
	}
	if (of_find_property(node, "qcom,high-hyst-temp", NULL)) {
		ret = of_property_read_u32(node, "qcom,high-hyst-temp", &value);
		if (ret || value > INT_MAX / 1000) {
			ret = ret ? ret : -ERANGE;
			goto failed;
		}
		high_hyst_temp = value;
	}
	if (of_find_property(node, "qcom,polling-interval", NULL)) {
		ret = of_property_read_u32(node, "qcom,polling-interval", &value);
		if (ret || value > INT_MAX) {
			ret = ret ? ret : -ERANGE;
			goto failed;
		}
		poll_ms = value;
	}
	ret = msm_core_freq_init();
	if (ret)
		goto failed;
	ret = msm_core_params_init(pdev);
	if (ret)
		goto failed;
	INIT_DEFERRABLE_WORK(&sampling_work, samplequeue_handle);
	c5_apss_work_initialized = true;
	ret = msm_core_task_init(&pdev->dev);
	if (ret)
		goto failed;
	ret = uio_init(pdev);
	if (ret)
		goto failed;
	c5_apss_uio_registered = true;
	ret = cpufreq_register_notifier(&cpu_policy, CPUFREQ_POLICY_NOTIFIER);
	if (ret)
		goto failed;
	c5_apss_freq_registered = true;
	ret = register_pm_notifier(&c5_apss_pm_notifier);
	if (ret)
		goto failed;
	c5_apss_pm_registered = true;
	for_each_possible_cpu(cpu) {
		ret = set_threshold(&activity[cpu]);
		if (ret)
			goto failed;
	}
	ret = misc_register(&msm_core_device);
	if (ret)
		goto failed;
	c5_apss_misc_registered = true;
	mutex_lock(&policy_update_mutex);
	WRITE_ONCE(c5_apss_ready, true);
	mutex_unlock(&policy_update_mutex);
	c5_apss_request_sample();
	ret = 0;
	goto unlock;
 failed:
	c5_apss_cleanup(pdev);
 unlock:
	mutex_unlock(&c5_apss_probe_mutex);
	return ret;
}

static int msm_core_remove(struct platform_device *pdev)
{
	mutex_lock(&c5_apss_probe_mutex);
	if (msm_core_pdev == pdev)
		c5_apss_cleanup(pdev);
	mutex_unlock(&c5_apss_probe_mutex);
	return 0;
}

static struct of_device_id msm_core_match_table[] = {
	{.compatible = "qcom,apss-core-ea"},
	{},
};

static struct platform_driver msm_core_driver = {
	.probe = msm_core_dev_probe,
	.driver = {
		.name = "msm_core",
		.owner = THIS_MODULE,
		.of_match_table = msm_core_match_table,
		},
	.remove = msm_core_remove,
};

static int __init msm_core_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		activity[cpu].sensor_id = -1;
	return platform_driver_register(&msm_core_driver);
}
late_initcall(msm_core_init);
