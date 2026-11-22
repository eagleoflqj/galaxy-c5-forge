/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_C5_LEGACY_SYNC_BRIDGE_H
#define _LINUX_C5_LEGACY_SYNC_BRIDGE_H

#include <linux/kconfig.h>
#include <linux/types.h>

struct file;
struct fence;

/* Keep the incompatible old and native userspace ABI headers separate. */
#if IS_ENABLED(CONFIG_C5_LEGACY_SW_SYNC)
struct fence *c5_legacy_sync_file_fence(struct file *file);
struct fence *c5_native_sync_file_fence(struct file *file);
long c5_native_sync_file_ioctl(struct fence *fence, unsigned int cmd,
			      unsigned long arg);
long c5_legacy_sync_file_ioctl(struct file *file, unsigned int cmd,
			      unsigned long arg);
#else
static inline struct fence *c5_legacy_sync_file_fence(struct file *file)
{
	return NULL;
}
#endif

#endif
