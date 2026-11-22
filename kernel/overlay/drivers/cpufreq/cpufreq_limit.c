/*
 * Samsung CPU-frequency limit compatibility for the fixed C5 profile.
 * Derived from drivers/cpufreq/cpufreq_limit.c and kernel/power/main.c:
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 * Minsung Kim <ms925.kim@samsung.com>
 * Chiwoong Byun <woong.byun@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2.
 *
 * Clock requests, hybrid units and legacy nodes are supported. The native
 * scheduler boost interface is deliberately never called by this provider.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/cpufreq_limit.h>
#include <linux/kobject.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/err.h>
#include <linux/sched.h>
#include <linux/moduleparam.h>

struct cpufreq_limit_handle {
	struct list_head node;
	unsigned long min, max;
	char label[20];
};

struct limit_params {
	unsigned long little_cpu_start, little_cpu_end;
	unsigned long big_cpu_start, big_cpu_end;
	unsigned long big_min_freq, big_max_freq, big_off_freq;
	unsigned long little_min_freq, little_max_freq, little_min_lock;
	unsigned long little_divider;
};

static struct limit_params params = {
	.little_cpu_start = 4, .little_cpu_end = 7,
	.big_cpu_start = 0, .big_cpu_end = 3,
	.big_min_freq = 960000, .big_max_freq = 1516800,
	.big_off_freq = 499200,
	.little_min_freq = 201600, .little_max_freq = 604800,
	.little_min_lock = 403200, .little_divider = 2,
};
static LIST_HEAD(limit_requests);
/* No callback takes update_lock: policy refresh invokes our notifier. */
static DEFINE_MUTEX(update_lock);
static DEFINE_SPINLOCK(limit_lock);
static bool limit_ready, notifier_added, limit_group_added, power_group_added;
static struct kobject *limit_parent, *power_parent;
static struct cpufreq_frequency_table *registered_tables[NR_CPUS];
static unsigned int generation, handle_count;
static int init_error, last_apply_error;
static u64 get_count, put_count, replace_count, policy_updates, policy_errors;
static u64 policy_callbacks, rejected_controls;

static bool little_cpu(const struct limit_params *p, unsigned int cpu)
{
	return cpu >= p->little_cpu_start && cpu <= p->little_cpu_end;
}

static bool params_valid(const struct limit_params *p)
{
	unsigned long cpu;

	if (p->little_cpu_start > p->little_cpu_end ||
	    p->big_cpu_start > p->big_cpu_end ||
	    p->little_cpu_end >= nr_cpu_ids || p->big_cpu_end >= nr_cpu_ids)
		return false;
	if (!(p->little_cpu_end < p->big_cpu_start ||
	      p->big_cpu_end < p->little_cpu_start))
		return false;
	for (cpu = p->little_cpu_start; cpu <= p->little_cpu_end; cpu++)
		if (!cpu_present(cpu))
			return false;
	for (cpu = p->big_cpu_start; cpu <= p->big_cpu_end; cpu++)
		if (!cpu_present(cpu))
			return false;
	if (!p->little_divider || p->little_divider >= 12 ||
	    p->big_min_freq > p->big_max_freq ||
	    p->big_off_freq > p->big_max_freq ||
	    p->little_min_freq > p->little_max_freq ||
	    p->little_min_lock > p->little_max_freq)
		return false;
	if (p->big_max_freq > UINT_MAX || p->little_max_freq > UINT_MAX ||
	    p->big_min_freq > UINT_MAX / p->little_divider ||
	    p->little_min_lock > UINT_MAX / p->little_divider)
		return false;
	return true;
}

