/* SPDX-License-Identifier: GPL-2.0-only */
/* Actual class-phase accounting. Included after the independent policy in
 * walt.c. A caller owns the task, primary rq and supplied hierarchy storage;
 * the policy writer needs all these rq locks before changing the active bank.
 */
#ifndef _C5_LEGACY_WINDOW_CLASS_ACCOUNT_H
#define _C5_LEGACY_WINDOW_CLASS_ACCOUNT_H

static int c5_legacy_window_class_account_error(struct rq *rq,int error)
{
    if (error && rq && !rq->c5_legacy_window.error)
        rq->c5_legacy_window.error=error<0?error:-EINVAL;
    return error;
}

int c5_legacy_window_native_class_account(
        struct c5_legacy_window_class_storage *storage,struct rq *rq,
        struct task_struct *task,int direction,bool sizes)
{
    struct c5_legacy_window_class_policy_view view;
    struct c5_legacy_window_class_sample sample;
    struct c5_legacy_window_class_input input;
    u32 size=C5_LEGACY_SIZE_NONE;
    int error;
    if (!storage || !rq || !task || (direction!=1 && direction!=-1))
        return c5_legacy_window_class_account_error(rq,-EINVAL);
    error=c5_legacy_window_native_class_load(task,rq,&sample);
    if (error)
        goto out;
    if (!sample.enabled || sample.disabled)
        return 0;
    error=c5_legacy_window_native_class_policy(rq,&view);
    if (error)
        goto out;
    if (sizes) {
        memset(&input,0,sizeof(input));
        input.load=sample.load;input.cpu_scale=view.cpu_scale[rq->cpu];
        input.max_scale=view.maximum_scale;
        input.upmigrate=view.upmigrate;input.small_task=view.small_task;
        input.nice=task_nice(task);input.nice_cutoff=view.nice_cutoff;
        /* Accepted independent cgroup-discourage implementation limit. */
        input.discouraged=false;
        error=c5_legacy_window_classify(&input,&size);
        if (error)
            goto out;
    }
    error=c5_legacy_window_class_task(&storage->bank[view.bank],direction,
                                    size,sample.load,sizes,true,true,false);
#ifdef CONFIG_C5_SCHED_ELIGIBLE_COMPAT
    if (!error && storage==&rq->c5_legacy_class)
        error=c5_legacy_window_publish_eligible(rq,&view);
#endif
out:
    /* A failed primitive never publishes its partial result. If an earlier
     * hierarchy node was updated, quarantine this rq; do not report valid
     * complete statistics or disturb native scheduling/control.
     */
    return c5_legacy_window_class_account_error(rq,error);
}

int c5_legacy_window_native_class_transfer(
        struct c5_legacy_window_class_storage *destination,
        const struct c5_legacy_window_class_storage *source,
        struct rq *rq,int direction)
{
    struct c5_legacy_window_class_policy_view view;
    int error;
    if (!destination || !source || !rq || (direction!=1 && direction!=-1))
        return c5_legacy_window_class_account_error(rq,-EINVAL);
    error=c5_legacy_window_native_class_policy(rq,&view);
    if (!error) {
        /* The old throttle aggregate has no HMP/disabled gate of its own. */
        error=c5_legacy_window_class_transfer(&destination->bank[view.bank],
                                             &source->bank[view.bank],direction);
#ifdef CONFIG_C5_SCHED_ELIGIBLE_COMPAT
        if (!error && destination==&rq->c5_legacy_class)
            error=c5_legacy_window_publish_eligible(rq,&view);
#endif
    }
    return c5_legacy_window_class_account_error(rq,error);
}
#endif
