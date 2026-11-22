/*
 * Flashlight Class Device Driver
 * Copyright (C) 2013 Richtek Technology Corp.
 * Author: Patrick Chang <patrick_chang@richtek.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * C5 4.9 adaptation: real legacy class/API, no automatic hardware instance.
 * Every caller owns a valid device reference. Provider callbacks/resources
 * stay valid until externally serialized unregister completes. Do not call
 * unregister from this object's admitted callback. Per-method callback lock
 * contexts follow the old provider; this is not a stale-pointer guarantee.
 * Admission drains synchronous calls only. Async charge work/completions
 * require separate provider quiescence and owned references (see header).
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/flashlight.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/limits.h>
#include <linux/mm.h>

static struct class *flashlight_class;
static const char * const flashlight_type_string[] = { "Xenon", "LED", "Bulb" };
static const char * const flashlight_mode_string[] = { "Off", "Torch", "Flash", "Mixed" };

static int flashlight_begin(struct flashlight_device *f,
			    const struct flashlight_ops **ops)
{
	if (!f)
		return -EINVAL;
	mutex_lock(&f->lifecycle_lock);
	if (!f->ready || f->closing) {
		mutex_unlock(&f->lifecycle_lock);
		return -ENODEV;
	}
	if (f->active_ops == UINT_MAX) {
		mutex_unlock(&f->lifecycle_lock);
		return -EOVERFLOW;
	}
	get_device(&f->dev);
	f->active_ops++;
	*ops = f->ops;
	mutex_unlock(&f->lifecycle_lock);
	return 0;
}

static void flashlight_end(struct flashlight_device *f)
{
	bool idle;

	mutex_lock(&f->lifecycle_lock);
	idle = --f->active_ops == 0;
	mutex_unlock(&f->lifecycle_lock);
	if (idle)
		wake_up_all(&f->idle);
	put_device(&f->dev);
}

/* Called within one admitted operation. The callback is ops_lock-held, as
 * for both old public and sysfs brightness setters. A callback failure does
 * not publish a successful cached property.
 */
static int flashlight_brightness(struct flashlight_device *f,
		const struct flashlight_ops *ops, int value, bool torch,
		int missing_method)
{
	int ret;

	mutex_lock(&f->ops_lock);
	if (!ops || (torch ? !ops->set_torch_brightness :
				 !ops->set_strobe_brightness))
		ret = missing_method;
	else if (value < 0 || value > (torch ? f->props.torch_max_brightness :
					       f->props.strobe_max_brightness))
		ret = -EINVAL;
	else {
		ret = torch ? ops->set_torch_brightness(f, value) :
			      ops->set_strobe_brightness(f, value);
		if (ret >= 0) {
			if (torch)
				f->props.torch_brightness = value;
			else
				f->props.strobe_brightness = value;
		}
	}
	mutex_unlock(&f->ops_lock);
	return ret;
}

int flashlight_set_torch_brightness(struct flashlight_device *f, int value)
{
	const struct flashlight_ops *ops;
	int ret = flashlight_begin(f, &ops);

	if (ret)
		return ret;
	ret = flashlight_brightness(f, ops, value, true, -EINVAL);
	flashlight_end(f);
	return ret;
}
EXPORT_SYMBOL(flashlight_set_torch_brightness);

int flashlight_set_strobe_brightness(struct flashlight_device *f, int value)
{
	const struct flashlight_ops *ops;
	int ret = flashlight_begin(f, &ops);

	if (ret)
		return ret;
	ret = flashlight_brightness(f, ops, value, false, -EINVAL);
	flashlight_end(f);
	return ret;
}
EXPORT_SYMBOL(flashlight_set_strobe_brightness);

int flashlight_list_color_temperature(struct flashlight_device *f, int selector)
{
	const struct flashlight_ops *ops;
	int ret = flashlight_begin(f, &ops);

	if (ret)
		return ret;
	ret = ops && ops->list_color_temperature ?
		ops->list_color_temperature(f, selector) : -EINVAL;
	flashlight_end(f);
	return ret;
}
EXPORT_SYMBOL(flashlight_list_color_temperature);

