/* SPDX-License-Identifier: GPL-2.0-only */
/* Pure old window migration transaction. Locks, clock, policy publication,
 * native class mirrors and actual event delivery remain integration gates.
 * All scratch and output storage must be distinct and exclusively owned by
 * the caller. In particular, never allocate the whole workspace on a kernel
 * scheduler stack or reuse a per-CPU workspace without proving its ownership.
 */
#ifndef _C5_LEGACY_WINDOW_MIGRATION_H
#define _C5_LEGACY_WINDOW_MIGRATION_H
#include "c5_legacy_window_demand.h"

struct c5_legacy_window_migration_input {
    u64 wallclock;
    bool hmp_enabled, migration_fixup, waking, disabled;
    /* source current, destination current, then moving task; scalar values
     * are explicit observations for each old update phase, not proof that
     * one global snapshot is sufficient for concurrent live writers. */
    struct c5_legacy_window_demand_input account[3];
};
struct c5_legacy_window_migration_record {
    u32 task_slot, rq_slot;
    struct c5_legacy_window_record value;
};
#define C5_LEGACY_MIGRATION_RECORDS (3*C5_LEGACY_WINDOW_RECORDS+4)
struct c5_legacy_window_migration_journal {
    u32 count;
    struct c5_legacy_window_migration_record records[C5_LEGACY_MIGRATION_RECORDS];
};
struct c5_legacy_window_migration_scratch {
    struct c5_legacy_window_task tasks[3];
    struct c5_legacy_window_rq queues[2];
    struct c5_legacy_window_update_scratch account;
    struct c5_legacy_window_migration_journal pending;
};

static inline int c5_legacy_migration_append(
        struct c5_legacy_window_migration_journal *journal, u32 task_slot,
        u32 rq_slot, const struct c5_legacy_window_record *value)
{
    struct c5_legacy_window_migration_record *record;
    if (journal->count>=C5_LEGACY_MIGRATION_RECORDS)
        return -ENOSPC;
    record=&journal->records[journal->count++];
    record->task_slot=task_slot;record->rq_slot=rq_slot;record->value=*value;
    return 0;
}

static inline int c5_legacy_migration_phase(
        struct c5_legacy_window_migration_journal *journal, u32 task_slot,
        u32 rq_slot, int kind, const struct c5_legacy_window_task *task,
        const struct c5_legacy_window_rq *rq)
{
    struct c5_legacy_window_migration_record *record;
    if (journal->count>=C5_LEGACY_MIGRATION_RECORDS)
        return -ENOSPC;
    record=&journal->records[journal->count++];
    memset(record,0,sizeof(*record));
    record->task_slot=task_slot;record->rq_slot=rq_slot;
    record->value.kind=(enum c5_legacy_record_kind)kind;
    record->value.task=*task;record->value.rq=*rq;
    return 0;
}

static inline int c5_legacy_migration_update(
        struct c5_legacy_window_migration_scratch *scratch,
        struct c5_legacy_window_task *task, u32 task_slot, u32 rq_slot,
        const struct c5_legacy_window_demand_input *observations,
        u64 wallclock, int event, bool is_current,
        const struct c5_legacy_window_busy_ops *ops, void *cookie)
{
    struct c5_legacy_window_demand_input input=*observations;
    u32 i;
    int ret;
    input.busy.wallclock=wallclock;input.busy.irqtime=0;
    input.busy.event=event;input.busy.is_current=is_current;
    ret=c5_legacy_window_prepare_update(task,&scratch->queues[rq_slot],&input,
                                        ops,cookie,&scratch->account);
    if (ret)
        return ret;
    *task=scratch->account.next_task;
    scratch->queues[rq_slot]=scratch->account.next_rq;
    for (i=0;i<scratch->account.pending.count;i++) {
        ret=c5_legacy_migration_append(&scratch->pending,task_slot,rq_slot,
                                      &scratch->account.pending.records[i]);
        if (ret)
            return ret;
    }
    return 0;
}

