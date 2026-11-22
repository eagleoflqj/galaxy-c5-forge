/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/moduleparam.h>
static DEFINE_RAW_SPINLOCK(c5_active_ws_lock);
static u32 c5_active_ws_calls, c5_active_ws_visited, c5_active_ws_active;

/* SRCU protects the registered source and immutable name through printing. */
static noinline bool c5_print_active_wakeup_source(struct wakeup_source *ws)
{
 unsigned long flags, count;
 ktime_t active_time, total, prevent, now;
 const char *name;

 spin_lock_irqsave(&ws->lock, flags);
 if (!ws->active) {
  spin_unlock_irqrestore(&ws->lock, flags);
  return false;
 }
 name = ws->name;
 count = ws->active_count;
 now = ktime_get();
 active_time = ktime_sub(now, ws->last_time);
 total = ktime_add(ws->total_time, active_time);
 prevent = ws->prevent_sleep_time;
 if (ws->autosleep_enabled)
  prevent = ktime_add(prevent, ktime_sub(now, ws->start_prevent_time));
 spin_unlock_irqrestore(&ws->lock, flags);
 pr_info("<%s>\tCount(%lu) Time(%lld/%lld) Prevent(%lld)\n",
         name, count, ktime_to_ms(active_time), ktime_to_ms(total),
         ktime_to_ms(prevent));
 return true;
}
int wakeup_sources_stats_active(void)
{
 struct wakeup_source *ws;
 unsigned long flags;
 u32 visited = 0, active = 0;
 int index;

 pr_info("Active wake lock:\n");
 index = srcu_read_lock(&wakeup_srcu);
 list_for_each_entry_rcu(ws, &wakeup_sources, entry) {
  if (visited != ~0U)
   visited++;
  if (c5_print_active_wakeup_source(ws) && active != ~0U)
   active++;
 }
 srcu_read_unlock(&wakeup_srcu, index);
 raw_spin_lock_irqsave(&c5_active_ws_lock, flags);
 if (c5_active_ws_calls != ~0U)
  c5_active_ws_calls++;
 c5_active_ws_visited = visited;
 c5_active_ws_active = active;
 raw_spin_unlock_irqrestore(&c5_active_ws_lock, flags);
 return 0;
}
EXPORT_SYMBOL_GPL(wakeup_sources_stats_active);
static int c5_active_ws_state_get(char *buf, const struct kernel_param *kp)
{
 unsigned long flags;
 u32 calls, visited, active;
 raw_spin_lock_irqsave(&c5_active_ws_lock, flags);
 calls = c5_active_ws_calls;
 visited = c5_active_ws_visited;
 active = c5_active_ws_active;
 raw_spin_unlock_irqrestore(&c5_active_ws_lock, flags);
 return scnprintf(buf, PAGE_SIZE,
                  "C5_ACTIVE_WS1 ready=1 calls=%u visited=%u active=%u rc=0\n",
                  calls, visited, active);
}
static int c5_active_ws_probe_get(char *buf, const struct kernel_param *kp)
{
 wakeup_sources_stats_active();
 return c5_active_ws_state_get(buf, kp);
}
static const struct kernel_param_ops c5_active_ws_state_ops = { .get = c5_active_ws_state_get };
static const struct kernel_param_ops c5_active_ws_probe_ops = { .get = c5_active_ws_probe_get };
module_param_cb(c5_active_ws_state, &c5_active_ws_state_ops, NULL, 0400);
module_param_cb(c5_active_ws_probe, &c5_active_ws_probe_ops, NULL, 0400);