static void translate_limits(const struct limit_params *p,
		const struct cpufreq_policy *policy, unsigned long *floor,
		unsigned long *ceiling)
{
#ifdef CONFIG_CPU_FREQ_LIMIT_HMP
	if (p->little_divider != 1) {
		if (little_cpu(p, policy->cpu)) {
			*floor = *floor >= p->big_min_freq ?
				p->little_min_lock * p->little_divider :
				*floor * p->little_divider;
			*ceiling = *ceiling >= p->big_min_freq ?
				policy->cpuinfo.max_freq :
				*ceiling * p->little_divider;
		} else {
			if (*floor < p->big_min_freq)
				*floor = p->big_off_freq;
			if (*ceiling < p->big_min_freq)
				*ceiling = p->big_off_freq;
		}
	}
#endif
}

static int limit_notifier(struct notifier_block *nb, unsigned long event,
		void *data)
{
	struct cpufreq_policy *policy = data;
	struct cpufreq_limit_handle *r;
	unsigned long flags, floor = 0, ceiling = ULONG_MAX;
	struct limit_params p;
	bool any = false;

	if (event != CPUFREQ_ADJUST)
		return NOTIFY_OK;
	spin_lock_irqsave(&limit_lock, flags);
	policy_callbacks++;
	p = params;
	list_for_each_entry(r, &limit_requests, node) {
		floor = max(floor, r->min);
		if (r->max)
			ceiling = min(ceiling, r->max);
		any = true;
	}
	spin_unlock_irqrestore(&limit_lock, flags);
	if (!any || (!floor && ceiling == ULONG_MAX))
		return NOTIFY_OK;
	if (!floor) {
		floor = policy->cpuinfo.min_freq;
#ifdef CONFIG_CPU_FREQ_LIMIT_HMP
		if (little_cpu(&p, policy->cpu))
			floor /= p.little_divider;
#endif
	}
	if (ceiling == ULONG_MAX) {
		ceiling = policy->cpuinfo.max_freq;
#ifdef CONFIG_CPU_FREQ_LIMIT_HMP
		if (little_cpu(&p, policy->cpu))
			ceiling /= p.little_divider;
#endif
	}
	translate_limits(&p, policy, &floor, &ceiling);
	/* Run last, preserve existing caps, and let a cap win over a floor. */
	ceiling = min(ceiling, (unsigned long)policy->max);
	floor = min(floor, ceiling);
	cpufreq_verify_within_limits(policy, floor, ceiling);
	return NOTIFY_OK;
}

static struct notifier_block limit_nb = {
	.notifier_call = limit_notifier,
	.priority = INT_MIN,
};

/* Called with update_lock; callbacks only take limit_lock. */
static int refresh_policies(void)
{
	struct cpufreq_policy *p;
	cpumask_t visited;
	unsigned int cpu;
	unsigned long flags;
	int error = 0, ret, refreshed = 0;

	cpumask_clear(&visited);
	get_online_cpus();
	for_each_online_cpu(cpu) {
		if (cpumask_test_cpu(cpu, &visited))
			continue;
		p = cpufreq_cpu_get(cpu);
		if (!p)
			continue;
		cpumask_or(&visited, &visited, p->related_cpus);
		ret = cpufreq_update_policy(cpu);
		cpufreq_cpu_put(p);
		refreshed++;
		spin_lock_irqsave(&limit_lock, flags);
		policy_updates++;
		if (ret)
			policy_errors++;
		spin_unlock_irqrestore(&limit_lock, flags);
		if (ret && !error)
			error = ret;
	}
	put_online_cpus();
	return error ? error : refreshed ? 0 : -ENODEV;
}

static bool request_present(struct cpufreq_limit_handle *needle)
{
	struct cpufreq_limit_handle *r;

	list_for_each_entry(r, &limit_requests, node)
		if (r == needle)
			return true;
	return false;
}

