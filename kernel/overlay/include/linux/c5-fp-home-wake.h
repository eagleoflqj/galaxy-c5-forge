/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_C5_FP_HOME_WAKE_H
#define _LINUX_C5_FP_HOME_WAKE_H
#include <linux/kconfig.h>
#include <linux/types.h>
struct c5_fp_home_state {
	u64 suspends, home_irqs, taken;
	bool suspended, pending_home;
};
#if IS_BUILTIN(CONFIG_KEYBOARD_GPIO) && IS_ENABLED(CONFIG_C5_VFSSPI_SECURE)
bool c5_fp_wakeup_by_home(void);
void c5_fp_home_snapshot(struct c5_fp_home_state *state);
#else
static inline bool c5_fp_wakeup_by_home(void) { return false; }
static inline void c5_fp_home_snapshot(struct c5_fp_home_state *state)
{
	*state = (struct c5_fp_home_state) { 0 };
}
#endif
#endif
