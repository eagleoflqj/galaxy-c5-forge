/* SPDX-License-Identifier: GPL-2.0-only */
/* Include in fair.c after rq_of/cfs_rq_of/entity_is_task are defined.
 * Native callbacks provide the existing rq or task-pi serialization.
 * Migration reads only an atomic source counter, never a remote rq clock.
 */
#ifndef _C5_LEGACY_PELT_OBSERVER_H
#define _C5_LEGACY_PELT_OBSERVER_H
#include "c5_legacy_pelt_clock.h"

static inline void c5_legacy_observe_fault(struct c5_legacy_pelt_entity *entity,
                                         int error)
{
    if (error < 0 && !entity->error) {
        WRITE_ONCE(entity->error, error);
        atomic64_inc(&c5_legacy_pelt_faults);
    }
}

static inline void c5_legacy_observe_rq(struct cfs_rq *cfs_rq)
{
    u64 now = c5_legacy_pelt_cfs_clock(cfs_rq) >> 20;
    u64 decays = now - cfs_rq->c5_legacy_last_decay;

    if (decays) {
        atomic64_add(decays, &cfs_rq->c5_legacy_decay_counter);
        cfs_rq->c5_legacy_last_decay = now;
    }
}

static inline int c5_legacy_observe_math(struct cfs_rq *cfs_rq,
                                        struct c5_legacy_pelt_state *samples,
                                        int runnable)
{
    return c5_legacy_pelt_update_live(samples,
              c5_legacy_pelt_cfs_clock(cfs_rq), runnable,
              cpu_of(rq_of(cfs_rq)), READ_ONCE(c5_legacy_pelt_hmp_mode));
}


/* Commit the next PELT sample only after its active class contribution can
 * be replaced. All existing enqueue/update phases keep their own clock call.
 */
static inline int c5_legacy_observe_task_math(struct cfs_rq *cfs_rq,
        struct sched_entity *se,struct c5_legacy_pelt_state *samples,int runnable)
{
#ifdef CONFIG_C5_SCHED_WINDOW_TRACE_COMPAT
    struct c5_legacy_pelt_state next=*samples;
    int ret=c5_legacy_observe_math(cfs_rq,&next,runnable);
    int error;
    if (ret>=0 && se->on_rq && c5_legacy_window_native_class_pelt_active()) {
        error=c5_legacy_window_native_class_replace_pelt(task_of(se),rq_of(cfs_rq),
                    samples->runnable_avg_sum_scaled,next.runnable_avg_sum_scaled);
        if (error)
            return error;
    }
    if (ret>=0)
        *samples=next;
    return ret;
#else
    return c5_legacy_observe_math(cfs_rq,samples,runnable);
#endif
}

static inline void c5_legacy_observe_update(struct sched_entity *se)
{
    struct c5_legacy_pelt_entity *entity;
    int ret;

    if (!entity_is_task(se))
        return;
    entity = &se->c5_legacy_pelt;
    if (entity->error)
        return;
    ret = c5_legacy_observe_task_math(cfs_rq_of(se), se, &entity->samples, se->on_rq);
    c5_legacy_observe_fault(entity, ret);
}

static inline void c5_legacy_observe_enqueue(struct cfs_rq *cfs_rq,
                                           struct sched_entity *se, int wakeup)
{
    struct c5_legacy_pelt_entity *entity, next;
    u64 magnitude;
    int ret;

    if (!entity_is_task(se))
        goto advance;
    entity = &se->c5_legacy_pelt;
    if (entity->error)
        goto advance;
    next = *entity;
    if (next.decay_count <= 0) {
        next.samples.last_runnable_update = rq_clock_task(rq_of(cfs_rq));
        if (next.decay_count) {
            magnitude = 0ULL - (u64)next.decay_count;
            if (magnitude > (0x7fffffffffffffffULL >> 20)) {
                c5_legacy_observe_fault(entity, -ERANGE);
                goto advance;
            }
            next.samples.last_runnable_update -= magnitude << 20;
            ret = c5_legacy_observe_task_math(cfs_rq, se, &next.samples, se->on_rq);
            if (ret < 0) {
                c5_legacy_observe_fault(entity, ret);
                goto advance;
            }
            next.decay_count = 0;
        }
        wakeup = 0;
    } else {
        /* Old synchronization changes only omitted policy contribution. */
        next.decay_count = 0;
    }
    if (wakeup) {
        ret = c5_legacy_observe_task_math(cfs_rq, se, &next.samples, se->on_rq);
        if (ret < 0) {
            c5_legacy_observe_fault(entity, ret);
            goto advance;
        }
    }
    *entity = next;
advance:
    c5_legacy_observe_rq(cfs_rq);
}

static inline void c5_legacy_observe_dequeue(struct cfs_rq *cfs_rq,
                                           struct sched_entity *se, int sleep)
{
    c5_legacy_observe_update(se);
    c5_legacy_observe_rq(cfs_rq);
    if (entity_is_task(se) && !se->c5_legacy_pelt.error && sleep)
        se->c5_legacy_pelt.decay_count =
            atomic64_read(&cfs_rq->c5_legacy_decay_counter);
}

static inline void c5_legacy_observe_migrate(struct task_struct *p)
{
    struct c5_legacy_pelt_entity *entity = &p->se.c5_legacy_pelt;
    struct cfs_rq *source;
    u64 decays;

    if (entity->error || !entity->decay_count)
        return;
    source = cfs_rq_of(&p->se);
    decays = (u64)atomic64_read(&source->c5_legacy_decay_counter);
    decays -= (u64)entity->decay_count;
    entity->decay_count = (s64)(0ULL - decays);
}

static inline void c5_legacy_observe_leave(struct task_struct *p)
{
    p->se.c5_legacy_pelt.decay_count = 0;
}

static inline void c5_legacy_observe_group_rebind(struct task_struct *p,
                                                bool blocked)
{
    if (blocked && !p->se.c5_legacy_pelt.error)
        p->se.c5_legacy_pelt.decay_count =
            atomic64_read(&cfs_rq_of(&p->se)->c5_legacy_decay_counter);
}
#endif
