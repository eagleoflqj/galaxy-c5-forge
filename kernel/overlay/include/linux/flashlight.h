/* include/linux/leds/flashlight.h
 * Header of Flashlight Class Device Driver
 *
 * Copyright (C) 2013 Richtek Technology Corp.
 * Patrick Chang <patrick_chang@richtek.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef LINUX_LEDS_FLASHLIGHT_H
#define LINUX_LEDS_FLASHLIGHT_H

#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/wait.h>


typedef enum flashlight_type {
	FLASHLIGHT_TYPE_XENON = 0,
	FLASHLIGHT_TYPE_LED,
	FLASHLIFHT_TYPE_BULB,
	FLASHLIGHT_TYPE_MAX,
} flashlight_type_t;
typedef enum flashlight_mode {
    FLASHLIGHT_MODE_OFF = 0,
    FLASHLIGHT_MODE_TORCH,
    FLASHLIGHT_MODE_FLASH,
    /* MIXED mode means TORCH + FLASH */
    FLASHLIGHT_MODE_MIXED,
    FLASHLIGHT_MODE_MAX,
} flashlight_mode_t;

struct flashlight_device;

typedef int (*flashlight_charge_event_cb)(void *data, int remains);

struct flashlight_ops {
    int (*set_torch_brightness)(struct flashlight_device *, int);
    int (*set_strobe_brightness)(struct flashlight_device *, int);
    int (*set_strobe_timeout)(struct flashlight_device *, int);
    int (*list_strobe_timeout)(struct flashlight_device *, int);
    int (*set_mode)(struct flashlight_device *, int);
    int (*set_color_temperature)(struct flashlight_device *, int);
    int (*list_color_temperature)(struct flashlight_device *, int);
    int (*strobe_charge)(struct flashlight_device *,
                         flashlight_charge_event_cb, void *, int);
#if defined(CONFIG_FLED_SM5703) || defined(CONFIG_LEDS_SM5705)
    int (*strobe)(struct flashlight_device *, int);
#else
    int (*strobe)(struct flashlight_device *);
#endif
    int (*suspend)(struct flashlight_device *, pm_message_t);
    int (*resume)(struct flashlight_device *);
};

struct flashlight_properties {
    /* Flashlight type */
	enum flashlight_type type;
    /* Xenon type flashlight doesn't support torch mode */
    enum flashlight_mode mode;
    /* Color temperature, unit: K, 0 means unknown */
    int color_temperature;
    int torch_brightness;
    int torch_max_brightness;
    int strobe_brightness;
    int strobe_max_brightness;
    int strobe_delay;
    int strobe_timeout;
    const char *alias_name;
};

struct flashlight_device {
	/* Flashlight properties */
	struct flashlight_properties props;
    const struct flashlight_ops *ops;
	struct mutex ops_lock;
	struct device dev;

	/* Private lifetime tail: callers own a live device reference. Register,
	 * unregister and provider-resource destruction are externally serialized.
	 * Unregister must not be called from this object's admitted callback.
	 */
	struct mutex lifecycle_lock;
	wait_queue_head_t idle;
	unsigned int active_ops;
	bool closing;
	bool ready;
};


extern struct flashlight_device *flashlight_device_register(const char *name,
    struct device *parent, void *devdata, const struct flashlight_ops *ops,
    const struct flashlight_properties *props);
extern void flashlight_device_unregister(struct flashlight_device *flashlight_dev);
extern struct flashlight_device *find_flashlight_by_name(char *name);
extern int flashlight_list_color_temperature(
                        struct flashlight_device *flashlight_dev,
                        int selector);
extern int flashlight_set_color_temperature(
                        struct flashlight_device *flashlight_dev,
                        int minK, int maxK);
extern int flashlight_set_torch_brightness(
                        struct flashlight_device *flashlight_dev,
                        int brightness_level);
extern int flashlight_set_strobe_brightness(
                        struct flashlight_device *flashlight_dev,
                        int brightness_level);
extern int flashlight_list_strobe_timeout(
                        struct flashlight_device *flashlight_dev,
                        int selector);
extern int flashlight_set_strobe_timeout(
                        struct flashlight_device *flashlight_dev,
                        int min_ms, int max_ms);
extern int flashlight_set_mode(struct flashlight_device *flashlight_dev,
                               int mode);

/* Historical unresolved declaration: no implementation existed in the
 * selected old provider. No success stub or restoration claim is supplied.
 */
extern int flashlight_flash(struct flashlight_device *flashlight_dev, int turn_way);
#if defined(CONFIG_FLED_SM5703) || defined(CONFIG_LEDS_SM5705)
extern int flashlight_strobe(struct flashlight_device *flashlight_dev, int turn_way);
#else
extern int flashlight_strobe(struct flashlight_device *flashlight_dev);
#endif

/* flashlight_charge_event_cb(void *data, int remains)
 * description :
 *   callback function of flashlight charging progress
 * arguments :
 *  @data : data pass by flashlight_strobe_charge()
 *  @remains : remained time to full chargerd, unit : ms ; 0 means ready
 * return : 0 means succeess, otherwise see definitions in errno.h
 */

/* flashlight_strobe_chargestruct flashlight_device *flashlight_dev,
 *                      flashlight_charge_event_cb cb, void *data, int start)
 * description :
 * flashlight start / stop  charging
 * @flashlight_dev : flashlight devices
 * @flashlight_charge_event_cb : callback function to report progress
 * @data : bypass to callback function
 * @start : 1 means start; 0 means stop
 */
/* strobe_charge admission protects only the synchronous provider call.
 * Any work/completion queued beyond its return belongs to the provider.
 * The provider must prevent queue-after-quiesce (close async admission and
 * synchronize ongoing calls), and stop/join its jobs before releasing their
 * resources. Alternatively it must retain all needed device/provider refs
 * across core unregister and stop/join before dropping those refs/resources.
 * The completion owner retains cb/data, and any needed device reference,
 * until completion or cancellation. Core active_ops==0 is not an async drain.
 * A synchronous fallback completion must not unregister this same object.
 */
extern int flashlight_strobe_charge(struct flashlight_device *flashlight_dev,
                        flashlight_charge_event_cb cb, void *data, int start);

#define to_flashlight_device(obj) container_of(obj, struct flashlight_device, dev)

static inline void * flashlight_get_data(
                                struct flashlight_device *flashlight_dev)
{
	return dev_get_drvdata(&flashlight_dev->dev);
}
#endif /*LINUX_LEDS_FLASHLIGHT_H*/
