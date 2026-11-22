/* SPDX-License-Identifier: GPL-2.0-only */
/* Pure old demand/history/outer-update component. Policy snapshots, scale/I/O
 * adapters and journal replay are explicit integration boundaries. No native
 * scheduler counter, collector, actual task payload or hardware is accessed.
 */
#ifndef _C5_LEGACY_WINDOW_DEMAND_H
#define _C5_LEGACY_WINDOW_DEMAND_H
#include <linux/kernel.h>
#include <linux/string.h>
#include "c5_legacy_window_busy.h"

enum c5_legacy_history_policy {
    C5_LEGACY_HISTORY_RECENT=0, C5_LEGACY_HISTORY_MAX=1,
    C5_LEGACY_HISTORY_MAX_RECENT_AVG=2, C5_LEGACY_HISTORY_AVG=3,
};
struct c5_legacy_window_demand_input {
    struct c5_legacy_window_busy_input busy;
    u32 history_size, history_policy;
    bool account_wait, on_rq, has_dl_policy, dl_throttled;
    bool use_pelt, disabled;
};
#define C5_LEGACY_WINDOW_RECORDS 7
struct c5_legacy_window_journal {
    u32 count;
    struct c5_legacy_window_record records[C5_LEGACY_WINDOW_RECORDS];
};

static inline int c5_legacy_window_record(
        struct c5_legacy_window_journal *journal, enum c5_legacy_record_kind kind,
        const struct c5_legacy_window_task *task, const struct c5_legacy_window_rq *rq,
        u32 runtime, int samples, int event)
{
    struct c5_legacy_window_record *record;
    if (journal->count >= C5_LEGACY_WINDOW_RECORDS)
        return -ENOSPC;
    record = &journal->records[journal->count++];
    record->kind=kind;record->runtime=runtime;record->samples=samples;record->event=event;
    record->task=*task;record->rq=*rq;
    return 0;
}
static inline int c5_legacy_window_account_demand(
        const struct c5_legacy_window_task *p,
        const struct c5_legacy_window_demand_input *input)
{
    int event=input->busy.event;

	/* No need to bother updating task demand for exiting tasks
	 * or the idle task. */
	if (c5_legacy_window_exiting(p) || input->busy.is_idle)
		return 0;

	/* When a task is waking up it is completing a segment of non-busy
	 * time. Likewise, if wait time is not treated as busy time, then
	 * when a task begins to run or is migrated, it is not running and
	 * is completing a segment of non-busy time. */
	if (event == C5_LEGACY_WAKE || (!input->account_wait &&
			 (event == C5_LEGACY_PICK_NEXT || event == C5_LEGACY_MIGRATE)))
		return 0;

	return 1;

}
static inline int c5_legacy_window_history_raw(
        struct c5_legacy_window_rq *rq, struct c5_legacy_window_task *p,
        u32 runtime, int samples, const struct c5_legacy_window_demand_input *input,
        struct c5_legacy_window_journal *journal)
{
    int ret,event=input->busy.event;

	u32 *hist = &p->sum_history[0];
	int ridx, widx;
	u32 max = 0, avg, demand;
	u64 sum = 0;

	/* Ignore windows where task had no activity */
	if (!runtime || input->busy.is_idle || c5_legacy_window_exiting(p) || !samples)
			goto done;

	if (!input->history_size || input->history_size > 5 || samples < 0)
		return -ERANGE;

	/* Push new 'runtime' value onto stack */
	widx = input->history_size - 1;
	ridx = widx - samples;
	for (; ridx >= 0; --widx, --ridx) {
		hist[widx] = hist[ridx];
		sum += hist[widx];
		if (hist[widx] > max)
			max = hist[widx];
	}

	for (widx = 0; widx < samples && widx < input->history_size; widx++) {
		hist[widx] = runtime;
		sum += hist[widx];
		if (hist[widx] > max)
			max = hist[widx];
	}

	p->sum = 0;

	/*
	 * A throttled deadline sched class task gets dequeued without
	 * changing input->on_rq. Since the dequeue decrements hmp stats
	 * avoid decrementing it here again.
	 */
	if (input->on_rq && (!input->has_dl_policy || !input->dl_throttled))
		{
		ret = c5_legacy_window_record(journal, C5_LEGACY_RECORD_DEC,
				 p, rq, runtime, samples, event);
		if (ret)
			return ret;
	}

	avg = div64_u64(sum, input->history_size);

	if (input->history_policy == C5_LEGACY_HISTORY_RECENT)
		demand = runtime;
	else if (input->history_policy == C5_LEGACY_HISTORY_MAX)
		demand = max;
	else if (input->history_policy == C5_LEGACY_HISTORY_AVG)
		demand = avg;
	else
		demand = max(avg, runtime);

