/*
 * Copyright (c) 2014-2018, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 */
#include <linux/init.h>
#include <linux/notifier.h>
#include <linux/cpu.h>
#include <linux/moduleparam.h>
#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/sysfs.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/ctype.h>
#include <linux/wait.h>

struct cpu_status {
	unsigned int min;
	unsigned int max;
};
static DEFINE_PER_CPU(struct cpu_status, cpu_stats) = {
	.min = 0,
	.max = UINT_MAX,
};
static DEFINE_MUTEX(perf_request_lock);
static DEFINE_SPINLOCK(perf_vote_lock);
static DEFINE_SPINLOCK(cpu_hotplug_lock);
static DECLARE_WAIT_QUEUE_HEAD(events_wait);
static struct task_struct *events_notify_thread;
static struct kobject *events_kobj;
static bool events_group_created;
static bool cpu_hotplug_pending;
static bool events_ready;
static int perf_state; /* zero: early parameters, one: ready, negative: failed */
static int perf_last_error;
static int perf_cpuhp_state = -1;
static bool perf_notifier_registered;
static bool early_votes;
static u64 min_requests, max_requests, invalid_requests;
static u64 policy_updates, policy_update_errors, policy_callbacks;

/* Decimal CPU:value grammar; -1 is an additional unsigned reset spelling. */
static int perf_parse_uint(const char **cursor, unsigned int *result)
{
	const char *p = *cursor;
	unsigned int value = 0, digit;

	if (*p == '-' && p[1] == '1' && !isdigit(p[2])) {
		*cursor = p + 2;
		*result = UINT_MAX;
		return 0;
	}
	if (*p == '+')
		p++;
	if (!isdigit(*p))
		return -EINVAL;
	while (isdigit(*p)) {
		digit = *p++ - '0';
		if (value > (UINT_MAX - digit) / 10)
			return -EINVAL;
		value = value * 10 + digit;
	}
	*cursor = p;
	*result = value;
	return 0;
}

static int perf_parse_votes(const char *buf, struct cpumask *mask,
			   unsigned int *values)
{
	const char *p = buf;
	unsigned int cpu, value;
	bool any = false;

	if (strnlen(buf, PAGE_SIZE) == PAGE_SIZE)
		return -EINVAL;
	while (isspace(*p))
		p++;
	while (*p) {
		if (perf_parse_uint(&p, &cpu) || *p++ != ':' ||
		    perf_parse_uint(&p, &value) ||
		    cpu >= nr_cpu_ids || !cpu_present(cpu))
			return -EINVAL;
		if (*p && !isspace(*p))
			return -EINVAL;
		values[cpu] = value;
		cpumask_set_cpu(cpu, mask);
		any = true;
		while (isspace(*p))
			p++;
	}
	return any ? 0 : -EINVAL;
}

/* Caller owns perf_request_lock. Each related policy is refreshed once. */
static void perf_refresh_policies(struct cpumask *mask)
{
	struct cpufreq_policy *policy;
	unsigned int cpu, sibling;
	int ret;

	get_online_cpus();
	for_each_online_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;
		if (cpumask_intersects(mask, policy->related_cpus)) {
			ret = cpufreq_update_policy(cpu);
			policy_updates++;
			if (ret)
				policy_update_errors++;
			for_each_cpu(sibling, policy->related_cpus)
				cpumask_clear_cpu(sibling, mask);
		}
		cpufreq_cpu_put(policy);
	}
	put_online_cpus();
}

static int set_cpu_freq(const char *buf, bool maximum)
{
	cpumask_var_t mask;
	unsigned int *values, cpu;
	unsigned long flags;
	int ret;

	values = kcalloc(nr_cpu_ids, sizeof(*values), GFP_KERNEL);
	if (!values)
		return -ENOMEM;
	if (!zalloc_cpumask_var(&mask, GFP_KERNEL)) {
		kfree(values);
		return -ENOMEM;
	}
	ret = perf_parse_votes(buf, mask, values);
	mutex_lock(&perf_request_lock);
	if (perf_state < 0)
		ret = -ENODEV;
	if (ret) {
		invalid_requests++;
		goto unlock;
	}
	spin_lock_irqsave(&perf_vote_lock, flags);
	for_each_cpu(cpu, mask) {
		if (maximum)
			per_cpu(cpu_stats, cpu).max = values[cpu];
		else
			per_cpu(cpu_stats, cpu).min = values[cpu];
	}
	spin_unlock_irqrestore(&perf_vote_lock, flags);
	if (maximum)
		max_requests++;
	else
		min_requests++;
	if (perf_state == 1)
		perf_refresh_policies(mask);
	else
		early_votes = true;
unlock:
	mutex_unlock(&perf_request_lock);
	free_cpumask_var(mask);
	kfree(values);
	return ret;
}

