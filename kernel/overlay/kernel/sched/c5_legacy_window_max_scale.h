/* SPDX-License-Identifier: GPL-2.0-only */
/* Included only by WALT with the legacy statistical observer selected. */
#ifndef _C5_LEGACY_WINDOW_MAX_SCALE_H
#define _C5_LEGACY_WINDOW_MAX_SCALE_H
#include <linux/moduleparam.h>

/* Match the real old internal declaration and permanent initialized data. */
unsigned int max_load_scale_factor = 1024;
unsigned int up_down_migrate_scale_factor = 1024;

static int c5_scale_data_state_get(char *buffer,
                                const struct kernel_param *unused)
{
    unsigned long flags;
    unsigned int published,shadow,up_down,shadow_up_down;
    u64 generation;
    int error;

    /* Sysfs getters run in task context; refuse an unsafe internal caller. */
    if (in_interrupt() || irqs_disabled())
        return -EPERM;
    acquire_rq_locks_irqsave(cpu_possible_mask,&flags);
    published=READ_ONCE(max_load_scale_factor);
    shadow=READ_ONCE(c5_legacy_window_class_scales.maximum_scale);
    generation=READ_ONCE(c5_legacy_window_class_scales.generation);
    up_down=READ_ONCE(up_down_migrate_scale_factor);
    shadow_up_down=READ_ONCE(c5_legacy_window_policy.up_down_scale);
    error=c5_legacy_window_class_policy_error_read();
    release_rq_locks_irqrestore(cpu_possible_mask,&flags);
    return scnprintf(buffer,PAGE_SIZE,
        "C5_SCALE_DATA1\nPUBLIC=%u\nSHADOW=%u\nGEN=%llu\nUP_DOWN=%u\nSHADOW_UP_DOWN=%u\nPOLICY_ERROR=%d\n"
        "C5_SCALE_DATA_COMPLETE\n",
        published,shadow,(unsigned long long)generation,up_down,shadow_up_down,error);
}

static const struct kernel_param_ops c5_scale_data_state_ops = {
    .get = c5_scale_data_state_get,
};
module_param_cb(c5_scale_data_state,&c5_scale_data_state_ops,NULL,0400);
#endif
