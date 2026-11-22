/*
 * Copyright (c) 2014-2015, The Linux Foundation. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Adapted from the accepted C5 thermal_core.c threshold-client algorithms.
 * Native 4.9 zones/trips use milli-Celsius; legacy C5 clients use Celsius.
 * Only C5 TSENS zones are registered here. The native thermal core is retained.
 * Notifications are sleepable TSENS-work callbacks. As in the original API,
 * clients must not reenter sensor APIs from notify; the selected monitor
 * callbacks only publish flags and signal completion.
 */
#include <linux/c5-thermal-sensor.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>

struct c5_sensor {
	struct list_head registry;
	struct list_head thresholds;
	struct thermal_zone_device *tz;
	int hi, lo;
	long high, low;
};
static LIST_HEAD(c5_sensors);
static DEFINE_MUTEX(c5_sensor_lock);

static struct c5_sensor *find_sensor(u32 id)
{
	struct c5_sensor *s;
	list_for_each_entry(s, &c5_sensors, registry)
		if (s->tz->id == id)
			return s;
	return NULL;
}

static int update_thresholds(struct c5_sensor *s)
{
	struct sensor_threshold *t;
	long hi = LONG_MAX, lo = LONG_MIN;
	int ret;

	list_for_each_entry(t, &s->thresholds, list) {
		if (!t->active)
			continue;
		if (t->trip == THERMAL_TRIP_CONFIGURABLE_HI)
			hi = min(hi, t->temp);
		else if (t->trip == THERMAL_TRIP_CONFIGURABLE_LOW)
			lo = max(lo, t->temp);
	}
	if (hi != LONG_MAX) {
		ret = s->tz->ops->set_trip_temp(s->tz, s->hi, hi * 1000);
		if (ret)
			return ret;
		s->high = hi;
	}
	ret = s->tz->ops->activate_trip_type(s->tz, s->hi,
		hi == LONG_MAX ? THERMAL_TRIP_ACTIVATION_DISABLED :
		THERMAL_TRIP_ACTIVATION_ENABLED);
	if (ret)
		return ret;
	if (lo != LONG_MIN) {
		ret = s->tz->ops->set_trip_temp(s->tz, s->lo, lo * 1000);
		if (ret)
			return ret;
		s->low = lo;
	}
	return s->tz->ops->activate_trip_type(s->tz, s->lo,
		lo == LONG_MIN ? THERMAL_TRIP_ACTIVATION_DISABLED :
		THERMAL_TRIP_ACTIVATION_ENABLED);
}

int c5_sensor_register(struct thermal_zone_device *tz)
{
	struct c5_sensor *s;
	enum thermal_trip_type type;
	int i;
	if (!tz || !tz->ops->get_trip_type || !tz->ops->get_temp ||
	    !tz->ops->set_trip_temp || !tz->ops->activate_trip_type)
		return -EINVAL;
	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->tz = tz;
	s->hi = s->lo = -1;
	INIT_LIST_HEAD(&s->thresholds);
	for (i = 0; i < tz->trips; i++) {
		if (tz->ops->get_trip_type(tz, i, &type))
			continue;
		if (type == THERMAL_TRIP_CONFIGURABLE_HI)
			s->hi = i;
		if (type == THERMAL_TRIP_CONFIGURABLE_LOW)
			s->lo = i;
	}
	if (s->hi < 0 || s->lo < 0) {
		kfree(s);
		return -EINVAL;
	}
	mutex_lock(&c5_sensor_lock);
	if (find_sensor(tz->id)) {
		mutex_unlock(&c5_sensor_lock);
		kfree(s);
		return -EEXIST;
	}
	list_add_tail(&s->registry, &c5_sensors);
	mutex_unlock(&c5_sensor_lock);
	return 0;
}

void c5_sensor_unregister(struct thermal_zone_device *tz)
{
	struct c5_sensor *s;
	struct sensor_threshold *t, *next;
	mutex_lock(&c5_sensor_lock);
	s = find_sensor(tz->id);
	if (s) {
		list_for_each_entry_safe(t, next, &s->thresholds, list) {
			t->active = 0;
			list_del_init(&t->list);
		}
		list_del(&s->registry);
		kfree(s);
	}
	mutex_unlock(&c5_sensor_lock);
}

