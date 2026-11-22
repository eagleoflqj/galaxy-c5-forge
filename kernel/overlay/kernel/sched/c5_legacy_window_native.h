/* SPDX-License-Identifier: GPL-2.0-only */
/* Included in native walt.c after sched.h/walt.h. These CPU callbacks sample
 * real inputs once for each consumed scaled contribution. The integration
 * must own the supplied rq and prove the cluster lifetime at every call site.
 * Storage/constructor integration alone does not establish an accounting
 * producer, live policy publication or a restored raw trace event.
 */
#ifndef _C5_LEGACY_WINDOW_NATIVE_H
#define _C5_LEGACY_WINDOW_NATIVE_H
#include "c5_legacy_window_initialization.h"
#include "c5_legacy_window_math.h"
#include "c5_legacy_window_lifecycle.h"
#include "c5_legacy_window_epoch.h"
#include "c5_legacy_window_workspace.h"
#include "c5_legacy_window_class.h"
#include "c5_legacy_window_class_scale.h"
#include <linux/percpu.h>
#include <linux/hardirq.h>

static u32 __read_mostly c5_legacy_window_boot_period=10000000U;
static bool __read_mostly c5_legacy_window_boot_use_pelt;

static int __init c5_legacy_window_period_setup(char *text)
{
    int period=(int)READ_ONCE(c5_legacy_window_boot_period);
    get_option(&text,&period);
    c5_legacy_window_boot_period=(u32)period;
    c5_legacy_window_boot_use_pelt=(c5_legacy_window_boot_period<10000000U ||
                                   c5_legacy_window_boot_period>1000000000U);
    return 0;
}
early_param("sched_ravg_window",c5_legacy_window_period_setup);

void c5_legacy_window_init_task(struct task_struct *task,u32 parent_pct,
                               u32 default_pct)
{
    u32 period=READ_ONCE(c5_legacy_window_boot_period);
    u32 default_seed=(u32)div64_u64((u64)default_pct*period,100);
    int ret=c5_legacy_window_fork_seed(&task->se.c5_legacy_window,
                                      parent_pct,default_seed,period);
    if (ret)
        task->se.c5_legacy_window.error=ret;
}

static int c5_legacy_window_native_scale(void *cookie,u64 delta,u64 *scaled)
{
    struct rq *rq=cookie;
    struct sched_cluster *cluster;
    struct c5_legacy_window_scale_input input;
    if (!rq || !scaled)
        return -EINVAL;
    cluster=READ_ONCE(rq->cluster);
    if (!cluster)
        return -EAGAIN;
    input.current_frequency=READ_ONCE(cluster->cur_freq);
    input.maximum_frequency=READ_ONCE(max_possible_freq);
    input.cpu_efficiency=READ_ONCE(cluster->efficiency);
    input.maximum_efficiency=READ_ONCE(max_possible_efficiency);
    return c5_legacy_window_scale(delta,&input,scaled);
}

/* The old observer starts at zero, independently of native WALT's one.
 * The real shared setter publishes every subsequent request to both states.
 * Read the actual observer gate at each consumed I/O decision, then and only
 * then read the existing atomic rq count. Do not cache it in task input.
 */
static unsigned int __read_mostly c5_legacy_window_io_is_busy;

static int c5_legacy_window_native_waiting_on_io(void *cookie)
{
    struct rq *rq=cookie;
    if (!READ_ONCE(c5_legacy_window_io_is_busy))
        return 0;
    return atomic_read(&rq->nr_iowait);
}

const struct c5_legacy_window_busy_ops c5_legacy_window_cpu_ops={
    .scale=c5_legacy_window_native_scale,
    .read_waiting_on_io=c5_legacy_window_native_waiting_on_io,
};

/* Independent observer gate stays false under the accepted fixed-policy scope.
 * Old runtime phase/history controls and their reset trace are explicit limits;
 * no successful no-op reset writer is exposed. */
static bool c5_legacy_window_disabled;

static u64 c5_legacy_window_native_start_clock(void *cookie)
{
    /* Reuse the original native sample when that path already owns one.
     * The independent active observer samples only if native took no sample.
     */
    return cookie ? *(const u64 *)cookie : sched_ktime_clock();
}