int flashlight_list_strobe_timeout(struct flashlight_device *f, int selector)
{
	const struct flashlight_ops *ops;
	int ret = flashlight_begin(f, &ops);

	if (ret)
		return ret;
	ret = ops && ops->list_strobe_timeout ?
		ops->list_strobe_timeout(f, selector) : -EINVAL;
	flashlight_end(f);
	return ret;
}
EXPORT_SYMBOL(flashlight_list_strobe_timeout);

int flashlight_set_color_temperature(struct flashlight_device *f,
				     int minK, int maxK)
{
	const struct flashlight_ops *ops;
	int value, selector = 0;
	int ret = flashlight_begin(f, &ops);

	if (ret)
		return ret;
	if (minK < 0 || maxK < minK || !ops ||
	    !ops->list_color_temperature || !ops->set_color_temperature) {
		ret = -EINVAL;
		goto out;
	}
	for (;;) {
		/* Enumeration callbacks retain their old unlocked context. */
		value = ops->list_color_temperature(f, selector);
		if (value < 0) {
			ret = value;
			break;
		}
		if (value >= minK && value <= maxK) {
			mutex_lock(&f->ops_lock);
			ret = ops->set_color_temperature(f, value);
			if (!ret)
				f->props.color_temperature = value;
			mutex_unlock(&f->ops_lock);
			break;
		}
		if (selector == INT_MAX) {
			ret = -EOVERFLOW;
			break;
		}
		selector++;
	}
out:
	flashlight_end(f);
	return ret;
}
EXPORT_SYMBOL(flashlight_set_color_temperature);

int flashlight_set_strobe_timeout(struct flashlight_device *f,
				 int min_ms, int max_ms)
{
	const struct flashlight_ops *ops;
	int value, selector = 0;
	int ret = flashlight_begin(f, &ops);

	if (ret)
		return ret;
	if (min_ms < 0 || max_ms < min_ms || !ops ||
	    !ops->list_strobe_timeout || !ops->set_strobe_timeout) {
		ret = -EINVAL;
		goto out;
	}
	for (;;) {
		value = ops->list_strobe_timeout(f, selector);
		if (value < 0) {
			ret = value;
			break;
		}
		if (value >= min_ms && value <= max_ms) {
			mutex_lock(&f->ops_lock);
			ret = ops->set_strobe_timeout(f, value);
			if (!ret)
				f->props.strobe_timeout = value;
			mutex_unlock(&f->ops_lock);
			break;
		}
		if (selector == INT_MAX) {
			ret = -EOVERFLOW;
			break;
		}
		selector++;
	}
out:
	flashlight_end(f);
	return ret;
}
EXPORT_SYMBOL(flashlight_set_strobe_timeout);

int flashlight_set_mode(struct flashlight_device *f, int mode)
{
	const struct flashlight_ops *ops;
	int ret = flashlight_begin(f, &ops);

	if (ret)
		return ret;
	mutex_lock(&f->ops_lock);
	if (mode < 0 || mode >= FLASHLIGHT_MODE_MAX)
		ret = -EINVAL;
	else {
		ret = ops && ops->set_mode ? ops->set_mode(f, mode) : 0;
		if (ret >= 0)
			f->props.mode = mode;
	}
	mutex_unlock(&f->ops_lock);
	flashlight_end(f);
	return ret;
}
EXPORT_SYMBOL(flashlight_set_mode);

#if defined(CONFIG_FLED_SM5703) || defined(CONFIG_LEDS_SM5705)
int flashlight_strobe(struct flashlight_device *f, int turn_way)
{
	const struct flashlight_ops *ops;
	int ret = flashlight_begin(f, &ops);

	if (ret)
		return ret;
	/* Selected C5 API has two arguments and no alternate mode predicate. */
	ret = ops && ops->strobe ? ops->strobe(f, turn_way) : -EINVAL;
	flashlight_end(f);
	return ret;
}
#else
int flashlight_strobe(struct flashlight_device *f)
{
	const struct flashlight_ops *ops;
	bool flash_mode;
	int ret = flashlight_begin(f, &ops);

	if (ret)
		return ret;
	mutex_lock(&f->ops_lock);
	flash_mode = f->props.mode == FLASHLIGHT_MODE_FLASH ||
		     f->props.mode == FLASHLIGHT_MODE_MIXED;
	mutex_unlock(&f->ops_lock);
	ret = flash_mode && ops && ops->strobe ? ops->strobe(f) : -EINVAL;
	flashlight_end(f);
	return ret;
}
#endif
EXPORT_SYMBOL(flashlight_strobe);

