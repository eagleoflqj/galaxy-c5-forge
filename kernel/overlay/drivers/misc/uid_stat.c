/* SPDX-License-Identifier: GPL-2.0-only
 * Derived from Google drivers/misc/uid_stat.c, copyright2008-2009 Google.
 * Builtin entries live until reboot; proc publication precedes list exposure.
 */
#include <linux/atomic.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/preempt.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uid_stat.h>
#include <net/activity_stats.h>
static DEFINE_SPINLOCK(uid_lock);
static DEFINE_MUTEX(uid_create_lock);
static LIST_HEAD(uid_list);
static struct proc_dir_entry *parent;
struct uid_stat {
 struct list_head link;
 uid_t uid;
 atomic_t tcp_rcv;
 atomic_t tcp_snd;
};
static struct uid_stat *find_uid_stat(uid_t uid)
{
 struct uid_stat *entry;
 list_for_each_entry(entry, &uid_list, link)
  if (entry->uid == uid)
   return entry;
 return NULL;
}
static int uid_stat_atomic_int_show(struct seq_file *m, void *v)
{
 atomic_t *counter = m->private;
 unsigned int bytes = (unsigned int)atomic_read(counter) + (unsigned int)INT_MIN;
 seq_printf(m, "%u\n", bytes);
 return 0;
}
static int uid_stat_read_atomic_int_open(struct inode *inode, struct file *file)
{
 return single_open(file, uid_stat_atomic_int_show, PDE_DATA(inode));
}
static const struct file_operations uid_stat_read_atomic_int_fops = {
 .open = uid_stat_read_atomic_int_open,
 .read = seq_read,
 .llseek = seq_lseek,
 .release = single_release,
};
static struct uid_stat *find_or_create_uid_stat(uid_t uid)
{
 struct uid_stat *entry;
 struct proc_dir_entry *directory, *root;
 unsigned long flags;
 char uid_s[32];
 spin_lock_irqsave(&uid_lock, flags);
 entry = find_uid_stat(uid);
 spin_unlock_irqrestore(&uid_lock, flags);
 if (entry)
  return entry;
 root = smp_load_acquire(&parent);
 if (!root || in_atomic() || irqs_disabled())
  return NULL;
 mutex_lock(&uid_create_lock);
 spin_lock_irqsave(&uid_lock, flags);
 entry = find_uid_stat(uid);
 spin_unlock_irqrestore(&uid_lock, flags);
 if (entry)
  goto unlock;
 entry = kmalloc(sizeof(*entry), GFP_KERNEL);
 if (!entry)
  goto unlock;
 entry->uid = uid;
 atomic_set(&entry->tcp_rcv, INT_MIN);
 atomic_set(&entry->tcp_snd, INT_MIN);
 snprintf(uid_s, sizeof(uid_s), "%d", (int)uid);
 directory = proc_mkdir(uid_s, root);
 if (!directory)
  goto free_entry;
 if (!proc_create_data("tcp_snd", 0444, directory,
  &uid_stat_read_atomic_int_fops, &entry->tcp_snd) ||
  !proc_create_data("tcp_rcv", 0444, directory,
  &uid_stat_read_atomic_int_fops, &entry->tcp_rcv)) {
  proc_remove(directory);
  goto free_entry;
 }
 spin_lock_irqsave(&uid_lock, flags);
 list_add_tail(&entry->link, &uid_list);
 spin_unlock_irqrestore(&uid_lock, flags);
 goto unlock;
free_entry:
 kfree(entry);
 entry = NULL;
unlock:
 mutex_unlock(&uid_create_lock);
 return entry;
}
int uid_stat_tcp_snd(uid_t uid, int size)
{
 struct uid_stat *entry;
 activity_stats_update();
 entry = find_or_create_uid_stat(uid);
 if (!entry)
  return -1;
 atomic_add(size, &entry->tcp_snd);
 return 0;
}
int uid_stat_tcp_rcv(uid_t uid, int size)
{
 struct uid_stat *entry;
 activity_stats_update();
 entry = find_or_create_uid_stat(uid);
 if (!entry)
  return -1;
 atomic_add(size, &entry->tcp_rcv);
 return 0;
}
static int __init uid_stat_init(void)
{
 struct proc_dir_entry *entry = proc_mkdir("uid_stat", NULL);
 if (!entry)
  return -ENOMEM;
 smp_store_release(&parent, entry);
 return 0;
}
module_init(uid_stat_init);
