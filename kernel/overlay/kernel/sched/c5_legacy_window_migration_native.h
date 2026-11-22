/* SPDX-License-Identifier: GPL-2.0-only */
/* One locked, leased old migration transaction and its role-indexed journal.
 * The native caller owns source/destination until this observer returns.
 */
#ifndef _C5_LEGACY_WINDOW_MIGRATION_NATIVE_H
#define _C5_LEGACY_WINDOW_MIGRATION_NATIVE_H

/* Old default. A real serialized runtime setter/reset protocol is still
 * required; this variable is not a successful replacement for a missing knob.
 */
static bool c5_legacy_window_migration_fixup=true;

static int c5_legacy_window_migration_journal_roles(
        const struct c5_legacy_window_migration_journal *journal,
        struct task_struct *const actors[3],struct rq *const queues[2])
{
    const struct c5_legacy_window_migration_record *record;
    u32 i,j,count=journal->count;
    if (count>C5_LEGACY_MIGRATION_RECORDS)
        return -EINVAL;
    if (!count)
        return 0;
    if (count<2)
        return -EINVAL;
    for (i=0;i<count;i++) {
        record=&journal->records[i];
        if (record->task_slot>=3 || record->rq_slot>=2 ||
            !actors[record->task_slot] || !queues[record->rq_slot])
            return -EINVAL;
        /* Pure alias canonicalization always uses the first physical role. */
        for (j=0;j<record->task_slot;j++)
            if (actors[j]==actors[record->task_slot])
                return -EINVAL;
        if (i>=count-2) {
            if (record->value.kind!=C5_LEGACY_RECORD_MIGRATION ||
                record->task_slot!=0 || record->rq_slot!=i-(count-2))
                return -EINVAL;
            continue;
        }
        if (record->value.kind<C5_LEGACY_RECORD_DEC ||
            record->value.kind>C5_LEGACY_RECORD_UPDATE ||
            task_rq(actors[record->task_slot])!=queues[record->rq_slot])
            return -EINVAL;
        if ((record->value.kind==C5_LEGACY_RECORD_HISTORY ||
             record->value.kind==C5_LEGACY_RECORD_UPDATE) &&
            (record->value.event<C5_LEGACY_PUT_PREV ||
             record->value.event>C5_LEGACY_IRQ))
            return -EINVAL;
    }
    return 0;
}

static int c5_legacy_window_native_replay_migration(
        struct c5_legacy_window_lease *lease,struct task_struct *const actors[3],
        struct rq *const queues[2],u64 wallclock)
{
    struct c5_legacy_window_workspace *workspace;
    struct c5_legacy_window_class_policy_view policy,peer;
    struct c5_legacy_window_class_state counters;
    struct c5_legacy_window_class_sample sample;
    const struct c5_legacy_window_migration_record *record;
    struct task_struct *task;
    struct rq *rq;
    u32 i,count,load;
    bool enabled,disabled,use_pelt;
    int error;
    if (!lease || !actors || !queues || !queues[0] || !queues[1] ||
        queues[0]==queues[1] || in_nmi() || !irqs_disabled())
        return -EINVAL;
    workspace=this_cpu_ptr(&c5_legacy_window_workspaces);
    if (workspace->trace.active)
        return -EBUSY;
    if (lease->cpu!=raw_smp_processor_id() || workspace->cpu!=raw_smp_processor_id())
        return -EXDEV;
    if (lease->workspace!=workspace || workspace->kind!=C5_LEGACY_WORKSPACE_MIGRATION ||
        lease->kind!=workspace->kind || lease->generation!=workspace->generation ||
        lease->owner!=queues[0] || workspace->owner!=queues[0])
        return -ESTALE;
    lockdep_assert_held(&queues[0]->lock);
    lockdep_assert_held(&queues[1]->lock);
    error=c5_legacy_window_migration_journal_roles(&workspace->journal.migration,
                                                  actors,queues);
    if (error)
        return error;
    count=workspace->journal.migration.count;
    if (!count)
        return 0;
    error=c5_legacy_window_native_class_policy(queues[0],&policy);
    if (!error)
        error=c5_legacy_window_native_class_policy(queues[1],&peer);
    if (error)
        return error;
    if (policy.bank!=peer.bank || policy.generation!=peer.generation)
        return -ESTALE;
    enabled=c5_legacy_pelt_hmp_enabled();
    disabled=READ_ONCE(c5_legacy_window_disabled);
    use_pelt=READ_ONCE(c5_legacy_window_boot_use_pelt);
    for (i=0;i<count;i++) {
        record=&workspace->journal.migration.records[i];
        task=actors[record->task_slot];rq=queues[record->rq_slot];
        if (record->value.kind==C5_LEGACY_RECORD_DEC ||
            record->value.kind==C5_LEGACY_RECORD_INC) {
            load=record->value.task.demand;
            /* PELT outer updates are skipped, but old migration still makes
             * its temporary INC/DEC. Those consume actual owned PELT load,
             * not the inactive window-demand field in the phase record.
             */
            if (use_pelt) {
                error=c5_legacy_window_native_class_load(task,rq,&sample);
                if (error)
                    return error;
                load=sample.load;
            }
            error=c5_legacy_window_native_class_adjust_load(task,rq,&policy,
                      record->value.kind==C5_LEGACY_RECORD_DEC?-1:1,
                      load,enabled,disabled);
        } else {
            if (record->value.kind==C5_LEGACY_RECORD_HISTORY) {
                counters=rq->c5_legacy_class.bank[policy.bank];
                if (counters.nr_big_tasks<0 || counters.nr_small_tasks<0)
                    return -ERANGE;
            }
            error=c5_legacy_window_native_emit_record(lease,task,rq,&record->value,
                      wallclock,0,record->value.kind==C5_LEGACY_RECORD_HISTORY?&counters:NULL);
        }
        if (error)
            return error;
    }
    workspace->journal.migration.count=0;
    return 0;
}

