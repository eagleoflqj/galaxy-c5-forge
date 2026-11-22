/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _C5_INPUT_BOOSTER_H_
#define _C5_INPUT_BOOSTER_H_
#include <linux/input/c5-input-config.h>
#include <linux/input/c5-input-event.h>
#include <linux/input/c5-input-phase.h>
#include <linux/input/c5-input-frequency.h>
#include <linux/input/c5-input-hmp.h>
#include <linux/input/mt.h>
#include <linux/hashtable.h>
#include <linux/of.h>
#include <linux/reboot.h>
#include <linux/workqueue.h>

struct c5_input_profile {
	struct c5_input_row *rows;
	unsigned int nr;
};

struct c5_input_frontend;
struct c5_input_timer {
	struct delayed_work work;
	struct c5_input_frontend *frontend;
};

struct c5_input_frontend {
	const char *name;
	unsigned int type;
	unsigned int level;
	struct mutex lock;
	struct c5_input_phase phase;
	struct c5_input_timer timer[2];
	struct device *device;
	u64 active;
	bool control;
	bool group_created;
	int last_error;
};

struct c5_input_context {
	struct hlist_node node;
	struct input_dev *dev;
	struct c5_input_event_state state;
	u32 published[C5_GENRE_COUNT];
	unsigned int activity;
};

static struct c5_input_frontend c5_frontends[C5_GENRE_COUNT] = {
	[C5_TOUCH] = {.name = "touch", .type = 2, .level = 2},
	[C5_MULTITOUCH] = {.name = "multitouch", .type = 3, .level = 1},
	[C5_KEY] = {.name = "key", .type = 0, .level = 1},
	[C5_TOUCHKEY] = {.name = "touchkey", .type = 1, .level = 1},
	[C5_KEYBOARD] = {.name = "keyboard", .type = 4, .level = 1},
	[C5_MOUSE] = {.name = "mouse", .type = 5, .level = 1},
	[C5_WHEEL] = {.name = "mouse_wheel", .type = 6, .level = 1},
	[C5_PEN] = {.name = "pen", .type = 7, .level = 1},
	[C5_HOVER] = {.name = "hover", .type = 7, .level = 1},
};

static struct c5_input_profile c5_profiles[8];
static DEFINE_MUTEX(c5_input_config_lock);
static DEFINE_MUTEX(c5_input_devices_lock);
static DEFINE_SPINLOCK(c5_input_events_lock);
static DEFINE_HASHTABLE(c5_input_devices, 4);
static struct c5_input_pending c5_input_events;
static struct workqueue_struct *c5_input_queue;
static struct class *c5_input_class;
static unsigned int c5_input_class_files;
static unsigned int c5_input_debug;
static bool c5_input_ready;
static bool c5_input_initialized;
static bool c5_input_notifier_registered;
static struct c5_input_frequency c5_frequency_votes = {
	.lock = __MUTEX_INITIALIZER(c5_frequency_votes.lock),
};
static struct c5_input_hmp c5_hmp_votes = {
	.lock = __MUTEX_INITIALIZER(c5_hmp_votes.lock),
};

static unsigned int c5_input_frontend_id(struct c5_input_frontend *frontend)
{
	return frontend - c5_frontends;
}

static void c5_input_backend(struct c5_input_frontend *frontend,
			     const struct c5_input_param *param)
{
	unsigned int genre = c5_input_frontend_id(frontend);
	int hmp = c5_input_hmp_vote(&c5_hmp_votes, genre,
				   param ? param->hmp_boost : 0);
	int freq = c5_input_frequency_vote(&c5_frequency_votes, genre,
					   param ? param->cpu_freq : 0);

	frontend->last_error = hmp ? hmp : freq;
	if (frontend->last_error)
		pr_warn_ratelimited("input booster %s provider error: %d\n",
				    frontend->name, frontend->last_error);
	if (READ_ONCE(c5_input_debug))
		pr_info_ratelimited("input booster %s request %u time %u result %d\n",
			frontend->name, param ? param->cpu_freq : 0,
			param ? param->time : 0, frontend->last_error);
}

static void c5_input_apply(void *owner, const struct c5_input_param *param)
{
	c5_input_backend(owner, param);
}

