/* SPDX-License-Identifier: GPL-2.0-only */
/* Quiet control: keep call ABI; disable the custom recorder/print helpers. */
#include <linux/c5-boot-trace.h>
void notrace c5_boot_trace_enable(bool mapped) { }
void notrace c5_boot_trace_event(u16 stage, u64 a, u64 b, u64 c) { }
void notrace c5_boot_trace_fault(u16 stage, u64 pc, u64 esr, u64 far) { }
void notrace c5_boot_trace_stop(void)
{
	/* No custom output or mapping ownership remains. */
}
bool notrace c5_boot_trace_native_logs(void)
{
	/* The only selected callers guard C5BOOT diagnostic printk lines. */
	return false;
}
bool notrace c5_boot_trace_rate_active(void) { return false; }