static int c5_legacy_window_native_migrate_locked(struct task_struct *moving,
        struct rq *source,struct rq *destination)
{
    struct c5_legacy_window_lease lease={0};
    struct c5_legacy_window_migration_input input;
    struct c5_legacy_window_workspace *workspace;
    struct task_struct *actors[3];
    struct rq *queues[2];
    unsigned int cpu,i;
    int error,released;
    if (!moving || !source || !destination || source==destination ||
        in_nmi() || !irqs_disabled())
        return -EINVAL;
    lockdep_assert_held(&source->lock);
    lockdep_assert_held(&destination->lock);
    memset(&input,0,sizeof(input));
    input.hmp_enabled=c5_legacy_pelt_hmp_enabled();
    input.migration_fixup=READ_ONCE(c5_legacy_window_migration_fixup);
    input.waking=READ_ONCE(moving->state)==TASK_WAKING;
    input.disabled=READ_ONCE(c5_legacy_window_disabled);
    if (!input.hmp_enabled || !input.migration_fixup || input.disabled ||
        c5_legacy_window_exiting(&moving->se.c5_legacy_window) ||
        (!READ_ONCE(moving->on_rq) && !input.waking))
        return 0;
    queues[0]=source;queues[1]=destination;
    actors[0]=moving;actors[1]=READ_ONCE(source->curr);
    actors[2]=READ_ONCE(destination->curr);
    if (!actors[1] || !actors[2] || task_rq(moving)!=source ||
        task_rq(actors[1])!=source || task_rq(actors[2])!=destination)
        return -EXDEV;
    for (i=0;i<2;i++) {
        cpu=READ_ONCE(queues[i]->cpu);
        if (cpu>=NR_CPUS || cpu_rq(cpu)!=queues[i])
            return -EXDEV;
        error=READ_ONCE(queues[i]->c5_legacy_window.error);
        if (error)
            return error<0?error:-EINVAL;
    }
    if (!READ_ONCE(c5_legacy_window_boot_use_pelt)) {
        for (i=0;i<3;i++) {
            error=READ_ONCE(actors[i]->se.c5_legacy_window.error);
            if (error)
                return error<0?error:-EINVAL;
        }
    }
    error=c5_legacy_window_native_acquire(source,C5_LEGACY_WORKSPACE_MIGRATION,&lease);
    if (error)
        return error;
    workspace=lease.workspace;
    input.wallclock=sched_ktime_clock();
    error=c5_legacy_window_native_input(actors[1],source,TASK_UPDATE,input.wallclock,0,&input.account[0]);
    if (!error)
        error=c5_legacy_window_native_input(actors[2],destination,TASK_UPDATE,input.wallclock,0,&input.account[1]);
    if (!error)
        error=c5_legacy_window_native_input(moving,source,TASK_MIGRATE,input.wallclock,0,&input.account[2]);
    if (!error)
        error=c5_legacy_window_migrate(&moving->se.c5_legacy_window,
                   &actors[1]->se.c5_legacy_window,&actors[2]->se.c5_legacy_window,
                   &source->c5_legacy_window,&destination->c5_legacy_window,&input,
                   &c5_legacy_window_policy_ops,source,&c5_legacy_window_policy_ops,destination,
                   &workspace->scratch.migration,&workspace->journal.migration);
    if (!error)
        error=c5_legacy_window_native_replay_migration(&lease,actors,queues,input.wallclock);
    released=c5_legacy_window_native_release(source,&lease);
    if (!error)
        error=released;
    if (error) {
        c5_legacy_window_class_account_error(source,error);
        c5_legacy_window_class_account_error(destination,error);
    }
    return error;
}
#endif
