/* SPDX-License-Identifier: GPL-2.0-only */
/* Unintegrated task-statistics projection of the old c5lte CFS lifecycle.
 * Callers supply clock/counter snapshots and own serialization. These plain
 * structs are a model of the old state, not atomic shared-kernel storage.
 * No native scheduling/load-balancing or group-load accounting is changed.
 */
#ifndef _C5_LEGACY_PELT_LIFECYCLE_H
#define _C5_LEGACY_PELT_LIFECYCLE_H

#include "c5_legacy_pelt_math.h"

/* Fresh zero-backed runqueue initialization only. */
static inline int c5_legacy_pelt_rq_init(struct c5_legacy_pelt_rq *rq)
{
    if (!rq)
        return -EINVAL;
    rq->last_decay = 0;
    rq->decay_counter = 1;
    return 0;
}

/* The selected old initializer resets samples/count, but not the clock. */
static inline int c5_legacy_pelt_entity_init(struct c5_legacy_pelt_entity *entity,
                                           u32 seed)
{
    if (!entity)
        return -EINVAL;
    c5_legacy_pelt_reset_samples(&entity->samples, seed);
    entity->decay_count = 0;
    return 0;
}

/* Only the counter/time projection of update_cfs_rq_blocked_load. Its force
 * flag affects omitted policy/contribution bookkeeping, not these fields.
 */
static inline int c5_legacy_pelt_rq_advance(struct c5_legacy_pelt_rq *rq,
                                          u64 cfs_clock)
{
    u64 now, decays;

    if (!rq)
        return -EINVAL;
    now = cfs_clock >> 20;
    decays = now - rq->last_decay;
    if (decays) {
        rq->decay_counter += decays;
        rq->last_decay = now;
    }
    return 0;
}

/* Source-rq counter must be sampled under the integration's actual ownership
 * contract. Old migration promises pi_lock, not an unconditional rq lock.
 * Unsigned subtraction/negation preserves the selected ARM64 bit semantics.
 */
static inline int c5_legacy_pelt_migrate(struct c5_legacy_pelt_entity *entity,
                                       u64 source_decay_counter)
{
    u64 decays;

    if (!entity)
        return -EINVAL;
    if (entity->decay_count) {
        decays = source_decay_counter - (u64)entity->decay_count;
        entity->decay_count = (s64)(0ULL - decays);
    }
    return 0;
}

/* on_rq is the old entity state at this point, before account_entity_dequeue.
 * Commit both projections only after the arithmetic accepts its input.
 */
static inline int c5_legacy_pelt_dequeue(struct c5_legacy_pelt_entity *entity,
                                       struct c5_legacy_pelt_rq *rq,
                                       u64 cfs_clock, int on_rq, int sleep,
                                       const struct c5_legacy_pelt_scale *scale)
{
    struct c5_legacy_pelt_entity next;
    struct c5_legacy_pelt_rq next_rq;
    int err;

    if (!entity || !rq)
        return -EINVAL;
    next = *entity;
    next_rq = *rq;
    err = c5_legacy_pelt_update(&next.samples, cfs_clock, on_rq, scale);
    if (err < 0)
        return err;
    c5_legacy_pelt_rq_advance(&next_rq, cfs_clock);
    if (sleep)
        next.decay_count = (s64)next_rq.decay_counter;
    *entity = next;
    *rq = next_rq;
    return 0;
}

/* Preserve the distinct raw rq clock and throttling-aware CFS clock.
 * on_rq is sampled before account_entity_enqueue. Unused scale data is not
 * required on the fresh/migrated-runnable path that performs no update.
 */
static inline int c5_legacy_pelt_enqueue(struct c5_legacy_pelt_entity *entity,
                                       struct c5_legacy_pelt_rq *rq,
                                       u64 rq_clock, u64 cfs_clock,
                                       int on_rq, int wakeup,
                                       const struct c5_legacy_pelt_scale *scale)
{
    struct c5_legacy_pelt_entity next;
    struct c5_legacy_pelt_rq next_rq;
    u64 magnitude;
    int err;

    if (!entity || !rq)
        return -EINVAL;
    next = *entity;
    next_rq = *rq;
    if (next.decay_count <= 0) {
        next.samples.last_runnable_update = rq_clock;
        if (next.decay_count) {
            magnitude = 0ULL - (u64)next.decay_count;
            /* Beyond this domain the old signed left shift is undefined. */
            if (magnitude > (0x7fffffffffffffffULL >> 20))
                return -ERANGE;
            next.samples.last_runnable_update -= magnitude << 20;
            err = c5_legacy_pelt_update(&next.samples, cfs_clock, on_rq, scale);
            if (err < 0)
                return err;
            next.decay_count = 0;
        }
        wakeup = 0;
    } else {
        /* Synchronization decays only omitted load_avg_contrib; the three
         * trace samples and their timestamp are not changed here.
         */
        next.decay_count = 0;
    }
    if (wakeup) {
        err = c5_legacy_pelt_update(&next.samples, cfs_clock, on_rq, scale);
        if (err < 0)
            return err;
    }
    c5_legacy_pelt_rq_advance(&next_rq, cfs_clock);
    *entity = next;
    *rq = next_rq;
    return 0;
}

#endif