int flashlight_strobe_charge(struct flashlight_device *f,
		flashlight_charge_event_cb cb, void *data, int start)
{
	const struct flashlight_ops *ops;
	bool led_type;
	int ret = flashlight_begin(f, &ops);

	if (ret)
		return ret;
	if (ops && ops->strobe_charge)
		ret = ops->strobe_charge(f, cb, data, start);
	else {
		mutex_lock(&f->ops_lock);
		led_type = f->props.type == FLASHLIGHT_TYPE_LED;
		mutex_unlock(&f->ops_lock);
		if (led_type) {
			if (cb)
				cb(data, 0);
			ret = 0;
		} else
			ret = -EINVAL;
	}
	flashlight_end(f);
	return ret;
}
EXPORT_SYMBOL(flashlight_strobe_charge);

/* All attribute callbacks are admitted until the last property access and
 * return value is saved. Kernfs removal and device refs provide outer lifetime.
 */
enum flashlight_property {
	PROP_NAME, PROP_TYPE, PROP_MODE, PROP_TORCH_MAX, PROP_STROBE_MAX,
	PROP_COLOR, PROP_DELAY, PROP_TIMEOUT, PROP_TORCH, PROP_STROBE,
};
static ssize_t flashlight_show_property(struct device *dev, char *buf,
					 enum flashlight_property which)
{
	struct flashlight_device *f = to_flashlight_device(dev);
	const struct flashlight_ops *ops;
	const char *name;
	int value;
	ssize_t ret = flashlight_begin(f, &ops);

	if (ret)
		return ret;
	/* Old show callbacks took no ops_lock. Preserve that reentry context
	 * and snapshot each exposed property once; no multi-file atomic view.
	 */
	switch (which) {
	case PROP_NAME:
		name = READ_ONCE(f->props.alias_name);
		ret = scnprintf(buf, PAGE_SIZE, "%s\n", name ? name : "anonymous");
		break;
	case PROP_TYPE:
		value = READ_ONCE(f->props.type);
		ret = value >= 0 && value < FLASHLIGHT_TYPE_MAX ?
			sprintf(buf, "%s\n", flashlight_type_string[value]) : -EINVAL;
		break;
	case PROP_MODE:
		value = READ_ONCE(f->props.mode);
		ret = value >= 0 && value < FLASHLIGHT_MODE_MAX ?
			sprintf(buf, "%s\n", flashlight_mode_string[value]) : -EINVAL;
		break;
	case PROP_TORCH_MAX: ret = sprintf(buf, "%d\n", READ_ONCE(f->props.torch_max_brightness)); break;
	case PROP_STROBE_MAX: ret = sprintf(buf, "%d\n", READ_ONCE(f->props.strobe_max_brightness)); break;
	case PROP_COLOR: ret = sprintf(buf, "%d\n", READ_ONCE(f->props.color_temperature)); break;
	case PROP_DELAY: ret = sprintf(buf, "%d\n", READ_ONCE(f->props.strobe_delay)); break;
	case PROP_TIMEOUT: ret = sprintf(buf, "%d\n", READ_ONCE(f->props.strobe_timeout)); break;
	case PROP_TORCH: ret = sprintf(buf, "%d\n", READ_ONCE(f->props.torch_brightness)); break;
	case PROP_STROBE: ret = sprintf(buf, "%d\n", READ_ONCE(f->props.strobe_brightness)); break;
	default: ret = -EINVAL;
	}
	flashlight_end(f);
	return ret;
}
#define FLASHLIGHT_SHOW(name, property) \
static ssize_t flashlight_show_##name(struct device *dev, \
	struct device_attribute *attr, char *buf) \
{ return flashlight_show_property(dev, buf, property); }
FLASHLIGHT_SHOW(name, PROP_NAME)
FLASHLIGHT_SHOW(type, PROP_TYPE)
FLASHLIGHT_SHOW(mode, PROP_MODE)
FLASHLIGHT_SHOW(torch_max_brightness, PROP_TORCH_MAX)
FLASHLIGHT_SHOW(strobe_max_brightness, PROP_STROBE_MAX)
FLASHLIGHT_SHOW(color_temperature, PROP_COLOR)
FLASHLIGHT_SHOW(strobe_delay, PROP_DELAY)
FLASHLIGHT_SHOW(strobe_timeout, PROP_TIMEOUT)
FLASHLIGHT_SHOW(torch_brightness, PROP_TORCH)
FLASHLIGHT_SHOW(strobe_brightness, PROP_STROBE)

