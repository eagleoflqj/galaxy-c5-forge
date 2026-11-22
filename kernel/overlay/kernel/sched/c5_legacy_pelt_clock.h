/* SPDX-License-Identifier: GPL-2.0-only */
/* Statistics-only old CFS clock semantics. Include in fair.c after rq_of.
 * Native sched.h alone does not declare that fair.c-private helper.
 * Never replace the native clock; caller owns cfs_rq/clock serialization.
 */
#ifndef _C5_LEGACY_PELT_CLOCK_H
#define _C5_LEGACY_PELT_CLOCK_H

static inline u64 c5_legacy_pelt_cfs_clock(struct cfs_rq *cfs_rq)
{
#ifdef CONFIG_CFS_BANDWIDTH
    if (unlikely(cfs_rq->throttle_count))
        return cfs_rq->throttled_clock_task;
    return rq_clock_task(rq_of(cfs_rq)) - cfs_rq->throttled_clock_task_time;
#else
    return rq_clock_task(rq_of(cfs_rq));
#endif
}

#endif
