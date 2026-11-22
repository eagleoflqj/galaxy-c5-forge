/* SPDX-License-Identifier: GPL-2.0-only */
/* Include after native sched.h and walt.h. One fresh sample per scaled
 * contribution, not one frozen frequency sample for the entire update.
 * Scalar reads do not promise a coherent multi-field publication epoch.
 */
#ifndef _C5_LEGACY_PELT_BACKEND_H
#define _C5_LEGACY_PELT_BACKEND_H
#include "c5_legacy_pelt_lifecycle.h"

static inline u32 c5_legacy_pelt_seed_from_pct(u32 parent_pct, u32 default_pct)
{
    u32 pct = parent_pct ? parent_pct : default_pct;

    return (u32)div64_u64((u64)pct * C5_LEGACY_PELT_MAX, 100);
}

static inline int c5_legacy_pelt_sample_cpu(int cpu, bool enabled,
                                          struct c5_legacy_pelt_scale *sample)
{
    struct sched_cluster *cluster;
    struct c5_legacy_pelt_scale next;

    if (!sample || cpu < 0 || cpu >= nr_cpu_ids)
        return -EINVAL;
    next = (struct c5_legacy_pelt_scale){0, 0, 0, 0, enabled};
    if (!enabled) {
        *sample = next;
        return 0;
    }
    cluster = READ_ONCE(cpu_rq(cpu)->cluster);
    if (!cluster)
        return -EAGAIN;
    next.cur_freq = READ_ONCE(cluster->cur_freq);
    next.max_freq = READ_ONCE(max_possible_freq);
    next.efficiency = (u32)READ_ONCE(cluster->efficiency);
    next.max_efficiency = READ_ONCE(max_possible_efficiency);
    *sample = next;
    return 0;
}

/* This function is for an actually consumed contribution. The whole-update
 * caller still owes transactional state/error propagation and live call phase.
 */
static inline int c5_legacy_pelt_add_cpu(int cpu, bool enabled,
                                       struct c5_legacy_pelt_state *samples,
                                       u64 delta)
{
    struct c5_legacy_pelt_scale scale;
    int err;

    if (!samples)
        return -EINVAL;
    err = c5_legacy_pelt_sample_cpu(cpu, enabled, &scale);
    if (err)
        return err;
    if (enabled && (!scale.max_freq || !scale.max_efficiency ||
        scale.cur_freq > 0x7fffffffU ||
        scale.efficiency > 0x7fffffffU / 1024U))
        return -EINVAL;
    c5_legacy_pelt_add_scaled(&scale, samples, delta);
    return 0;
}
#endif
