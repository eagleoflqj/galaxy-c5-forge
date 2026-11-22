/* SPDX-License-Identifier: GPL-2.0-only */
/* One-shot delivery of an actual published UPDATE workspace. The task/rq
 * already contain the final math result; each DEC/INC uses its record's own
 * logical demand and each history trace observes its immediately prior counts.
 */
#ifndef _C5_LEGACY_WINDOW_REPLAY_H
#define _C5_LEGACY_WINDOW_REPLAY_H

static int c5_legacy_window_update_journal_shape(
        const struct c5_legacy_window_journal *journal)
{
    const struct c5_legacy_window_record *record,*last;
    u32 i=0,count=journal->count;
    if (count>C5_LEGACY_WINDOW_RECORDS)
        return -EINVAL;
    if (!count)
        return 0;
    last=&journal->records[count-1];
    if (last->kind!=C5_LEGACY_RECORD_UPDATE ||
        last->event<C5_LEGACY_PUT_PREV || last->event>C5_LEGACY_IRQ)
        return -EINVAL;
    while (i<count-1) {
        record=&journal->records[i];
        if (record->event!=last->event)
            return -EINVAL;
        if (record->kind==C5_LEGACY_RECORD_DEC) {
            if (i+2>=count-1 || journal->records[i+1].kind!=C5_LEGACY_RECORD_INC ||
                journal->records[i+2].kind!=C5_LEGACY_RECORD_HISTORY)
                return -EINVAL;
            if (journal->records[i+1].event!=last->event ||
                journal->records[i+2].event!=last->event ||
                journal->records[i+1].runtime!=record->runtime ||
                journal->records[i+2].runtime!=record->runtime ||
                journal->records[i+1].samples!=record->samples ||
                journal->records[i+2].samples!=record->samples)
                return -EINVAL;
            i+=3;
        } else if (record->kind==C5_LEGACY_RECORD_HISTORY) {
            i++;
        } else {
            return -EINVAL;
        }
    }
    return 0;
}

int c5_legacy_window_native_replay_update(
        struct c5_legacy_window_lease *lease,struct task_struct *task,
        struct rq *rq,u64 wallclock,u64 irqtime)
{
    struct c5_legacy_window_workspace *workspace;
    struct c5_legacy_window_class_policy_view policy;
    struct c5_legacy_window_class_state counters;
    const struct c5_legacy_window_record *record;
    u32 i,count;
    bool enabled,disabled;
    int error;
    if (!lease || !task || !rq || in_nmi() || !irqs_disabled())
        return -EINVAL;
    workspace=this_cpu_ptr(&c5_legacy_window_workspaces);
    if (workspace->trace.active)
        return -EBUSY;
    if (lease->cpu!=raw_smp_processor_id() || workspace->cpu!=raw_smp_processor_id())
        return -EXDEV;
    if (lease->workspace!=workspace || workspace->kind!=C5_LEGACY_WORKSPACE_UPDATE ||
        lease->kind!=workspace->kind || lease->generation!=workspace->generation ||
        lease->owner!=rq || workspace->owner!=rq)
        return -ESTALE;
    if (task_rq(task)!=rq)
        return -EXDEV;
    lockdep_assert_held(&rq->lock);
    error=c5_legacy_window_update_journal_shape(&workspace->journal.update);
    if (error)
        return error;
    count=workspace->journal.update.count;
    if (!count)
        return 0;
    if (READ_ONCE(c5_legacy_window_boot_use_pelt))
        return -EINVAL;
    error=c5_legacy_window_native_class_policy(rq,&policy);
    if (error)
        return error;
    enabled=c5_legacy_pelt_hmp_enabled();
    disabled=READ_ONCE(c5_legacy_window_disabled);
    for (i=0;i<count;i++) {
        record=&workspace->journal.update.records[i];
        if (record->kind==C5_LEGACY_RECORD_DEC || record->kind==C5_LEGACY_RECORD_INC) {
            error=c5_legacy_window_native_class_adjust_load(task,rq,&policy,
                      record->kind==C5_LEGACY_RECORD_DEC?-1:1,
                      record->task.demand,enabled,disabled);
        } else {
            if (record->kind==C5_LEGACY_RECORD_HISTORY) {
                counters=rq->c5_legacy_class.bank[policy.bank];
                if (counters.nr_big_tasks<0 || counters.nr_small_tasks<0) {
                    error=-ERANGE;
                    goto failed;
                }
            }
            error=c5_legacy_window_native_emit_record(lease,task,rq,record,
                       wallclock,irqtime,record->kind==C5_LEGACY_RECORD_HISTORY?&counters:NULL);
        }
        if (error)
            goto failed;
    }
    /* Successful consumption cannot replay its DEC/INC or duplicate traces.
     * Retain the lease for the caller's normal checked release.
     */
    workspace->journal.update.count=0;
    return 0;
failed:
    return c5_legacy_window_class_account_error(rq,error);
}
#endif