static inline int c5_legacy_window_migrate(
        struct c5_legacy_window_task *moving,
        struct c5_legacy_window_task *source_current,
        struct c5_legacy_window_task *destination_current,
        struct c5_legacy_window_rq *source, struct c5_legacy_window_rq *destination,
        const struct c5_legacy_window_migration_input *input,
        const struct c5_legacy_window_busy_ops *source_ops, void *source_cookie,
        const struct c5_legacy_window_busy_ops *destination_ops, void *destination_cookie,
        struct c5_legacy_window_migration_scratch *scratch,
        struct c5_legacy_window_migration_journal *journal)
{
    struct c5_legacy_window_task *original[3],*next[3];
    u32 slot[3],i,j;
    int ret;
    bool on_rq;
    if (!moving || !source_current || !destination_current || !source ||
        !destination || source==destination || !input || !scratch || !journal ||
        journal==&scratch->pending)
        return -EINVAL;
    /* Role aliases among the three tasks are supported. All caller objects
     * remain disjoint from scratch and the output journal, even on no-op paths.
     */
    if (c5_legacy_window_storage_overlaps(scratch,sizeof(*scratch),moving,sizeof(*moving)) ||
        c5_legacy_window_storage_overlaps(scratch,sizeof(*scratch),source_current,sizeof(*source_current)) ||
        c5_legacy_window_storage_overlaps(scratch,sizeof(*scratch),destination_current,sizeof(*destination_current)) ||
        c5_legacy_window_storage_overlaps(scratch,sizeof(*scratch),source,sizeof(*source)) ||
        c5_legacy_window_storage_overlaps(scratch,sizeof(*scratch),destination,sizeof(*destination)) ||
        c5_legacy_window_storage_overlaps(scratch,sizeof(*scratch),input,sizeof(*input)) ||
        c5_legacy_window_storage_overlaps(scratch,sizeof(*scratch),source_ops,source_ops ? sizeof(*source_ops) : 0) ||
        c5_legacy_window_storage_overlaps(scratch,sizeof(*scratch),destination_ops,destination_ops ? sizeof(*destination_ops) : 0) ||
        c5_legacy_window_storage_overlaps(scratch,sizeof(*scratch),journal,sizeof(*journal)) ||
        c5_legacy_window_storage_overlaps(journal,sizeof(*journal),moving,sizeof(*moving)) ||
        c5_legacy_window_storage_overlaps(journal,sizeof(*journal),source_current,sizeof(*source_current)) ||
        c5_legacy_window_storage_overlaps(journal,sizeof(*journal),destination_current,sizeof(*destination_current)) ||
        c5_legacy_window_storage_overlaps(journal,sizeof(*journal),source,sizeof(*source)) ||
        c5_legacy_window_storage_overlaps(journal,sizeof(*journal),destination,sizeof(*destination)) ||
        c5_legacy_window_storage_overlaps(journal,sizeof(*journal),input,sizeof(*input)) ||
        c5_legacy_window_storage_overlaps(journal,sizeof(*journal),source_ops,source_ops ? sizeof(*source_ops) : 0) ||
        c5_legacy_window_storage_overlaps(journal,sizeof(*journal),destination_ops,destination_ops ? sizeof(*destination_ops) : 0))
        return -EINVAL;
    on_rq=input->account[2].on_rq;
    if (!input->hmp_enabled || !input->migration_fixup ||
        c5_legacy_window_exiting(moving) || (!on_rq && !input->waking) ||
        input->disabled) {
        memset(journal,0,sizeof(*journal));
        return 0;
    }
    if (!source_ops || !source_ops->scale || !source_ops->read_waiting_on_io ||
        !destination_ops || !destination_ops->scale || !destination_ops->read_waiting_on_io)
        return -EINVAL;
    memset(scratch,0,sizeof(*scratch));
    original[0]=moving;original[1]=source_current;original[2]=destination_current;
    /* Copy each physical task once. All subsequent roles share that copy. */
    for (i=0;i<3;i++) {
        slot[i]=i;
        for (j=0;j<i;j++)
            if (original[i]==original[j]) {
                slot[i]=slot[j];
                break;
            }
        if (slot[i]==i)
            scratch->tasks[i]=*original[i];
        next[i]=&scratch->tasks[slot[i]];
    }
    scratch->queues[0]=*source;scratch->queues[1]=*destination;
    ret=c5_legacy_migration_update(scratch,next[1],slot[1],0,&input->account[0],
               input->wallclock,C5_LEGACY_UPDATE,true,source_ops,source_cookie);
    if (ret)
        return ret;
    ret=c5_legacy_migration_update(scratch,next[2],slot[2],1,&input->account[1],
               input->wallclock,C5_LEGACY_UPDATE,true,destination_ops,destination_cookie);
    if (ret)
        return ret;
    if (on_rq) {
        ret=c5_legacy_migration_phase(&scratch->pending,slot[0],0,
                 C5_LEGACY_RECORD_INC,next[0],&scratch->queues[0]);
        if (ret)
            return ret;
    }
    ret=c5_legacy_migration_update(scratch,next[0],slot[0],0,&input->account[2],
               input->wallclock,C5_LEGACY_MIGRATE,moving==source_current,
               source_ops,source_cookie);
    if (ret)
        return ret;
    if (on_rq) {
        ret=c5_legacy_migration_phase(&scratch->pending,slot[0],0,
                 C5_LEGACY_RECORD_DEC,next[0],&scratch->queues[0]);
        if (ret)
            return ret;
    }
    /* Preserve the old full task contribution, including unsigned wrapping.
     * Negative signed source sums are refused atomically instead of BUG_ON. */
    if (next[0]->curr_window) {
        scratch->queues[0].curr_runnable_sum-=next[0]->curr_window;
        scratch->queues[1].curr_runnable_sum+=next[0]->curr_window;
    }
    if (next[0]->prev_window) {
        scratch->queues[0].prev_runnable_sum-=next[0]->prev_window;
        scratch->queues[1].prev_runnable_sum+=next[0]->prev_window;
    }
    if ((s64)scratch->queues[0].curr_runnable_sum<0 ||
        (s64)scratch->queues[0].prev_runnable_sum<0)
        return -ERANGE;
    ret=c5_legacy_migration_phase(&scratch->pending,slot[0],0,
             C5_LEGACY_RECORD_MIGRATION,next[0],&scratch->queues[0]);
    if (ret)
        return ret;
    ret=c5_legacy_migration_phase(&scratch->pending,slot[0],1,
             C5_LEGACY_RECORD_MIGRATION,next[0],&scratch->queues[1]);
    if (ret)
        return ret;
    for (i=0;i<3;i++)
        if (slot[i]==i)
            *original[i]=scratch->tasks[i];
    *source=scratch->queues[0];*destination=scratch->queues[1];*journal=scratch->pending;
    return 0;
}
#endif
