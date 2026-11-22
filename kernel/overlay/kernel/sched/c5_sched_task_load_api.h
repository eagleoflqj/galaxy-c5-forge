/* SPDX-License-Identifier: GPL-2.0-only */
/* Pure old load queries use the maintained old observer, independently of
 * native WALT's window and demand. No refresh, clock sample, reset or native
 * scheduler policy write is performed. Boot selectors retain their existing
 * immutable-after-boot contract; no new runtime mode/window switch is added.
 */
#ifndef _C5_SCHED_TASK_LOAD_API_H
#define _C5_SCHED_TASK_LOAD_API_H
#include <linux/moduleparam.h>

static atomic_t c5_task_load_api_error=ATOMIC_INIT(0);
static atomic64_t c5_task_load_api_refusals=ATOMIC64_INIT(0);

unsigned int max_task_load(void)
{
    if (READ_ONCE(c5_legacy_window_boot_use_pelt))
        return 47742U;
    return READ_ONCE(c5_legacy_window_boot_period);
}

static unsigned int c5_task_load_api_refuse(int error)
{
    if (error>=0)
        error=-EINVAL;
    atomic_cmpxchg(&c5_task_load_api_error,0,error);
    atomic64_inc(&c5_task_load_api_refusals);
    return 0;
}

unsigned int pct_task_load(struct task_struct *p)
{
    unsigned int load,maximum;
    int error;

    if (!p)
        return c5_task_load_api_refuse(-EINVAL);
    /* Like the old task_load(), this does not gate the last maintained load
     * on placement enable or window-disabled flags, nor take an rq lock.
     * It is not an arbitrary whole-task coherent snapshot.
     */
    if (READ_ONCE(c5_legacy_window_boot_use_pelt)) {
        error=READ_ONCE(p->se.c5_legacy_pelt.error);
        load=READ_ONCE(p->se.c5_legacy_pelt.samples.runnable_avg_sum_scaled);
    } else {
        error=READ_ONCE(p->se.c5_legacy_window.error);
        load=READ_ONCE(p->se.c5_legacy_window.demand);
    }
    if (error)
        return c5_task_load_api_refuse(error);
    maximum=max_task_load();
    if (!maximum)
        return c5_task_load_api_refuse(-ERANGE);
    /* Preserve the real old u32 -> u64 multiply/divide -> u32 contract. */
    return div64_u64((u64)load*100,(u64)maximum);
}

static int c5_task_load_api_state_get(char *buf,const struct kernel_param *kp)
{
    (void)kp;
    return scnprintf(buf,PAGE_SIZE,
        "PELT=%u PERIOD=%u ERROR=%d REFUSALS=%llu\n",
        (unsigned int)READ_ONCE(c5_legacy_window_boot_use_pelt),
        READ_ONCE(c5_legacy_window_boot_period),
        atomic_read(&c5_task_load_api_error),
        (unsigned long long)atomic64_read(&c5_task_load_api_refusals));
}
static const struct kernel_param_ops c5_task_load_api_state_ops={
    .get=c5_task_load_api_state_get,
};
module_param_cb(c5_task_load_api_state,&c5_task_load_api_state_ops,NULL,0400);
#endif
