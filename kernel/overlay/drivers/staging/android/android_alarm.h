/* Android alarm compat commands. Derived from the C5 3.10 header.
 * Copyright (C) 2006-2007 Google, Inc.; GPL version 2.
 */
#ifndef _LINUX_ANDROID_ALARM_H
#define _LINUX_ANDROID_ALARM_H
#include <linux/compat.h>
#include "uapi/android_alarm.h"
#ifdef CONFIG_COMPAT
#define ANDROID_ALARM_SET_COMPAT(type) ALARM_IOW(2, type, struct compat_timespec)
#define ANDROID_ALARM_SET_AND_WAIT_COMPAT(type) ALARM_IOW(3, type, struct compat_timespec)
#define ANDROID_ALARM_GET_TIME_COMPAT(type) ALARM_IOW(4, type, struct compat_timespec)
#define ANDROID_ALARM_SET_RTC_COMPAT _IOW('a', 5, struct compat_timespec)
#define ANDROID_ALARM_IOCTL_NR(cmd) (_IOC_NR(cmd) & ((1 << 4) - 1))
#define ANDROID_ALARM_COMPAT_TO_NORM(cmd) ALARM_IOW(ANDROID_ALARM_IOCTL_NR(cmd), ANDROID_ALARM_IOCTL_TO_TYPE(cmd), struct timespec)
#endif
#endif
