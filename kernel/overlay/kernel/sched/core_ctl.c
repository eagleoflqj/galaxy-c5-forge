/* Copyright (c) 2014-2018, 2020 The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt)	"core_ctl: " fmt

#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/sched/rt.h>
#include <linux/syscore_ops.h>
#include <linux/completion.h>
#include <linux/moduleparam.h>
#include <linux/module.h>

#include <trace/events/sched.h>
#include "sched.h"
#include "walt.h"

#define MAX_CPUS_PER_CLUSTER 6
#define MAX_CLUSTERS 2

struct cluster_data {
	bool inited;
	unsigned int min_cpus;
	unsigned int max_cpus;
	unsigned int offline_delay_ms;
	unsigned int busy_up_thres[MAX_CPUS_PER_CLUSTER];
	unsigned int busy_down_thres[MAX_CPUS_PER_CLUSTER];
	unsigned int active_cpus;
	unsigned int num_cpus;
	unsigned int nr_isolated_cpus;
	unsigned int nr_not_preferred_cpus;
#ifdef CONFIG_SCHED_CORE_ROTATE
	unsigned long set_max;
	unsigned long set_cur;
#endif
	cpumask_t cpu_mask;
	unsigned int need_cpus;
	unsigned int task_thres;
	unsigned int max_nr;
	s64 need_ts;
	struct list_head lru;
	bool pending;
	spinlock_t pending_lock;
	bool is_big_cluster;
	bool enable;
	int nrrun;
	struct task_struct *core_ctl_thread;
	unsigned int first_cpu;
	unsigned int boost;
	struct kobject kobj;
	bool kobj_initialized;
	struct completion kobj_released;
};

struct cpu_data {
	bool is_busy;
	unsigned int busy;
	unsigned int cpu;
	bool not_preferred;
	struct cluster_data *cluster;
	struct list_head sib;
	bool isolated_by_us;
};

static DEFINE_PER_CPU(struct cpu_data, cpu_state);
static struct cluster_data cluster_state[MAX_CLUSTERS];
static unsigned int num_clusters;

#define for_each_cluster(cluster, idx) \
	for (; (idx) < num_clusters && ((cluster) = &cluster_state[idx]);\
		(idx)++)

static DEFINE_SPINLOCK(state_lock);
static void apply_need(struct cluster_data *state);
static void wake_up_core_ctl_thread(struct cluster_data *state);
static bool initialized;
static int init_error;
static bool init_skipped;
static int online_hp_state = -1;
static bool dead_hp_registered;
#ifdef CONFIG_SCHED_CORE_ROTATE
static bool rotate_ops_registered;
#endif
static u64 isolate_calls, unisolate_calls, isolate_failures, unisolate_failures;
static int last_isolate_error, last_unisolate_error;

/* Every caller below drops state_lock before calling the native primitive. */
static int core_ctl_isolate(unsigned int cpu)
{
	unsigned long flags;
	int ret = sched_isolate_cpu(cpu);

	spin_lock_irqsave(&state_lock, flags);
	isolate_calls++;
	isolate_failures += ret != 0;
	if (ret)
		last_isolate_error = ret;
	spin_unlock_irqrestore(&state_lock, flags);
	return ret;
}

static int core_ctl_unisolate(unsigned int cpu)
{
	unsigned long flags;
	int ret = sched_unisolate_cpu(cpu);

	spin_lock_irqsave(&state_lock, flags);
	unisolate_calls++;
	unisolate_failures += ret != 0;
	if (ret)
		last_unisolate_error = ret;
	spin_unlock_irqrestore(&state_lock, flags);
	return ret;
}


static unsigned int get_active_cpu_count(const struct cluster_data *cluster);
static void cpuset_next(struct cluster_data *cluster);

/* ========================= sysfs interface =========================== */

static int parse_uint(const char *buf, size_t count, unsigned int *value)
{
	char copy[64];

	if (!count || count >= sizeof(copy) || memchr(buf, '\0', count))
		return -EINVAL;
	memcpy(copy, buf, count);
	copy[count] = '\0';
	return kstrtouint(strim(copy), 10, value);
}

static int parse_values(const char *buf, size_t count, unsigned int *values)
{
	char copy[128], *cursor, *token;
	unsigned int n = 0;
	int ret;

	if (!count || count >= sizeof(copy) || memchr(buf, '\0', count))
		return -EINVAL;
	memcpy(copy, buf, count);
	copy[count] = '\0';
	cursor = copy;
	while ((token = strsep(&cursor, " \t\r\n\v\f")) != NULL) {
		if (!*token)
			continue;
		if (n == MAX_CPUS_PER_CLUSTER)
			return -EINVAL;
		ret = kstrtouint(token, 10, &values[n]);
		if (ret)
			return ret;
		n++;
	}
	return n ? n : -EINVAL;
}


static ssize_t store_min_cpus(struct cluster_data *state,
		const char *buf, size_t count)
{
	unsigned int value;
	unsigned long flags;
	int ret = parse_uint(buf, count, &value);

	if (ret)
		return ret;
	spin_lock_irqsave(&state_lock, flags);
	state->min_cpus = min(value, state->max_cpus);
	spin_unlock_irqrestore(&state_lock, flags);
	cpuset_next(state);
	wake_up_core_ctl_thread(state);
	return count;
}

static ssize_t show_min_cpus(const struct cluster_data *state, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", state->min_cpus);
}

static ssize_t store_max_cpus(struct cluster_data *state,
		const char *buf, size_t count)
{
	unsigned int value;
	unsigned long flags;
	int ret = parse_uint(buf, count, &value);

	if (ret)
		return ret;
	spin_lock_irqsave(&state_lock, flags);
	state->max_cpus = min(value, state->num_cpus);
	state->min_cpus = min(state->min_cpus, state->max_cpus);
	spin_unlock_irqrestore(&state_lock, flags);
	cpuset_next(state);
	wake_up_core_ctl_thread(state);
	return count;
}