static void c5_input_remove(void *owner)
{
	c5_input_backend(owner, NULL);
}

static bool c5_input_timer_pending(void *owner, unsigned int slot)
{
	struct c5_input_frontend *frontend = owner;

	return delayed_work_pending(&frontend->timer[slot].work);
}

static void c5_input_timer_cancel(void *owner, unsigned int slot)
{
	struct c5_input_frontend *frontend = owner;

	cancel_delayed_work(&frontend->timer[slot].work);
}

static void c5_input_timer_schedule(void *owner, unsigned int slot,
				   unsigned int ms)
{
	struct c5_input_frontend *frontend = owner;
	unsigned long flags;

	spin_lock_irqsave(&c5_input_events_lock, flags);
	if (c5_input_ready)
		queue_delayed_work(c5_input_queue, &frontend->timer[slot].work,
				   msecs_to_jiffies(ms));
	spin_unlock_irqrestore(&c5_input_events_lock, flags);
}

static void c5_input_timeout(struct work_struct *work)
{
	struct c5_input_timer *timer = container_of(to_delayed_work(work),
						   struct c5_input_timer, work);
	struct c5_input_frontend *frontend = timer->frontend;

	mutex_lock(&frontend->lock);
	if (READ_ONCE(c5_input_ready) && frontend->level)
		c5_input_phase_timeout(&frontend->phase);
	mutex_unlock(&frontend->lock);
}

/* A snapshot contains final held counts plus whether any new press occurred.
 * Even a complete click before this worker runs produces ON then OFF. */
static void c5_input_frontend_event(struct c5_input_frontend *frontend,
				   bool activity)
{
	bool held = frontend->active || frontend->control;
	bool was_active = frontend->phase.count != 0;

	frontend->phase.count = held;
	if (!frontend->level)
		return;
	if (activity)
		c5_input_phase_event(&frontend->phase, true);
	if (!held && (was_active || activity))
		c5_input_phase_event(&frontend->phase, false);
}

static void c5_input_apply_events(struct work_struct *work);
static DECLARE_WORK(c5_input_event_work, c5_input_apply_events);

static void c5_input_apply_events(struct work_struct *work)
{
	unsigned int batch;

	/* Bound one execution so a sustained input stream cannot starve timers
	 * or a synchronous sysfs control request on the same ordered queue. */
	for (batch = 0; batch < 16; batch++) {
		struct c5_input_snapshot snapshot;
		unsigned long flags;
		bool have;
		unsigned int i;

		spin_lock_irqsave(&c5_input_events_lock, flags);
		have = c5_input_ready &&
			c5_input_pending_take(&c5_input_events, &snapshot);
		spin_unlock_irqrestore(&c5_input_events_lock, flags);
		if (!have)
			return;
		for (i = 0; i < C5_GENRE_COUNT; i++) {
			struct c5_input_frontend *frontend = &c5_frontends[i];

			mutex_lock(&frontend->lock);
			if (READ_ONCE(c5_input_ready)) {
				frontend->active = snapshot.count[i];
				c5_input_frontend_event(frontend,
					!!(snapshot.activity & (1U << i)));
			}
			mutex_unlock(&frontend->lock);
		}
	}
	/* While executing, the pending bit is clear, so this queues one later
	 * execution.  IRQ publishers leave armed=true until that execution. */
	{
		unsigned long flags;

		spin_lock_irqsave(&c5_input_events_lock, flags);
		if (c5_input_ready)
			queue_work(c5_input_queue, &c5_input_event_work);
		spin_unlock_irqrestore(&c5_input_events_lock, flags);
	}
}

static void c5_input_publish(struct c5_input_context *context)
{
	unsigned long flags;
	int queue = 0;

	spin_lock_irqsave(&c5_input_events_lock, flags);
	if (c5_input_ready)
		queue = c5_input_pending_publish(&c5_input_events,
			context->published, context->state.count, context->activity);
	if (queue >= 0) {
		memcpy(context->published, context->state.count,
		       sizeof(context->published));
		context->activity = 0;
	}
	if (queue > 0)
		queue_work(c5_input_queue, &c5_input_event_work);
	spin_unlock_irqrestore(&c5_input_events_lock, flags);
	WARN_ON_ONCE(queue < 0);
}

