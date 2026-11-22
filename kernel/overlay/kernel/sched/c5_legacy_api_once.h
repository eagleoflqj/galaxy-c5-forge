/* SPDX-License-Identifier: GPL-2.0-only */
/* Diagnostic-only, default-off. Reading is inert; one explicit root write
 * invokes legacy queries. No native average/controller query is called.
 */
#ifndef _C5_LEGACY_API_ONCE_H
#define _C5_LEGACY_API_ONCE_H

struct c5_legacy_api_guarded_int {
    u32 before;
    int value;
    u32 after;
};

static DEFINE_MUTEX(c5_legacy_api_once_lock);
static struct {
    u32 state, calls, maximum, percent, canaries;
    int error, average, iowait, big;
} c5_legacy_api_once;

static int c5_legacy_api_once_source_error(void)
{
    unsigned int cpu;
    int error=atomic_read(&c5_task_load_api_error);
    if (error)
        return error<0?error:-EINVAL;
    if (atomic64_read(&c5_task_load_api_refusals))
        return -EIO;
    for_each_possible_cpu(cpu) {
        error=c5_legacy_window_eligible_error(cpu);
        if (error)
            return error<0?error:-EINVAL;
    }
    return 0;
}

static int c5_legacy_api_once_set(const char *value,const struct kernel_param *kp)
{
    struct c5_legacy_api_guarded_int answer[3];
    struct rq_flags flags;
    struct rq *rq;
    unsigned int i;
    int error;
    (void)kp;
    if (!sysfs_streq(value,"1"))
        return -EINVAL;
    if (!mutex_trylock(&c5_legacy_api_once_lock))
        return -EBUSY;
    if (c5_legacy_api_once.state) {
        mutex_unlock(&c5_legacy_api_once_lock);
        return -EALREADY;
    }
    c5_legacy_api_once.state=1;
    error=c5_legacy_api_once_source_error();
    if (error)
        goto out;
    /* This query may normally emit aggregate trace values. Refuse an already
     * enabled legacy event rather than activating or collecting any trace.
     */
    if (trace_sched_get_nr_running_avg_enabled()) {
        error=-EBUSY;
        goto out;
    }
    for (i=0;i<3;i++) {
        answer[i].before=0x351de68aU^i;
        answer[i].value=-1;
        answer[i].after=0x8a12def3U^i;
    }
    /* Pin/serialize only the caller's own task while reading its last
     * maintained old load. Release the rq before the average's nr locks.
     */
    rq=task_rq_lock(current,&flags);
    c5_legacy_api_once.maximum=max_task_load();
    c5_legacy_api_once.percent=pct_task_load(current);
    task_rq_unlock(rq,current,&flags);
    error=c5_legacy_api_once_source_error();
    if (error || !c5_legacy_api_once.maximum) {
        if (!error)
            error=-ERANGE;
        goto out;
    }
    /* Only this independent legacy bank is drained. The five-output native
     * collector, its integrals/maxima and core_ctl policy are not invoked.
     */
    c5_legacy_api_once.calls=1;
    sched_get_nr_running_avg(&answer[0].value,&answer[1].value,&answer[2].value);
    for (i=0;i<3;i++) {
        if (answer[i].before!=(0x351de68aU^i) ||
            answer[i].after!=(0x8a12def3U^i) || answer[i].value<0) {
            error=-EIO;
            goto out;
        }
    }
    c5_legacy_api_once.canaries=1;
    c5_legacy_api_once.average=answer[0].value;
    c5_legacy_api_once.iowait=answer[1].value;
    c5_legacy_api_once.big=answer[2].value;
    error=c5_legacy_api_once_source_error();
out:
    c5_legacy_api_once.error=error;
    c5_legacy_api_once.state=error?3:2;
    mutex_unlock(&c5_legacy_api_once_lock);
    return error;
}

static int c5_legacy_api_once_get(char *buf,const struct kernel_param *kp)
{
    int size,error;
    (void)kp;
    error=mutex_lock_interruptible(&c5_legacy_api_once_lock);
    if (error)
        return error;
    size=scnprintf(buf,PAGE_SIZE,
        "STATE=%u CALLS=%u MAX=%u PCT=%u CANARIES=%u ERROR=%d AVG=%d IO=%d BIG=%d\n",
        c5_legacy_api_once.state,c5_legacy_api_once.calls,
        c5_legacy_api_once.maximum,c5_legacy_api_once.percent,
        c5_legacy_api_once.canaries,c5_legacy_api_once.error,
        c5_legacy_api_once.average,c5_legacy_api_once.iowait,c5_legacy_api_once.big);
    mutex_unlock(&c5_legacy_api_once_lock);
    return size;
}

static const struct kernel_param_ops c5_legacy_api_once_ops={
    .set=c5_legacy_api_once_set,
    .get=c5_legacy_api_once_get,
};
module_param_cb(c5_legacy_api_once,&c5_legacy_api_once_ops,NULL,0600);
#endif
