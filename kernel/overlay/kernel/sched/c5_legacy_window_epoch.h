/* SPDX-License-Identifier: GPL-2.0-only */
/* Owned epoch protocol. Entry and return own the local rq with IRQs disabled.
 * lock_pair returns with local+peer held; unlock_peer retains local ownership.
 * Every role writer must own the old role rq, and hotplug peers must remain
 * online under the actual hotplug serialization. Native bindings are required.
 */
#ifndef _C5_LEGACY_WINDOW_EPOCH_H
#define _C5_LEGACY_WINDOW_EPOCH_H
#include <linux/c5_legacy_window_types.h>
#include <linux/errno.h>

struct c5_legacy_window_epoch_input {u32 cpu,nr_cpus;bool enabled;};
struct c5_legacy_window_epoch_ops {
    u32 (*read_role)(void *cookie);
    u64 (*clock)(void *cookie);
    void (*unlock_local)(void *cookie);
    void (*lock_pair)(void *cookie,u32 peer);
    void (*unlock_peer)(void *cookie,u32 peer);
    u64 (*read_window)(void *cookie,u32 peer);
    struct c5_legacy_window_task *(*running_task)(void *cookie,u32 cpu);
    void (*publish_role)(void *cookie,u32 peer);
};

static inline int c5_legacy_window_epoch_initialize(
        struct c5_legacy_window_rq *rq,
        const struct c5_legacy_window_epoch_input *input,
        const struct c5_legacy_window_epoch_ops *ops,void *cookie)
{
    struct c5_legacy_window_task *running;
    u32 role;
    if (!rq || !input || !input->nr_cpus || input->cpu>=input->nr_cpus)
        return -EINVAL;
    if (rq->window_start || !input->enabled)
        return 0;
    if (!ops || !ops->read_role || !ops->running_task)
        return -EINVAL;
    for (;;) {
        /* Another legitimate initializer may run while local is unlocked. */
        if (rq->window_start)
            return 0;
        role=ops->read_role(cookie);
        if (role>=input->nr_cpus)
            return -EINVAL;
        if (role==input->cpu) {
            if (!ops->clock)
                return -EINVAL;
            rq->window_start=ops->clock(cookie);
        } else {
            if (!ops->unlock_local || !ops->lock_pair || !ops->unlock_peer ||
                !ops->read_window)
                return -EINVAL;
            ops->unlock_local(cookie);
            ops->lock_pair(cookie,role);
            if (rq->window_start) {
                ops->unlock_peer(cookie,role);
                return 0;
            }
            if (ops->read_role(cookie)!=role) {
                ops->unlock_peer(cookie,role);
                continue;
            }
            rq->window_start=ops->read_window(cookie,role);
            rq->curr_runnable_sum=rq->prev_runnable_sum=0;
            ops->unlock_peer(cookie,role);
        }
        running=ops->running_task(cookie,input->cpu);
        if (!running)
            return -EINVAL;
        running->mark_start=rq->window_start;
        return 0;
    }
}

static inline int c5_legacy_window_epoch_handoff(
        struct c5_legacy_window_rq *dying,struct c5_legacy_window_rq *peer_rq,
        u32 cpu,u32 peer,u32 nr_cpus,
        const struct c5_legacy_window_epoch_ops *ops,void *cookie)
{
    struct c5_legacy_window_task *running;
    if (!dying || !nr_cpus || cpu>=nr_cpus || !ops || !ops->read_role)
        return -EINVAL;
    if (ops->read_role(cookie)!=cpu)
        return 0;
    if (peer>=nr_cpus)
        return -EAGAIN;
    if (peer==cpu || !peer_rq || peer_rq==dying || !ops->unlock_local ||
        !ops->lock_pair || !ops->unlock_peer || !ops->publish_role ||
        !ops->running_task)
        return -EINVAL;
    ops->unlock_local(cookie);
    ops->lock_pair(cookie,peer);
    if (ops->read_role(cookie)==cpu) {
        if (!peer_rq->window_start && dying->window_start) {
            running=ops->running_task(cookie,peer);
            if (!running) {
                ops->unlock_peer(cookie,peer);
                return -EINVAL;
            }
            peer_rq->window_start=dying->window_start;
            peer_rq->curr_runnable_sum=peer_rq->prev_runnable_sum=0;
            running->mark_start=peer_rq->window_start;
        }
        /* Publish only after the peer epoch and its current task are ready. */
        ops->publish_role(cookie,peer);
    }
    ops->unlock_peer(cookie,peer);
    return 0;
}
#endif
