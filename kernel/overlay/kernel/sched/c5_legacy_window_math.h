/* SPDX-License-Identifier: GPL-2.0-only */
/* Pure legacy window arithmetic. This is not native WALT policy.
 * Each contribution must receive its own actual input sample; callers must
 * not freeze one sample across an entire task/window update.
 */
#ifndef _C5_LEGACY_WINDOW_MATH_H
#define _C5_LEGACY_WINDOW_MATH_H
#include <linux/types.h>
#include <linux/math64.h>
#include <linux/errno.h>

struct c5_legacy_window_scale_input {
    u32 current_frequency;
    u32 maximum_frequency;
    s32 cpu_efficiency;
    u32 maximum_efficiency;
};

static inline int c5_legacy_window_scale(
        u64 delta, const struct c5_legacy_window_scale_input *input,
        u64 *scaled)
{
    u32 cur_frequency, maximum_frequency, maximum_efficiency;
    s32 efficiency;
    int sf;

    if (!input || !scaled)
        return -EINVAL;
    cur_frequency = input->current_frequency;
    maximum_frequency = input->maximum_frequency;
    efficiency = input->cpu_efficiency;
    maximum_efficiency = input->maximum_efficiency;
    if (!maximum_frequency || !maximum_efficiency)
        return -EINVAL;
    /* The old cpu_efficiency() returns int. Its signed *1024 must be
     * defined before the unsigned global maximum promotes the sum.
     */
    if (efficiency < 0 || efficiency > 0x7fffffff / 1024)
        return -ERANGE;
    if (cur_frequency > maximum_frequency)
        cur_frequency = maximum_frequency;

    /* Retain the original u64 wrap and upward frequency rounding. */
    delta = div64_u64(delta * cur_frequency + maximum_frequency - 1,
                     maximum_frequency);
    /* The old DIV_ROUND_UP numerator is u32 after adding its unsigned
     * maximum. Widening it to u64 changes defined legacy values.
     */
    sf = ((u32)(efficiency * 1024) + maximum_efficiency - 1U) /
         maximum_efficiency;
    delta *= sf;
    delta >>= 10;
    *scaled = delta;
    return 0;
}
#endif
