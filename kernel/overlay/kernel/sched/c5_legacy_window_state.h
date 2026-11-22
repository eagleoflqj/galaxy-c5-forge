/* SPDX-License-Identifier: GPL-2.0-only */
/* Cached numeric observer metadata only. Each CPU row is copied under that
 * rq's lock; rows are separate observations, not one global SMP snapshot.
 * Never sample a task, clock, raw trace or PC and never change observer state.
 */
#ifndef _C5_LEGACY_WINDOW_STATE_H
#define _C5_LEGACY_WINDOW_STATE_H
#include <linux/moduleparam.h>

static int c5_legacy_window_state_get(char *buffer,const struct kernel_param *unused)
{
    struct c5_legacy_window_rq window;
    struct c5_legacy_window_class_state classes;
    struct rq *rq;
    unsigned int cpu,bank;
    unsigned long flags;
    u64 generation;
    int used=0;
    BUILD_BUG_ON(NR_CPUS!=8);
    used+=scnprintf(buffer+used,PAGE_SIZE-used,
        "C5_WINDOW1\nHMP=%u\nPELT=%u\nDISABLED=%u\nIO_BUSY=%u\n"
        "PERIOD_NS=%u\nPOLICY_ERROR=%d\n",
        c5_legacy_pelt_hmp_enabled(),READ_ONCE(c5_legacy_window_boot_use_pelt),
        READ_ONCE(c5_legacy_window_disabled),READ_ONCE(c5_legacy_window_io_is_busy),
        READ_ONCE(c5_legacy_window_boot_period),c5_legacy_window_class_policy_error_read());
    for_each_possible_cpu(cpu) {
        rq=cpu_rq(cpu);
        raw_spin_lock_irqsave(&rq->lock,flags);
        bank=c5_legacy_window_policy.view.bank;
        generation=c5_legacy_window_policy.view.generation;
        window=rq->c5_legacy_window;
        if (bank>1) {
            raw_spin_unlock_irqrestore(&rq->lock,flags);
            return -EINVAL;
        }
        classes=rq->c5_legacy_class.bank[bank];
        raw_spin_unlock_irqrestore(&rq->lock,flags);
        if (PAGE_SIZE-used<192)
            return -ENOSPC;
        used+=scnprintf(buffer+used,PAGE_SIZE-used,
            "CPU=%u BANK=%u GEN=%llu START_NS=%llu CUR=%llu PREV=%llu "
            "BIG=%d SMALL=%d CRA=%llu ERROR=%d\n",
            cpu,bank,generation,window.window_start,window.curr_runnable_sum,
            window.prev_runnable_sum,classes.nr_big_tasks,classes.nr_small_tasks,
            classes.cumulative_runnable_avg,window.error);
    }
    used+=scnprintf(buffer+used,PAGE_SIZE-used,"C5_WINDOW_COMPLETE\n");
    return used;
}

static const struct kernel_param_ops c5_legacy_window_state_ops={
    .get=c5_legacy_window_state_get,
};
module_param_cb(c5_window_state,&c5_legacy_window_state_ops,NULL,0400);
MODULE_PARM_DESC(c5_window_state,"Read-only cached legacy window/class metadata");
#endif