static ssize_t show_max_cpus(const struct cluster_data *state, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", state->max_cpus);
}

static ssize_t store_offline_delay_ms(struct cluster_data *state,
		const char *buf, size_t count)
{
	unsigned int value;
	unsigned long flags;
	int ret = parse_uint(buf, count, &value);

	if (ret)
		return ret;
	spin_lock_irqsave(&state_lock, flags);
	state->offline_delay_ms = value;
	spin_unlock_irqrestore(&state_lock, flags);
	apply_need(state);
	return count;
}

static ssize_t show_task_thres(const struct cluster_data *state, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", state->task_thres);
}

static ssize_t store_task_thres(struct cluster_data *state,
		const char *buf, size_t count)
{
	unsigned int value;
	unsigned long flags;
	int ret = parse_uint(buf, count, &value);

	if (ret)
		return ret;
	if (value < state->num_cpus)
		return -EINVAL;
	spin_lock_irqsave(&state_lock, flags);
	state->task_thres = value;
	spin_unlock_irqrestore(&state_lock, flags);
	apply_need(state);
	return count;
}

static ssize_t show_offline_delay_ms(const struct cluster_data *state,
				     char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", state->offline_delay_ms);
}

static ssize_t store_busy_up_thres(struct cluster_data *state,
		const char *buf, size_t count)
{
	unsigned int values[MAX_CPUS_PER_CLUSTER], i;
	unsigned long flags;
	int n = parse_values(buf, count, values);

	if (n < 0)
		return n;
	if (n != 1 && n != state->num_cpus)
		return -EINVAL;
	spin_lock_irqsave(&state_lock, flags);
	for (i = 0; i < state->num_cpus; i++)
		state->busy_up_thres[i] = values[n == 1 ? 0 : i];
	spin_unlock_irqrestore(&state_lock, flags);
	apply_need(state);
	return count;
}

static ssize_t show_busy_up_thres(const struct cluster_data *state, char *buf)
{
	unsigned long flags;
	unsigned int i;
	int n = 0;

	spin_lock_irqsave(&state_lock, flags);
	for (i = 0; i < state->num_cpus; i++)
		n += scnprintf(buf + n, PAGE_SIZE - n, "%u ", state->busy_up_thres[i]);
	spin_unlock_irqrestore(&state_lock, flags);
	return n + scnprintf(buf + n, PAGE_SIZE - n, "\n");
}

static ssize_t store_busy_down_thres(struct cluster_data *state,
		const char *buf, size_t count)
{
	unsigned int values[MAX_CPUS_PER_CLUSTER], i;
	unsigned long flags;
	int n = parse_values(buf, count, values);

	if (n < 0)
		return n;
	if (n != 1 && n != state->num_cpus)
		return -EINVAL;
	spin_lock_irqsave(&state_lock, flags);
	for (i = 0; i < state->num_cpus; i++)
		state->busy_down_thres[i] = values[n == 1 ? 0 : i];
	spin_unlock_irqrestore(&state_lock, flags);
	apply_need(state);
	return count;
}

static ssize_t show_busy_down_thres(const struct cluster_data *state, char *buf)
{
	unsigned long flags;
	unsigned int i;
	int n = 0;

	spin_lock_irqsave(&state_lock, flags);
	for (i = 0; i < state->num_cpus; i++)
		n += scnprintf(buf + n, PAGE_SIZE - n, "%u ", state->busy_down_thres[i]);
	spin_unlock_irqrestore(&state_lock, flags);
	return n + scnprintf(buf + n, PAGE_SIZE - n, "\n");
}

static ssize_t store_is_big_cluster(struct cluster_data *state,
		const char *buf, size_t count)
{
	unsigned int value;
	unsigned long flags;
	int ret = parse_uint(buf, count, &value);

	if (ret)
		return ret;
	spin_lock_irqsave(&state_lock, flags);
	state->is_big_cluster = !!value;
	spin_unlock_irqrestore(&state_lock, flags);

	return count;
}

static ssize_t show_is_big_cluster(const struct cluster_data *state, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", state->is_big_cluster);
}

static ssize_t store_enable(struct cluster_data *state,
		const char *buf, size_t count)
{
	unsigned int value;
	unsigned long flags;
	bool changed;
	int ret = parse_uint(buf, count, &value);

	if (ret)
		return ret;
	spin_lock_irqsave(&state_lock, flags);
	changed = state->enable != !!value;
	state->enable = !!value;
	spin_unlock_irqrestore(&state_lock, flags);
	if (changed)
		apply_need(state);
	return count;
}

static ssize_t show_enable(const struct cluster_data *state, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", state->enable);
}

static ssize_t show_need_cpus(const struct cluster_data *state, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", state->need_cpus);
}

static ssize_t show_active_cpus(const struct cluster_data *state, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", state->active_cpus);
}