static const struct c5_legacy_window_task_start_ops
c5_legacy_window_native_start_ops={c5_legacy_window_native_start_clock};

static void c5_legacy_window_native_task_start(
        struct task_struct *task, struct rq *rq, u64 *sample)
{
    int ret;
    lockdep_assert_held(&rq->lock);
    ret=c5_legacy_window_task_start(&task->se.c5_legacy_window,
             &rq->c5_legacy_window,READ_ONCE(c5_legacy_window_disabled),
             &c5_legacy_window_native_start_ops,sample);
    if (ret)
        task->se.c5_legacy_window.error=ret;
}

void c5_legacy_window_native_task_exit(struct task_struct *task,u64 wallclock)
{
    int ret;
    lockdep_assert_held(&task_rq(task)->lock);
    ret=c5_legacy_window_task_exit_reset(&task->se.c5_legacy_window,wallclock);
    if (ret)
        task->se.c5_legacy_window.error=ret;
}

/* Role publication owns the old role rq. Native hotplug serialization keeps
 * the selected online peer stable while the ordered pair is acquired.
 */
static unsigned int c5_legacy_window_sync_cpu;
struct c5_legacy_window_native_epoch_context {struct rq *rq;};

static u32 c5_legacy_window_native_epoch_role(void *cookie)
{
    (void)cookie;
    return READ_ONCE(c5_legacy_window_sync_cpu);
}

static u64 c5_legacy_window_native_epoch_clock(void *cookie)
{
    (void)cookie;
    return sched_ktime_clock();
}

static void c5_legacy_window_native_epoch_unlock(void *cookie)
{
    struct c5_legacy_window_native_epoch_context *ctx=cookie;
    lockdep_assert_held(&ctx->rq->lock);
    raw_spin_unlock(&ctx->rq->lock);
}

static void c5_legacy_window_native_epoch_pair(void *cookie,u32 peer)
{
    struct c5_legacy_window_native_epoch_context *ctx=cookie;
    double_rq_lock(ctx->rq,cpu_rq(peer));
}

static void c5_legacy_window_native_epoch_unlock_peer(void *cookie,u32 peer)
{
    struct c5_legacy_window_native_epoch_context *ctx=cookie;
    /* Also return the retained lock to its normal lockdep subclass. */
    double_unlock_balance(ctx->rq,cpu_rq(peer));
}

static u64 c5_legacy_window_native_epoch_read(void *cookie,u32 peer)
{
    struct c5_legacy_window_native_epoch_context *ctx=cookie;
    struct rq *source=cpu_rq(peer);
    lockdep_assert_held(&ctx->rq->lock);
    lockdep_assert_held(&source->lock);
    return READ_ONCE(source->c5_legacy_window.window_start);
}

static struct c5_legacy_window_task *
c5_legacy_window_native_epoch_current(void *cookie,u32 cpu)
{
    struct c5_legacy_window_native_epoch_context *ctx=cookie;
    struct rq *rq=cpu_rq(cpu);
    struct task_struct *task;
    lockdep_assert_held(&ctx->rq->lock);
    lockdep_assert_held(&rq->lock);
    task=READ_ONCE(rq->curr);
    return task ? &task->se.c5_legacy_window : NULL;
}

static void c5_legacy_window_native_epoch_publish(void *cookie,u32 peer)
{
    struct c5_legacy_window_native_epoch_context *ctx=cookie;
    lockdep_assert_held(&ctx->rq->lock);
    lockdep_assert_held(&cpu_rq(peer)->lock);
    WRITE_ONCE(c5_legacy_window_sync_cpu,peer);
}

static const struct c5_legacy_window_epoch_ops c5_legacy_window_native_epoch_ops={
    .read_role=c5_legacy_window_native_epoch_role,
    .clock=c5_legacy_window_native_epoch_clock,
    .unlock_local=c5_legacy_window_native_epoch_unlock,
    .lock_pair=c5_legacy_window_native_epoch_pair,
    .unlock_peer=c5_legacy_window_native_epoch_unlock_peer,
    .read_window=c5_legacy_window_native_epoch_read,
    .running_task=c5_legacy_window_native_epoch_current,
    .publish_role=c5_legacy_window_native_epoch_publish,
};

