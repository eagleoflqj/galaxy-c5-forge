/*
 * Android legacy alarm device interface.
 * Derived from the C5 3.10 Android alarm driver, Copyright (C) 2007-2009 Google.
 * Licensed under the GNU General Public License, version 2.
 *
 * The native two alarm bases, timerfd, RTC and PM policies are unchanged.
 * Poweroff type 4 is read-only: its mutating commands return -EOPNOTSUPP.
 */
#include <linux/time.h>
#include <linux/ktime.h>
#include <linux/timekeeping.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/alarmtimer.h>
#include <linux/pm_wakeup.h>
#include "android_alarm.h"

#define ANDROID_ALARM_PRINT_INFO (1U << 0)
#define ANDROID_ALARM_PRINT_IO   (1U << 1)
#define ANDROID_ALARM_PRINT_INT  (1U << 2)
static int debug_mask = ANDROID_ALARM_PRINT_INFO;
module_param_named(debug_mask, debug_mask, int, S_IRUGO | S_IWUSR | S_IWGRP);
#define alarm_dbg(mask, fmt, ...) do { \
	if (debug_mask & ANDROID_ALARM_PRINT_##mask) \
		pr_info(fmt, ##__VA_ARGS__); \
} while (0)

static int alarm_opened;
static DEFINE_SPINLOCK(alarm_slock);
static DEFINE_MUTEX(alarm_mutex);
static struct wakeup_source *alarm_wake_lock;
static DECLARE_WAIT_QUEUE_HEAD(alarm_wait_queue);
static u32 alarm_pending;
static u32 alarm_enabled;
static u32 wait_pending;

struct devalarm {
	union {
		struct hrtimer hrt;
		struct alarm alrm;
	} u;
	enum android_alarm_type type;
};
static struct devalarm alarms[ANDROID_ALARM_TYPE_COUNT];

static bool is_mutable_type(enum android_alarm_type type)
{
	return type != ANDROID_ALARM_RTC_POWEROFF_WAKEUP;
}

static bool is_wakeup(enum android_alarm_type type)
{
	return type == ANDROID_ALARM_RTC_WAKEUP ||
		type == ANDROID_ALARM_ELAPSED_REALTIME_WAKEUP;
}

static void devalarm_start(struct devalarm *alrm, ktime_t expiry)
{
	if (is_wakeup(alrm->type))
		alarm_start(&alrm->u.alrm, expiry);
	else
		hrtimer_start(&alrm->u.hrt, expiry, HRTIMER_MODE_ABS);
}

static void devalarm_cancel(struct devalarm *alrm)
{
	if (is_wakeup(alrm->type))
		alarm_cancel(&alrm->u.alrm);
	else
		hrtimer_cancel(&alrm->u.hrt);
}

/* Validate before usercopy, lease acquisition or any timer/backend effect. */
static int alarm_validate_cmd(unsigned int cmd)
{
	unsigned int type = ANDROID_ALARM_IOCTL_TO_TYPE(cmd);
	unsigned int base = ANDROID_ALARM_BASE_CMD(cmd);

	if (type >= ANDROID_ALARM_TYPE_COUNT)
		return -EINVAL;
	switch (base) {
	case ANDROID_ALARM_CLEAR(0):
	case ANDROID_ALARM_SET(0):
	case ANDROID_ALARM_SET_AND_WAIT(0):
		if (!is_mutable_type(type))
			return -EOPNOTSUPP;
		break;
	case ANDROID_ALARM_WAIT:
	case ANDROID_ALARM_SET_RTC:
		/* Only canonical global commands are declared by the UAPI. */
		if (type)
			return -EINVAL;
		break;
	case ANDROID_ALARM_GET_TIME(0):
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

/* Caller holds alarm_mutex; cancellation never holds the callback's lock. */
static void alarm_clear(enum android_alarm_type type)
{
	unsigned long flags;
	u32 mask = 1U << type;

	spin_lock_irqsave(&alarm_slock, flags);
	alarm_enabled &= ~mask;
	alarm_pending &= ~mask;
	if (!alarm_pending && !wait_pending)
		__pm_relax(alarm_wake_lock);
	spin_unlock_irqrestore(&alarm_slock, flags);
	devalarm_cancel(&alarms[type]);
	alarm_dbg(IO, "alarm %d clear\n", type);
}

/* Drain the old generation before publishing and arming the new one. */
static void alarm_set(enum android_alarm_type type, const struct timespec *ts)
{
	unsigned long flags;
	u32 mask = 1U << type;

	spin_lock_irqsave(&alarm_slock, flags);
	alarm_enabled &= ~mask;
	spin_unlock_irqrestore(&alarm_slock, flags);
	devalarm_cancel(&alarms[type]);
	spin_lock_irqsave(&alarm_slock, flags);
	alarm_enabled |= mask;
	spin_unlock_irqrestore(&alarm_slock, flags);
	devalarm_start(&alarms[type], timespec_to_ktime(*ts));
	alarm_dbg(IO, "alarm %d set %ld.%09ld\n", type,
		  ts->tv_sec, ts->tv_nsec);
}

/* No mutation mutex is held while waiting. A second waiter rechecks pending. */
static int alarm_wait(void)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&alarm_slock, flags);
	if (!alarm_pending && wait_pending) {
		__pm_relax(alarm_wake_lock);
		wait_pending = 0;
	}
	spin_unlock_irqrestore(&alarm_slock, flags);
	for (;;) {
		ret = wait_event_interruptible(alarm_wait_queue,
					       READ_ONCE(alarm_pending));
		if (ret)
			return ret;
		spin_lock_irqsave(&alarm_slock, flags);
		if (!alarm_pending) {
			spin_unlock_irqrestore(&alarm_slock, flags);
			continue;
		}
		ret = alarm_pending;
		wait_pending = 1;
		alarm_pending = 0;
		spin_unlock_irqrestore(&alarm_slock, flags);
		return ret;
	}
}

static int alarm_set_rtc(struct timespec *ts)
{
	struct rtc_time rtc_time;
	struct rtc_device *rtc;
	unsigned long flags;
	int ret;

	/* Match native clock validation before the RTC calendar conversion. */
	if (!timespec_valid_strict(ts))
		return -EINVAL;
	rtc_time_to_tm(ts->tv_sec, &rtc_time);
	rtc = alarmtimer_get_rtcdev();
	ret = do_settimeofday(ts);
	if (ret < 0)
		return ret;
	if (rtc)
		ret = rtc_set_time(rtc, &rtc_time);
	spin_lock_irqsave(&alarm_slock, flags);
	alarm_pending |= ANDROID_ALARM_TIME_CHANGE_MASK;
	wake_up(&alarm_wait_queue);
	spin_unlock_irqrestore(&alarm_slock, flags);
	return ret;
}

static int alarm_get_time(enum android_alarm_type type, struct timespec *ts)
{
	switch (type) {
	case ANDROID_ALARM_RTC_WAKEUP:
	case ANDROID_ALARM_RTC:
	case ANDROID_ALARM_RTC_POWEROFF_WAKEUP:
		getnstimeofday(ts);
		break;
	case ANDROID_ALARM_ELAPSED_REALTIME_WAKEUP:
	case ANDROID_ALARM_ELAPSED_REALTIME:
		get_monotonic_boottime(ts);
		break;
	case ANDROID_ALARM_SYSTEMTIME:
		ktime_get_ts(ts);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static long alarm_do_ioctl(struct file *file, unsigned int cmd,
			   struct timespec *ts)
{
	enum android_alarm_type type = ANDROID_ALARM_IOCTL_TO_TYPE(cmd);
	unsigned int base = ANDROID_ALARM_BASE_CMD(cmd);
	unsigned long flags;
	int ret = alarm_validate_cmd(cmd);

	if (ret)
		return ret;
	/* Reject malformed ordinary deadlines before lease or timer effects. */
	if ((base == ANDROID_ALARM_SET(0) ||
	     base == ANDROID_ALARM_SET_AND_WAIT(0)) && !timespec_valid(ts))
		return -EINVAL;
	if (base == ANDROID_ALARM_GET_TIME(0))
		return alarm_get_time(type, ts);
	if ((file->f_flags & O_ACCMODE) == O_RDONLY)
		return -EPERM;
	mutex_lock(&alarm_mutex);
	spin_lock_irqsave(&alarm_slock, flags);
	/* Same-file concurrent first calls must recheck private_data here. */
	if (!file->private_data && cmd != ANDROID_ALARM_SET_RTC) {
		if (alarm_opened) {
			spin_unlock_irqrestore(&alarm_slock, flags);
			mutex_unlock(&alarm_mutex);
			return -EBUSY;
		}
		alarm_opened = 1;
		file->private_data = (void *)1;
	}
	spin_unlock_irqrestore(&alarm_slock, flags);
	switch (base) {
	case ANDROID_ALARM_CLEAR(0):
		alarm_clear(type);
		ret = 0;
		break;
	case ANDROID_ALARM_SET(0):
		alarm_set(type, ts);
		ret = 0;
		break;
	case ANDROID_ALARM_SET_AND_WAIT(0):
		alarm_set(type, ts);
		mutex_unlock(&alarm_mutex);
		return alarm_wait();
	case ANDROID_ALARM_WAIT:
		mutex_unlock(&alarm_mutex);
		return alarm_wait();
	case ANDROID_ALARM_SET_RTC:
		ret = alarm_set_rtc(ts);
		break;
	default:
		ret = -EINVAL;
	}
	mutex_unlock(&alarm_mutex);
	return ret;
}

static long alarm_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct timespec ts = { 0 };
	int ret = alarm_validate_cmd(cmd);

	if (ret)
		return ret;
	switch (ANDROID_ALARM_BASE_CMD(cmd)) {
	case ANDROID_ALARM_SET_AND_WAIT(0):
	case ANDROID_ALARM_SET(0):
	case ANDROID_ALARM_SET_RTC:
	case ANDROID_ALARM_CLEAR(0): /* Historical native16-byte copy for _IO. */
		if (copy_from_user(&ts, (void __user *)arg, sizeof(ts)))
			return -EFAULT;
	}
	ret = alarm_do_ioctl(file, cmd, &ts);
	if (ret)
		return ret;
	if (ANDROID_ALARM_BASE_CMD(cmd) == ANDROID_ALARM_GET_TIME(0) &&
	    copy_to_user((void __user *)arg, &ts, sizeof(ts)))
		return -EFAULT;
	return 0;
}

#ifdef CONFIG_COMPAT
static long alarm_compat_ioctl(struct file *file, unsigned int cmd,
			       unsigned long arg)
{
	struct timespec ts = { 0 };
	bool input = false;
	int ret;

	switch (ANDROID_ALARM_BASE_CMD(cmd)) {
	case ANDROID_ALARM_SET_AND_WAIT_COMPAT(0):
	case ANDROID_ALARM_SET_COMPAT(0):
	case ANDROID_ALARM_SET_RTC_COMPAT:
		input = true;
		/* fall through */
	case ANDROID_ALARM_GET_TIME_COMPAT(0):
		cmd = ANDROID_ALARM_COMPAT_TO_NORM(cmd);
		break;
	case ANDROID_ALARM_CLEAR(0):
	case ANDROID_ALARM_WAIT:
		break;
	default:
		return -EINVAL;
	}
	ret = alarm_validate_cmd(cmd);
	if (ret)
		return ret;
	if (input && compat_get_timespec(&ts, (void __user *)arg))
		return -EFAULT;
	ret = alarm_do_ioctl(file, cmd, &ts);
	if (ret)
		return ret;
	if (ANDROID_ALARM_BASE_CMD(cmd) == ANDROID_ALARM_GET_TIME(0) &&
	    compat_put_timespec(&ts, (void __user *)arg))
		return -EFAULT;
	return 0;
}
#endif

static int alarm_open(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	return 0;
}

static void alarm_disable_all(void)
{
	unsigned long flags;
	u32 enabled, pending;
	int i;

	spin_lock_irqsave(&alarm_slock, flags);
	enabled = alarm_enabled;
	pending = alarm_pending;
	alarm_enabled = 0;
	spin_unlock_irqrestore(&alarm_slock, flags);
	for (i = 0; i < ANDROID_ALARM_TYPE_COUNT; i++) {
		if (!is_mutable_type(i))
			continue;
		if (enabled & (1U << i))
			alarm_dbg(INFO, "release: clear alarm %d, pending %d\n",
				  i, !!(pending & (1U << i)));
		devalarm_cancel(&alarms[i]);
	}
	spin_lock_irqsave(&alarm_slock, flags);
	if (alarm_pending || wait_pending)
		__pm_relax(alarm_wake_lock);
	alarm_pending = 0;
	wait_pending = 0;
	alarm_opened = 0;
	spin_unlock_irqrestore(&alarm_slock, flags);
}

static int alarm_release(struct inode *inode, struct file *file)
{
	mutex_lock(&alarm_mutex);
	if (file->private_data) {
		alarm_disable_all();
		file->private_data = NULL;
	}
	mutex_unlock(&alarm_mutex);
	return 0;
}

static void devalarm_triggered(struct devalarm *alrm)
{
	unsigned long flags;
	u32 mask = 1U << alrm->type;

	spin_lock_irqsave(&alarm_slock, flags);
	if (alarm_enabled & mask) {
		__pm_wakeup_event(alarm_wake_lock, 5000);
		alarm_enabled &= ~mask;
		alarm_pending |= mask;
		wake_up(&alarm_wait_queue);
	}
	spin_unlock_irqrestore(&alarm_slock, flags);
	alarm_dbg(INT, "alarm %d fired\n", alrm->type);
}

static enum hrtimer_restart devalarm_hrthandler(struct hrtimer *timer)
{
	struct devalarm *alrm = container_of(timer, struct devalarm, u.hrt);

	devalarm_triggered(alrm);
	return HRTIMER_NORESTART;
}

static enum alarmtimer_restart devalarm_alarmhandler(struct alarm *timer,
						  ktime_t now)
{
	struct devalarm *alrm = container_of(timer, struct devalarm, u.alrm);

	devalarm_triggered(alrm);
	return ALARMTIMER_NORESTART;
}

static const struct file_operations alarm_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = alarm_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = alarm_compat_ioctl,
#endif
	.open = alarm_open,
	.release = alarm_release,
};
static struct miscdevice alarm_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "alarm",
	.fops = &alarm_fops,
};