static ssize_t show_global_state(const struct cluster_data *state, char *buf)
{
	struct cpu_data *c;
	struct cluster_data *cluster;
	ssize_t count = 0;
	unsigned int cpu;

	spin_lock_irq(&state_lock);
	for_each_possible_cpu(cpu) {
		c = &per_cpu(cpu_state, cpu);
		cluster = c->cluster;
		if (!cluster || !cluster->inited)
			continue;

		count += snprintf(buf + count, PAGE_SIZE - count,
					"CPU%u\n", cpu);
		count += snprintf(buf + count, PAGE_SIZE - count,
					"\tCPU: %u\n", c->cpu);
		count += snprintf(buf + count, PAGE_SIZE - count,
					"\tOnline: %u\n",
					cpu_online(c->cpu));
		count += snprintf(buf + count, PAGE_SIZE - count,
					"\tIsolated: %u\n",
					cpu_isolated(c->cpu));
		count += snprintf(buf + count, PAGE_SIZE - count,
					"\tFirst CPU: %u\n",
						cluster->first_cpu);
		count += snprintf(buf + count, PAGE_SIZE - count,
					"\tBusy%%: %u\n", c->busy);
		count += snprintf(buf + count, PAGE_SIZE - count,
					"\tIs busy: %u\n", c->is_busy);
		count += snprintf(buf + count, PAGE_SIZE - count,
					"\tNot preferred: %u\n",
						c->not_preferred);
		count += snprintf(buf + count, PAGE_SIZE - count,
					"\tNr running: %u\n", cluster->nrrun);
		count += snprintf(buf + count, PAGE_SIZE - count,
			"\tActive CPUs: %u\n", get_active_cpu_count(cluster));
		count += snprintf(buf + count, PAGE_SIZE - count,
				"\tNeed CPUs: %u\n", cluster->need_cpus);
		count += snprintf(buf + count, PAGE_SIZE - count,
				"\tNr isolated CPUs: %u\n",
						cluster->nr_isolated_cpus);
		count += snprintf(buf + count, PAGE_SIZE - count,
				"\tBoost: %u\n", (unsigned int) cluster->boost);
	}
	spin_unlock_irq(&state_lock);

	return count;
}

static ssize_t store_not_preferred(struct cluster_data *state,
		const char *buf, size_t count)
{
	unsigned int values[MAX_CPUS_PER_CLUSTER], cpu, i = 0, total = 0;
	unsigned long flags;
	int n = parse_values(buf, count, values);

	if (n < 0)
		return n;
	if (n != state->num_cpus)
		return -EINVAL;
	spin_lock_irqsave(&state_lock, flags);
	for_each_cpu(cpu, &state->cpu_mask) {
		per_cpu(cpu_state, cpu).not_preferred = !!values[i];
		total += !!values[i++];
	}
	state->nr_not_preferred_cpus = total;
	spin_unlock_irqrestore(&state_lock, flags);
	return count;
}

static ssize_t show_not_preferred(const struct cluster_data *state, char *buf)
{
	unsigned int cpu;
	unsigned long flags;
	int n = 0;

	spin_lock_irqsave(&state_lock, flags);
	for_each_cpu(cpu, &state->cpu_mask)
		n += scnprintf(buf + n, PAGE_SIZE - n,
#ifdef CONFIG_MSM_CORE_CTL
			"\tCPU:%u %u\n",
#else
			"CPU#%u: %u\n",
#endif
			cpu, per_cpu(cpu_state, cpu).not_preferred);
	spin_unlock_irqrestore(&state_lock, flags);
	return n;
}


#ifdef CONFIG_MSM_CORE_CTL
static ssize_t show_cpus(const struct cluster_data *state, char *buf)
{
	struct cpu_data *c;
	unsigned long flags;
	ssize_t count = 0;

	get_online_cpus();
	spin_lock_irqsave(&state_lock, flags);
	list_for_each_entry(c, &state->lru, sib)
		count += scnprintf(buf + count, PAGE_SIZE - count,
			"CPU%u (%s)\n", c->cpu,
			cpu_online(c->cpu) ? "Online" : "Offline");
	spin_unlock_irqrestore(&state_lock, flags);
	put_online_cpus();
	return count;
}

/* Physical online count is not the isolation controller's active count. */
static ssize_t show_online_cpus(const struct cluster_data *state, char *buf)
{
	unsigned int cpu, online = 0;

	get_online_cpus();
	for_each_cpu(cpu, &state->cpu_mask)
		online += !!cpu_online(cpu);
	put_online_cpus();
	return scnprintf(buf, PAGE_SIZE, "%u\n", online);
}

static ssize_t show_disable(const struct cluster_data *state, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", !READ_ONCE(state->enable));
}

static ssize_t store_disable(struct cluster_data *state,
		const char *buf, size_t count)
{
	char copy[64], inverse[2];
	unsigned int value;
	ssize_t ret;

	if (!count || count >= sizeof(copy) || memchr(buf, '\0', count))
		return -EINVAL;
	memcpy(copy, buf, count);
	copy[count] = '\0';
	if (kstrtouint(strim(copy), 10, &value))
		return -EINVAL;
	inverse[0] = value ? '0' : '1';
	inverse[1] = '\0';
	ret = store_enable(state, inverse, 1);
	return ret < 0 ? ret : count;
}
#endif

struct core_ctl_attr {
	struct attribute attr;
	ssize_t (*show)(const struct cluster_data *, char *);
	ssize_t (*store)(struct cluster_data *, const char *, size_t count);
};