/* Up to two replacements, with complete software rollback and refresh. */
static int commit_requests(struct cpufreq_limit_handle **old,
		struct cpufreq_limit_handle **new, unsigned int count,
		bool *removed_min)
{
	struct list_head *previous[2] = { NULL, NULL };
	unsigned long flags;
	unsigned int i;
	int ret, rollback;

	if (!count || count > 2 || (count == 2 && old[0] && old[0] == old[1]))
		return -EINVAL;
	*removed_min = false;
	mutex_lock(&update_lock);
	spin_lock_irqsave(&limit_lock, flags);
	if (!limit_ready) {
		ret = -ENODEV;
		goto refuse;
	}
	for (i = 0; i < count; i++)
		if (old[i] && !request_present(old[i])) {
			ret = -EINVAL;
			goto refuse;
		}
	for (i = 0; i < count; i++) {
		if (old[i]) {
			previous[i] = old[i]->node.prev;
			*removed_min |= old[i]->min != 0;
			list_del_init(&old[i]->node);
			handle_count--;
		}
	}
	/* Detach all old nodes first: a rollback predecessor must never be new. */
	for (i = 0; i < count; i++) {
		if (new[i]) {
			list_add_tail(&new[i]->node, &limit_requests);
			handle_count++;
		}
	}
	spin_unlock_irqrestore(&limit_lock, flags);
	ret = refresh_policies();
	if (ret) {
		spin_lock_irqsave(&limit_lock, flags);
		for (i = 0; i < count; i++)
			if (new[i]) {
				list_del_init(&new[i]->node);
				handle_count--;
			}
		for (i = count; i > 0; i--)
			if (old[i - 1]) {
				list_add(&old[i - 1]->node, previous[i - 1]);
				handle_count++;
			}
		spin_unlock_irqrestore(&limit_lock, flags);
		rollback = refresh_policies();
		if (rollback)
			ret = -EIO;
	}
	spin_lock_irqsave(&limit_lock, flags);
	last_apply_error = ret;
	spin_unlock_irqrestore(&limit_lock, flags);
	mutex_unlock(&update_lock);
	return ret;
refuse:
	spin_unlock_irqrestore(&limit_lock, flags);
	mutex_unlock(&update_lock);
	return ret;
}

static struct cpufreq_limit_handle *new_request(unsigned long floor,
		unsigned long ceiling, char *label)
{
	struct cpufreq_limit_handle *r;

	if (!label || floor > UINT_MAX || ceiling > UINT_MAX ||
	    (ceiling && ceiling < floor))
		return ERR_PTR(-EINVAL);
	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return ERR_PTR(-ENOMEM);
	r->min = floor;
	r->max = ceiling;
	strlcpy(r->label, label, sizeof(r->label));
	INIT_LIST_HEAD(&r->node);
	return r;
}

static void notify_min_release(void)
{
	unsigned int cpu;

	get_online_cpus();
	for_each_online_cpu(cpu)
		atomic_notifier_call_chain(&load_alert_notifier_head, 0,
				(void *)(long)cpu);
	put_online_cpus();
}

struct cpufreq_limit_handle *cpufreq_limit_get(unsigned long floor,
		unsigned long ceiling, char *label)
{
	struct cpufreq_limit_handle *old[1] = { NULL }, *new[1];
	unsigned long flags;
	bool removed;
	int ret;

	new[0] = new_request(floor, ceiling, label);
	if (IS_ERR(new[0]))
		return new[0];
	ret = commit_requests(old, new, 1, &removed);
	if (ret) {
		kfree(new[0]);
		return ERR_PTR(ret);
	}
	spin_lock_irqsave(&limit_lock, flags);
	get_count++;
	spin_unlock_irqrestore(&limit_lock, flags);
	return new[0];
}
EXPORT_SYMBOL(cpufreq_limit_get);

int cpufreq_limit_put(struct cpufreq_limit_handle *handle, int release)
{
	struct cpufreq_limit_handle *old[1] = { handle }, *new[1] = { NULL };
	unsigned long flags;
	bool removed;
	int ret;

	if (IS_ERR_OR_NULL(handle))
		return -EINVAL;
	ret = commit_requests(old, new, 1, &removed);
	if (ret)
		return ret;
	kfree(handle);
	spin_lock_irqsave(&limit_lock, flags);
	put_count++;
	spin_unlock_irqrestore(&limit_lock, flags);
	if (release && removed)
		notify_min_release();
	return 0;
}
EXPORT_SYMBOL(cpufreq_limit_put);