/* Called under dev->event_lock, before releasing the raw input event.  The
 * registry is RCU-owned; queued work never retains an input_dev/context. */
static void c5_input_capture(struct input_dev *dev, unsigned int type,
			      unsigned int code, int value)
{
	struct c5_input_context *context;

	if (!READ_ONCE(c5_input_ready))
		return;
	if (type == EV_KEY && (code > KEY_MAX || !test_bit(code, dev->keybit)))
		return;
	if (type == EV_MSC && (code > MSC_MAX || !test_bit(code, dev->mscbit)))
		return;
	if (type == EV_REL && (code > REL_MAX || !test_bit(code, dev->relbit)))
		return;
	rcu_read_lock();
	hash_for_each_possible_rcu(c5_input_devices, context, node,
				   (unsigned long)dev) {
		u32 before[C5_GENRE_COUNT];
		unsigned int i;

		if (context->dev != dev)
			continue;
		memcpy(before, context->state.count, sizeof(before));
		context->activity |= c5_input_event_value(&context->state,
							 type, code, value);
		if (type == EV_ABS && code == ABS_MT_TRACKING_ID && dev->mt) {
			unsigned int contacts = 0;

			for (i = 0; i < dev->mt->num_slots; i++)
				contacts += input_mt_is_active(&dev->mt->slots[i]);
			c5_input_event_mt(&context->state, contacts);
		}
		for (i = 0; i < C5_GENRE_COUNT; i++)
			if (context->state.count[i] > before[i])
				context->activity |= 1U << i;
		if (type == EV_SYN && code == SYN_REPORT)
			c5_input_publish(context);
		break;
	}
	rcu_read_unlock();
}

/* Called from the input registration path while input_mutex is held. */
static int c5_input_device_add(struct input_dev *dev)
{
	struct c5_input_context *context;
	unsigned long flags;

	if (!READ_ONCE(c5_input_ready))
		return 0;
	context = kzalloc(sizeof(*context), GFP_KERNEL);
	if (!context)
		return -ENOMEM;
	context->dev = dev;
	context->state.has_pen = test_bit(BTN_TOOL_PEN, dev->keybit);
	context->state.has_mt = dev->mt &&
		test_bit(ABS_MT_TRACKING_ID, dev->absbit);
	mutex_lock(&c5_input_devices_lock);
	spin_lock_irqsave(&c5_input_events_lock, flags);
	if (!c5_input_ready) {
		spin_unlock_irqrestore(&c5_input_events_lock, flags);
		mutex_unlock(&c5_input_devices_lock);
		kfree(context);
		return 0;
	}
	hash_add_rcu(c5_input_devices, &context->node, (unsigned long)dev);
	spin_unlock_irqrestore(&c5_input_events_lock, flags);
	mutex_unlock(&c5_input_devices_lock);
	return 0;
}

static void c5_input_device_remove(struct input_dev *dev)
{
	struct c5_input_context *context, *found = NULL;
	unsigned long flags;

	mutex_lock(&c5_input_devices_lock);
	hash_for_each_possible(c5_input_devices, context, node, (unsigned long)dev) {
		if (context->dev != dev)
			continue;
		spin_lock_irqsave(&dev->event_lock, flags);
		hash_del_rcu(&context->node);
		memset(context->state.count, 0, sizeof(context->state.count));
		context->activity = 0;
		c5_input_publish(context);
		spin_unlock_irqrestore(&dev->event_lock, flags);
		found = context;
		break;
	}
	if (found) {
		synchronize_rcu();
		kfree(found);
	}
	mutex_unlock(&c5_input_devices_lock);
}