#define core_ctl_attr_ro(_name)		\
static struct core_ctl_attr _name =	\
__ATTR(_name, 0444, show_##_name, NULL)

#define core_ctl_attr_rw(_name)			\
static struct core_ctl_attr _name =		\
__ATTR(_name, 0644, show_##_name, store_##_name)

core_ctl_attr_rw(min_cpus);
core_ctl_attr_rw(max_cpus);
core_ctl_attr_rw(offline_delay_ms);
core_ctl_attr_rw(busy_up_thres);
core_ctl_attr_rw(busy_down_thres);
core_ctl_attr_rw(task_thres);
core_ctl_attr_rw(is_big_cluster);
core_ctl_attr_ro(need_cpus);
core_ctl_attr_ro(active_cpus);
core_ctl_attr_ro(global_state);
core_ctl_attr_rw(not_preferred);
core_ctl_attr_rw(enable);
#ifdef CONFIG_MSM_CORE_CTL
core_ctl_attr_ro(cpus);
core_ctl_attr_ro(online_cpus);
core_ctl_attr_rw(disable);
#endif

static struct attribute *default_attrs[] = {
	&min_cpus.attr,
	&max_cpus.attr,
	&offline_delay_ms.attr,
	&busy_up_thres.attr,
	&busy_down_thres.attr,
	&task_thres.attr,
	&is_big_cluster.attr,
	&enable.attr,
	&need_cpus.attr,
	&active_cpus.attr,
	&global_state.attr,
	&not_preferred.attr,
#ifdef CONFIG_MSM_CORE_CTL
	&cpus.attr, &online_cpus.attr, &disable.attr,
#endif
	NULL
};

#define to_cluster_data(k) container_of(k, struct cluster_data, kobj)
#define to_attr(a) container_of(a, struct core_ctl_attr, attr)
static ssize_t show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct cluster_data *data = to_cluster_data(kobj);
	struct core_ctl_attr *cattr = to_attr(attr);
	ssize_t ret = -EIO;

	if (cattr->show)
		ret = cattr->show(data, buf);

	return ret;
}

static ssize_t store(struct kobject *kobj, struct attribute *attr,
		     const char *buf, size_t count)
{
	struct cluster_data *data = to_cluster_data(kobj);
	struct core_ctl_attr *cattr = to_attr(attr);
	ssize_t ret = -EIO;

	if (!smp_load_acquire(&initialized) || !READ_ONCE(data->inited))
		return -ENODEV;

	if (cattr->store)
		ret = cattr->store(data, buf, count);

	return ret;
}

static const struct sysfs_ops sysfs_ops = {
	.show	= show,
	.store	= store,
};

static void core_ctl_kobj_release(struct kobject *kobj)
{
	struct cluster_data *cluster = to_cluster_data(kobj);

	/* cluster_state is static; completion permits safe init-failure reuse. */
	complete(&cluster->kobj_released);
}

static struct kobj_type ktype_core_ctl = {
	.sysfs_ops	= &sysfs_ops,
	.default_attrs	= default_attrs,
	.release	= core_ctl_kobj_release,
};

/* ==================== runqueue based core count =================== */

static void update_running_avg(void)
{
	int avg, iowait_avg, big_avg;
	int max_nr, big_max_nr;
	struct cluster_data *cluster;
	unsigned int index = 0;
	unsigned long flags;

#ifdef CONFIG_C5_SCHED_AVG_COMPAT
	sched_get_nr_running_avg_native(&avg, &iowait_avg, &big_avg,
					&max_nr, &big_max_nr);
#else
	sched_get_nr_running_avg(&avg, &iowait_avg, &big_avg,
				 &max_nr, &big_max_nr);
#endif
	walt_rotation_checkpoint(big_avg);

	spin_lock_irqsave(&state_lock, flags);
	for_each_cluster(cluster, index) {
		if (!cluster->inited)
			continue;
		cluster->nrrun = cluster->is_big_cluster ? big_avg : avg;
		cluster->max_nr = cluster->is_big_cluster ? big_max_nr : max_nr;
	}
	spin_unlock_irqrestore(&state_lock, flags);
}

#define MAX_NR_THRESHOLD	4
/* adjust needed CPUs based on current runqueue information */
static unsigned int apply_task_need(const struct cluster_data *cluster,
				    unsigned int new_need)
{
	/* unisolate all cores if there are enough tasks */
	if (cluster->nrrun >= cluster->task_thres)
		return cluster->num_cpus;

	/* only unisolate more cores if there are tasks to run */
	if (cluster->nrrun > new_need)
		new_need = new_need + 1;

	/*
	 * We don't want tasks to be overcrowded in a cluster.
	 * If any CPU has more than MAX_NR_THRESHOLD in the last
	 * window, bring another CPU to help out.
	 */
	if (cluster->max_nr > MAX_NR_THRESHOLD)
		new_need = new_need + 1;

	return new_need;
}

/* ======================= load based core count  ====================== */

static unsigned int apply_limits(const struct cluster_data *cluster,
				 unsigned int need_cpus)
{
	return min(max(cluster->min_cpus, need_cpus), cluster->max_cpus);
}

static unsigned int get_active_cpu_count(const struct cluster_data *cluster)
{
	return cluster->num_cpus -
				sched_isolate_count(&cluster->cpu_mask, true);
}

static bool is_active(const struct cpu_data *state)
{
	return cpu_online(state->cpu) && !cpu_isolated(state->cpu);
}

static bool adjustment_possible(const struct cluster_data *cluster,
							unsigned int need)
{
	return (need < cluster->active_cpus || (need > cluster->active_cpus &&
						cluster->nr_isolated_cpus));
}

static bool eval_need(struct cluster_data *cluster)
{
	unsigned long flags;
	struct cpu_data *c;
	unsigned int need_cpus = 0, last_need, thres_idx;
	int ret = 0;
	bool need_flag = false;
	unsigned int new_need;
	s64 now, elapsed;

	if (unlikely(!cluster->inited))
		return 0;

	spin_lock_irqsave(&state_lock, flags);

	if (cluster->boost || !cluster->enable) {
		need_cpus = cluster->max_cpus;
	} else {
		cluster->active_cpus = get_active_cpu_count(cluster);
		thres_idx = cluster->active_cpus ? cluster->active_cpus - 1 : 0;
		list_for_each_entry(c, &cluster->lru, sib) {
			bool old_is_busy = c->is_busy;

			if (c->busy >= cluster->busy_up_thres[thres_idx] ||
			    sched_cpu_high_irqload(c->cpu))
				c->is_busy = true;
			else if (c->busy < cluster->busy_down_thres[thres_idx])
				c->is_busy = false;

			trace_core_ctl_set_busy(c->cpu, c->busy, old_is_busy,
						c->is_busy);
			need_cpus += c->is_busy;
		}
		need_cpus = apply_task_need(cluster, need_cpus);
	}
	new_need = apply_limits(cluster, need_cpus);
	need_flag = adjustment_possible(cluster, new_need);

	last_need = cluster->need_cpus;
	now = ktime_to_ms(ktime_get());

	if (new_need > cluster->active_cpus) {
		ret = 1;
	} else {
		/*
		 * When there is no change in need and there are no more
		 * active CPUs than currently needed, just update the
		 * need time stamp and return.
		 */
		if (new_need == last_need && new_need == cluster->active_cpus) {
			cluster->need_ts = now;
			spin_unlock_irqrestore(&state_lock, flags);
			return 0;
		}

		elapsed =  now - cluster->need_ts;
		ret = elapsed >= cluster->offline_delay_ms;
	}

	if (ret) {
		cluster->need_ts = now;
		cluster->need_cpus = new_need;
	}
	trace_core_ctl_eval_need(cluster->first_cpu, last_need, new_need,
				 ret && need_flag);
	spin_unlock_irqrestore(&state_lock, flags);

	return ret && need_flag;
}

static void apply_need(struct cluster_data *cluster)
{
	if (!smp_load_acquire(&initialized) || !READ_ONCE(cluster->inited))
		return;
	if (eval_need(cluster))
		wake_up_core_ctl_thread(cluster);
}

/* ========================= core count enforcement ==================== */

static void wake_up_core_ctl_thread(struct cluster_data *cluster)
{
	unsigned long flags;

	if (!smp_load_acquire(&initialized) || !READ_ONCE(cluster->inited))
		return;

	spin_lock_irqsave(&cluster->pending_lock, flags);
	cluster->pending = true;
	spin_unlock_irqrestore(&cluster->pending_lock, flags);
	wake_up_process(cluster->core_ctl_thread);
}

static u64 core_ctl_check_timestamp;

int core_ctl_set_boost(bool boost)
{
	unsigned int index = 0;
	struct cluster_data *cluster = NULL;
	unsigned long flags;
	int ret = 0;
	bool changed = false;

	if (!smp_load_acquire(&initialized))
		return -ENODEV;
	spin_lock_irqsave(&state_lock, flags);
	if (!num_clusters) {
		ret = -ENODEV;
		goto out;
	}
	/* Validate every group before modifying any reference. */
	for_each_cluster(cluster, index) {
		if (!cluster->inited || (boost && cluster->boost == UINT_MAX) ||
		    (!boost && !cluster->boost)) {
			ret = !cluster->inited ? -ENODEV : boost ? -EOVERFLOW : -EINVAL;
			goto out;
		}
	}
	index = 0;
	for_each_cluster(cluster, index) {
		if (boost) {
			changed |= cluster->boost == 0;
			cluster->boost++;
		} else {
			cluster->boost--;
			changed |= cluster->boost == 0;
		}
	}
out:
	spin_unlock_irqrestore(&state_lock, flags);
	if (!ret && changed) {
		index = 0;
		for_each_cluster(cluster, index)
			apply_need(cluster);
	}
	if (cluster)
		trace_core_ctl_set_boost(READ_ONCE(cluster->boost), ret);
	return ret;
}
EXPORT_SYMBOL(core_ctl_set_boost);

void core_ctl_check(u64 window_start)
{
	int cpu;
	struct cpu_data *c;
	struct cluster_data *cluster;
	unsigned int index = 0;
	unsigned long flags;

	if (unlikely(!smp_load_acquire(&initialized)))
		return;

	if (window_start == core_ctl_check_timestamp)
		return;

	core_ctl_check_timestamp = window_start;

	spin_lock_irqsave(&state_lock, flags);
	for_each_possible_cpu(cpu) {

		c = &per_cpu(cpu_state, cpu);
		cluster = c->cluster;

		if (!cluster || !cluster->inited)
			continue;

		c->busy = sched_get_cpu_util(cpu);
	}
	spin_unlock_irqrestore(&state_lock, flags);

	update_running_avg();

	for_each_cluster(cluster, index) {
		if (eval_need(cluster))
			wake_up_core_ctl_thread(cluster);
	}
}

static void move_cpu_lru(struct cpu_data *cpu_data)
{
	unsigned long flags;

	spin_lock_irqsave(&state_lock, flags);
	list_del(&cpu_data->sib);
	list_add_tail(&cpu_data->sib, &cpu_data->cluster->lru);
	spin_unlock_irqrestore(&state_lock, flags);
}

#ifdef CONFIG_SCHED_CORE_ROTATE
static void cpuset_next(struct cluster_data *cluster)
{
	int cpus_needed = cluster->num_cpus - cluster->min_cpus;

	cluster->set_cur++;
	cluster->set_cur = min(cluster->set_cur, cluster->set_max);

	/*
	 * This loop generates bit sets from 0 to pow(num_cpus, 2) - 1.
	 * We start loop from set_cur to set_cur - 1 and break when weight of
	 * set_cur equals to cpus_needed.
	 */

	while (1) {
		if (bitmap_weight(&cluster->set_cur, BITS_PER_LONG) ==
		    cpus_needed) {
			break;
		}
		cluster->set_cur++;
		cluster->set_cur = min(cluster->set_cur, cluster->set_max);
		if (cluster->set_cur == cluster->set_max)
			/* roll over */
			cluster->set_cur = 0;
	};

	pr_debug("first_cpu=%d cpus_needed=%d set_cur=0x%lx\n",
		 cluster->first_cpu, cpus_needed, cluster->set_cur);
}

static bool should_we_isolate(int cpu, struct cluster_data *cluster)
{
	/* cpu should be part of cluster */
	return !!(cluster->set_cur & (1 << (cpu - cluster->first_cpu)));
}

static void core_ctl_resume(void)
{
	unsigned int i = 0;
	struct cluster_data *cluster;

	/* move to next isolation cpu set */
	for_each_cluster(cluster, i)
		cpuset_next(cluster);
}

static struct syscore_ops core_ctl_syscore_ops = {
	.resume	= core_ctl_resume,
};

#else

static void cpuset_next(struct cluster_data *cluster) { }

static bool should_we_isolate(int cpu, struct cluster_data *cluster)
{
	return true;
}

#endif

static void try_to_isolate(struct cluster_data *cluster, unsigned int need)
{
	struct cpu_data *c, *tmp;
	unsigned long flags;
	unsigned int num_cpus = cluster->num_cpus;
	unsigned int nr_isolated = 0;
	bool first_pass = cluster->nr_not_preferred_cpus;

	/*
	 * Protect against entry being removed (and added at tail) by other
	 * thread (hotplug).
	 */
	spin_lock_irqsave(&state_lock, flags);
	list_for_each_entry_safe(c, tmp, &cluster->lru, sib) {
		if (!num_cpus--)
			break;

		if (!is_active(c))
			continue;
		if (cluster->active_cpus == need)
			break;
		/* Don't isolate busy CPUs. */
		if (c->is_busy)
			continue;

		/*
		 * We isolate only the not_preferred CPUs. If none
		 * of the CPUs are selected as not_preferred, then
		 * all CPUs are eligible for isolation.
		 */
		if (cluster->nr_not_preferred_cpus && !c->not_preferred)
			continue;

		if (!should_we_isolate(c->cpu, cluster))
			continue;

		spin_unlock_irqrestore(&state_lock, flags);

		pr_debug("Trying to isolate CPU%u\n", c->cpu);
		if (!core_ctl_isolate(c->cpu)) {
			c->isolated_by_us = true;
			move_cpu_lru(c);
			nr_isolated++;
		} else {
			pr_debug("Unable to isolate CPU%u\n", c->cpu);
		}
		cluster->active_cpus = get_active_cpu_count(cluster);
		spin_lock_irqsave(&state_lock, flags);
	}
	cluster->nr_isolated_cpus += nr_isolated;
	spin_unlock_irqrestore(&state_lock, flags);

again:
	/*
	 * If the number of active CPUs is within the limits, then
	 * don't force isolation of any busy CPUs.
	 */
	if (cluster->active_cpus <= cluster->max_cpus)
		return;

	nr_isolated = 0;
	num_cpus = cluster->num_cpus;
	spin_lock_irqsave(&state_lock, flags);
	list_for_each_entry_safe(c, tmp, &cluster->lru, sib) {
		if (!num_cpus--)
			break;

		if (!is_active(c))
			continue;
		if (cluster->active_cpus <= cluster->max_cpus)
			break;

		if (first_pass && !c->not_preferred)
			continue;

		spin_unlock_irqrestore(&state_lock, flags);

		pr_debug("Trying to isolate CPU%u\n", c->cpu);
		if (!core_ctl_isolate(c->cpu)) {
			c->isolated_by_us = true;
			move_cpu_lru(c);
			nr_isolated++;
		} else {
			pr_debug("Unable to isolate CPU%u\n", c->cpu);
		}
		cluster->active_cpus = get_active_cpu_count(cluster);
		spin_lock_irqsave(&state_lock, flags);
	}
	cluster->nr_isolated_cpus += nr_isolated;
	spin_unlock_irqrestore(&state_lock, flags);

	if (first_pass && cluster->active_cpus > cluster->max_cpus) {
		first_pass = false;
		goto again;
	}
}

static void __try_to_unisolate(struct cluster_data *cluster,
			       unsigned int need, bool force)
{
	struct cpu_data *c, *tmp;
	unsigned long flags;
	unsigned int num_cpus = cluster->num_cpus;
	unsigned int nr_unisolated = 0;

	/*
	 * Protect against entry being removed (and added at tail) by other
	 * thread (hotplug).
	 */
	spin_lock_irqsave(&state_lock, flags);
	list_for_each_entry_safe(c, tmp, &cluster->lru, sib) {
		if (!num_cpus--)
			break;

		if (!c->isolated_by_us)
			continue;
		if ((cpu_online(c->cpu) && !cpu_isolated(c->cpu)) ||
			(!force && c->not_preferred))
			continue;
		if (cluster->active_cpus == need)
			break;

		spin_unlock_irqrestore(&state_lock, flags);

		pr_debug("Trying to unisolate CPU%u\n", c->cpu);
		if (!core_ctl_unisolate(c->cpu)) {
			c->isolated_by_us = false;
			move_cpu_lru(c);
			nr_unisolated++;
		} else {
			pr_debug("Unable to unisolate CPU%u\n", c->cpu);
		}
		cluster->active_cpus = get_active_cpu_count(cluster);
		spin_lock_irqsave(&state_lock, flags);
	}
	cluster->nr_isolated_cpus -= nr_unisolated;
	spin_unlock_irqrestore(&state_lock, flags);
}

static void try_to_unisolate(struct cluster_data *cluster, unsigned int need)
{
	bool force_use_non_preferred = false;

	__try_to_unisolate(cluster, need, force_use_non_preferred);

	if (cluster->active_cpus == need)
		return;

	force_use_non_preferred = true;
	__try_to_unisolate(cluster, need, force_use_non_preferred);
}

static void __ref do_core_ctl(struct cluster_data *cluster)
{
	unsigned int need;

	if (!smp_load_acquire(&initialized) || !READ_ONCE(cluster->inited))
		return;

	need = apply_limits(cluster, cluster->need_cpus);

	if (adjustment_possible(cluster, need)) {
		pr_debug("Trying to adjust group %u from %u to %u\n",
				cluster->first_cpu, cluster->active_cpus, need);

		if (cluster->active_cpus > need)
			try_to_isolate(cluster, need);
		else if (cluster->active_cpus < need)
			try_to_unisolate(cluster, need);
	}
}

static int __ref try_core_ctl(void *data)
{
	struct cluster_data *cluster = data;
	unsigned long flags;

	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		spin_lock_irqsave(&cluster->pending_lock, flags);
		if (!cluster->pending) {
			spin_unlock_irqrestore(&cluster->pending_lock, flags);
			schedule();
			if (kthread_should_stop())
				break;
			spin_lock_irqsave(&cluster->pending_lock, flags);
		}
		set_current_state(TASK_RUNNING);
		cluster->pending = false;
		spin_unlock_irqrestore(&cluster->pending_lock, flags);

		if (kthread_should_stop())
			break;
		do_core_ctl(cluster);
	}

	return 0;
}