void cpufreq_limit_set_table(int cpu, struct cpufreq_frequency_table *table)
{
	unsigned long flags;

	if (cpu < 0 || cpu >= nr_cpu_ids || !cpu_present(cpu))
		return;
	/* As in the old API, the supplier owns table lifetime until cleared. */
	spin_lock_irqsave(&limit_lock, flags);
	registered_tables[cpu] = table;
	generation++;
	spin_unlock_irqrestore(&limit_lock, flags);
}
EXPORT_SYMBOL(cpufreq_limit_set_table);

static int append_table(char *buf, int count,
		struct cpufreq_frequency_table *table,
		const struct limit_params *p, bool little)
{
	unsigned int i, n, freq;
	int written;

	if (!table)
		return -ENODEV;
	for (n = 0; n < 1024 && table[n].frequency != CPUFREQ_TABLE_END; n++)
		;
	if (n == 1024)
		return -EOVERFLOW;
	for (i = n; i > 0; i--) {
		freq = table[i - 1].frequency;
		/* Check the sentinel before dividing it into a bogus frequency. */
		if (freq == CPUFREQ_ENTRY_INVALID)
			continue;
#ifdef CONFIG_CPU_FREQ_LIMIT_HMP
		if (little)
			freq /= p->little_divider;
		if (freq < (little ? p->little_min_freq : p->big_min_freq))
			continue;
#endif
		written = snprintf(buf + count, PAGE_SIZE - count, "%u ", freq);
		if (written < 0 || written >= PAGE_SIZE - count)
			return -EOVERFLOW;
		count += written;
	}
	return count;
}

ssize_t cpufreq_limit_get_table(char *buf)
{
	struct cpufreq_policy *big, *little = NULL;
	struct cpufreq_policy *first, *second = NULL;
	struct cpufreq_frequency_table *table;
	struct limit_params p;
	unsigned long flags;
	unsigned int gen;
	int count, ret;

	spin_lock_irqsave(&limit_lock, flags);
	p = params;
	gen = generation;
	spin_unlock_irqrestore(&limit_lock, flags);
	get_online_cpus();
	big = cpufreq_cpu_get(p.big_cpu_start);
	if (!big) {
		ret = -ENODEV;
		goto online;
	}
#ifdef CONFIG_CPU_FREQ_LIMIT_HMP
	if (p.little_divider != 1) {
		little = cpufreq_cpu_get(p.little_cpu_start);
		if (!little) {
			ret = -ENODEV;
			goto big_put;
		}
	}
#endif
	/* A concurrent range change must not reverse the policy lock order. */
	first = big;
	if (little && little != big) {
		first = big->cpu < little->cpu ? big : little;
		second = first == big ? little : big;
	}
	down_read(&first->rwsem);
	if (second)
		down_read(&second->rwsem);
	spin_lock_irqsave(&limit_lock, flags);
	if (gen != generation) {
		ret = -EAGAIN;
		goto unlock;
	}
	table = registered_tables[p.big_cpu_start];
	count = append_table(buf, 0, table ? table : big->freq_table, &p, false);
	if (count >= 0 && little) {
		table = registered_tables[p.little_cpu_start];
		count = append_table(buf, count,
				table ? table : little->freq_table, &p, true);
	}
	if (count <= 0) {
		ret = count < 0 ? count : -ENODATA;
		goto unlock;
	}
	buf[count - 1] = '\n';
	buf[count] = '\0';
	ret = count;
unlock:
	spin_unlock_irqrestore(&limit_lock, flags);
	if (second)
		up_read(&second->rwsem);
	up_read(&first->rwsem);
	if (little)
		cpufreq_cpu_put(little);
big_put:
	cpufreq_cpu_put(big);
online:
	put_online_cpus();
	return ret;
}
EXPORT_SYMBOL(cpufreq_limit_get_table);