static int c5_input_dt_parse(void)
{
	static const char * const properties[] = {
		"input_booster,levels", "input_booster,cpu_freqs",
		"input_booster,bimc_freqs", "input_booster,hmp_boost",
		"input_booster,head_times", "input_booster,tail_times",
	};
	struct device_node *parent, *child;
	unsigned int seen = 0;
	int ret = -ENODEV;

	parent = of_find_compatible_node(NULL, NULL, "input_booster");
	if (!parent)
		return ret;
	if (!of_device_is_available(parent))
		goto out_parent;
	for_each_available_child_of_node(parent, child) {
		struct c5_input_profile *profile;
		const char *label;
		u32 type;
		int nr, i, j;

		ret = of_property_read_u32(child, "input_booster,type", &type);
		if (ret || type >= 8 || (seen & (1U << type))) {
			ret = -EINVAL;
			goto out_child;
		}
		ret = of_property_read_string(child, "input_booster,label", &label);
		if (ret || !label[0]) {
			ret = -EINVAL;
			goto out_child;
		}
		nr = of_property_count_u32_elems(child, properties[0]);
		if (nr <= 0 || nr > C5_INPUT_MAX_ROWS) {
			ret = -EINVAL;
			goto out_child;
		}
		for (i = 1; i < ARRAY_SIZE(properties); i++)
			if (of_property_count_u32_elems(child, properties[i]) != nr) {
				ret = -EINVAL;
				goto out_child;
			}
		profile = &c5_profiles[type];
		profile->rows = kcalloc(nr, sizeof(*profile->rows), GFP_KERNEL);
		if (!profile->rows) {
			ret = -ENOMEM;
			goto out_child;
		}
		profile->nr = nr;
		for (i = 0; i < nr; i++) {
			struct c5_input_row *row = &profile->rows[i];
			u32 values[ARRAY_SIZE(properties)];

			for (j = 0; j < ARRAY_SIZE(properties); j++) {
				ret = of_property_read_u32_index(child, properties[j],
							       i, &values[j]);
				if (ret)
					goto out_child;
			}
			*row = (struct c5_input_row) {
				.level = values[0], .cpu_freq = values[1],
				.bimc_freq = values[2], .hmp_boost = values[3],
				.head_time = values[4], .tail_time = values[5],
			};
			ret = c5_input_validate_row(row);
			if (ret)
				goto out_child;
			for (j = 0; j < i; j++)
				if (profile->rows[j].level == row->level) {
					ret = -EINVAL;
					goto out_child;
				}
		}
		seen |= 1U << type;
	}
	ret = seen == 255 ? 0 : -EINVAL;
	goto out_parent;
out_child:
	of_node_put(child);
out_parent:
	of_node_put(parent);
	return ret;
}

/* Config callers hold c5_input_config_lock.  Every genre sharing a DT table
 * is staged before publication, then all affected mutexes are held for swap. */
static int c5_input_replace_rows(unsigned int type, struct c5_input_row *rows)
{
	struct c5_input_profile *profile = &c5_profiles[type];
	struct c5_input_param staged[C5_GENRE_COUNT][2];
	struct c5_input_row *old;
	unsigned int i;
	int ret;

	for (i = 0; i < C5_GENRE_COUNT; i++)
		if (c5_frontends[i].type == type) {
			ret = c5_input_select_params(rows, profile->nr,
				c5_frontends[i].level, staged[i], NULL);
			if (ret)
				return ret;
		}
	for (i = 0; i < C5_GENRE_COUNT; i++)
		if (c5_frontends[i].type == type)
			mutex_lock(&c5_frontends[i].lock);
	old = profile->rows;
	profile->rows = rows;
	for (i = 0; i < C5_GENRE_COUNT; i++)
		if (c5_frontends[i].type == type)
			memcpy(c5_frontends[i].phase.param, staged[i], sizeof(staged[i]));
	for (i = C5_GENRE_COUNT; i > 0; i--)
		if (c5_frontends[i - 1].type == type)
			mutex_unlock(&c5_frontends[i - 1].lock);
	kfree(old);
	return 0;
}

static int c5_input_set_level(struct c5_input_frontend *frontend, u32 level)
{
	struct c5_input_profile *profile = &c5_profiles[frontend->type];
	struct c5_input_param params[2];
	int ret;

	if (level == C5_INPUT_KEEP)
		return 0; /* Legacy signed level=-1 is a no-change request. */
	ret = c5_input_select_params(profile->rows, profile->nr, level, params, NULL);
	if (ret)
		return ret;
	mutex_lock(&frontend->lock);
	frontend->level = level;
	memcpy(frontend->phase.param, params, sizeof(params));
	if (!level) {
		c5_input_timer_cancel(frontend, 0);
		c5_input_timer_cancel(frontend, 1);
		frontend->phase.press = false;
		frontend->phase.index = 0;
		frontend->control = false;
		frontend->phase.count = !!frontend->active;
		c5_input_remove(frontend);
		ret = frontend->last_error;
	}
	mutex_unlock(&frontend->lock);
	return ret;
}