static int isolation_cpuhp_state(unsigned int cpu,  bool online)
{
	struct cpu_data *state = &per_cpu(cpu_state, cpu);
	struct cluster_data *cluster = state->cluster;
	unsigned int need;
	bool do_wakeup = false, unisolated = false;
	unsigned long flags;

	if (unlikely(!cluster || !cluster->inited))
		return 0;

	if (online) {
		cluster->active_cpus = get_active_cpu_count(cluster);

		/*
		 * Moving to the end of the list should only happen in
		 * CPU_ONLINE and not on CPU_UP_PREPARE to prevent an
		 * infinite list traversal when thermal (or other entities)
		 * reject trying to online CPUs.
		 */
		move_cpu_lru(state);
	} else {
		/*
		 * We don't want to have a CPU both offline and isolated.
		 * So unisolate a CPU that went down if it was isolated by us.
		 */
		if (state->isolated_by_us) {
			sched_unisolate_cpu_unlocked(cpu);
			state->isolated_by_us = false;
			unisolated = true;
		}

		/* Move a CPU to the end of the LRU when it goes offline. */
		move_cpu_lru(state);

		state->busy = 0;
		cluster->active_cpus = get_active_cpu_count(cluster);
	}

	need = apply_limits(cluster, cluster->need_cpus);
	spin_lock_irqsave(&state_lock, flags);
	if (unisolated)
		cluster->nr_isolated_cpus--;
	do_wakeup = adjustment_possible(cluster, need);
	spin_unlock_irqrestore(&state_lock, flags);
	if (do_wakeup)
		wake_up_core_ctl_thread(cluster);

	return 0;
}

