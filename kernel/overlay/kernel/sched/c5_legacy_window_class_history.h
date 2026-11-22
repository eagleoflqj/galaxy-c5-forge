/* SPDX-License-Identifier: GPL-2.0-only */
/* Logical window-history contributions. Never overwrite the task's final
 * demand just to make a replayed old class method observe an earlier phase.
 */
#ifndef _C5_LEGACY_WINDOW_CLASS_HISTORY_H
#define _C5_LEGACY_WINDOW_CLASS_HISTORY_H
#include "c5_legacy_window_class.h"

int c5_legacy_window_native_class_adjust_load(struct task_struct *task,
        struct rq *rq,const struct c5_legacy_window_class_policy_view *view,
        int direction,u32 load,bool enabled,bool disabled)
{
    struct c5_legacy_window_class_input input;
    struct c5_legacy_window_class_state next;
    struct c5_legacy_window_class_state *storage;
    struct sched_entity *se;
    struct cfs_rq *cfs;
    u32 size=C5_LEGACY_SIZE_NONE;
    unsigned int pass;
    bool fair;
    int error;
    if (!task || !rq || !view || view->bank>1 || in_nmi() || !irqs_disabled() ||
        (direction!=1 && direction!=-1))
        return -EINVAL;
    if (task_rq(task)!=rq || rq->cpu>=8)
        return -EXDEV;
    lockdep_assert_held(&rq->lock);
    if (!enabled || disabled)
        return 0;
    fair=task->sched_class==&fair_sched_class;
    if (!fair && task->sched_class!=&rt_sched_class &&
        task->sched_class!=&dl_sched_class && task->sched_class!=&stop_sched_class) {
        if (task->sched_class==&idle_sched_class)
            return 0;
        return -EOPNOTSUPP;
    }
    if (fair) {
        memset(&input,0,sizeof(input));
        input.load=load;input.cpu_scale=view->cpu_scale[rq->cpu];
        input.max_scale=view->maximum_scale;
        input.upmigrate=view->upmigrate;input.small_task=view->small_task;
        input.nice=task_nice(task);input.nice_cutoff=view->nice_cutoff;
        input.discouraged=false;
        error=c5_legacy_window_classify(&input,&size);
        if (error)
            return error;
    }
    /* Preflight the whole contribution before changing any hierarchy node.
     * No callback, allocation or lock drop separates preflight from commit.
     * All inputs and storage are stable under the actual primary rq lock.
     */
    for (pass=0;pass<2;pass++) {
        se=fair?&task->se:NULL;
        for_each_sched_entity(se) {
            cfs=cfs_rq_of(se);
            storage=&cfs->c5_legacy_class.bank[view->bank];next=*storage;
            error=c5_legacy_window_class_task(&next,direction,size,load,
                                             true,true,true,false);
            if (error)
                return error;
            if (pass)
                *storage=next;
            if (cfs_rq_throttled(cfs))
                break;
        }
        if (!se) {
            storage=&rq->c5_legacy_class.bank[view->bank];next=*storage;
            error=c5_legacy_window_class_task(&next,direction,size,load,
                                             fair,true,true,false);
            if (error)
                return error;
            if (pass) {
                *storage=next;
#ifdef CONFIG_C5_SCHED_ELIGIBLE_COMPAT
                error=c5_legacy_window_publish_eligible(rq,view);
                if (error)
                    return error;
#endif
            }
        }
    }
    return 0;
}
#endif
