/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_C5_BOOT_TRACE_H
#define _LINUX_C5_BOOT_TRACE_H
#include <linux/types.h>
#include <linux/compiler.h>

#ifdef CONFIG_C5_RECOVERY_ONLY
void notrace c5_pm_checkpoint(u16 stage,u32 arg0,u32 arg1);
void notrace c5_boot_trace_enable(bool mapped);
void notrace c5_boot_trace_event(u16 stage, u64 value0, u64 value1, u64 value2);
void notrace c5_boot_trace_fault(u16 stage, u64 pc, u64 esr, u64 far);
void notrace c5_boot_trace_stop(void);
bool notrace c5_boot_trace_native_logs(void);
void notrace c5_pstore_stage_record(u16 stage, u64 value0, u64 value1,
				  u64 value2, bool fault);
void notrace c5_pstore_checkpoint(u16 stage, u64 value0, u64 value1, u64 value2);
bool notrace c5_boot_trace_rate_active(void);
volatile u32 *notrace c5_pstore_remote_claim(void);
void notrace c5_pstore_remote_release(void);
#else
static inline void c5_pm_checkpoint(u16 s,u32 a,u32 b) { }
static inline void c5_boot_trace_enable(bool mapped) { }
static inline void c5_boot_trace_event(u16 stage, u64 v0, u64 v1, u64 v2) { }
static inline void c5_boot_trace_fault(u16 stage, u64 pc, u64 esr, u64 far) { }
static inline void c5_boot_trace_stop(void) { }
static inline bool c5_boot_trace_native_logs(void) { return false; }
static inline void c5_pstore_checkpoint(u16 stage, u64 v0, u64 v1, u64 v2) { }
#endif
#endif