static int core_ctl_isolation_online_cpu(unsigned int cpu)
{
	return isolation_cpuhp_state(cpu, true);
}

static int core_ctl_isolation_dead_cpu(unsigned int cpu)
{
	return isolation_cpuhp_state(cpu, false);
}

/* ============================ init code ============================== */

static cpumask_var_t core_ctl_disable_cpumask;
static bool core_ctl_disable_cpumask_present;

static int __init core_ctl_disable_setup(char *str)
{
	if (!*str)
		return -EINVAL;

	alloc_bootmem_cpumask_var(&core_ctl_disable_cpumask);

	if (cpulist_parse(str, core_ctl_disable_cpumask) < 0) {
		free_bootmem_cpumask_var(core_ctl_disable_cpumask);
		return -EINVAL;
	}

	core_ctl_disable_cpumask_present = true;
	pr_info("disable_cpumask=%*pbl\n",
			cpumask_pr_args(core_ctl_disable_cpumask));

	return 0;
}
early_param("core_ctl_disable_cpumask", core_ctl_disable_setup);

static bool should_skip(const struct cpumask *mask)
{
	if (!core_ctl_disable_cpumask_present)
		return false;

	/*
	 * We operate on a cluster basis. Disable the core_ctl for
	 * a cluster, if all of it's cpus are specified in
	 * core_ctl_disable_cpumask
	 */
	return cpumask_subset(mask, core_ctl_disable_cpumask);
}

