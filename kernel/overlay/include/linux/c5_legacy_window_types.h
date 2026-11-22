/* SPDX-License-Identifier: GPL-2.0-only */
/* Independent observer storage, never an alias of native WALT state. */
#ifndef _LINUX_C5_LEGACY_WINDOW_TYPES_H
#define _LINUX_C5_LEGACY_WINDOW_TYPES_H
#include <linux/types.h>
struct c5_legacy_window_task {
    u64 mark_start;
    u32 sum, demand, sum_history[5];
    u32 curr_window, prev_window;
    s32 error;
};
struct c5_legacy_window_rq {
    u64 window_start, curr_runnable_sum, prev_runnable_sum;
    s32 error;
    u32 reserved;
};

enum c5_legacy_record_kind {
    C5_LEGACY_RECORD_DEC=1, C5_LEGACY_RECORD_INC=2,
    C5_LEGACY_RECORD_HISTORY=3, C5_LEGACY_RECORD_UPDATE=4,
    C5_LEGACY_RECORD_MIGRATION=5,
};
struct c5_legacy_window_record {
    enum c5_legacy_record_kind kind;
    u32 runtime;
    int samples, event;
    struct c5_legacy_window_task task;
    struct c5_legacy_window_rq rq;
};


struct c5_legacy_window_class_scale_sample {
    u32 cpu_scale,maximum_scale;
    u64 generation;
};

/* A real load observation, not initialized/maintained class counts. */
struct c5_legacy_window_class_sample {
    u32 load;
    bool enabled,disabled,use_pelt;
};

/* Same statistics shape as the old class accounting, not native WALT aliases.
 * An actual class provider must maintain these values before history delivery.
 */
struct c5_legacy_window_class_state {
    s32 nr_big_tasks,nr_small_tasks;
    u64 cumulative_runnable_avg;
};
/* Two complete banks allow failed reclassification to leave the active
 * counters and policy intact. CRA is copied, not recomputed, on policy change.
 */
struct c5_legacy_window_class_storage {
    struct c5_legacy_window_class_state bank[2];
};
struct c5_legacy_window_class_policy_view {
    u32 cpu_scale[8],maximum_scale,upmigrate,downmigrate,small_task;
    s32 nice_cutoff;
    u32 bank;
    u64 generation;
};
struct task_struct;
struct rq;
struct c5_legacy_window_trace_view {
    const struct c5_legacy_window_record *record;
    const struct task_struct *task;
    const struct rq *rq;
    u64 generation;
    struct c5_legacy_window_class_state classes;
    bool active;
};
#ifdef CONFIG_C5_SCHED_WINDOW_TRACE_COMPAT
/* Borrowed only for the active IRQ-off lease and matching record dispatch. */
extern const struct c5_legacy_window_trace_view *
c5_legacy_window_trace_current(const struct task_struct *task,
                              const struct rq *rq,unsigned int kind);
extern const char *sched_window_reset_reasons[6];
#endif
#endif