static ssize_t show_requests(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	struct cpufreq_limit_handle *r;
	unsigned long flags;
	int count = 0, n;

	spin_lock_irqsave(&limit_lock, flags);
	list_for_each_entry(r, &limit_requests, node) {
		n = snprintf(buf + count, PAGE_SIZE - count, "%s\t%lu\t%lu\n",
				r->label, r->min, r->max);
		if (n < 0 || n >= PAGE_SIZE - count) {
			count = -EOVERFLOW;
			break;
		}
		count += n;
	}
	spin_unlock_irqrestore(&limit_lock, flags);
	return count;
}
static struct kobj_attribute requests_attr =
	__ATTR(cpufreq_limit_requests, 0444, show_requests, NULL);

static int parse_ulong_value(const char *buf, size_t count, unsigned long *value)
{
	char copy[64];

	if (!count || count >= sizeof(copy) || memchr(buf, '\0', count))
		return -EINVAL;
	memcpy(copy, buf, count);
	copy[count] = '\0';
	return kstrtoul(strim(copy), 10, value);
}

#ifdef CONFIG_CPU_FREQ_LIMIT_HMP
static ssize_t store_params(struct limit_params *next, size_t count)
{
	struct limit_params previous;
	unsigned long flags;
	bool active;
	int ret = 0, rollback;

	if (!params_valid(next))
		return -EINVAL;
	/* Caller owns update_lock, so a prepared struct cannot lose another write. */
	spin_lock_irqsave(&limit_lock, flags);
	if (!limit_ready) {
		spin_unlock_irqrestore(&limit_lock, flags);
		return -ENODEV;
	}
	previous = params;
	active = handle_count != 0;
	params = *next;
	generation++;
	spin_unlock_irqrestore(&limit_lock, flags);
	if (active)
		ret = refresh_policies();
	if (ret) {
		spin_lock_irqsave(&limit_lock, flags);
		params = previous;
		generation++;
		spin_unlock_irqrestore(&limit_lock, flags);
		rollback = refresh_policies();
		if (rollback)
			ret = -EIO;
	}
	spin_lock_irqsave(&limit_lock, flags);
	last_apply_error = ret;
	spin_unlock_irqrestore(&limit_lock, flags);
	return ret ? ret : count;
}