static void c5_legacy_window_native_set_epoch(struct rq *rq)
{
    struct c5_legacy_window_native_epoch_context ctx={rq};
    struct c5_legacy_window_epoch_input input={
        cpu_of(rq),nr_cpu_ids,c5_legacy_pelt_hmp_enabled()
    };
    int ret;
    lockdep_assert_held(&rq->lock);
    if (!irqs_disabled()) {
        rq->c5_legacy_window.error=-EINVAL;
        return;
    }
    ret=c5_legacy_window_epoch_initialize(&rq->c5_legacy_window,&input,
                              &c5_legacy_window_native_epoch_ops,&ctx);
    if (ret)
        rq->c5_legacy_window.error=ret;
}

void c5_legacy_window_native_cpu_dying(struct rq *rq)
{
    struct c5_legacy_window_native_epoch_context ctx={rq};
    unsigned int cpu=cpu_of(rq),peer;
    int ret;
    lockdep_assert_held(&rq->lock);
    if (!irqs_disabled()) {
        rq->c5_legacy_window.error=-EINVAL;
        return;
    }
    if (READ_ONCE(c5_legacy_window_sync_cpu)!=cpu)
        return;
    peer=cpumask_any_but(cpu_online_mask,cpu);
    if (peer>=nr_cpu_ids) {
        rq->c5_legacy_window.error=-EAGAIN;
        return;
    }
    ret=c5_legacy_window_epoch_handoff(&rq->c5_legacy_window,
                     &cpu_rq(peer)->c5_legacy_window,cpu,peer,nr_cpu_ids,
                     &c5_legacy_window_native_epoch_ops,&ctx);
    if (ret)
        rq->c5_legacy_window.error=ret;
}

/* Exclusive accounting scratch. No producer is connected by this block.
 * The successful caller must retain IRQ-off CPU stability and the primary rq
 * lock through prepare/commit/journal delivery, then consume the lease. There
 * is no fallback, NMI access, waiting or implicit IRQ/preemption manipulation.
 */
static DEFINE_PER_CPU(struct c5_legacy_window_workspace,
                     c5_legacy_window_workspaces);

int c5_legacy_window_native_acquire(struct rq *rq,unsigned int kind,
                                   struct c5_legacy_window_lease *lease)
{
    if (!rq || !lease || in_nmi() || !irqs_disabled())
        return -EINVAL;
    lockdep_assert_held(&rq->lock);
    return c5_legacy_window_workspace_acquire(
              this_cpu_ptr(&c5_legacy_window_workspaces),
              raw_smp_processor_id(),rq,kind,lease);
}

int c5_legacy_window_native_release(struct rq *rq,
                                   struct c5_legacy_window_lease *lease)
{
    if (!rq || !lease || in_nmi() || !irqs_disabled())
        return -EINVAL;
    lockdep_assert_held(&rq->lock);
    return c5_legacy_window_workspace_release(
              this_cpu_ptr(&c5_legacy_window_workspaces),
              raw_smp_processor_id(),rq,lease);
}

/* Typed legacy trace delivery. Class DEC/INC records are not trace events;
 * the caller must apply their real class effects separately, in order.
 */
static bool c5_legacy_window_native_record_owned(
        const struct c5_legacy_window_workspace *workspace,
        const struct c5_legacy_window_record *record)
{
    unsigned long base,address=(unsigned long)record,stride,delta;
    u32 count,limit;
    if (workspace->kind==C5_LEGACY_WORKSPACE_UPDATE) {
        base=(unsigned long)&workspace->journal.update.records[0];
        stride=sizeof(workspace->journal.update.records[0]);
        count=workspace->journal.update.count;limit=C5_LEGACY_WINDOW_RECORDS;
    } else if (workspace->kind==C5_LEGACY_WORKSPACE_MIGRATION) {
        base=(unsigned long)&workspace->journal.migration.records[0].value;
        stride=sizeof(workspace->journal.migration.records[0]);
        count=workspace->journal.migration.count;limit=C5_LEGACY_MIGRATION_RECORDS;
    } else
        return false;
    if (count>limit || address<base)
        return false;
    delta=address-base;
    return delta%stride==0 && delta/stride<count;
}

