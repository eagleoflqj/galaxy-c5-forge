/* SPDX-License-Identifier: GPL-2.0-only */
/* Old scalar window state and lock/copy protocol. Callbacks must implement the
 * stated native locking and clock contract before any real integration.
 * CPU-role publication/interleavings and actual Linux hotplug are not proved.
 */
#ifndef _C5_LEGACY_WINDOW_INITIALIZATION_H
#define _C5_LEGACY_WINDOW_INITIALIZATION_H
#include "c5_legacy_window_demand.h"

struct c5_legacy_window_start_input {
    u32 cpu, sync_cpu;
    bool hmp_enabled;
};
struct c5_legacy_window_start_ops {
    u64 (*clock)(void *cookie);
    void (*unlock_local)(void *cookie);
    void (*lock_pair)(void *cookie);
    u64 (*read_sync_window)(void *cookie);
    void (*unlock_sync)(void *cookie);
    struct c5_legacy_window_task *(*running_task)(void *cookie);
};

static inline int c5_legacy_window_initialize(
        struct c5_legacy_window_rq *rq,
        const struct c5_legacy_window_start_input *input,
        const struct c5_legacy_window_start_ops *ops, void *cookie)
{
    struct c5_legacy_window_task *running;
    if (!rq || !input)
        return -EINVAL;
    if (rq->window_start || !input->hmp_enabled)
        return 0;
    if (!ops || !ops->running_task)
        return -EINVAL;
    if (input->cpu==input->sync_cpu) {
        if (!ops || !ops->clock)
            return -EINVAL;
        rq->window_start=ops->clock(cookie);
    } else {
        if (!ops || !ops->unlock_local || !ops->lock_pair ||
            !ops->read_sync_window || !ops->unlock_sync)
            return -EINVAL;
        ops->unlock_local(cookie);
        ops->lock_pair(cookie);
        rq->window_start=ops->read_sync_window(cookie);
        rq->curr_runnable_sum=rq->prev_runnable_sum=0;
        ops->unlock_sync(cookie);
    }
    /* Original protocol changes the current task only after sync unlock. */
    running=ops->running_task(cookie);
    if (!running)
        return -EINVAL;
    running->mark_start=rq->window_start;
    return 0;
}

/* These inputs are the actual old primary default and parent override. The
 * live primary/default publication and PELT/native task fields are not owned
 * by this projection. Preserve u64 multiply/divide followed by u32 assignment.
 */
static inline int c5_legacy_window_fork_seed(
        struct c5_legacy_window_task *task, u32 parent_pct,
        u32 default_window_seed, u32 window_size)
{
    u32 seed=default_window_seed;
    int i;
    if (!task)
        return -EINVAL;
    if (parent_pct)
        seed=div64_u64((u64)parent_pct*(u64)window_size,100);
    memset(task,0,sizeof(*task));
    task->demand=seed;
    for (i=0;i<5;i++)
        task->sum_history[i]=seed;
    return 0;
}

static inline int c5_legacy_window_sync_cpu_after_dying(
        u32 old_sync, u32 dying_cpu, u32 executing_cpu, u32 *new_sync)
{
    if (!new_sync)
        return -EINVAL;
    *new_sync=old_sync;
    if (dying_cpu==old_sync)
        *new_sync=executing_cpu;
    return 0;
}
#endif
