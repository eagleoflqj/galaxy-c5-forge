/* SPDX-License-Identifier: GPL-2.0 */
/* C5's GPL Qualcomm threshold-client interface, without replacing 4.9 core. */
#ifndef _C5_THERMAL_SENSOR_H
#define _C5_THERMAL_SENSOR_H
#include <linux/thermal.h>

struct sensor_threshold {
	long temp; /* Legacy C5 client unit: degrees Celsius. */
	enum thermal_trip_type trip;
	int (*notify)(enum thermal_trip_type type, int temp, void *data);
	void *data;
	u8 active;
	struct list_head list;
};

int c5_sensor_register(struct thermal_zone_device *tz);
void c5_sensor_unregister(struct thermal_zone_device *tz);
int sensor_get_id(char *name);
int sensor_get_temp(u32 id, long *temp);
int sensor_set_trip(u32 id, struct sensor_threshold *threshold);
int sensor_cancel_trip(u32 id, struct sensor_threshold *threshold);
int sensor_activate_trip(u32 id, struct sensor_threshold *threshold, bool enable);
int thermal_sensor_trip(struct thermal_zone_device *tz,
		       enum thermal_trip_type trip, long temp);
#endif
