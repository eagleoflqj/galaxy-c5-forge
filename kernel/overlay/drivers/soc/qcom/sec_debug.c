// SPDX-License-Identifier: GPL-2.0-only
/* Isolated query/enable contract derived from Samsung drivers/debug/sec_debug.c.
 * Broad SEC_DEBUG hooks and PARAM-backed debug-level controls remain separate.
 */
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/mm.h>
#include <linux/c5_sec_debug_query.h>

#ifdef CONFIG_C5_SEC_DEBUG_QUERY
static unsigned int enable = 1;
module_param_named(enable, enable, uint, 0644);

noinline int sec_debug_is_enabled(void)
{
 return READ_ONCE(enable);
}

static int c5_sec_debug_state_get(char *buf, const struct kernel_param *kp)
{
 int value = sec_debug_is_enabled();
 return scnprintf(buf, PAGE_SIZE,
                  "C5_SEC_DEBUG_QUERY1 raw=%u api=%d nonzero=%u\n",
                  (unsigned int)value, value, value != 0);
}
static const struct kernel_param_ops c5_sec_debug_state_ops = { .get = c5_sec_debug_state_get };
module_param_cb(c5_query_state, &c5_sec_debug_state_ops, NULL, 0400);

#endif
