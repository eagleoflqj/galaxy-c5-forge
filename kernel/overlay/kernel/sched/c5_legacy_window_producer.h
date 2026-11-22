/* SPDX-License-Identifier: GPL-2.0-only */
/* Explicit old event producer. Native WALT continues to process its own
 * events independently, including its different prev==next and cycle paths.
 */
#ifndef _C5_LEGACY_WINDOW_PRODUCER_H
#define _C5_LEGACY_WINDOW_PRODUCER_H

static int c5_legacy_window_policy_scale(void *cookie,u64 delta,u64 *scaled)
{
    struct rq *rq=cookie;
    const struct c5_legacy_window_policy_cluster *cluster;
    struct c5_legacy_window_scale_input input;
    unsigned int cpu,slot;
    int error;
    if (!rq || !scaled || in_nmi() || !irqs_disabled())
        return -EINVAL;
    cpu=READ_ONCE(rq->cpu);
    if (cpu>=NR_CPUS || cpu_rq(cpu)!=rq)
        return -EXDEV;
    lockdep_assert_held(&rq->lock);
    error=c5_legacy_window_class_policy_error_read();
    if (error)
        return error;
    slot=c5_legacy_window_policy.cpu_cluster[cpu];
    if (slot>c5_legacy_window_policy.clusters)
        return -EINVAL;
    cluster=&c5_legacy_window_policy.cluster[slot];
    input.current_frequency=cluster->cur_freq;
    input.maximum_frequency=c5_legacy_window_policy.maximum_frequency;
    input.cpu_efficiency=cluster->efficiency;
    input.maximum_efficiency=c5_legacy_window_policy.maximum_efficiency;
    return c5_legacy_window_scale(delta,&input,scaled);
}

static const struct c5_legacy_window_busy_ops c5_legacy_window_policy_ops={
    .scale=c5_legacy_window_policy_scale,
    .read_waiting_on_io=c5_legacy_window_native_waiting_on_io,
};

int c5_legacy_window_native_update(struct task_struct *task,
        struct rq *rq,int event,u64 wallclock,u64 irqtime)
{
    struct c5_legacy_window_lease lease={0};
    struct c5_legacy_window_demand_input input;
    struct c5_legacy_window_workspace *workspace;
    unsigned int cpu;
    int error,released;
    if (!task || !rq || in_nmi() || !irqs_disabled())
        return -EINVAL;
    cpu=READ_ONCE(rq->cpu);
    if (cpu>=NR_CPUS || cpu_rq(cpu)!=rq || task_rq(task)!=rq)
        return -EXDEV;
    lockdep_assert_held(&rq->lock);
    /* Epoch initialization belongs only to its real tick/CPU-start sites.
     * It can drop rq ownership and must never occur while a slot is leased.
     */
    if (READ_ONCE(c5_legacy_window_boot_use_pelt) ||
        READ_ONCE(c5_legacy_window_disabled) || !rq->c5_legacy_window.window_start)
        return 0;
    error=READ_ONCE(rq->c5_legacy_window.error);
    if (!error)
        error=READ_ONCE(task->se.c5_legacy_window.error);
    if (error)
        return error<0?error:-EINVAL;
    error=c5_legacy_window_native_acquire(rq,C5_LEGACY_WORKSPACE_UPDATE,&lease);
    if (error)
        return error;
    workspace=lease.workspace;
    error=c5_legacy_window_native_input(task,rq,event,wallclock,irqtime,&input);
    if (!error)
        error=c5_legacy_window_update(&task->se.c5_legacy_window,&rq->c5_legacy_window,
                      &input,&c5_legacy_window_policy_ops,rq,
                      &workspace->scratch.update,&workspace->journal.update);
    if (!error)
        error=c5_legacy_window_native_replay_update(&lease,task,rq,wallclock,irqtime);
    released=c5_legacy_window_native_release(rq,&lease);
    if (!error)
        error=released;
    return c5_legacy_window_class_account_error(rq,error);
}

static int c5_legacy_window_transition_notify(struct notifier_block *block,
        unsigned long action,void *data)
{
    const struct cpufreq_freqs *frequency=data;
    struct c5_legacy_window_policy_cluster *cluster;
    unsigned long flags;
    unsigned int cpu,slot;
    int error=0;
    if (action!=CPUFREQ_POSTCHANGE)
        return NOTIFY_DONE;
    if (!frequency || in_nmi() || !frequency->new || frequency->cpu>=NR_CPUS ||
        !cpu_possible(frequency->cpu)) {
        c5_legacy_window_class_policy_error_set(-EINVAL);
        return NOTIFY_DONE;
    }
    /* Unlike the native cycle-counter shortcut, old accounting always
     * consumes each affected current task before publishing the new clock.
     * All-rq ownership also serializes this owned cluster map with NOTIFY.
     */
    acquire_rq_locks_irqsave(cpu_possible_mask,&flags);
    error=c5_legacy_window_class_policy_error_read();
    slot=c5_legacy_window_policy.cpu_cluster[frequency->cpu];
    if (!error && slot>c5_legacy_window_policy.clusters)
        error=-EINVAL;
    if (error)
        goto out;
    cluster=&c5_legacy_window_policy.cluster[slot];
    if (cluster->cur_freq==frequency->new)
        goto out;
    for_each_cpu(cpu,&cluster->cpus) {
        struct rq *rq=cpu_rq(cpu);
        error=c5_legacy_window_native_update(rq->curr,rq,TASK_UPDATE,
                                            sched_ktime_clock(),0);
        if (error)
            break;
    }
    /* The physical transition already happened. Retain its observed current
     * frequency even if observer accounting is quarantined by a failure.
     */
    cluster->cur_freq=frequency->new;
out:
    c5_legacy_window_class_policy_error_set(error);
    release_rq_locks_irqrestore(cpu_possible_mask,&flags);
    return NOTIFY_DONE;
}

static struct notifier_block c5_legacy_window_transition_block={
    .notifier_call=c5_legacy_window_transition_notify,
};
#endif