#define ULONG_ATTR(name) \
static ssize_t show_##name(struct kobject *kobj, \
		struct kobj_attribute *attr, char *buf) \
{ \
	unsigned long flags, value; \
	spin_lock_irqsave(&limit_lock, flags); \
	value = params.name; \
	spin_unlock_irqrestore(&limit_lock, flags); \
	return snprintf(buf, PAGE_SIZE, "%lu\n", value); \
} \
static ssize_t store_##name(struct kobject *kobj, \
		struct kobj_attribute *attr, const char *buf, size_t count) \
{ \
	struct limit_params next; \
	unsigned long flags, value; \
	ssize_t ret = parse_ulong_value(buf, count, &value); \
	if (ret) return ret; \
	mutex_lock(&update_lock); \
	spin_lock_irqsave(&limit_lock, flags); \
	next = params; \
	spin_unlock_irqrestore(&limit_lock, flags); \
	next.name = value; \
	ret = store_params(&next, count); \
	mutex_unlock(&update_lock); \
	return ret; \
} \
static struct kobj_attribute name##_attr = \
	__ATTR(name, 0644, show_##name, store_##name)

ULONG_ATTR(big_min_freq);
ULONG_ATTR(big_max_freq);
ULONG_ATTR(big_off_freq);
ULONG_ATTR(little_min_freq);
ULONG_ATTR(little_max_freq);
ULONG_ATTR(little_min_lock);
ULONG_ATTR(little_divider);

static ssize_t show_cpu_range(char *buf, bool little)
{
	struct limit_params p;
	unsigned long flags;

	spin_lock_irqsave(&limit_lock, flags);
	p = params;
	spin_unlock_irqrestore(&limit_lock, flags);
	return snprintf(buf, PAGE_SIZE, "%lu-%lu\n",
		little ? p.little_cpu_start : p.big_cpu_start,
		little ? p.little_cpu_end : p.big_cpu_end);
}

static ssize_t store_cpu_range(const char *buf, size_t count, bool little)
{
	char copy[64], *start, *dash;
	struct limit_params next;
	unsigned long first, last, flags;
	ssize_t ret;

	if (!count || count >= sizeof(copy) || memchr(buf, '\0', count))
		return -EINVAL;
	memcpy(copy, buf, count);
	copy[count] = '\0';
	start = strim(copy);
	dash = strchr(start, '-');
	if (!dash || strchr(dash + 1, '-'))
		return -EINVAL;
	*dash++ = '\0';
	if (kstrtoul(start, 10, &first) || kstrtoul(dash, 10, &last))
		return -EINVAL;
	mutex_lock(&update_lock);
	spin_lock_irqsave(&limit_lock, flags);
	next = params;
	spin_unlock_irqrestore(&limit_lock, flags);
	if (little) {
		next.little_cpu_start = first;
		next.little_cpu_end = last;
	} else {
		next.big_cpu_start = first;
		next.big_cpu_end = last;
	}
	ret = store_params(&next, count);
	mutex_unlock(&update_lock);
	return ret;
}

#define RANGE_ATTR(name, is_little) \
static ssize_t show_##name(struct kobject *kobj, \
		struct kobj_attribute *attr, char *buf) \
{ return show_cpu_range(buf, is_little); } \
static ssize_t store_##name(struct kobject *kobj, \
		struct kobj_attribute *attr, const char *buf, size_t count) \
{ return store_cpu_range(buf, count, is_little); } \
static struct kobj_attribute name##_attr = \
	__ATTR(name, 0644, show_##name, store_##name)
RANGE_ATTR(little_cpu_num, true);
RANGE_ATTR(big_cpu_num, false);

static ssize_t show_hmp_boost_type(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "0\n");
}
static ssize_t store_hmp_boost_type(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	unsigned long value, flags;
	int ret = parse_ulong_value(buf, count, &value);

	if (ret)
		return ret;
	if (!value)
		return count;
	spin_lock_irqsave(&limit_lock, flags);
	rejected_controls++;
	spin_unlock_irqrestore(&limit_lock, flags);
	return value <= 2 ? -EOPNOTSUPP : -EINVAL;
}
static struct kobj_attribute hmp_boost_type_attr =
	__ATTR(hmp_boost_type, 0644, show_hmp_boost_type, store_hmp_boost_type);
#endif

static struct attribute *limit_attrs[] = {
	&requests_attr.attr,
#ifdef CONFIG_CPU_FREQ_LIMIT_HMP
	&little_cpu_num_attr.attr, &big_cpu_num_attr.attr,
	&big_min_freq_attr.attr, &big_max_freq_attr.attr, &big_off_freq_attr.attr,
	&little_min_freq_attr.attr, &little_max_freq_attr.attr,
	&little_min_lock_attr.attr, &little_divider_attr.attr,
	&hmp_boost_type_attr.attr,
#endif
	NULL,
};
static struct attribute_group limit_attr_group = {
	.name = "cpufreq_limit", .attrs = limit_attrs,
};

#ifdef CONFIG_CPU_FREQ_LIMIT_USERSPACE
static DEFINE_MUTEX(client_lock);
enum client_id { USER_MIN, USER_MAX, TOUCH_MIN, FINGER_MIN, CLIENT_COUNT };
static struct cpufreq_limit_handle *clients[CLIENT_COUNT];
static int user_min = -1, user_max = -1;

