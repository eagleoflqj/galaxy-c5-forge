/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/moduleparam.h>
static DEFINE_RAW_SPINLOCK(c5_freezer_diag_lock);
static u32 c5_freezer_notes, c5_freezer_aborts;

struct c5_freezer_last {
 pid_t pid;
 char comm[TASK_COMM_LEN];
};
/* Caller holds tasklist_lock; retain copied identity, never a task pointer. */
static noinline void c5_freezer_note(struct c5_freezer_last *last,
                                    struct task_struct *task)
{
 unsigned long flags;
 last->pid = task_pid_nr(task);
 get_task_comm(last->comm, task);
 raw_spin_lock_irqsave(&c5_freezer_diag_lock, flags);
 if (c5_freezer_notes != ~0U)
  c5_freezer_notes++;
 raw_spin_unlock_irqrestore(&c5_freezer_diag_lock, flags);
}
static noinline void c5_freezer_print(const struct c5_freezer_last *last,
                                     bool user_only)
{
 unsigned long flags;
 pr_err("Freezing of %s aborted (%d) (%s)\n",
        user_only ? "user space " : "tasks ", last->pid, last->comm);
 raw_spin_lock_irqsave(&c5_freezer_diag_lock, flags);
 if (c5_freezer_aborts != ~0U)
  c5_freezer_aborts++;
 raw_spin_unlock_irqrestore(&c5_freezer_diag_lock, flags);
}
static int c5_freezer_state_get(char *buf, const struct kernel_param *kp)
{
 unsigned long flags;
 u32 notes, aborts;
 raw_spin_lock_irqsave(&c5_freezer_diag_lock, flags);
 notes = c5_freezer_notes;
 aborts = c5_freezer_aborts;
 raw_spin_unlock_irqrestore(&c5_freezer_diag_lock, flags);
 return scnprintf(buf, PAGE_SIZE,
                  "C5_FREEZER_DIAG1 ready=1 notes=%u aborts=%u rc=0\n",
                  notes, aborts);
}
static const struct kernel_param_ops c5_freezer_state_ops = { .get = c5_freezer_state_get };
module_param_cb(c5_freezer_diag_state, &c5_freezer_state_ops, NULL, 0400);