static ssize_t c5_input_class_show(struct class *class,
				  struct class_attribute *attr, char *buf)
{
	struct c5_input_frontend *frontend = &c5_frontends[C5_TOUCH];
	struct c5_input_param params[2];
	int phase, ret;

	mutex_lock(&c5_input_config_lock);
	if (!READ_ONCE(c5_input_ready)) {
		ret = -ENODEV;
	} else if (!strcmp(attr->attr.name, "debug_level")) {
		ret = scnprintf(buf, PAGE_SIZE, "%u\n", c5_input_debug);
	} else if (!strcmp(attr->attr.name, "level")) {
		ret = scnprintf(buf, PAGE_SIZE, "%u\n", frontend->level);
	} else {
		struct c5_input_profile *profile = &c5_profiles[frontend->type];

		ret = c5_input_select_params(profile->rows, profile->nr,
					    frontend->level, params, NULL);
		phase = !strcmp(attr->attr.name, "tail");
		if (!ret)
			ret = scnprintf(buf, PAGE_SIZE, "%u %u %u %u\n",
				params[phase].time, params[phase].cpu_freq,
				params[phase].bimc_freq, params[phase].hmp_boost);
	}
	mutex_unlock(&c5_input_config_lock);
	return ret;
}

static ssize_t c5_input_class_store(struct class *class,
				   struct class_attribute *attr,
				   const char *buf, size_t count)
{
	struct c5_input_frontend *frontend = &c5_frontends[C5_TOUCH];
	u32 fields[4];
	bool level = !strcmp(attr->attr.name, "level");
	bool debug = !strcmp(attr->attr.name, "debug_level");
	int ret = c5_input_parse_fields(buf, count, level || debug ? 1 : 4,
				       level ? 1 : 0, fields);

	if (ret)
		return ret;
	mutex_lock(&c5_input_config_lock);
	if (!READ_ONCE(c5_input_ready)) {
		ret = -ENODEV;
	} else if (debug) {
		if (fields[0] != C5_INPUT_KEEP)
			WRITE_ONCE(c5_input_debug, fields[0]);
	} else if (level) {
		ret = c5_input_set_level(frontend, fields[0]);
	} else {
		struct c5_input_profile *profile = &c5_profiles[frontend->type];
		struct c5_input_param params[2];
		struct c5_input_row values = {
			.cpu_freq = fields[1], .bimc_freq = fields[2],
			.hmp_boost = fields[3], .head_time = fields[0],
			.tail_time = fields[0],
		};
		struct c5_input_row *copy;
		bool tail = !strcmp(attr->attr.name, "tail");
		int selected[2];

		ret = c5_input_select_params(profile->rows, profile->nr,
					    frontend->level, params, selected);
		if (!ret && selected[tail] < 0)
			ret = -ENODATA;
		if (!ret) {
			copy = kmemdup(profile->rows, profile->nr * sizeof(*copy),
					GFP_KERNEL);
			if (!copy) {
				ret = -ENOMEM;
			} else {
				ret = c5_input_patch_row(&copy[selected[tail]],
					C5_ROW_CPU | C5_ROW_BIMC | C5_ROW_HMP |
					(tail ? C5_ROW_TAIL : C5_ROW_HEAD), &values);
				if (!ret)
					ret = c5_input_replace_rows(frontend->type, copy);
				if (ret)
					kfree(copy);
			}
		}
	}
	mutex_unlock(&c5_input_config_lock);
	return ret ? ret : count;
}

static struct class_attribute c5_input_class_attrs[] = {
	__ATTR(debug_level, 0644, c5_input_class_show, c5_input_class_store),
	__ATTR(head, 0644, c5_input_class_show, c5_input_class_store),
	__ATTR(tail, 0644, c5_input_class_show, c5_input_class_store),
	__ATTR(level, 0644, c5_input_class_show, c5_input_class_store),
};