static int set_cpu_min_freq(const char *buf, const struct kernel_param *kp)
{
	return set_cpu_freq(buf, false);
}

static int set_cpu_max_freq(const char *buf, const struct kernel_param *kp)
{
	return set_cpu_freq(buf, true);
}

static int get_cpu_freq(char *buf, bool maximum)
{
	unsigned int cpu, value;
	int count = 0;

	mutex_lock(&perf_request_lock);
	for_each_present_cpu(cpu) {
		value = maximum ? per_cpu(cpu_stats, cpu).max :
			per_cpu(cpu_stats, cpu).min;
		count += scnprintf(buf + count, PAGE_SIZE - count,
				   "%u:%u ", cpu, value);
	}
	count += scnprintf(buf + count, PAGE_SIZE - count, "\n");
	mutex_unlock(&perf_request_lock);
	return count;
}

static int get_cpu_min_freq(char *buf, const struct kernel_param *kp)
{
	return get_cpu_freq(buf, false);
}

static int get_cpu_max_freq(char *buf, const struct kernel_param *kp)
{
	return get_cpu_freq(buf, true);
}

static const struct kernel_param_ops param_ops_cpu_min_freq = {
	.set = set_cpu_min_freq,
	.get = get_cpu_min_freq,
};
static const struct kernel_param_ops param_ops_cpu_max_freq = {
	.set = set_cpu_max_freq,
	.get = get_cpu_max_freq,
};
module_param_cb(cpu_min_freq, &param_ops_cpu_min_freq, NULL, 0644);
module_param_cb(cpu_max_freq, &param_ops_cpu_max_freq, NULL, 0644);

static int perf_adjust_notify(struct notifier_block *nb, unsigned long event,
			      void *data)
{
	struct cpufreq_policy *policy = data;
	unsigned int cpu, minimum = 0, maximum = UINT_MAX;
	unsigned long flags;

	if (event != CPUFREQ_ADJUST)
		return NOTIFY_OK;
	spin_lock_irqsave(&perf_vote_lock, flags);
	for_each_cpu(cpu, policy->related_cpus) {
		minimum = max(minimum, per_cpu(cpu_stats, cpu).min);
		maximum = min(maximum, per_cpu(cpu_stats, cpu).max);
	}
	policy_callbacks++;
	spin_unlock_irqrestore(&perf_vote_lock, flags);
	cpufreq_verify_within_limits(policy, minimum, maximum);
	return NOTIFY_OK;
}

static struct notifier_block perf_cpufreq_nb = {
	.notifier_call = perf_adjust_notify,
};

static ssize_t show_cpu_hotplug(struct kobject *kobj,
			      struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "\n");
}
static struct kobj_attribute cpu_hotplug_attr =
	__ATTR(cpu_hotplug, 0444, show_cpu_hotplug, NULL);
static struct attribute *events_attrs[] = {
	&cpu_hotplug_attr.attr,
	NULL,
};
static struct attribute_group events_attr_group = {
	.attrs = events_attrs,
};

static int hotplug_notify(unsigned int cpu)
{
	unsigned long flags;

	spin_lock_irqsave(&cpu_hotplug_lock, flags);
	if (events_ready) {
		cpu_hotplug_pending = true;
		wake_up_interruptible(&events_wait);
	}
	spin_unlock_irqrestore(&cpu_hotplug_lock, flags);
	return 0;
}

static int events_notify_userspace(void *unused)
{
	unsigned long flags;
	bool changed;

	for (;;) {
		wait_event_interruptible(events_wait,
			kthread_should_stop() || READ_ONCE(cpu_hotplug_pending));
		if (kthread_should_stop())
			break;
		spin_lock_irqsave(&cpu_hotplug_lock, flags);
		changed = cpu_hotplug_pending;
		cpu_hotplug_pending = false;
		spin_unlock_irqrestore(&cpu_hotplug_lock, flags);
		if (changed)
			sysfs_notify(events_kobj, NULL, "cpu_hotplug");
	}
	return 0;
}

static void destroy_events_group(void)
{
	unsigned long flags;

	spin_lock_irqsave(&cpu_hotplug_lock, flags);
	events_ready = false;
	spin_unlock_irqrestore(&cpu_hotplug_lock, flags);
	if (events_notify_thread) {
		kthread_stop(events_notify_thread);
		events_notify_thread = NULL;
	}
	if (events_group_created) {
		sysfs_remove_group(events_kobj, &events_attr_group);
		events_group_created = false;
	}
	if (events_kobj) {
		kobject_put(events_kobj);
		events_kobj = NULL;
	}
}