/* Caller owns client_lock. Allocation precedes one all-or-nothing replacement. */
static int replace_clients(unsigned int mask, unsigned long floor,
		unsigned long ceiling, bool release, bool *released_min)
{
	static char *labels[] = { "user lock(min)", "user lock(max)",
		"touch min", "finger min" };
	struct cpufreq_limit_handle *old[2] = { NULL, NULL };
	struct cpufreq_limit_handle *new[2] = { NULL, NULL };
	unsigned int ids[2], n = 0, id, i;
	unsigned long flags;
	int ret = 0;

	*released_min = false;
	for (id = 0; id < CLIENT_COUNT; id++) {
		if (!(mask & (1U << id)))
			continue;
		if (n == 2) {
			ret = -EINVAL;
			goto free_new;
		}
		ids[n] = id;
		old[n] = clients[id];
		if (!release) {
			new[n] = new_request(floor, ceiling, labels[id]);
			if (IS_ERR(new[n])) {
				ret = PTR_ERR(new[n]);
				new[n] = NULL;
				goto free_new;
			}
		}
		n++;
	}
	if (!n)
		return 0;
	ret = commit_requests(old, new, n, released_min);
	if (ret)
		goto free_new;
	for (i = 0; i < n; i++) {
		clients[ids[i]] = new[i];
		kfree(old[i]);
	}
	spin_lock_irqsave(&limit_lock, flags);
	replace_count++;
	spin_unlock_irqrestore(&limit_lock, flags);
	return 0;
free_new:
	for (i = 0; i < 2; i++)
		kfree(new[i]);
	return ret;
}

int set_freq_limit(unsigned long id, unsigned int freq)
{
	unsigned int mask = 0;
	bool release = !freq || freq == UINT_MAX, removed;
	int ret;

	/* Preserve the old bit-mask dispatch, including its existing aliases. */
	if (id & DVFS_TOUCH_ID)
		mask |= 1U << TOUCH_MIN;
	if (id & DVFS_FINGER_ID)
		mask |= 1U << FINGER_MIN;
	mutex_lock(&client_lock);
	ret = replace_clients(mask, freq, 0, release, &removed);
	mutex_unlock(&client_lock);
	if (!ret && release && removed)
		notify_min_release();
	return ret;
}
EXPORT_SYMBOL(set_freq_limit);

static ssize_t show_table(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return cpufreq_limit_get_table(buf);
}
static ssize_t store_table(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	return -EINVAL;
}
static struct kobj_attribute table_attr =
	__ATTR(cpufreq_table, 0644, show_table, store_table);

static ssize_t show_user_limit(char *buf, bool maximum)
{
	int value;

	mutex_lock(&client_lock);
	value = maximum ? user_max : user_min;
	mutex_unlock(&client_lock);
	return snprintf(buf, PAGE_SIZE, "%d\n", value);
}

static ssize_t store_user_limit(const char *buf, size_t count, bool maximum)
{
	char copy[64];
	int value, ret;
	bool release, removed;

	if (!count || count >= sizeof(copy) || memchr(buf, '\0', count))
		return -EINVAL;
	memcpy(copy, buf, count);
	copy[count] = '\0';
	if (kstrtoint(strim(copy), 10, &value) || value < -1)
		return -EINVAL;
	release = value == -1 || (!maximum && value == 0);
	mutex_lock(&client_lock);
	ret = replace_clients(1U << (maximum ? USER_MAX : USER_MIN),
		maximum || release ? 0 : value,
		!maximum || release ? 0 : value, release, &removed);
	if (!ret) {
		if (maximum)
			user_max = release ? -1 : value;
		else
			user_min = release ? -1 : value;
	}
	mutex_unlock(&client_lock);
	if (!ret && !maximum && release && removed)
		notify_min_release();
	return ret ? ret : count;
}

