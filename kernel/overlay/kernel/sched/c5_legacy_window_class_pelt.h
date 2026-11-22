/* SPDX-License-Identifier: GPL-2.0-only */
/* Replace the owned old PELT contribution under the existing primary rq lock.
 * The full ancestry is preflighted before either counters or samples commit.
 */
#ifndef _C5_LEGACY_WINDOW_CLASS_PELT_H
#define _C5_LEGACY_WINDOW_CLASS_PELT_H
#include "c5_legacy_window_class.h"

int c5_legacy_window_native_class_replace_pelt(struct task_struct *task,
        struct rq *rq,u32 old_load,u32 new_load)
{
    struct c5_legacy_window_class_policy_view view;
    struct c5_legacy_window_class_sample sample;
    struct c5_legacy_window_class_input input;
    struct c5_legacy_window_class_state next,*storage;
    struct sched_entity *se;
    struct cfs_rq *cfs;
    u32 old_size,new_size;
    unsigned int pass;
    int error;
    if (!task || !rq || in_nmi() || !irqs_disabled())
        return -EINVAL;
    if (task_rq(task)!=rq || rq->cpu>=8)
        return -EXDEV;
    lockdep_assert_held(&rq->lock);
    if (!c5_legacy_window_native_class_pelt_active() || !task->se.on_rq)
        return 0;
    if (task->sched_class!=&fair_sched_class)
        return -EINVAL;
    error=c5_legacy_window_native_class_load(task,rq,&sample);
    if (error)
        goto out;
    if (!sample.use_pelt || !sample.enabled || sample.disabled || sample.load!=old_load) {
        error=-ESTALE;
        goto out;
    }
    error=c5_legacy_window_native_class_policy(rq,&view);
    if (error)
        goto out;
    memset(&input,0,sizeof(input));
    input.load=old_load;input.cpu_scale=view.cpu_scale[rq->cpu];
    input.max_scale=view.maximum_scale;input.upmigrate=view.upmigrate;
    input.small_task=view.small_task;input.nice=task_nice(task);
    input.nice_cutoff=view.nice_cutoff;input.discouraged=false;
    error=c5_legacy_window_classify(&input,&old_size);
    input.load=new_load;
    if (!error)
        error=c5_legacy_window_classify(&input,&new_size);
    if (error)
        goto out;
    for (pass=0;pass<2;pass++) {
        se=&task->se;
        for_each_sched_entity(se) {
            cfs=cfs_rq_of(se);storage=&cfs->c5_legacy_class.bank[view.bank];next=*storage;
            error=c5_legacy_window_class_task(&next,-1,old_size,old_load,true,true,true,false);
            if (!error)
                error=c5_legacy_window_class_task(&next,1,new_size,new_load,true,true,true,false);
            if (error)
                goto out;
            if (pass)
                *storage=next;
            if (cfs_rq_throttled(cfs))
                break;
        }
        if (!se) {
            storage=&rq->c5_legacy_class.bank[view.bank];next=*storage;
            error=c5_legacy_window_class_task(&next,-1,old_size,old_load,true,true,true,false);
            if (!error)
                error=c5_legacy_window_class_task(&next,1,new_size,new_load,true,true,true,false);
            if (error)
                goto out;
            if (pass) {
                *storage=next;
#ifdef CONFIG_C5_SCHED_ELIGIBLE_COMPAT
                error=c5_legacy_window_publish_eligible(rq,&view);
                if (error)
                    goto out;
#endif
            }
        }
    }
out:
    if (error && !rq->c5_legacy_window.error)
        rq->c5_legacy_window.error=error<0?error:-EINVAL;
    return error;
}
#endif