static ssize_t flashlight_store_brightness(struct device *dev,
		const char *buf, size_t count, bool torch)
{
	struct flashlight_device *f = to_flashlight_device(dev);
	const struct flashlight_ops *ops;
	long value;
	ssize_t ret = flashlight_begin(f, &ops);

	if (ret)
		return ret;
	ret = kstrtol(buf, 0, &value);
	if (ret)
		goto out;
	if (value < 0 || value > INT_MAX) {
		ret = -EINVAL;
		goto out;
	}
	ret = flashlight_brightness(f, ops, value, torch, -ENXIO);
	if (ret >= 0)
		ret = count;
out:
	flashlight_end(f);
	return ret;
}
static ssize_t flashlight_store_torch_brightness(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	return flashlight_store_brightness(dev, buf, count, true);
}
static ssize_t flashlight_store_strobe_brightness(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	return flashlight_store_brightness(dev, buf, count, false);
}
static ssize_t flashlight_store_strobe_timeout(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct flashlight_device *f = to_flashlight_device(dev);
	const struct flashlight_ops *ops;
	long value;
	ssize_t ret = flashlight_begin(f, &ops);

	if (ret)
		return ret;
	ret = kstrtol(buf, 0, &value);
	if (ret)
		goto out;
	if (value < 0 || value > INT_MAX || !ops || !ops->set_strobe_timeout) {
		ret = -EINVAL;
		goto out;
	}
	/* The old sysfs timeout callback was unlocked and direct: do not
	 * introduce range enumeration or an ops-lock-held reentry context.
	 */
	ret = ops->set_strobe_timeout(f, value);
	if (!ret) {
		mutex_lock(&f->ops_lock);
		f->props.strobe_timeout = value;
		mutex_unlock(&f->ops_lock);
		ret = count;
	}
out:
	flashlight_end(f);
	return ret;
}

static DEVICE_ATTR(name, 0444, flashlight_show_name, NULL);
static DEVICE_ATTR(type, 0444, flashlight_show_type, NULL);
static DEVICE_ATTR(mode, 0444, flashlight_show_mode, NULL);
static DEVICE_ATTR(torch_max_brightness, 0444, flashlight_show_torch_max_brightness, NULL);
static DEVICE_ATTR(strobe_max_brightness, 0444, flashlight_show_strobe_max_brightness, NULL);
static DEVICE_ATTR(color_temperature, 0444, flashlight_show_color_temperature, NULL);
static DEVICE_ATTR(strobe_delay, 0444, flashlight_show_strobe_delay, NULL);
static DEVICE_ATTR(strobe_timeout, 0644, flashlight_show_strobe_timeout, flashlight_store_strobe_timeout);
static DEVICE_ATTR(torch_brightness, 0644, flashlight_show_torch_brightness, flashlight_store_torch_brightness);
static DEVICE_ATTR(strobe_brightness, 0644, flashlight_show_strobe_brightness, flashlight_store_strobe_brightness);
static struct attribute *flashlight_attrs[] = {
	&dev_attr_name.attr, &dev_attr_type.attr, &dev_attr_mode.attr,
	&dev_attr_torch_max_brightness.attr, &dev_attr_strobe_max_brightness.attr,
	&dev_attr_color_temperature.attr, &dev_attr_strobe_delay.attr,
	&dev_attr_strobe_timeout.attr, &dev_attr_torch_brightness.attr,
	&dev_attr_strobe_brightness.attr, NULL,
};
ATTRIBUTE_GROUPS(flashlight);

