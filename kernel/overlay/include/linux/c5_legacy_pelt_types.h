/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_C5_LEGACY_PELT_TYPES_H
#define _LINUX_C5_LEGACY_PELT_TYPES_H
#include <linux/types.h>
struct c5_legacy_pelt_state {
    u32 runnable_avg_sum;
    u32 runnable_avg_period;
    u32 runnable_avg_sum_scaled;
    u64 last_runnable_update;
};

struct c5_legacy_pelt_scale {
    u32 cur_freq;
    u32 max_freq;
    u32 efficiency;
    u32 max_efficiency;
    bool hmp_enabled;
};

struct c5_legacy_pelt_entity {
    struct c5_legacy_pelt_state samples;
    s64 decay_count;
    s32 error; /* Sticky until fresh task initialization; legacy event filters it. */
};

struct c5_legacy_pelt_rq {
    u64 last_decay;
    u64 decay_counter;
};

#endif