static struct cluster_data *find_cluster_by_first_cpu(unsigned int first_cpu)
{
	unsigned int i;

	for (i = 0; i < num_clusters; ++i) {
		if (cluster_state[i].first_cpu == first_cpu)
			return &cluster_state[i];
	}

	return NULL;
}

static int cluster_init(const struct cpumask *mask)
{
	struct device *dev;
	unsigned int first_cpu = cpumask_first(mask), count = cpumask_weight(mask);
	struct cluster_data *cluster;
	struct cpu_data *state;
	unsigned int cpu, i;
	int ret;
	struct sched_param param = { .sched_priority = MAX_RT_PRIO-1 };

	if (should_skip(mask))
		return 0;
	cluster = find_cluster_by_first_cpu(first_cpu);
	if (cluster)
		return cpumask_equal(mask, &cluster->cpu_mask) ? 0 : -EINVAL;
	if (!count || count > MAX_CPUS_PER_CLUSTER || num_clusters >= MAX_CLUSTERS)
		return -EINVAL;
	for (i = 0; i < num_clusters; i++)
		if (cpumask_intersects(mask, &cluster_state[i].cpu_mask))
			return -EINVAL;
	dev = get_cpu_device(first_cpu);
	if (!dev)
		return -ENODEV;
	cluster = &cluster_state[num_clusters++];
	cpumask_copy(&cluster->cpu_mask, mask);
	cluster->num_cpus = count;
	cluster->first_cpu = first_cpu;
	cluster->min_cpus = 1;
	cluster->max_cpus = count;
	cluster->need_cpus = count;
	cluster->offline_delay_ms = 100;
	cluster->task_thres = UINT_MAX;
	cluster->nrrun = count;
#ifdef CONFIG_SCHED_CORE_ROTATE
	cluster->set_max = count * count;
	cluster->set_cur = cluster->set_max - 1;
#endif
	cluster->enable = true;
	INIT_LIST_HEAD(&cluster->lru);
	spin_lock_init(&cluster->pending_lock);
	for_each_cpu(cpu, mask) {
		state = &per_cpu(cpu_state, cpu);
		state->cluster = cluster;
		state->cpu = cpu;
		list_add_tail(&state->sib, &cluster->lru);
	}
	cluster->active_cpus = get_active_cpu_count(cluster);
	cluster->core_ctl_thread = kthread_run(try_core_ctl, cluster,
			"core_ctl/%d", first_cpu);
	if (IS_ERR(cluster->core_ctl_thread)) {
		ret = PTR_ERR(cluster->core_ctl_thread);
		cluster->core_ctl_thread = NULL;
		return ret;
	}
	ret = sched_setscheduler_nocheck(cluster->core_ctl_thread, SCHED_FIFO, &param);
	if (ret)
		return ret;
	init_completion(&cluster->kobj_released);
	kobject_init(&cluster->kobj, &ktype_core_ctl);
	cluster->kobj_initialized = true;
	return kobject_add(&cluster->kobj, &dev->kobj, "core_ctl");
}

