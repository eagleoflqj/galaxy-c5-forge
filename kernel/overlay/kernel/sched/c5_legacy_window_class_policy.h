/* SPDX-License-Identifier: GPL-2.0-only */
/* Old statistical policy, independent of native WALT placement/control.
 * Included at the end of walt.c, after the actual rq-lock helpers. A policy
 * and its complete class-count rebuild commit under the same all-rq locks.
 */
#ifndef _C5_LEGACY_WINDOW_CLASS_POLICY_H
#define _C5_LEGACY_WINDOW_CLASS_POLICY_H

struct c5_legacy_window_policy_cluster {
    cpumask_t cpus;
    u32 cur_freq,min_freq,max_freq,possible_freq,efficiency;
    u32 capacity,possible_capacity,load_scale;
};
struct c5_legacy_window_class_policy {
    /* Slot zero is the old initial cluster, not an active list member.
     * A new cluster's nonempty mask is disjoint from every previously added
     * mask; therefore at most NR_CPUS active clusters can ever be created.
     * Fixed storage removes allocation under rq locks without a fake success.
     */
    struct c5_legacy_window_policy_cluster cluster[9];
    cpumask_t covered;
    u8 cpu_cluster[8];
    u32 clusters,minimum_efficiency,maximum_efficiency;
    u32 minimum_frequency,maximum_frequency,maximum_capacity;
    u32 up_down_scale,window_size;
    bool use_pelt,defaults_ready;
    struct c5_legacy_window_class_policy_view view;
};
static struct c5_legacy_window_class_policy c5_legacy_window_policy={
    .cluster={{.cur_freq=1,.min_freq=1,.max_freq=1,.possible_freq=1,
               .efficiency=1,.capacity=1024,.possible_capacity=1024,.load_scale=1024}},
    .minimum_efficiency=(u32)-1,.maximum_efficiency=1,
    .minimum_frequency=1,.maximum_frequency=1,.maximum_capacity=1024,
    .up_down_scale=1024,.window_size=10000000,
    .view={.cpu_scale={1024,1024,1024,1024,1024,1024,1024,1024},
           .maximum_scale=1024,.nice_cutoff=15},
};
static int c5_legacy_window_policy_error;

static int c5_legacy_window_class_policy_error_read(void)
{
    return READ_ONCE(c5_legacy_window_policy_error);
}

/* First failure quarantines this observer, never the cpufreq notifier chain.
 * cmpxchg also covers a malformed notification before rq locks are acquired.
 */
static void c5_legacy_window_class_policy_error_set(int error)
{
    if (error)
        cmpxchg(&c5_legacy_window_policy_error,0,error<0?error:-EINVAL);
}

static void c5_legacy_window_class_thresholds(
        struct c5_legacy_window_class_policy *policy)
{
    u32 load=policy->use_pelt?47742U:policy->window_size;
    u32 up=(u32)div64_u64((u64)80*load,100);
    u32 down=(u32)div64_u64((u64)60*load,100);
    u32 delta;
    /* Keep the old microsecond truncation and unsigned arithmetic. In PELT
     * mode the old cap is still sched_ravg_window, not LOAD_AVG_MAX.
     */
    if (policy->up_down_scale!=1024) {
        delta=up-down;
        up/=1000;up*=policy->up_down_scale;up>>=10;up*=1000;
        up=min(up,policy->window_size);
        down/=1000;down*=policy->up_down_scale;down>>=10;down*=1000;
        down=min(down,up-delta);
    }
    policy->view.upmigrate=up;
    policy->view.downmigrate=down;
}

void __init c5_legacy_window_native_class_defaults(void)
{
    struct c5_legacy_window_class_policy *policy=&c5_legacy_window_policy;
    u32 load;
    BUILD_BUG_ON(NR_CPUS>C5_LEGACY_CLASS_SCALE_CPUS);
    /* sched_init calls this after every rq is initialized and before the
     * boot idle task is weighted, matching old set_hmp_defaults placement.
     */
    policy->window_size=READ_ONCE(c5_legacy_window_boot_period);
    policy->use_pelt=READ_ONCE(c5_legacy_window_boot_use_pelt);
    load=policy->use_pelt?47742U:policy->window_size;
    policy->view.small_task=(u32)div64_u64((u64)10*load,100);
    policy->view.nice_cutoff=15;
    cpumask_copy(&policy->cluster[0].cpus,cpu_possible_mask);
    c5_legacy_window_class_thresholds(policy);
    policy->defaults_ready=true;
}

