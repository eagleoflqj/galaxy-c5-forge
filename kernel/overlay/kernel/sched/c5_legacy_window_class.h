/* SPDX-License-Identifier: GPL-2.0-only */
/* Independent old class arithmetic. Inputs must come from the actual owned
 * old accounting/policy phase; these helpers neither sample native WALT nor
 * create missing tuning/cgroup implementations. They never change placement.
 */
#ifndef _C5_LEGACY_WINDOW_CLASS_H
#define _C5_LEGACY_WINDOW_CLASS_H
#include <linux/c5_legacy_window_types.h>
#include <linux/errno.h>

enum c5_legacy_window_task_size {
    C5_LEGACY_SIZE_NONE=0,C5_LEGACY_SIZE_BIG=1,C5_LEGACY_SIZE_SMALL=2,
};
struct c5_legacy_window_class_input {
    u32 load,cpu_scale,max_scale,upmigrate,small_task;
    s32 nice,nice_cutoff;
    bool discouraged;
};

static inline int c5_legacy_window_classify(
        const struct c5_legacy_window_class_input *input,u32 *size)
{
    u64 load;
    u32 next=C5_LEGACY_SIZE_NONE;
    if (!input || !size || input->cpu_scale>0x7fffffffU)
        return -EINVAL;
    /* Old big classification wins over small when both predicates hold. */
    if (input->nice<=input->nice_cutoff && !input->discouraged) {
        load=(u64)input->load*input->cpu_scale;
        load/=1024;
        if (load>input->upmigrate)
            next=C5_LEGACY_SIZE_BIG;
    }
    if (next==C5_LEGACY_SIZE_NONE) {
        load=(u64)input->load*input->max_scale;
        load/=1024;
        if (load<input->small_task)
            next=C5_LEGACY_SIZE_SMALL;
    }
    *size=next;
    return 0;
}

static inline int c5_legacy_window_class_task(
        struct c5_legacy_window_class_state *stats,int direction,u32 size,
        u32 load,bool sizes,bool change_cra,bool enabled,bool disabled)
{
    struct c5_legacy_window_class_state next;
    s32 *counter;
    if (!stats || (direction!=1 && direction!=-1) || size>C5_LEGACY_SIZE_SMALL)
        return -EINVAL;
    if (!enabled || disabled)
        return 0;
    next=*stats;
    if (next.nr_big_tasks<0 || next.nr_small_tasks<0)
        return -EINVAL;
    if (sizes && size!=C5_LEGACY_SIZE_NONE) {
        counter=size==C5_LEGACY_SIZE_BIG?&next.nr_big_tasks:&next.nr_small_tasks;
        if (direction==1) {
            if (*counter==0x7fffffff)
                return -EOVERFLOW;
            ++*counter;
        } else {
            if (!*counter)
                return -ERANGE;
            --*counter;
        }
    }
    if (change_cra) {
        if (direction==1) {
            if ((u64)-1-next.cumulative_runnable_avg<load)
                return -EOVERFLOW;
            next.cumulative_runnable_avg+=load;
        } else {
            if (next.cumulative_runnable_avg<load ||
                next.cumulative_runnable_avg-load>((u64)-1>>1))
                return -ERANGE;
            next.cumulative_runnable_avg-=load;
        }
    }
    *stats=next;
    return 0;
}

/* Old throttle transfer changes both counts and CRA without its own HMP
 * gate. The actual caller controls the phase and stabilizes both objects.
 */
static inline int c5_legacy_window_class_transfer(
        struct c5_legacy_window_class_state *stats,
        const struct c5_legacy_window_class_state *contribution,int direction)
{
    struct c5_legacy_window_class_state next;
    if (!stats || !contribution || (direction!=1 && direction!=-1))
        return -EINVAL;
    if (stats->nr_big_tasks<0 || stats->nr_small_tasks<0 ||
        contribution->nr_big_tasks<0 || contribution->nr_small_tasks<0)
        return -EINVAL;
    next=*stats;
    if (direction==1) {
        if (contribution->nr_big_tasks>0x7fffffff-next.nr_big_tasks ||
            contribution->nr_small_tasks>0x7fffffff-next.nr_small_tasks ||
            contribution->cumulative_runnable_avg>(u64)-1-next.cumulative_runnable_avg)
            return -EOVERFLOW;
        next.nr_big_tasks+=contribution->nr_big_tasks;
        next.nr_small_tasks+=contribution->nr_small_tasks;
        next.cumulative_runnable_avg+=contribution->cumulative_runnable_avg;
    } else {
        if (contribution->nr_big_tasks>next.nr_big_tasks ||
            contribution->nr_small_tasks>next.nr_small_tasks ||
            contribution->cumulative_runnable_avg>next.cumulative_runnable_avg ||
            next.cumulative_runnable_avg-contribution->cumulative_runnable_avg>((u64)-1>>1))
            return -ERANGE;
        next.nr_big_tasks-=contribution->nr_big_tasks;
        next.nr_small_tasks-=contribution->nr_small_tasks;
        next.cumulative_runnable_avg-=contribution->cumulative_runnable_avg;
    }
    *stats=next;
    return 0;
}

static inline int c5_legacy_window_class_reset(
        struct c5_legacy_window_class_state *stats,bool reset_cra)
{
    if (!stats)
        return -EINVAL;
    stats->nr_big_tasks=0;stats->nr_small_tasks=0;
    if (reset_cra)
        stats->cumulative_runnable_avg=0;
    return 0;
}
#endif