static ssize_t c5_input_device_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct c5_input_frontend *frontend = dev_get_drvdata(dev);
	struct c5_input_profile *profile = &c5_profiles[frontend->type];
	struct c5_input_param params[2];
	int selected[2], ret, length = 0;
	unsigned int phase;

	mutex_lock(&c5_input_config_lock);
	if (!READ_ONCE(c5_input_ready)) {
		ret = -ENODEV;
		goto out;
	}
	if (!strcmp(attr->attr.name, "level")) {
		ret = scnprintf(buf, PAGE_SIZE, "%u\n", frontend->level);
		goto out;
	}
	ret = c5_input_select_params(profile->rows, profile->nr,
				    frontend->level, params, selected);
	if (ret)
		goto out;
	for (phase = 0; phase < 2; phase++) {
		if (phase)
			length += scnprintf(buf + length, PAGE_SIZE - length, "|");
		if (selected[phase] < 0)
			continue;
		if (!strcmp(attr->attr.name, "freq"))
			length += scnprintf(buf + length, PAGE_SIZE - length,
				"%u %u %u %u", profile->rows[selected[phase]].level,
				params[phase].cpu_freq, params[phase].bimc_freq,
				params[phase].hmp_boost);
		else
			length += scnprintf(buf + length, PAGE_SIZE - length,
				"%u %u %u", profile->rows[selected[phase]].level,
				profile->rows[selected[phase]].head_time,
				profile->rows[selected[phase]].tail_time);
	}
	length += scnprintf(buf + length, PAGE_SIZE - length, "\n");
	ret = length;
out:
	mutex_unlock(&c5_input_config_lock);
	return ret;
}

struct c5_input_control {
	struct work_struct work;
	struct c5_input_frontend *frontend;
	bool enabled;
	int result;
};

static void c5_input_control_work(struct work_struct *work)
{
	struct c5_input_control *request = container_of(work,
						       struct c5_input_control, work);
	struct c5_input_frontend *frontend = request->frontend;
	bool activity;

	mutex_lock(&frontend->lock);
	if (!READ_ONCE(c5_input_ready) || !frontend->level) {
		request->result = -ENODEV;
	} else {
		activity = request->enabled && !frontend->control;
		frontend->control = request->enabled;
		frontend->last_error = 0;
		c5_input_frontend_event(frontend, activity);
		request->result = frontend->last_error;
	}
	mutex_unlock(&frontend->lock);
}

static ssize_t c5_input_device_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct c5_input_frontend *frontend = dev_get_drvdata(dev);
	bool control = !strcmp(attr->attr.name, "control");
	bool level = !strcmp(attr->attr.name, "level");
	bool freq = !strcmp(attr->attr.name, "freq");
	u32 fields[4];
	unsigned int nr = control ? 2 : level ? 1 : freq ? 4 : 3;
	int ret = c5_input_parse_fields(buf, count, nr, control ? 2 : 1, fields);

	if (ret)
		return ret;
	mutex_lock(&c5_input_config_lock);
	if (!READ_ONCE(c5_input_ready)) {
		ret = -ENODEV;
	} else if (level) {
		ret = c5_input_set_level(frontend, fields[0]);
	} else if (control) {
		/* The old first type field is parsed but not used for routing. */
		if (fields[1] > 1) {
			ret = -EINVAL;
		} else {
			struct c5_input_control request = {
				.frontend = frontend, .enabled = fields[1],
				.result = -EINPROGRESS,
			};

			INIT_WORK_ONSTACK(&request.work, c5_input_control_work);
			queue_work(c5_input_queue, &request.work);
			flush_work(&request.work);
			destroy_work_on_stack(&request.work);
			ret = request.result;
		}
	} else if (fields[0] != C5_INPUT_KEEP) {
		struct c5_input_profile *profile = &c5_profiles[frontend->type];
		struct c5_input_row values = {
			.cpu_freq = fields[1], .bimc_freq = fields[2],
			.hmp_boost = freq ? fields[3] : C5_INPUT_KEEP,
			.head_time = fields[1], .tail_time = fields[2],
		};
		struct c5_input_row *copy;
		unsigned int i;

		for (i = 0; i < profile->nr; i++)
			if (profile->rows[i].level == fields[0])
				break;
		if (i == profile->nr) {
			ret = -EINVAL;
		} else {
			copy = kmemdup(profile->rows, profile->nr * sizeof(*copy),
					GFP_KERNEL);
			if (!copy) {
				ret = -ENOMEM;
			} else {
				ret = c5_input_patch_row(&copy[i], freq ?
					C5_ROW_CPU | C5_ROW_BIMC | C5_ROW_HMP :
					C5_ROW_HEAD | C5_ROW_TAIL, &values);
				if (!ret)
					ret = c5_input_replace_rows(frontend->type, copy);
				if (ret)
					kfree(copy);
			}
		}
	}
	mutex_unlock(&c5_input_config_lock);
	return ret ? ret : count;
}