int sensor_get_id(char *name)
{
	struct c5_sensor *s;
	int id = -ENODEV;
	if (!name)
		return -EINVAL;
	mutex_lock(&c5_sensor_lock);
	list_for_each_entry(s, &c5_sensors, registry)
		if (!strcmp(name, s->tz->type)) {
			id = s->tz->id;
			break;
		}
	mutex_unlock(&c5_sensor_lock);
	return id;
}

int sensor_get_temp(u32 id, long *temp)
{
	struct c5_sensor *s;
	int ret = -ENODEV, value;
	if (!temp)
		return -EINVAL;
	mutex_lock(&c5_sensor_lock);
	s = find_sensor(id);
	if (s) {
		ret = s->tz->ops->get_temp(s->tz, &value);
		if (!ret)
			*temp = value / 1000;
	}
	mutex_unlock(&c5_sensor_lock);
	return ret;
}

int sensor_set_trip(u32 id, struct sensor_threshold *threshold)
{
	struct c5_sensor *s;
	struct sensor_threshold *t;
	int ret = -ENODEV;
	if (!threshold || !threshold->notify || threshold->temp > INT_MAX / 1000 ||
	    threshold->temp < INT_MIN / 1000 ||
	    (threshold->trip != THERMAL_TRIP_CONFIGURABLE_HI &&
	     threshold->trip != THERMAL_TRIP_CONFIGURABLE_LOW))
		return -EINVAL;
	mutex_lock(&c5_sensor_lock);
	s = find_sensor(id);
	if (!s)
		goto out;
	list_for_each_entry(t, &s->thresholds, list)
		if (t == threshold)
			goto found;
	INIT_LIST_HEAD(&threshold->list);
	list_add_tail(&threshold->list, &s->thresholds);
found:
	threshold->active = 0;
	ret = 0;
out:
	mutex_unlock(&c5_sensor_lock);
	return ret;
}

int sensor_activate_trip(u32 id, struct sensor_threshold *threshold, bool enable)
{
	struct c5_sensor *s;
	struct sensor_threshold *t;
	int ret = -ENODEV;
	if (!threshold)
		return -EINVAL;
	mutex_lock(&c5_sensor_lock);
	s = find_sensor(id);
	if (!s)
		goto out;
	list_for_each_entry(t, &s->thresholds, list) {
		if (t != threshold)
			continue;
		t->active = enable;
		ret = update_thresholds(s);
		break;
	}
out:
	mutex_unlock(&c5_sensor_lock);
	return ret;
}

int sensor_cancel_trip(u32 id, struct sensor_threshold *threshold)
{
	struct c5_sensor *s;
	struct sensor_threshold *t;
	int ret = -ENODEV;
	if (!threshold)
		return -EINVAL;
	mutex_lock(&c5_sensor_lock);
	s = find_sensor(id);
	if (s) {
		list_for_each_entry(t, &s->thresholds, list)
			if (t == threshold) {
				t->active = 0;
				list_del_init(&t->list);
				ret = update_thresholds(s);
				break;
			}
	}
	mutex_unlock(&c5_sensor_lock);
	return ret;
}

int thermal_sensor_trip(struct thermal_zone_device *tz,
		       enum thermal_trip_type trip, long temp)
{
	struct c5_sensor *s;
	struct sensor_threshold *t;
	int ret = -ENODEV;
	mutex_lock(&c5_sensor_lock);
	s = find_sensor(tz->id);
	if (!s)
		goto out;
	list_for_each_entry(t, &s->thresholds, list) {
		if (!t->active || t->trip != trip)
			continue;
		if ((trip == THERMAL_TRIP_CONFIGURABLE_HI && t->temp <= temp &&
		     t->temp >= s->high) ||
		    (trip == THERMAL_TRIP_CONFIGURABLE_LOW && t->temp >= temp &&
		     t->temp <= s->low)) {
			t->active = 0;
			t->notify(trip, temp, t->data);
		}
	}
	ret = update_thresholds(s);
out:
	mutex_unlock(&c5_sensor_lock);
	return ret;
}