static int c5_legacy_window_class_capacity(
        const struct c5_legacy_window_class_policy *policy,
        struct c5_legacy_window_policy_cluster *cluster,bool maximum)
{
    u64 efficiency_factor,frequency_factor,capacity=1024,value;
    int error;
    if (!cluster->efficiency || !cluster->max_freq || !cluster->possible_freq ||
        !policy->minimum_efficiency || !policy->minimum_frequency)
        return -EINVAL;
    /* Old efficiency multiplication is signed int, frequency multiplication
     * unsigned int, and both compound capacity products end in signed int.
     * Refuse outside their positive defined numerical domain before shifts.
     */
    value=(u64)1024*cluster->efficiency;
    if (value>0x7fffffffU)
        return -EOVERFLOW;
    efficiency_factor=div64_u64(value,policy->minimum_efficiency);
    value=(u64)1024*cluster->max_freq;
    if (value>(u32)-1)
        return -EOVERFLOW;
    frequency_factor=div64_u64(value,policy->minimum_frequency);
    capacity*=efficiency_factor;
    if (capacity>0x7fffffffU)
        return -EOVERFLOW;
    capacity>>=10;
    capacity*=frequency_factor;
    if (capacity>0x7fffffffU)
        return -EOVERFLOW;
    capacity>>=10;
    cluster->capacity=(u32)capacity;
    if (maximum) {
        value=div64_u64(capacity*cluster->possible_freq,cluster->max_freq);
        if (value>0x7fffffffU)
            return -EOVERFLOW;
        cluster->possible_capacity=(u32)value;
    }
    error=c5_legacy_window_class_cpu_scale(cluster->efficiency,
                policy->maximum_efficiency,cluster->max_freq,
                policy->maximum_frequency,&cluster->load_scale);
    return error;
}

/* Complete statistical CPUFREQ_NOTIFY transition. Pure preparation reads no
 * mutable native cluster or policy alias and never publishes partial state.
 * The callback below owns every rq across this and the real count rebuild.
 */
static int c5_legacy_window_class_policy_prepare(
        const struct c5_legacy_window_class_policy *before,
        const struct cpufreq_policy *input,u32 efficiency,
        struct c5_legacy_window_class_policy *next)
{
    struct c5_legacy_window_policy_cluster *cluster;
    u64 highest_capacity=0,highest_scale=0,scale;
    u32 old_minimum,old_maximum,slot;
    unsigned int cpu,i;
    bool update_max;
    int error;
    if (!before || !input || !next || before==next || !efficiency ||
        cpumask_empty(input->related_cpus) ||
        !cpumask_subset(input->related_cpus,cpu_possible_mask) ||
        !input->max || !input->cpuinfo.max_freq)
        return -EINVAL;
    if (before->clusters>NR_CPUS || before->view.bank>1)
        return -EINVAL;
    if (before->view.generation==(u64)-1)
        return -EOVERFLOW;
    *next=*before;
    old_minimum=next->minimum_frequency;
    old_maximum=next->maximum_frequency;
    if (!cpumask_intersects(input->related_cpus,&next->covered)) {
        if (next->clusters>=NR_CPUS)
            return -EOVERFLOW;
        slot=++next->clusters;
        cluster=&next->cluster[slot];
        /* Old add_cluster used kzalloc, not init_cluster's 1024 capacities. */
        memset(cluster,0,sizeof(*cluster));
        cpumask_copy(&cluster->cpus,input->related_cpus);
        cluster->efficiency=efficiency;
        cpumask_or(&next->covered,&next->covered,input->related_cpus);
        for_each_cpu(cpu,input->related_cpus)
            next->cpu_cluster[cpu]=(u8)slot;
        next->maximum_efficiency=max(next->maximum_efficiency,efficiency);
        next->minimum_efficiency=min(next->minimum_efficiency,efficiency);
    }
    cpu=cpumask_first(input->related_cpus);
    slot=next->cpu_cluster[cpu];
    if (slot>next->clusters)
        return -EINVAL;
    cluster=&next->cluster[slot];
    cluster->min_freq=input->min;cluster->max_freq=input->max;
    cluster->cur_freq=input->cur;cluster->possible_freq=input->cpuinfo.max_freq;
    next->maximum_frequency=max(next->maximum_frequency,input->cpuinfo.max_freq);
    if (next->minimum_frequency==1)
        next->minimum_frequency=(u32)-1;
    next->minimum_frequency=min(next->minimum_frequency,input->cpuinfo.max_freq);
    update_max=old_minimum!=next->minimum_frequency ||
               old_maximum!=next->maximum_frequency;
    if (update_max) {
        for (i=1;i<=next->clusters;i++) {
            error=c5_legacy_window_class_capacity(next,&next->cluster[i],true);
            if (error)
                return error;
            highest_capacity=max(highest_capacity,(u64)next->cluster[i].possible_capacity);
            scale=div64_u64((u64)next->cluster[i].load_scale*
                    next->cluster[i].possible_freq,next->cluster[i].max_freq);
            highest_scale=max(highest_scale,scale);
        }
        next->maximum_capacity=(u32)highest_capacity;
        /* The old unsigned global has defined u64-to-u32 truncation. */
        next->view.maximum_scale=(u32)highest_scale;
    } else {
        error=c5_legacy_window_class_capacity(next,cluster,false);
        if (error)
            return error;
    }
    /* Old automatic threshold update keeps its previous factor on the
     * highest-capacity cluster, including after a changed global bound.
     */
    if (cluster->possible_capacity!=next->maximum_capacity) {
        if (cluster->possible_freq==cluster->max_freq)
            next->up_down_scale=1024;
        else
            next->up_down_scale=(1024U*cluster->possible_freq)/cluster->max_freq;
        c5_legacy_window_class_thresholds(next);
    }
    for_each_possible_cpu(cpu) {
        slot=next->cpu_cluster[cpu];
        if (slot>next->clusters)
            return -EINVAL;
        next->view.cpu_scale[cpu]=next->cluster[slot].load_scale;
    }
    next->view.bank=1-before->view.bank;
    next->view.generation++;
    return 0;
}