static DEVICE_ATTR(level, 0644, c5_input_device_show, c5_input_device_store);
static DEVICE_ATTR(freq, 0644, c5_input_device_show, c5_input_device_store);
static DEVICE_ATTR(time, 0644, c5_input_device_show, c5_input_device_store);
static DEVICE_ATTR(control, 0644, NULL, c5_input_device_store);
static struct attribute *c5_input_device_attrs[] = {
	&dev_attr_level.attr, &dev_attr_freq.attr, &dev_attr_time.attr,
	&dev_attr_control.attr, NULL,
};
static const struct attribute_group c5_input_device_group = {
	.attrs = c5_input_device_attrs,
};

static void c5_input_stop(void)
{
	struct c5_input_context *context;
	struct hlist_node *next;
	unsigned int bucket, i, slot;
	unsigned long flags;

	if (!c5_input_initialized)
		return;
	/* Never hold config_lock while removing active sysfs methods: removal
	 * waits for them, and an admitted method may itself await config_lock. */
	mutex_lock(&c5_input_config_lock);
	spin_lock_irqsave(&c5_input_events_lock, flags);
	WRITE_ONCE(c5_input_ready, false);
	spin_unlock_irqrestore(&c5_input_events_lock, flags);
	mutex_unlock(&c5_input_config_lock);
	for (i = C5_GENRE_COUNT; i > 0; i--) {
		struct c5_input_frontend *frontend = &c5_frontends[i - 1];

		if (!frontend->device)
			continue;
		if (frontend->group_created) {
			sysfs_remove_group(&frontend->device->kobj, &c5_input_device_group);
			frontend->group_created = false;
		}
		device_unregister(frontend->device);
		frontend->device = NULL;
	}
	while (c5_input_class_files)
		class_remove_file(c5_input_class,
			&c5_input_class_attrs[--c5_input_class_files]);
	if (c5_input_class) {
		class_destroy(c5_input_class);
		c5_input_class = NULL;
	}
	if (c5_input_queue) {
		cancel_work_sync(&c5_input_event_work);
		for (i = 0; i < C5_GENRE_COUNT; i++)
			for (slot = 0; slot < 2; slot++)
				cancel_delayed_work_sync(&c5_frontends[i].timer[slot].work);
		flush_workqueue(c5_input_queue);
		destroy_workqueue(c5_input_queue);
		c5_input_queue = NULL;
	}
	mutex_lock(&c5_input_devices_lock);
	hash_for_each_safe(c5_input_devices, bucket, next, context, node) {
		spin_lock_irqsave(&context->dev->event_lock, flags);
		hash_del_rcu(&context->node);
		spin_unlock_irqrestore(&context->dev->event_lock, flags);
		synchronize_rcu();
		kfree(context);
	}
	mutex_unlock(&c5_input_devices_lock);
	for (i = 0; i < C5_GENRE_COUNT; i++) {
		mutex_lock(&c5_frontends[i].lock);
		/* A failed release remains an explicit provider error, never a
		 * fabricated cleared request.  Do not retry or change clock policy. */
		c5_input_remove(&c5_frontends[i]);
		c5_frontends[i].active = 0;
		c5_frontends[i].control = false;
		c5_frontends[i].phase.press = false;
		c5_frontends[i].phase.index = 0;
		mutex_unlock(&c5_frontends[i].lock);
	}
	for (i = 0; i < ARRAY_SIZE(c5_profiles); i++) {
		kfree(c5_profiles[i].rows);
		c5_profiles[i].rows = NULL;
		c5_profiles[i].nr = 0;
	}
	memset(&c5_input_events, 0, sizeof(c5_input_events));
	c5_input_initialized = false;
}