#define USER_ATTR(name, maximum) \
static ssize_t show_##name(struct kobject *kobj, \
		struct kobj_attribute *attr, char *buf) \
{ return show_user_limit(buf, maximum); } \
static ssize_t store_##name(struct kobject *kobj, \
		struct kobj_attribute *attr, const char *buf, size_t count) \
{ return store_user_limit(buf, count, maximum); } \
static struct kobj_attribute name##_attr = \
	__ATTR(name, 0644, show_##name, store_##name)
USER_ATTR(cpufreq_min_limit, false);
USER_ATTR(cpufreq_max_limit, true);
static struct attribute *power_attrs[] = {
	&table_attr.attr, &cpufreq_min_limit_attr.attr,
	&cpufreq_max_limit_attr.attr, NULL,
};
static struct attribute_group power_attr_group = { .attrs = power_attrs };
#endif

static int c5_state_get(char *buf, const struct kernel_param *kp)
{
	unsigned long flags;
	int count;

	spin_lock_irqsave(&limit_lock, flags);
	count = scnprintf(buf, PAGE_SIZE,
		"v1 ready=%u init_error=%d notifier=%u group=%u power=%u "
		"handles=%u get=%llu put=%llu replace=%llu updates=%llu "
		"failed=%llu callbacks=%llu rejected=%llu apply_error=%d migration_boost=0\n",
		limit_ready, init_error, notifier_added, limit_group_added,
		power_group_added, handle_count, (unsigned long long)get_count,
		(unsigned long long)put_count, (unsigned long long)replace_count,
		(unsigned long long)policy_updates, (unsigned long long)policy_errors,
		(unsigned long long)policy_callbacks,
		(unsigned long long)rejected_controls, last_apply_error);
	spin_unlock_irqrestore(&limit_lock, flags);
	return count;
}
static const struct kernel_param_ops state_ops = { .get = c5_state_get };
module_param_cb(c5_state, &state_ops, NULL, 0400);

static void remove_groups(void)
{
#ifdef CONFIG_CPU_FREQ_LIMIT_USERSPACE
	if (power_group_added) {
		sysfs_remove_group(power_parent, &power_attr_group);
		power_group_added = false;
	}
#endif
	if (limit_group_added) {
		sysfs_remove_group(limit_parent, &limit_attr_group);
		limit_group_added = false;
	}
	if (power_parent) {
		kobject_put(power_parent);
		power_parent = NULL;
	}
	if (limit_parent) {
		kobject_put(limit_parent);
		limit_parent = NULL;
	}
}

static int __init cpufreq_limit_init(void)
{
	unsigned long flags;
	int ret;

	if (!params_valid(&params) || !cpufreq_global_kobject) {
		init_error = -ENODEV;
		return init_error;
	}
	limit_parent = kobject_get(cpufreq_global_kobject);
#ifdef CONFIG_CPU_FREQ_LIMIT_USERSPACE
	if (!power_kobj) {
		ret = -ENODEV;
		goto fail;
	}
	power_parent = kobject_get(power_kobj);
#endif
	ret = cpufreq_register_notifier(&limit_nb, CPUFREQ_POLICY_NOTIFIER);
	if (ret)
		goto fail;
	notifier_added = true;
	ret = sysfs_create_group(limit_parent, &limit_attr_group);
	if (ret)
		goto fail;
	limit_group_added = true;
#ifdef CONFIG_CPU_FREQ_LIMIT_USERSPACE
	ret = sysfs_create_group(power_parent, &power_attr_group);
	if (ret)
		goto fail;
	power_group_added = true;
#endif
	spin_lock_irqsave(&limit_lock, flags);
	limit_ready = true;
	spin_unlock_irqrestore(&limit_lock, flags);
	return 0;
fail:
	remove_groups();
	if (notifier_added) {
		cpufreq_unregister_notifier(&limit_nb, CPUFREQ_POLICY_NOTIFIER);
		notifier_added = false;
	}
	init_error = ret;
	return ret;
}
late_initcall(cpufreq_limit_init);
MODULE_DESCRIPTION("C5 checked Samsung CPU clock-limit compatibility");
MODULE_LICENSE("GPL v2");