	p->demand = demand;

	if (input->on_rq && (!input->has_dl_policy || !input->dl_throttled))
		{
		ret = c5_legacy_window_record(journal, C5_LEGACY_RECORD_INC,
				 p, rq, runtime, samples, event);
		if (ret)
			return ret;
	}

done:
	return c5_legacy_window_record(journal, C5_LEGACY_RECORD_HISTORY,
				 p, rq, runtime, samples, event);

}
static inline int c5_legacy_window_add_task(
        struct c5_legacy_window_task *p, u64 delta,
        const struct c5_legacy_window_demand_input *input,
        const struct c5_legacy_window_busy_ops *ops, void *cookie)
{
    int ret;

	ret = ops->scale(cookie, delta, &delta);
	if (ret)
		return ret;
	p->sum += delta;
	if (unlikely(p->sum > input->busy.window_size))
		p->sum = input->busy.window_size;

	return 0;

}
static inline int c5_legacy_window_demand_raw(
        struct c5_legacy_window_task *p, struct c5_legacy_window_rq *rq,
        const struct c5_legacy_window_demand_input *input,
        const struct c5_legacy_window_busy_ops *ops, void *cookie,
        struct c5_legacy_window_journal *journal)
{
    int ret;
    u64 wallclock=input->busy.wallclock,scaled;

	u64 mark_start = p->mark_start;
	u64 delta, window_start = rq->window_start;
	int new_window, nr_full_windows;
	u32 window_size = input->busy.window_size;

	new_window = mark_start < window_start;
	if (!c5_legacy_window_account_demand(p, input)) {
		if (new_window)
			/* If the time accounted isn't being accounted as
			 * busy time, and a new window started, only the
			 * previous window need be closed out with the
			 * pre-existing demand. Multiple windows may have
			 * elapsed, but since empty windows are dropped,
			 * it is not necessary to account those. */
			{
				ret = c5_legacy_window_history_raw(rq, p, p->sum, 1, input, journal);
				if (ret)
					return ret;
			}
		return 0;
	}

	if (!new_window) {
		/* The simple case - busy time contained within the existing
		 * window. */
		ret = c5_legacy_window_add_task(p, wallclock - mark_start, input, ops, cookie);
		if (ret)
			return ret;
		return 0;
	}

	/* Busy time spans at least two windows. Temporarily rewind
	 * window_start to first window boundary after mark_start. */
	delta = window_start - mark_start;
	nr_full_windows = div64_u64(delta, window_size);
	window_start -= (u64)nr_full_windows * (u64)window_size;

	/* Process (window_start - mark_start) first */
	ret = c5_legacy_window_add_task(p, window_start - mark_start, input, ops, cookie);
	if (ret)
		return ret;

	/* Push new sample(s) into task's demand history */
	{
		ret = c5_legacy_window_history_raw(rq, p, p->sum, 1, input, journal);
		if (ret)
			return ret;
	}
	if (nr_full_windows) {
        ret = ops->scale(cookie, window_size, &scaled);
        if (ret)
            return ret;
        ret = c5_legacy_window_history_raw(rq, p, (u32)scaled,
                    nr_full_windows, input, journal);
        if (ret)
            return ret;
    }

	/* Roll window_start back to current to process any remainder
	 * in current window. */
	window_start += (u64)nr_full_windows * (u64)window_size;

	/* Process (wallclock - window_start) next */
	mark_start = window_start;
	ret = c5_legacy_window_add_task(p, wallclock - mark_start, input, ops, cookie);
	if (ret)
		return ret;

	return 0;

}

static inline int c5_legacy_window_advance(
        struct c5_legacy_window_rq *rq, u64 wallclock, u32 window_size)
{
    s64 delta=wallclock-rq->window_start;
    int nr_windows;
    if (delta<0)
        return -ERANGE;
    if (!window_size)
        return -EINVAL;
    if (delta<window_size)
        return 0;
    /* Preserve the old target's signed-int conversion and unsigned product. */
    nr_windows=div64_u64(delta,window_size);
    rq->window_start+=(u64)nr_windows*(u64)window_size;
    return 0;
}

/* The caller owns this storage exclusively through preparation, commit and
 * journal delivery. Do not allocate it on a scheduler stack. This pure API
 * does not acquire a per-CPU lease, mask interrupts or permit NMI use.
 */
struct c5_legacy_window_update_scratch {
    struct c5_legacy_window_task next_task;
    struct c5_legacy_window_rq next_rq;
    struct c5_legacy_window_journal pending;
};

static inline bool c5_legacy_window_storage_overlaps(
        const void *first, unsigned long first_size,
        const void *second, unsigned long second_size)
{
    unsigned long a=(unsigned long)first, b=(unsigned long)second;
    if (!first_size || !second_size)
        return false;
    /* Avoid end-address addition and its overflow. Valid supplied objects
     * must still exist in the caller's address space for the entire call.
     */
    return a<=b ? b-a<first_size : a-b<second_size;
}