int c5_legacy_window_native_class_policy(const struct rq *rq,
        struct c5_legacy_window_class_policy_view *view)
{
    unsigned int cpu;
    int error;
    if (!rq || !view || in_nmi() || !irqs_disabled())
        return -EINVAL;
    cpu=READ_ONCE(rq->cpu);
    if (cpu>=NR_CPUS || cpu_rq(cpu)!=rq)
        return -EXDEV;
    lockdep_assert_held(&rq->lock);
    error=c5_legacy_window_class_policy_error_read();
    if (!error)
        error=READ_ONCE(rq->c5_legacy_window.error);
    if (error)
        return error<0?error:-EINVAL;
    if (!READ_ONCE(c5_legacy_window_policy.defaults_ready))
        return -EAGAIN;
    *view=c5_legacy_window_policy.view;
    return 0;
}


#ifdef CONFIG_C5_SCHED_ELIGIBLE_COMPAT
/* Pure source-health read: no rq acquisition, clock, reset or accounting.
 * Average readers own nr_lock, so acquiring rq here would invert writer
 * rq -> nr_lock ordering. Source errors are already sticky/quarantined.
 */
int c5_legacy_window_eligible_error(int cpu)
{
    int error;
    if (cpu<0 || cpu>=nr_cpu_ids || !cpu_possible(cpu))
        return -EINVAL;
    error=c5_legacy_window_class_policy_error_read();
    if (!error)
        error=READ_ONCE(cpu_rq(cpu)->c5_legacy_window.error);
    return error<0?error:error?-EINVAL:0;
}

int c5_legacy_window_publish_eligible(struct rq *rq,
        const struct c5_legacy_window_class_policy_view *view)
{
    const struct c5_legacy_window_class_state *classes;
    unsigned int cpu,slot;
    int error;
    if (!rq || !view || view->bank>1 || in_nmi() || !irqs_disabled())
        return -EINVAL;
    cpu=cpu_of(rq);
    if (cpu>=NR_CPUS || cpu_rq(cpu)!=rq)
        return -EXDEV;
    lockdep_assert_held(&rq->lock);
    error=c5_legacy_window_eligible_error(cpu);
    if (error)
        return error;
    if (view->bank!=c5_legacy_window_policy.view.bank ||
        view->generation!=c5_legacy_window_policy.view.generation)
        return -ESTALE;
    slot=c5_legacy_window_policy.cpu_cluster[cpu];
    if (slot>c5_legacy_window_policy.clusters)
        return -EINVAL;
    classes=&rq->c5_legacy_class.bank[view->bank];
    return c5_sched_avg_publish_eligible(cpu,classes->nr_big_tasks,
        classes->nr_small_tasks,
        c5_legacy_window_policy.cluster[slot].possible_capacity==
            c5_legacy_window_policy.maximum_capacity,
        view->generation);
}
#endif