const struct c5_legacy_window_trace_view *
c5_legacy_window_trace_current(const struct task_struct *task,
                              const struct rq *rq,unsigned int kind)
{
    struct c5_legacy_window_workspace *workspace;
    const struct c5_legacy_window_trace_view *view;
    if (!task || !rq || in_nmi() || !irqs_disabled())
        return NULL;
    workspace=this_cpu_ptr(&c5_legacy_window_workspaces);
    view=&workspace->trace;
    if (!view->active || !workspace->kind || workspace->cpu!=raw_smp_processor_id() ||
        view->generation!=workspace->generation || view->task!=task || view->rq!=rq ||
        !c5_legacy_window_native_record_owned(workspace,view->record) ||
        (unsigned int)view->record->kind!=kind)
        return NULL;
    return view;
}

int c5_legacy_window_native_emit_record(struct c5_legacy_window_lease *lease,
        struct task_struct *task,struct rq *rq,
        const struct c5_legacy_window_record *record,u64 wallclock,u64 irqtime,
        const struct c5_legacy_window_class_state *classes)
{
    struct c5_legacy_window_workspace *workspace;
    struct c5_legacy_window_trace_view *view;
    struct rq *primary;
    if (!lease || !task || !rq || !record || in_nmi() || !irqs_disabled())
        return -EINVAL;
    workspace=this_cpu_ptr(&c5_legacy_window_workspaces);
    if (lease->cpu!=raw_smp_processor_id() || workspace->cpu!=raw_smp_processor_id())
        return -EXDEV;
    if (lease->workspace!=workspace || !workspace->kind ||
        lease->generation!=workspace->generation || lease->kind!=workspace->kind ||
        !lease->owner || lease->owner!=workspace->owner)
        return -ESTALE;
    if (workspace->trace.active)
        return -EBUSY;
    if (!c5_legacy_window_native_record_owned(workspace,record))
        return -EINVAL;
    if (record->kind==C5_LEGACY_RECORD_DEC || record->kind==C5_LEGACY_RECORD_INC)
        return -EOPNOTSUPP;
    if (record->kind!=C5_LEGACY_RECORD_HISTORY && record->kind!=C5_LEGACY_RECORD_UPDATE &&
        record->kind!=C5_LEGACY_RECORD_MIGRATION)
        return -EINVAL;
    if (record->kind==C5_LEGACY_RECORD_MIGRATION &&
        workspace->kind!=C5_LEGACY_WORKSPACE_MIGRATION)
        return -EINVAL;
    if ((record->kind==C5_LEGACY_RECORD_HISTORY || record->kind==C5_LEGACY_RECORD_UPDATE) &&
        (record->event<C5_LEGACY_PUT_PREV || record->event>C5_LEGACY_IRQ))
        return -EINVAL;
    if (record->kind==C5_LEGACY_RECORD_HISTORY &&
        (!classes || classes->nr_big_tasks<0 || classes->nr_small_tasks<0))
        return -EINVAL;
    primary=(struct rq *)workspace->owner;
    lockdep_assert_held(&primary->lock);
    lockdep_assert_held(&rq->lock);
    view=&workspace->trace;
    view->record=record;view->task=task;view->rq=rq;
    view->generation=workspace->generation;
    if (record->kind==C5_LEGACY_RECORD_HISTORY)
        view->classes=*classes;
    else
        memset(&view->classes,0,sizeof(view->classes));
    view->active=true;
    if (record->kind==C5_LEGACY_RECORD_HISTORY)
        trace_sched_update_history(rq,task,record->runtime,record->samples,
                                   (enum task_event)record->event);
    else if (record->kind==C5_LEGACY_RECORD_UPDATE)
        trace_sched_update_task_ravg(task,rq,(enum task_event)record->event,
                                     wallclock,irqtime);
    else
        trace_sched_migration_update_sum(rq,task);
    /* Trace callbacks retain the same IRQ/CPU contract as the caller. Do not
     * clear another CPU's context if a broken callback violates it.
     */
    if (in_nmi() || !irqs_disabled())
        return -EINVAL;
    if (raw_smp_processor_id()!=lease->cpu)
        return -EXDEV;
    view->active=false;
    return 0;
}
#endif

#ifdef CONFIG_C5_SCHED_WINDOW_TRACE_COMPAT
static int c5_legacy_window_native_migrate_locked(struct task_struct *moving,
        struct rq *source,struct rq *destination);
#endif
