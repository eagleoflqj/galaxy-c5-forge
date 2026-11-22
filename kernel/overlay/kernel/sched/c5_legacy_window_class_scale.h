/* SPDX-License-Identifier: GPL-2.0-only */
/* Old numerical scalar snapshots, separate from native placement factors.
 * The independent policy observer owns all possible rq locks with IRQs off.
 * Consumers retain their primary rq lock; READ_ONCE is not the exclusion.
 */
#ifndef _C5_LEGACY_WINDOW_CLASS_SCALE_H
#define _C5_LEGACY_WINDOW_CLASS_SCALE_H
#include <linux/errno.h>
#include <linux/math64.h>
#include <linux/hardirq.h>

#define C5_LEGACY_CLASS_SCALE_CPUS 8
struct c5_legacy_window_class_scale_state {
    u32 cpu_scale[C5_LEGACY_CLASS_SCALE_CPUS];
    u32 maximum_scale,min_max_freq,max_possible_freq;
    u64 generation;
};
static struct c5_legacy_window_class_scale_state c5_legacy_window_class_scales={
    .cpu_scale={1024,1024,1024,1024,1024,1024,1024,1024},
    .maximum_scale=1024,.min_max_freq=1,.max_possible_freq=1,
};

/* Preserve the old two ceil/shift stages on their valid positive domain.
 * Old's unsigned ceil numerators and signed intermediate have finite ranges;
 * reject invalid/overflowing inputs instead of executing old BUG/overflow.
 */
static int c5_legacy_window_class_cpu_scale(u32 efficiency,u32 maximum_efficiency,
        u32 frequency,u32 maximum_frequency,u32 *result)
{
    u64 numerator,efficiency_scale,frequency_scale,load=1024;
    if (!result || !efficiency || !maximum_efficiency || !frequency || !maximum_frequency)
        return -EINVAL;
    numerator=(u64)maximum_efficiency*1024+efficiency-1;
    if (numerator>(u32)-1)
        return -EOVERFLOW;
    efficiency_scale=div64_u64(numerator,efficiency);
    numerator=(u64)maximum_frequency*1024+frequency-1;
    if (numerator>(u32)-1)
        return -EOVERFLOW;
    frequency_scale=div64_u64(numerator,frequency);
    load*=efficiency_scale;
    if (load>0x7fffffffU)
        return -EOVERFLOW;
    load>>=10;
    load*=frequency_scale;
    if (load>0x7fffffffU)
        return -EOVERFLOW;
    load>>=10;
    *result=(u32)load;
    return 0;
}

#endif
