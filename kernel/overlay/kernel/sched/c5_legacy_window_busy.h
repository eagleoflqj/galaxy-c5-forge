/* SPDX-License-Identifier: GPL-2.0-only */
/* Pure old busy-time component, not native WALT policy or a live backend.
 * Scalar policy inputs are supplied by the caller for this component call;
 * their real publication/concurrent-writer contract remains an integration gate.
 * Each consumed scale and I/O observation uses its callback at the old phase.
 */
#ifndef _C5_LEGACY_WINDOW_BUSY_H
#define _C5_LEGACY_WINDOW_BUSY_H
#include <linux/types.h>
#include <linux/math64.h>
#include <linux/errno.h>

enum c5_legacy_window_event {
    C5_LEGACY_PUT_PREV = 0,
    C5_LEGACY_PICK_NEXT = 1,
    C5_LEGACY_WAKE = 2,
    C5_LEGACY_MIGRATE = 3,
    C5_LEGACY_UPDATE = 4,
    C5_LEGACY_IRQ = 5,
};

#include <linux/c5_legacy_window_types.h>

struct c5_legacy_window_busy_input {
    u64 wallclock, irqtime;
    u32 window_size, frequency_account_wait, heavy_task_threshold;
    bool is_idle, is_current;
    int event;
};

struct c5_legacy_window_busy_ops {
    int (*scale)(void *cookie, u64 delta, u64 *scaled);
    int (*read_waiting_on_io)(void *cookie);
};

static inline int c5_legacy_window_exiting(const struct c5_legacy_window_task *p)
{
    return p->sum_history[0] == 0xdeaddeadU;
}

/* The callback reads the actual I/O-busy gate and, only if enabled,
 * the waiting count at this decision point. An earlier scalar snapshot is
 * not a substitute; unrelated CPUs may update the native gate meanwhile.
 */
static inline int c5_legacy_window_waiting_on_io(
        const struct c5_legacy_window_busy_ops *ops, void *cookie)
{
    return ops->read_waiting_on_io(cookie);
}

static inline int c5_legacy_window_account_busy(
        const struct c5_legacy_window_busy_input *input,
        const struct c5_legacy_window_busy_ops *ops, void *cookie)
{
    u64 irqtime = input->irqtime;
    int event = input->event;

	if (input->is_idle) {
		/* C5_LEGACY_WAKE && C5_LEGACY_MIGRATE is not possible on idle task! */
		if (event == C5_LEGACY_PICK_NEXT)
			return 0;

		/* C5_LEGACY_PUT_PREV, C5_LEGACY_UPDATE && C5_LEGACY_IRQ are left */
		return irqtime || c5_legacy_window_waiting_on_io(ops, cookie);
	}

	if (event == C5_LEGACY_WAKE)
		return 0;

	if (event == C5_LEGACY_PUT_PREV || event == C5_LEGACY_IRQ ||
					 event == C5_LEGACY_UPDATE)
		return 1;

	/* Only C5_LEGACY_MIGRATE && C5_LEGACY_PICK_NEXT left */
	return input->frequency_account_wait;

}

static inline int c5_legacy_window_heavy_wakeup(
        const struct c5_legacy_window_task *p, const struct c5_legacy_window_rq *rq,
        const struct c5_legacy_window_busy_input *input)
{
    int event = input->event;

	u32 task_demand = p->demand;

	if (!input->heavy_task_threshold || event != C5_LEGACY_WAKE ||
	    task_demand < input->heavy_task_threshold || c5_legacy_window_exiting(p))
		return 0;

	if (p->mark_start > rq->window_start)
		return 0;

	/* has a full window elapsed since task slept? */
	return (rq->window_start - p->mark_start > input->window_size);

}