static int c5_input_reboot(struct notifier_block *notifier,
			    unsigned long event, void *unused)
{
	c5_input_stop();
	return NOTIFY_DONE;
}

static struct notifier_block c5_input_reboot_notifier = {
	.notifier_call = c5_input_reboot,
};

static int c5_input_booster_init(void)
{
	unsigned int i, slot;
	unsigned long flags;
	int ret;

	BUILD_BUG_ON(C5_GENRE_COUNT != C5_INPUT_GENRES);
	for (i = 0; i < C5_GENRE_COUNT; i++) {
		struct c5_input_frontend *frontend = &c5_frontends[i];

		mutex_init(&frontend->lock);
		frontend->phase = (struct c5_input_phase) {
			.owner = frontend, .pending = c5_input_timer_pending,
			.cancel = c5_input_timer_cancel, .schedule = c5_input_timer_schedule,
			.apply = c5_input_apply, .remove = c5_input_remove,
			.change_on_release = i == C5_MULTITOUCH,
		};
		for (slot = 0; slot < 2; slot++) {
			frontend->timer[slot].frontend = frontend;
			INIT_DELAYED_WORK(&frontend->timer[slot].work, c5_input_timeout);
		}
	}
	c5_input_initialized = true;
	ret = c5_input_dt_parse();
	if (ret)
		goto fail;
	for (i = 0; i < C5_GENRE_COUNT; i++) {
		struct c5_input_frontend *frontend = &c5_frontends[i];
		struct c5_input_profile *profile = &c5_profiles[frontend->type];

		ret = c5_input_select_params(profile->rows, profile->nr,
			frontend->level, frontend->phase.param, NULL);
		if (ret)
			goto fail;
	}
	c5_input_queue = alloc_ordered_workqueue("c5_input_booster", WQ_MEM_RECLAIM);
	if (!c5_input_queue) {
		ret = -ENOMEM;
		goto fail;
	}
	c5_input_class = class_create(THIS_MODULE, "input_booster");
	if (IS_ERR(c5_input_class)) {
		ret = PTR_ERR(c5_input_class);
		c5_input_class = NULL;
		goto fail;
	}
	for (i = 0; i < ARRAY_SIZE(c5_input_class_attrs); i++) {
		ret = class_create_file(c5_input_class, &c5_input_class_attrs[i]);
		if (ret)
			goto fail;
		c5_input_class_files++;
	}
	for (i = 0; i < C5_GENRE_COUNT; i++) {
		struct c5_input_frontend *frontend = &c5_frontends[i];

		frontend->device = device_create(c5_input_class, NULL, 0,
						 frontend, "%s", frontend->name);
		if (IS_ERR(frontend->device)) {
			ret = PTR_ERR(frontend->device);
			frontend->device = NULL;
			goto fail;
		}
		ret = sysfs_create_group(&frontend->device->kobj, &c5_input_device_group);
		if (ret)
			goto fail;
		frontend->group_created = true;
	}
	ret = register_reboot_notifier(&c5_input_reboot_notifier);
	if (ret)
		goto fail;
	c5_input_notifier_registered = true;
	mutex_lock(&c5_input_config_lock);
	spin_lock_irqsave(&c5_input_events_lock, flags);
	WRITE_ONCE(c5_input_ready, true);
	spin_unlock_irqrestore(&c5_input_events_lock, flags);
	mutex_unlock(&c5_input_config_lock);
	return 0;
fail:
	c5_input_stop();
	return ret;
}

static void c5_input_booster_exit(void)
{
	if (c5_input_notifier_registered) {
		unregister_reboot_notifier(&c5_input_reboot_notifier);
		c5_input_notifier_registered = false;
	}
	c5_input_stop();
}
#endif
