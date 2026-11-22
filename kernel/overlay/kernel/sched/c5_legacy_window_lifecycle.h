/* SPDX-License-Identifier: GPL-2.0-only */
/* Owned old task-field projection only. This is not a native WALT hook,
 * tasklist reset, synchronized epoch or complete exit accounting protocol.
 * The caller owns the task and rq and supplies the old disabled/clock phase.
 */
#ifndef _C5_LEGACY_WINDOW_LIFECYCLE_H
#define _C5_LEGACY_WINDOW_LIFECYCLE_H
#include <linux/c5_legacy_window_types.h>
#include <linux/errno.h>
#include <linux/string.h>
#define C5_LEGACY_WINDOW_EXIT_MARKER 0xdeaddeadU

struct c5_legacy_window_task_start_ops {
    u64 (*clock)(void *cookie);
};

static inline int c5_legacy_window_task_reset(struct c5_legacy_window_task *task)
{
    u32 marker;
    if (!task)
        return -EINVAL;
    marker=task->sum_history[0]==C5_LEGACY_WINDOW_EXIT_MARKER ?
           C5_LEGACY_WINDOW_EXIT_MARKER : 0;
    memset(task,0,sizeof(*task));
    task->sum_history[0]=marker;
    return 0;
}

static inline int c5_legacy_window_task_start(
        struct c5_legacy_window_task *task,
        const struct c5_legacy_window_rq *rq, bool disabled,
        const struct c5_legacy_window_task_start_ops *ops, void *cookie)
{
    if (!task || !rq)
        return -EINVAL;
    if (!rq->window_start || disabled)
        return c5_legacy_window_task_reset(task);
    if (!ops || !ops->clock)
        return -EINVAL;
    task->mark_start=ops->clock(cookie);
    return 0;
}

/* Only the final reset/mark fragment of old sched_exit. The integration must
 * account current, dequeue and mirror class fields at the separate old phases.
 */
static inline int c5_legacy_window_task_exit_reset(
        struct c5_legacy_window_task *task, u64 wallclock)
{
    int ret=c5_legacy_window_task_reset(task);
    if (ret)
        return ret;
    task->mark_start=wallclock;
    task->sum_history[0]=C5_LEGACY_WINDOW_EXIT_MARKER;
    return 0;
}
#endif
