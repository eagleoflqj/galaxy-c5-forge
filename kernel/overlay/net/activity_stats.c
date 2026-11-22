/* SPDX-License-Identifier: GPL-2.0-only
 * Derived from Google net/activity_stats.c, copyright2010 Google, Inc.
 * Optional builtin ten-bucket legacy interface; no new network hook here.
 */
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/suspend.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>
#include <net/net_namespace.h>
#include <net/activity_stats.h>
#define BUCKET_MAX 10
static unsigned long activity_stats[BUCKET_MAX];
static ktime_t last_transmit;
static ktime_t suspend_time;
static bool suspend_prepared;
static DEFINE_SPINLOCK(activity_lock);
void activity_stats_update(void)
{
 unsigned long flags;
 ktime_t now;
 s64 delta;
 int i;
 spin_lock_irqsave(&activity_lock, flags);
 now = ktime_get();
 delta = ktime_to_ns(ktime_sub(now, last_transmit));
 if (delta >= 0) {
  for (i = BUCKET_MAX - 1; i >= 0; i--) {
   if ((u64)delta < (1000000000ULL << i))
    continue;
   activity_stats[i]++;
   last_transmit = now;
   break;
  }
 }
 spin_unlock_irqrestore(&activity_lock, flags);
}
static int activity_stats_show(struct seq_file *m, void *v)
{
 unsigned long flags, snapshot[BUCKET_MAX];
 int i;
 spin_lock_irqsave(&activity_lock, flags);
 for (i = 0; i < BUCKET_MAX; i++)
  snapshot[i] = activity_stats[i];
 spin_unlock_irqrestore(&activity_lock, flags);
 seq_printf(m, "Min Bucket(sec) Count\n");
 for (i = 0; i < BUCKET_MAX; i++)
  seq_printf(m, "%15d %lu\n", 1 << i, snapshot[i]);
 return 0;
}
static int activity_stats_notifier(struct notifier_block *nb,
 unsigned long event, void *dummy)
{
 unsigned long flags;
 spin_lock_irqsave(&activity_lock, flags);
 if (event == PM_SUSPEND_PREPARE) {
  suspend_time = ktime_get_real();
  suspend_prepared = true;
 } else if (event == PM_POST_SUSPEND && suspend_prepared) {
  last_transmit = ktime_sub(last_transmit,
   ktime_sub(ktime_get_real(), suspend_time));
  suspend_prepared = false;
 }
 spin_unlock_irqrestore(&activity_lock, flags);
 return 0;
}
static int activity_stats_open(struct inode *inode, struct file *file)
{
 return single_open(file, activity_stats_show, PDE_DATA(inode));
}
static const struct file_operations activity_stats_fops = {
 .open = activity_stats_open,
 .read = seq_read,
 .llseek = seq_lseek,
 .release = single_release,
};
static struct notifier_block activity_stats_notifier_block = {
 .notifier_call = activity_stats_notifier,
};
static int __init activity_stats_init(void)
{
 struct proc_dir_entry *entry;
 int rc;
 entry = proc_create("activity", 0444, init_net.proc_net_stat,
  &activity_stats_fops);
 if (!entry)
  return -ENOMEM;
 rc = register_pm_notifier(&activity_stats_notifier_block);
 if (rc)
  proc_remove(entry);
 return rc;
}
subsys_initcall(activity_stats_init);