static int init_events_group(void)
{
	struct kobject *module_kobj;
	int ret;

	module_kobj = kset_find_obj(module_kset, KBUILD_MODNAME);
	if (!module_kobj)
		return -ENOENT;
	events_kobj = kobject_create_and_add("events", module_kobj);
	kobject_put(module_kobj);
	if (!events_kobj)
		return -ENOMEM;
	ret = sysfs_create_group(events_kobj, &events_attr_group);
	if (ret)
		goto fail;
	events_group_created = true;
	events_notify_thread = kthread_run(events_notify_userspace, NULL,
					 "msm_perf:events_notify");
	if (IS_ERR(events_notify_thread)) {
		ret = PTR_ERR(events_notify_thread);
		events_notify_thread = NULL;
		goto fail;
	}
	return 0;
fail:
	destroy_events_group();
	return ret;
}

static int c5_state_get(char *buf, const struct kernel_param *kp)
{
	unsigned long flags;
	u64 callbacks;
	bool ready;
	int count;

	mutex_lock(&perf_request_lock);
	spin_lock_irqsave(&perf_vote_lock, flags);
	callbacks = policy_callbacks;
	spin_unlock_irqrestore(&perf_vote_lock, flags);
	spin_lock_irqsave(&cpu_hotplug_lock, flags);
	ready = events_ready;
	spin_unlock_irqrestore(&cpu_hotplug_lock, flags);
	count = scnprintf(buf, PAGE_SIZE,
		"v1 state=%d error=%d notifier=%u events=%u hotplug=%u "
		"min=%llu max=%llu invalid=%llu updates=%llu failed=%llu callbacks=%llu\n",
		perf_state, perf_last_error, perf_notifier_registered,
		ready, perf_cpuhp_state >= 0,
		(unsigned long long)min_requests, (unsigned long long)max_requests,
		(unsigned long long)invalid_requests, (unsigned long long)policy_updates,
		(unsigned long long)policy_update_errors, (unsigned long long)callbacks);
	mutex_unlock(&perf_request_lock);
	return count;
}
static const struct kernel_param_ops c5_state_ops = {
	.get = c5_state_get,
};
module_param_cb(c5_state, &c5_state_ops, NULL, 0400);

static int __init msm_performance_init(void)
{
	cpumask_t mask;
	unsigned long flags;
	int ret;

	ret = cpufreq_register_notifier(&perf_cpufreq_nb, CPUFREQ_POLICY_NOTIFIER);
	if (ret)
		goto failed;
	perf_notifier_registered = true;
	ret = init_events_group();
	if (ret)
		goto unregister;
	ret = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN,
				"msm_performance_cpu_hotplug", hotplug_notify, NULL);
	if (ret < 0)
		goto destroy;
	perf_cpuhp_state = ret;
	spin_lock_irqsave(&cpu_hotplug_lock, flags);
	events_ready = true;
	spin_unlock_irqrestore(&cpu_hotplug_lock, flags);
	mutex_lock(&perf_request_lock);
	perf_state = 1;
	if (early_votes) {
		cpumask_copy(&mask, cpu_present_mask);
		perf_refresh_policies(&mask);
	}
	mutex_unlock(&perf_request_lock);
	return 0;
destroy:
	destroy_events_group();
unregister:
	cpufreq_unregister_notifier(&perf_cpufreq_nb, CPUFREQ_POLICY_NOTIFIER);
	perf_notifier_registered = false;
failed:
	mutex_lock(&perf_request_lock);
	perf_last_error = ret;
	perf_state = -1;
	mutex_unlock(&perf_request_lock);
	return ret;
}
late_initcall(msm_performance_init);

static void __exit msm_performance_exit(void)
{
	cpumask_t mask;
	unsigned long flags;

	mutex_lock(&perf_request_lock);
	perf_state = -1;
	mutex_unlock(&perf_request_lock);
	spin_lock_irqsave(&cpu_hotplug_lock, flags);
	events_ready = false;
	spin_unlock_irqrestore(&cpu_hotplug_lock, flags);
	if (perf_cpuhp_state >= 0) {
		cpuhp_remove_state_nocalls(perf_cpuhp_state);
		perf_cpuhp_state = -1;
	}
	destroy_events_group();
	if (perf_notifier_registered) {
		cpufreq_unregister_notifier(&perf_cpufreq_nb, CPUFREQ_POLICY_NOTIFIER);
		perf_notifier_registered = false;
	}
	mutex_lock(&perf_request_lock);
	cpumask_copy(&mask, cpu_present_mask);
	perf_refresh_policies(&mask);
	mutex_unlock(&perf_request_lock);
}
module_exit(msm_performance_exit);
MODULE_DESCRIPTION("Qualcomm CPU frequency votes and hotplug notifications");
MODULE_LICENSE("GPL v2");