static int flashlight_suspend(struct device *dev, pm_message_t state)
{
	struct flashlight_device *f = to_flashlight_device(dev);
	const struct flashlight_ops *ops;
	int ret = flashlight_begin(f, &ops);

	if (ret)
		return ret;
	if (ops && ops->suspend)
		ops->suspend(f, state);
	flashlight_end(f);
	return 0; /* Preserve legacy class callback return behavior. */
}
static int flashlight_resume(struct device *dev)
{
	struct flashlight_device *f = to_flashlight_device(dev);
	const struct flashlight_ops *ops;
	int ret = flashlight_begin(f, &ops);

	if (ret)
		return ret;
	if (ops && ops->resume)
		ops->resume(f);
	flashlight_end(f);
	return 0;
}

static void flashlight_device_release(struct device *dev)
{
	struct flashlight_device *f = to_flashlight_device(dev);

	mutex_destroy(&f->ops_lock);
	mutex_destroy(&f->lifecycle_lock);
	kfree(f);
}
struct flashlight_device *flashlight_device_register(const char *name,
		struct device *parent, void *data, const struct flashlight_ops *ops,
		const struct flashlight_properties *props)
{
	struct flashlight_device *f;
	int ret;

	if (!name || !*name)
		return ERR_PTR(-EINVAL);
	if (!flashlight_class)
		return ERR_PTR(-ENODEV);
	if (props && ((unsigned int)props->type >= FLASHLIGHT_TYPE_MAX ||
		      (unsigned int)props->mode >= FLASHLIGHT_MODE_MAX ||
		      props->torch_max_brightness < 0 || props->strobe_max_brightness < 0))
		return ERR_PTR(-EINVAL);
	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return ERR_PTR(-ENOMEM);
	mutex_init(&f->ops_lock);
	mutex_init(&f->lifecycle_lock);
	init_waitqueue_head(&f->idle);
	if (props)
		f->props = *props;
	f->ops = ops;
	device_initialize(&f->dev);
	f->dev.class = flashlight_class;
	f->dev.parent = parent;
	f->dev.release = flashlight_device_release;
	dev_set_drvdata(&f->dev, data);
	ret = dev_set_name(&f->dev, "%s", name);
	if (ret)
		goto put;
	ret = device_add(&f->dev);
	if (ret)
		goto put;
	mutex_lock(&f->lifecycle_lock);
	f->ready = true;
	mutex_unlock(&f->lifecycle_lock);
	return f;
put:
	put_device(&f->dev);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL(flashlight_device_register);

void flashlight_device_unregister(struct flashlight_device *f)
{
	if (!f)
		return;
	mutex_lock(&f->lifecycle_lock);
	if (f->closing) {
		mutex_unlock(&f->lifecycle_lock);
		return;
	}
	f->closing = true;
	f->ready = false;
	mutex_unlock(&f->lifecycle_lock);
	/* Never hold either callback lock while waiting for operations/kernfs. */
	wait_event(f->idle, !READ_ONCE(f->active_ops));
	mutex_lock(&f->ops_lock);
	f->ops = NULL;
	mutex_unlock(&f->ops_lock);
	device_unregister(&f->dev);
}
EXPORT_SYMBOL(flashlight_device_unregister);

static int flashlight_match_device_by_name(struct device *dev, const void *name)
{
	return strcmp(dev_name(dev), name) == 0;
}
struct flashlight_device *find_flashlight_by_name(char *name)
{
	struct device *dev;

	if (!name || !flashlight_class)
		return NULL;
	dev = class_find_device(flashlight_class, NULL, name,
				flashlight_match_device_by_name);
	/* Native class_find_device returns a reference; caller must put_device. */
	return dev ? to_flashlight_device(dev) : NULL;
}
EXPORT_SYMBOL(find_flashlight_by_name);

static int __init flashlight_class_init(void)
{
	struct class *class = class_create(THIS_MODULE, "flashlight");

	if (IS_ERR(class))
		return PTR_ERR(class);
	class->dev_groups = flashlight_groups;
	class->suspend = flashlight_suspend;
	class->resume = flashlight_resume;
	flashlight_class = class;
	return 0;
}
static void __exit flashlight_class_exit(void)
{
	struct class *class = flashlight_class;

	flashlight_class = NULL;
	if (class)
		class_destroy(class);
}
subsys_initcall(flashlight_class_init);
module_exit(flashlight_class_exit);
MODULE_DESCRIPTION("Legacy flashlight class and real control API");
MODULE_AUTHOR("Patrick Chang <patrick_chang@richtek.com>");
MODULE_LICENSE("GPL");