static int c5_legacy_window_class_policy_notify(struct notifier_block *block,
        unsigned long action,void *data)
{
    const struct cpufreq_policy *input=data;
    struct c5_legacy_window_class_policy next;
    unsigned long flags,efficiency;
    unsigned int cpu;
    int error;
    if (action!=CPUFREQ_NOTIFY)
        return NOTIFY_DONE;
    /* REMOVE only changed the old placement-capacity extrema, not the
     * statistical inputs mirrored here. Native placement remains native.
     */
    if (!input || cpumask_empty(input->related_cpus) || in_interrupt() ||
        irqs_disabled()) {
        c5_legacy_window_class_policy_error_set(-EINVAL);
        return NOTIFY_DONE;
    }
    cpu=cpumask_first(input->related_cpus);
    if (cpu>=NR_CPUS || !cpu_possible(cpu)) {
        c5_legacy_window_class_policy_error_set(-EINVAL);
        return NOTIFY_DONE;
    }
    efficiency=arch_get_cpu_efficiency(cpu);
    if (!efficiency || efficiency>0x7fffffffU) {
        c5_legacy_window_class_policy_error_set(-EINVAL);
        return NOTIFY_DONE;
    }
    acquire_rq_locks_irqsave(cpu_possible_mask,&flags);
    error=c5_legacy_window_class_policy_error_read();
    if (!error)
        error=c5_legacy_window_class_policy_prepare(&c5_legacy_window_policy,
                                                     input,(u32)efficiency,&next);
    if (!error)
        error=c5_legacy_window_native_reclassify_locked(&next.view,
                          READ_ONCE(c5_legacy_window_disabled));
    if (!error) {
        c5_legacy_window_policy=next;
        for_each_possible_cpu(cpu)
            c5_legacy_window_class_scales.cpu_scale[cpu]=next.view.cpu_scale[cpu];
        c5_legacy_window_class_scales.maximum_scale=next.view.maximum_scale;
#ifdef CONFIG_C5_SCHED_MAX_SCALE_ABI
        /* Public old data ABI follows only the committed real policy. */
        WRITE_ONCE(max_load_scale_factor,next.view.maximum_scale);
        WRITE_ONCE(up_down_migrate_scale_factor,next.up_down_scale);
#endif
        c5_legacy_window_class_scales.min_max_freq=next.minimum_frequency;
        c5_legacy_window_class_scales.max_possible_freq=next.maximum_frequency;
        c5_legacy_window_class_scales.generation=next.view.generation;
#ifdef CONFIG_C5_SCHED_ELIGIBLE_COMPAT
        /* The complete rebuilt bank is now active. Cache only coherent
         * per-rq tuples; do not invent an old average sampling event.
         */
        for_each_possible_cpu(cpu) {
            error=c5_legacy_window_publish_eligible(cpu_rq(cpu),&next.view);
            if (error) {
                c5_legacy_window_class_policy_error_set(error);
                break;
            }
        }
#endif
    } else {
        c5_legacy_window_class_policy_error_set(error);
    }
    release_rq_locks_irqrestore(cpu_possible_mask,&flags);
    return NOTIFY_DONE;
}

static struct notifier_block c5_legacy_window_class_policy_block={
    .notifier_call=c5_legacy_window_class_policy_notify,
};
static struct notifier_block c5_legacy_window_transition_block;
static int __init c5_legacy_window_class_policy_register(void)
{
    int error=cpufreq_register_notifier(&c5_legacy_window_class_policy_block,
                                       CPUFREQ_POLICY_NOTIFIER);
    if (!error) {
        error=cpufreq_register_notifier(&c5_legacy_window_transition_block,
                                       CPUFREQ_TRANSITION_NOTIFIER);
        if (error)
            cpufreq_unregister_notifier(&c5_legacy_window_class_policy_block,
                                         CPUFREQ_POLICY_NOTIFIER);
    }
    c5_legacy_window_class_policy_error_set(error);
    return error;
}
/* Native WALT registers at core_initcall; selected msm cpufreq registers
 * its actual driver at subsys_initcall. Both observers precede first policy.
 */
core_initcall(c5_legacy_window_class_policy_register);
#endif