static int __init alarm_dev_init(void)
{
	int i, ret;

	alarm_wake_lock = wakeup_source_register("alarm");
	if (!alarm_wake_lock)
		return -ENOMEM;
	for (i = 0; i < ANDROID_ALARM_TYPE_COUNT; i++) {
		alarms[i].type = i;
		if (!is_mutable_type(i))
			continue;
		if (is_wakeup(i)) {
			alarm_init(&alarms[i].u.alrm,
				   i == ANDROID_ALARM_RTC_WAKEUP ?
				   ALARM_REALTIME : ALARM_BOOTTIME,
				   devalarm_alarmhandler);
		} else {
			hrtimer_init(&alarms[i].u.hrt,
				     i == ANDROID_ALARM_RTC ? CLOCK_REALTIME :
				     i == ANDROID_ALARM_ELAPSED_REALTIME ?
				     CLOCK_BOOTTIME : CLOCK_MONOTONIC,
				     HRTIMER_MODE_ABS);
			alarms[i].u.hrt.function = devalarm_hrthandler;
		}
	}
	ret = misc_register(&alarm_device);
	if (ret) {
		wakeup_source_unregister(alarm_wake_lock);
		alarm_wake_lock = NULL;
	}
	return ret;
}

static void __exit alarm_dev_exit(void)
{
	/* The bool provider is built-in in the selected MODULES=n profile. */
	misc_deregister(&alarm_device);
	mutex_lock(&alarm_mutex);
	alarm_disable_all();
	mutex_unlock(&alarm_mutex);
	wakeup_source_unregister(alarm_wake_lock);
	alarm_wake_lock = NULL;
}
module_init(alarm_dev_init);
module_exit(alarm_dev_exit);
MODULE_LICENSE("GPL");