static inline int c5_legacy_window_busy_raw(
        struct c5_legacy_window_task *p, struct c5_legacy_window_rq *rq,
        const struct c5_legacy_window_busy_input *input,
        const struct c5_legacy_window_busy_ops *ops, void *cookie)
{
    int ret;
    u64 wallclock = input->wallclock, irqtime = input->irqtime;

	int new_window, nr_full_windows = 0;
	int p_is_curr_task = input->is_current;
	u64 mark_start = p->mark_start;
	u64 window_start = rq->window_start;
	u32 window_size = input->window_size;
	u64 delta;

	new_window = mark_start < window_start;
	if (new_window)
		nr_full_windows = div64_u64((window_start - mark_start),
						window_size);

	/* Handle per-task window rollover. We don't care about the idle
	 * task or exiting tasks. */
	if (new_window && !input->is_idle && !c5_legacy_window_exiting(p)) {
		u32 curr_window = 0;

		if (!nr_full_windows)
			curr_window = p->curr_window;

		p->prev_window = curr_window;
		p->curr_window = 0;
	}

	if (!c5_legacy_window_account_busy(input, ops, cookie)) {
		/* account_busy_for_cpu_time() = 0, so no update to the
		 * task's current window needs to be made. This could be
		 * for example
		 *
		 *   - a wakeup event on a task within the current
		 *     window (!new_window below, no action required),
		 *   - switching to a new task from idle (C5_LEGACY_PICK_NEXT)
		 *     in a new window where irqtime is 0 and we aren't
		 *     waiting on IO */

		if (!new_window)
			return 0;

		/* A new window has started. The RQ demand must be rolled
		 * over if p is the current task. */
		if (p_is_curr_task) {
			u64 prev_sum = 0;

			/* p is either idle task or an exiting task */
			if (!nr_full_windows)
				prev_sum = rq->curr_runnable_sum;

			rq->prev_runnable_sum = prev_sum;
			rq->curr_runnable_sum = 0;

		} else if (c5_legacy_window_heavy_wakeup(p, rq, input)) {
			/* A new window has started. If p is a waking
			 * heavy task its prev_window contribution is faked
			 * to be its window-based demand. Note that this can
			 * introduce phantom load into the system depending
			 * on the window policy and task behavior. This feature
			 * can be controlled via the input->heavy_task_threshold
			 * tunable. */
			p->prev_window = p->demand;
			rq->prev_runnable_sum += p->demand;
		}

		return 0;
	}

	if (!new_window) {
		/* account_busy_for_cpu_time() = 1 so busy time needs
		 * to be accounted to the current window. No rollover
		 * since we didn't start a new window. An example of this is
		 * when a task starts execution and then sleeps within the
		 * same window. */

		if (!irqtime || !input->is_idle || c5_legacy_window_waiting_on_io(ops, cookie))
			delta = wallclock - mark_start;
		else
			delta = irqtime;
		ret = ops->scale(cookie, delta, &delta);
		if (ret) {
			return ret;
		}
		rq->curr_runnable_sum += delta;
		if (!input->is_idle && !c5_legacy_window_exiting(p))
			p->curr_window += delta;

		return 0;
	}

	if (!p_is_curr_task) {
		/* account_busy_for_cpu_time() = 1 so busy time needs
		 * to be accounted to the current window. A new window
		 * has also started, but p is not the current task, so the
		 * window is not rolled over - just split up and account
		 * as necessary into curr and prev. The window is only
		 * rolled over when a new window is processed for the current
		 * task.
		 *
		 * Irqtime can't be accounted by a task that isn't the
		 * currently running task. */

		if (!nr_full_windows) {
			/* A full window hasn't elapsed, account partial
			 * contribution to previous completed window. */
			ret = ops->scale(cookie, window_start - mark_start, &delta);
			if (ret) {
				return ret;
			}
			if (!c5_legacy_window_exiting(p))
				p->prev_window += delta;
		} else {
			/* Since at least one full window has elapsed,
			 * the contribution to the previous window is the
			 * full window (window_size). */
			ret = ops->scale(cookie, window_size, &delta);
			if (ret) {
				return ret;
			}
			if (!c5_legacy_window_exiting(p))
				p->prev_window = delta;
		}
		rq->prev_runnable_sum += delta;

		/* Account piece of busy time in the current window. */
		ret = ops->scale(cookie, wallclock - window_start, &delta);
		if (ret) {
			return ret;
		}
		rq->curr_runnable_sum += delta;
		if (!c5_legacy_window_exiting(p))
			p->curr_window = delta;

		return 0;
	}

	if (!irqtime || !input->is_idle || c5_legacy_window_waiting_on_io(ops, cookie)) {
		/* account_busy_for_cpu_time() = 1 so busy time needs
		 * to be accounted to the current window. A new window
		 * has started and p is the current task so rollover is
		 * needed. If any of these three above conditions are true
		 * then this busy time can't be accounted as irqtime.
		 *
		 * Busy time for the idle task or exiting tasks need not
		 * be accounted.
		 *
		 * An example of this would be a task that starts execution
		 * and then sleeps once a new window has begun. */

		if (!nr_full_windows) {
			/* A full window hasn't elapsed, account partial
			 * contribution to previous completed window. */
			ret = ops->scale(cookie, window_start - mark_start, &delta);
			if (ret) {
				return ret;
			}
			if (!input->is_idle && !c5_legacy_window_exiting(p))
				p->prev_window += delta;
			delta += rq->curr_runnable_sum;
		} else {
			/* Since at least one full window has elapsed,
			 * the contribution to the previous window is the
			 * full window (window_size). */
			ret = ops->scale(cookie, window_size, &delta);
			if (ret) {
				return ret;
			}
			if (!input->is_idle && !c5_legacy_window_exiting(p))
				p->prev_window = delta;
		}
		/* Rollover is done here by overwriting the values in
		 * prev_runnable_sum and curr_runnable_sum. */
		rq->prev_runnable_sum = delta;

		/* Account piece of busy time in the current window. */
		ret = ops->scale(cookie, wallclock - window_start, &delta);
		if (ret) {
			return ret;
		}
		rq->curr_runnable_sum = delta;
		if (!input->is_idle && !c5_legacy_window_exiting(p))
			p->curr_window = delta;

		return 0;
	}

	if (irqtime) {
		/* account_busy_for_cpu_time() = 1 so busy time needs
		 * to be accounted to the current window. A new window
		 * has started and p is the current task so rollover is
		 * needed. The current task must be the idle task because
		 * irqtime is not accounted for any other task.
		 *
		 * Irqtime will be accounted each time we process IRQ activity
		 * after a period of idleness, so we know the IRQ busy time
		 * started at wallclock - irqtime. */

		if (!input->is_idle)
			return -EINVAL;
		mark_start = wallclock - irqtime;

		/* Roll window over. If IRQ busy time was just in the current
		 * window then that is all that need be accounted. */
		rq->prev_runnable_sum = rq->curr_runnable_sum;
		if (mark_start > window_start) {
			ret = ops->scale(cookie, irqtime, &delta);
			if (ret) {
				return ret;
			}
			rq->curr_runnable_sum = delta;
			return 0;
		}

		/* The IRQ busy time spanned multiple windows. Process the
		 * busy time preceding the current window start first. */
		delta = window_start - mark_start;
		if (delta > window_size)
			delta = window_size;
		ret = ops->scale(cookie, delta, &delta);
		if (ret) {
			return ret;
		}
		rq->prev_runnable_sum += delta;

		/* Process the remaining IRQ busy time in the current window. */
		delta = wallclock - window_start;
		ret = ops->scale(cookie, delta, &delta);
		if (ret) {
			return ret;
		}
		rq->curr_runnable_sum = delta;

		return 0;
	}

	return -EINVAL;

}

/* This helper does not advance task mark_start or rq window_start. The real
 * old outer lifecycle owns those phases, plus demand/history and sticky error.
 * Callback side effects are not rolled back. Owned statistics commit together.
 */
static inline int c5_legacy_window_update_busy(
        struct c5_legacy_window_task *task, struct c5_legacy_window_rq *rq,
        const struct c5_legacy_window_busy_input *input,
        const struct c5_legacy_window_busy_ops *ops, void *cookie)
{
    struct c5_legacy_window_task next_task;
    struct c5_legacy_window_rq next_rq;
    int ret;

    if (!task || !rq || !input || !ops || !ops->scale || !ops->read_waiting_on_io)
        return -EINVAL;
    if (!input->window_size || input->event < C5_LEGACY_PUT_PREV ||
        input->event > C5_LEGACY_IRQ || input->wallclock < task->mark_start ||
        input->wallclock < rq->window_start)
        return -EINVAL;
    next_task = *task;
    next_rq = *rq;
    ret = c5_legacy_window_busy_raw(&next_task, &next_rq, input, ops, cookie);
    if (ret)
        return ret;
    *task = next_task;
    *rq = next_rq;
    return 0;
}
#endif