/* Failed initialization never published policy or isolated any CPU. */
static void cleanup_core_ctl_init(void)
{
	unsigned int i, cpu, count = num_clusters;
	unsigned long flags;

	smp_store_release(&initialized, false);
	for (i = 0; i < count; i++)
		WRITE_ONCE(cluster_state[i].inited, false);
	if (dead_hp_registered) {
		cpuhp_remove_state_nocalls(CPUHP_CORE_CTL_ISOLATION_DEAD);
		dead_hp_registered = false;
	}
	if (online_hp_state >= 0) {
		cpuhp_remove_state_nocalls(online_hp_state);
		online_hp_state = -1;
	}
#ifdef CONFIG_SCHED_CORE_ROTATE
	if (rotate_ops_registered) {
		unregister_syscore_ops(&core_ctl_syscore_ops);
		rotate_ops_registered = false;
	}
#endif
	for (i = 0; i < count; i++) {
		struct cluster_data *cluster = &cluster_state[i];

		if (cluster->core_ctl_thread) {
			kthread_stop(cluster->core_ctl_thread);
			cluster->core_ctl_thread = NULL;
		}
		if (cluster->kobj_initialized) {
			kobject_put(&cluster->kobj);
			wait_for_completion(&cluster->kobj_released);
			cluster->kobj_initialized = false;
		}
		for_each_cpu(cpu, &cluster->cpu_mask)
			memset(&per_cpu(cpu_state, cpu), 0, sizeof(struct cpu_data));
	}
	spin_lock_irqsave(&state_lock, flags);
	num_clusters = 0;
	spin_unlock_irqrestore(&state_lock, flags);
	memset(cluster_state, 0, sizeof(cluster_state));
}

static int __init core_ctl_init(void)
{
	unsigned int cpu, i;
	struct cpumask cpus = *cpu_possible_mask;
	int ret;

	if (should_skip(cpu_possible_mask)) {
		init_skipped = true;
		return 0;
	}
	for_each_cpu(cpu, &cpus) {
		const struct cpumask *mask = cpu_coregroup_mask(cpu);

		ret = cluster_init(mask);
		if (ret)
			goto fail;
		cpumask_andnot(&cpus, &cpus, mask);
	}
	if (!num_clusters) {
		ret = -ENODEV;
		goto fail;
	}
#ifdef CONFIG_SCHED_CORE_ROTATE
	register_syscore_ops(&core_ctl_syscore_ops);
	rotate_ops_registered = true;
#endif
	ret = cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN,
			"core_ctl/isolation:online", core_ctl_isolation_online_cpu, NULL);
	if (ret < 0)
		goto fail;
	online_hp_state = ret;
	ret = cpuhp_setup_state_nocalls(CPUHP_CORE_CTL_ISOLATION_DEAD,
			"core_ctl/isolation:dead", NULL, core_ctl_isolation_dead_cpu);
	if (ret < 0)
		goto fail;
	dead_hp_registered = true;
	get_online_cpus();
	for (i = 0; i < num_clusters; i++) {
		cluster_state[i].active_cpus = get_active_cpu_count(&cluster_state[i]);
		WRITE_ONCE(cluster_state[i].inited, true);
	}
	smp_store_release(&initialized, true);
	put_online_cpus();
	return 0;
fail:
	init_error = ret;
	cleanup_core_ctl_init();
	return ret;
}

late_initcall(core_ctl_init);

#ifdef CONFIG_MSM_CORE_CTL
static int c5_state_get(char *buf, const struct kernel_param *kp)
{
	struct cluster_data *c;
	unsigned long flags;
	unsigned int i, threads = 0, kobjects = 0;
	int n;

	spin_lock_irqsave(&state_lock, flags);
	for (i = 0; i < num_clusters; i++) {
		threads += cluster_state[i].core_ctl_thread != NULL;
		kobjects += cluster_state[i].kobj_initialized;
	}
	n = scnprintf(buf, PAGE_SIZE,
		"v1 ready=%u error=%d skipped=%u groups=%u threads=%u kobjects=%u "
		"hp_online=%u hp_dead=%u isolate_calls=%llu isolate_failed=%llu "
		"unisolate_calls=%llu unisolate_failed=%llu last_isolate=%d last_unisolate=%d\n",
		initialized, init_error, init_skipped, num_clusters, threads, kobjects,
		online_hp_state >= 0, dead_hp_registered,
		(unsigned long long)isolate_calls, (unsigned long long)isolate_failures,
		(unsigned long long)unisolate_calls, (unsigned long long)unisolate_failures,
		last_isolate_error, last_unisolate_error);
	for (i = 0; i < num_clusters; i++) {
		c = &cluster_state[i];
		n += scnprintf(buf + n, PAGE_SIZE - n,
			"g%u=%u,%u,%u,%u,%u,%u,%u,%u,%u\n", i,
			c->first_cpu, c->num_cpus, c->min_cpus, c->max_cpus,
			c->need_cpus, c->active_cpus, c->nr_isolated_cpus,
			c->boost, c->enable);
	}
	spin_unlock_irqrestore(&state_lock, flags);
	return n;
}
static const struct kernel_param_ops c5_state_ops = { .get = c5_state_get };
module_param_cb(c5_state, &c5_state_ops, NULL, 0400);
#endif