/* Prepare only: task/rq and any external journal are not committed here.
 * A refusal can dirty scratch. Scratch must not alias any input object.
 */
static inline int c5_legacy_window_prepare_update(
        const struct c5_legacy_window_task *task,
        const struct c5_legacy_window_rq *rq,
        const struct c5_legacy_window_demand_input *input,
        const struct c5_legacy_window_busy_ops *ops, void *cookie,
        struct c5_legacy_window_update_scratch *scratch)
{
    int ret;
    if (!task || !rq || !input || !ops || !ops->scale || !ops->read_waiting_on_io || !scratch)
        return -EINVAL;
    if (c5_legacy_window_storage_overlaps(scratch,sizeof(*scratch),task,sizeof(*task)) ||
        c5_legacy_window_storage_overlaps(scratch,sizeof(*scratch),rq,sizeof(*rq)) ||
        c5_legacy_window_storage_overlaps(scratch,sizeof(*scratch),input,sizeof(*input)) ||
        c5_legacy_window_storage_overlaps(scratch,sizeof(*scratch),ops,sizeof(*ops)))
        return -EINVAL;
    scratch->next_task=*task;scratch->next_rq=*rq;
    memset(&scratch->pending,0,sizeof(scratch->pending));
    if (input->use_pelt || !rq->window_start || input->disabled)
        return 0;
    if (input->busy.event<C5_LEGACY_PUT_PREV || input->busy.event>C5_LEGACY_IRQ ||
        input->busy.wallclock<task->mark_start)
        return -EINVAL;
    ret=c5_legacy_window_advance(&scratch->next_rq,input->busy.wallclock,input->busy.window_size);
    if (ret)
        return ret;
    if (scratch->next_task.mark_start) {
        ret=c5_legacy_window_demand_raw(&scratch->next_task,&scratch->next_rq,input,
                                       ops,cookie,&scratch->pending);
        if (ret)
            return ret;
        ret=c5_legacy_window_busy_raw(&scratch->next_task,&scratch->next_rq,
                                     &input->busy,ops,cookie);
        if (ret)
            return ret;
    }
    ret=c5_legacy_window_record(&scratch->pending,C5_LEGACY_RECORD_UPDATE,
             &scratch->next_task,&scratch->next_rq,0,0,input->busy.event);
    if (ret)
        return ret;
    /* Preserve the record before advancing mark_start. */
    scratch->next_task.mark_start=input->busy.wallclock;
    return 0;
}

static inline int c5_legacy_window_update(
        struct c5_legacy_window_task *task, struct c5_legacy_window_rq *rq,
        const struct c5_legacy_window_demand_input *input,
        const struct c5_legacy_window_busy_ops *ops, void *cookie,
        struct c5_legacy_window_update_scratch *scratch,
        struct c5_legacy_window_journal *journal)
{
    int ret;
    if (!task || !rq || !input || !ops || !scratch || !journal)
        return -EINVAL;
    if (c5_legacy_window_storage_overlaps(journal,sizeof(*journal),scratch,sizeof(*scratch)) ||
        c5_legacy_window_storage_overlaps(journal,sizeof(*journal),task,sizeof(*task)) ||
        c5_legacy_window_storage_overlaps(journal,sizeof(*journal),rq,sizeof(*rq)) ||
        c5_legacy_window_storage_overlaps(journal,sizeof(*journal),input,sizeof(*input)) ||
        c5_legacy_window_storage_overlaps(journal,sizeof(*journal),ops,sizeof(*ops)) ||
        c5_legacy_window_storage_overlaps(task,sizeof(*task),rq,sizeof(*rq)))
        return -EINVAL;
    ret=c5_legacy_window_prepare_update(task,rq,input,ops,cookie,scratch);
    if (ret)
        return ret;
    *task=scratch->next_task;*rq=scratch->next_rq;*journal=scratch->pending;
    return 0;
}

static inline int c5_legacy_window_reset(struct c5_legacy_window_task *task)
{
    u32 marker;
    if (!task)
        return -EINVAL;
    marker=c5_legacy_window_exiting(task)?0xdeaddeadU:0;
    memset(task,0,sizeof(*task));
    task->sum_history[0]=marker;
    return 0;
}
static inline int c5_legacy_window_starting(
        struct c5_legacy_window_task *task, const struct c5_legacy_window_rq *rq,
        bool disabled, u64 wallclock)
{
    if (!task || !rq)
        return -EINVAL;
    if (!rq->window_start || disabled)
        return c5_legacy_window_reset(task);
    task->mark_start=wallclock;
    return 0;
}
#endif
