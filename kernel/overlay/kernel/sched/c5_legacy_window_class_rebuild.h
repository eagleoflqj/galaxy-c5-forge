/* SPDX-License-Identifier: GPL-2.0-only */
/* Included at the end of fair.c with the actual CFS hierarchy helpers. */
#ifndef _C5_LEGACY_WINDOW_CLASS_REBUILD_H
#define _C5_LEGACY_WINDOW_CLASS_REBUILD_H
#include "c5_legacy_window_class.h"

static int c5_legacy_window_rebuild_task(struct task_struct *task,struct rq *rq,
        const struct c5_legacy_window_class_policy_view *view)
{
    struct c5_legacy_window_class_sample sample;
    struct c5_legacy_window_class_input input;
    struct sched_entity *se=&task->se;
    struct cfs_rq *cfs;
    u32 size;
    int error;
    error=c5_legacy_window_native_class_load(task,rq,&sample);
    if (error || !sample.enabled || sample.disabled)
        return error;
    memset(&input,0,sizeof(input));
    input.load=sample.load;
    input.cpu_scale=view->cpu_scale[rq->cpu];
    input.max_scale=view->maximum_scale;
    input.upmigrate=view->upmigrate;input.small_task=view->small_task;
    input.nice=task_nice(task);input.nice_cutoff=view->nice_cutoff;
    /* cpu.upmigrate_discourage and its semantics are an accepted limit;
     * false is the retained default, not a restored cgroup implementation.
     */
    input.discouraged=false;
    error=c5_legacy_window_classify(&input,&size);
    if (error)
        return error;
    for_each_sched_entity(se) {
        cfs=cfs_rq_of(se);
        error=c5_legacy_window_class_task(&cfs->c5_legacy_class.bank[view->bank],
                   1,size,0,true,false,true,false);
        if (error)
            return error;
        if (cfs_rq_throttled(cfs))
            break;
    }
    if (!se)
        return c5_legacy_window_class_task(&rq->c5_legacy_class.bank[view->bank],
                   1,size,0,true,false,true,false);
    return 0;
}

int c5_legacy_window_native_reclassify_locked(
        const struct c5_legacy_window_class_policy_view *view,bool disabled)
{
    struct task_group *group;
    struct cfs_rq *cfs;
    struct task_struct *task;
    struct rq *rq;
    unsigned int cpu,old;
    int error=0;
    if (!view || view->bank>1 || in_nmi() || !irqs_disabled())
        return -EINVAL;
    old=1-view->bank;
    for_each_possible_cpu(cpu)
        lockdep_assert_held(&cpu_rq(cpu)->lock);
    rcu_read_lock();
    for_each_possible_cpu(cpu) {
        rq=cpu_rq(cpu);
        /* Include every task group, not just currently linked leaf queues:
         * a previously used inactive bank must not retain stale counters.
         * RCU protects group lifetime; the actual rq locks protect contents.
         */
        list_for_each_entry_rcu(group,&task_groups,list) {
            cfs=group->cfs_rq[cpu];
            if (!cfs)
                continue;
            cfs->c5_legacy_class.bank[view->bank]=cfs->c5_legacy_class.bank[old];
            c5_legacy_window_class_reset(&cfs->c5_legacy_class.bank[view->bank],false);
        }
        rq->c5_legacy_class.bank[view->bank]=rq->c5_legacy_class.bank[old];
        c5_legacy_window_class_reset(&rq->c5_legacy_class.bank[view->bank],false);
        if (!c5_legacy_pelt_hmp_enabled() || disabled)
            continue;
        list_for_each_entry(task,&rq->cfs_tasks,se.group_node) {
            error=c5_legacy_window_rebuild_task(task,rq,view);
            if (error)
                goto out;
        }
    }
out:
    rcu_read_unlock();
    /* Failure leaves only the inactive bank dirty. The actual notifier
     * refuses policy/bank publication and quarantines the observer.
     */
    return error;
}
#endif
