#include <linux/ftrace.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <asm/alternative.h>
#include <asm/cacheflush.h>
#include <asm/cpufeature.h>
#include <asm/debug-monitors.h>
#include <asm/exec.h>
#include <asm/pgtable.h>
#include <asm/memory.h>
#include <asm/mmu_context.h>
#include <asm/smp_plat.h>
#include <asm/suspend.h>
#include <asm/tlbflush.h>
#include <asm/virt.h>
#include <asm/cputype.h>
#include <soc/qcom/scm-boot.h>
#include <soc/qcom/lpm_levels.h>

#ifdef CONFIG_QTI_MPM_C5LTE
#include <linux/atomic.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/c5-boot-trace.h>
/* Temporary exact-C5 diagnostic. Save context normally, then reject before
 * invoking the firmware finisher. This is not a power-policy fix.
 */
static bool c5_ctx_diag_active;
static atomic_t c5_ctx_diag_aborted = ATOMIC_INIT(0);
static atomic_t c5_ctx_diag_last = ATOMIC_INIT(0);

/* One system-only selected entry experiment per boot. Software mode and
 * real-firmware mode are chosen by a read-only BOOT argument. Every later
 * target request retains the safe abort; mode0/unknown stays disarmed.
 * 0 unused, 1 claimed/before-flush-or-firmware, 2 full resume, 3 returned.
 */
/* Functional invocation scope, not a diagnostic record. Only the CPU0
 * system-suspend caller may consume the single firmware experiment.
 */
static DEFINE_PER_CPU(bool, c5_firmware_system_scope_active);

void c5_firmware_system_scope(bool active)
{
	this_cpu_write(c5_firmware_system_scope_active,
		active && smp_processor_id() == 0);
}

/* Read-only boot selection: 0 disables the target experiment, 1 uses
 * the software physical return, 2 invokes the highest firmware finisher.
 * Modes3/4 retain the accepted lower pair; mode5 is highest saved entry.
 * Both lower modes share one real save/idmap/finisher/restore callsite.
 */
extern bool c5_query_smc_baseline_ready(void);

static int c5_trial_mode;
module_param_named(c5_trial_mode, c5_trial_mode, int, 0400);

extern void c5_cpu0_saved_mmu_resume(void);
/* Success-only witness, consumed only after a complete same-boot return. */
unsigned int c5_saved_mmu_completed;

static bool c5_firmware_profile_active;
static bool c5_cpu0_warm_ready;
static atomic_t c5_firmware_state = ATOMIC_INIT(0);
static atomic_t c5_firmware_cpu = ATOMIC_INIT(-1);
static atomic_t c5_firmware_prepared = ATOMIC_INIT(0);
static atomic_t c5_firmware_resumed = ATOMIC_INIT(0);
static atomic_t c5_firmware_returned = ATOMIC_INIT(0);
static atomic_t c5_firmware_error = ATOMIC_INIT(0);

static void c5_ctx_diag_profile_init(void)
{
	struct device_node *psci;
	bool matched = false;

	WRITE_ONCE(c5_ctx_diag_active, false);
	WRITE_ONCE(c5_firmware_profile_active, false);
	if (!of_machine_is_compatible("qcom,msm8952-mtp"))
		return;
	psci = of_find_node_by_path("/psci");
	if (psci) {
		matched = of_device_is_compatible(psci, "arm,psci-1.0") &&
			of_property_read_bool(psci, "samsung,c5-legacy-psci-state");
		of_node_put(psci);
	}
	WRITE_ONCE(c5_ctx_diag_active, matched);
	WRITE_ONCE(c5_firmware_profile_active, matched &&
		read_sysreg(CurrentEL) == CurrentEL_EL1);
}

static bool c5_ctx_abort_after_save(unsigned long arg)
{
	if (!READ_ONCE(c5_ctx_diag_active) || (arg & ~0xffffffffUL))
		return false;
	/* Never let idle/freeze or a later CPU0 lower request bypass the
	 * single prepared system-suspend trial into an unpaired entry.
	 * Existing highest-state safe rejection remains unchanged.
	 */
	if ((arg & 0xf00UL) != 0x200UL &&
	    !(arg == 0x42000132UL && smp_processor_id() == 0 &&
	      (READ_ONCE(c5_trial_mode) == 3 ||
	       READ_ONCE(c5_trial_mode) == 4)))
		return false;
	atomic_inc(&c5_ctx_diag_aborted);
	atomic_set(&c5_ctx_diag_last, (u32)arg);
	c5_pm_checkpoint(0xe0d2, (u32)arg, 0);
	return true;
}

/* One declared target per immutable BOOT mode. Mode5 uses the same standard
 * SMC ABI and saved-MMU entry already accepted by the lower mode4 control.
 * Highest-state requests retain their real native RPM/MPM preparation.
 * Mode8 uses ordinary cpu_resume for the highest canonical-SMCCC request;
 * both the warm registration and PSCI argument select that native entry.
 */
static bool c5_trial_target(unsigned long state_id)
{
 int mode = READ_ONCE(c5_trial_mode);
 return ((mode == 3 || mode == 4) && state_id == 0x42000132UL) ||
        ((mode == 5 || mode == 6 || mode == 7 || mode == 8) &&
         state_id == 0x42000232UL);
}

/* Read-only dispatch choice; no runtime setter or event recording. */
bool c5_lower_saved_mmu_entry(unsigned long state_id)
{
	return READ_ONCE(c5_firmware_profile_active) &&
		(READ_ONCE(c5_trial_mode) == 4 ||
		 (READ_ONCE(c5_trial_mode) == 5 ||
		  (READ_ONCE(c5_trial_mode) == 6 ||
		   READ_ONCE(c5_trial_mode) == 7))) && c5_trial_target(state_id) &&
		smp_processor_id() == 0 &&
		this_cpu_read(c5_firmware_system_scope_active);
}

/* Repeated CPU0 platform-system highest entry, selected only by mode9.
 * This token is per call, independent of the historical one-shot witnesses:
 * 0 free, 1 firmware invocation in flight, 2 publishing completed native return.
 * It does not enable highest entry from idle/freeze or change the mode0 default.
 */
static atomic_t c5_repeat_inflight = ATOMIC_INIT(0);
static atomic64_t c5_repeat_completed = ATOMIC64_INIT(0);

static bool c5_repeat_eligible(unsigned long arg)
{
	return READ_ONCE(c5_trial_mode) == 9 && arg == 0x42000232UL &&
		smp_processor_id() == 0 &&
		this_cpu_read(c5_firmware_system_scope_active) &&
		READ_ONCE(c5_firmware_profile_active) &&
		READ_ONCE(c5_cpu0_warm_ready);
}

static bool c5_repeat_claim(unsigned long arg)
{
	if (!c5_repeat_eligible(arg) ||
	    atomic_cmpxchg(&c5_repeat_inflight, 0, 1) != 0)
		return false;
	WRITE_ONCE(c5_saved_mmu_completed, 0);
	return true;
}

/* An ordinary finisher return is not a native resume, including ret == 0.
 * Drop this call's token so its error cannot disable future system attempts.
 */
static void c5_repeat_note_return(unsigned long arg)
{
	if (c5_repeat_eligible(arg))
		atomic_cmpxchg(&c5_repeat_inflight, 1, 0);
}

/* Called only at the final C return, after native context/debug restoration.
 * A returning finisher cannot reach this with ret == 0: cpu_suspend converts
 * that result to -EOPNOTSUPP and has already released its token above.
 * Saturation preserves monotonicity instead of wrapping the success count.
 */
static void c5_repeat_note_complete(unsigned long arg, int ret)
{
	long long completed;

	if (ret || !c5_repeat_eligible(arg) ||
	    READ_ONCE(c5_saved_mmu_completed) != 0 ||
	    atomic_cmpxchg(&c5_repeat_inflight, 1, 2) != 1)
		return;
	do {
		completed = atomic64_read(&c5_repeat_completed);
		if (completed == LLONG_MAX)
			break;
	} while (atomic64_cmpxchg(&c5_repeat_completed, completed,
				 completed + 1) != completed);
	atomic_set(&c5_repeat_inflight, 0);
}

static int c5_repeat_success_count_get(char *buffer,
				     const struct kernel_param *kp)
{
	return scnprintf(buffer, PAGE_SIZE, "%llu",
		(unsigned long long)atomic64_read(&c5_repeat_completed));
}
static const struct kernel_param_ops c5_repeat_success_count_ops = {
	.get = c5_repeat_success_count_get,
};
module_param_cb(c5_repeat_success_count, &c5_repeat_success_count_ops, NULL, 0400);

static bool c5_firmware_claim_once(unsigned long arg)
{
	if (smp_processor_id() != 0 ||
	    !this_cpu_read(c5_firmware_system_scope_active) ||
	    !READ_ONCE(c5_firmware_profile_active) ||
	    !c5_trial_target(arg) ||
	    !READ_ONCE(c5_cpu0_warm_ready) ||
	    (READ_ONCE(c5_trial_mode) == 7 &&
	     !c5_query_smc_baseline_ready()) ||
	    atomic_cmpxchg(&c5_firmware_state, 0, 1) != 0)
		return false;
	WRITE_ONCE(c5_saved_mmu_completed, 0);
	atomic_set(&c5_firmware_cpu, smp_processor_id());
	atomic_inc(&c5_firmware_prepared);
	c5_pm_checkpoint(0xe0f4, (u32)arg, 0);
	return true;
}

static bool c5_firmware_pending(unsigned long arg)
{
	return READ_ONCE(c5_firmware_profile_active) && c5_trial_target(arg) &&
		atomic_read(&c5_firmware_state) == 1 &&
		atomic_read(&c5_firmware_cpu) == smp_processor_id();
}

/* Matched negative control: omit exactly the final highest CPU_SUSPEND SMC.
 * All current platform/context/cache/finisher dispatch runs before this hook.
 * No real power-state entry or resume is claimed by this separate witness.
 */
static bool c5_no_smc_omitted;
static bool c5_no_smc_completed;

bool c5_no_smc_control(unsigned long state_id)
{
	if (READ_ONCE(c5_trial_mode) != 6 || state_id != 0x42000232UL ||
	    smp_processor_id() != 0 ||
	    !this_cpu_read(c5_firmware_system_scope_active) ||
	    !c5_firmware_pending(state_id) ||
	    READ_ONCE(c5_no_smc_omitted))
		return false;
	WRITE_ONCE(c5_no_smc_omitted, true);
	return true;
}

static void c5_no_smc_note_complete(unsigned long arg, int ret)
{
	if (READ_ONCE(c5_trial_mode) == 6 && arg == 0x42000232UL &&
	    smp_processor_id() == 0 &&
	    this_cpu_read(c5_firmware_system_scope_active) &&
	    READ_ONCE(c5_no_smc_omitted) && ret == -EPERM &&
	    atomic_read(&c5_firmware_state) == 3 &&
	    atomic_read(&c5_firmware_returned) == 1 &&
	    atomic_read(&c5_firmware_error) == -EPERM)
		WRITE_ONCE(c5_no_smc_completed, true);
}

static int c5_no_smc_success_get(char *buffer, const struct kernel_param *kp)
{
	return scnprintf(buffer, PAGE_SIZE, "%u",
		(unsigned int)READ_ONCE(c5_no_smc_completed));
}
static const struct kernel_param_ops c5_no_smc_success_ops = {
	.get = c5_no_smc_success_get,
};
module_param_cb(c5_no_smc_success, &c5_no_smc_success_ops, NULL, 0400);

/* A separate returning-query control, never a power/resume witness. The
 * immutable mode7 selector refuses an ineligible highest call in the PSCI
 * dispatcher instead of falling through to the real CPU_SUSPEND operation.
 */
static bool c5_query_smc_invoked;
static bool c5_query_smc_completed;

bool c5_query_smc_selected(unsigned long state_id)
{
	return READ_ONCE(c5_trial_mode) == 7 && state_id == 0x42000232UL;
}

bool c5_query_smc_control(unsigned long state_id)
{
	if (!c5_query_smc_selected(state_id) || smp_processor_id() != 0 ||
	    !this_cpu_read(c5_firmware_system_scope_active) ||
	    !c5_firmware_pending(state_id) ||
	    !c5_query_smc_baseline_ready() ||
	    READ_ONCE(c5_query_smc_invoked))
		return false;
	WRITE_ONCE(c5_query_smc_invoked, true);
	return true;
}

static void c5_query_smc_note_complete(unsigned long arg, int ret)
{
	if (c5_query_smc_selected(arg) && smp_processor_id() == 0 &&
	    this_cpu_read(c5_firmware_system_scope_active) &&
	    READ_ONCE(c5_query_smc_invoked) && ret == -EPERM &&
	    atomic_read(&c5_firmware_state) == 3 &&
	    atomic_read(&c5_firmware_returned) == 1 &&
	    atomic_read(&c5_firmware_error) == -EPERM)
		WRITE_ONCE(c5_query_smc_completed, true);
}

static int c5_query_smc_success_get(char *buffer, const struct kernel_param *kp)
{
	return scnprintf(buffer, PAGE_SIZE, "%u",
		(unsigned int)READ_ONCE(c5_query_smc_completed));
}
static const struct kernel_param_ops c5_query_smc_success_ops = {
	.get = c5_query_smc_success_get,
};
module_param_cb(c5_query_smc_success, &c5_query_smc_success_ops, NULL, 0400);

static void c5_firmware_note_return(unsigned long arg, int ret)
{
	atomic_inc(&c5_firmware_returned);
	atomic_set(&c5_firmware_error, ret);
	atomic_set(&c5_firmware_state, 3);
	c5_pm_checkpoint(0xe0f5, (u32)arg, (u32)ret);
}

static void c5_firmware_note_resume(unsigned long arg)
{
	if (!c5_firmware_pending(arg))
		return;
	atomic_inc(&c5_firmware_resumed);
	atomic_set(&c5_firmware_state, 2);
	c5_pm_checkpoint(0xe0f6, (u32)arg, 0);
}

/* Independent atomics, not a coherent multi-counter snapshot. */
static int c5_firmware_diag_get(char *buffer, const struct kernel_param *kp)
{
	return scnprintf(buffer, PAGE_SIZE,
		"C5FWCTX1 ACTIVE=%u STATE=%u PREPARED=%u RESUMED=%u "
		"RETURNED=%u ERR=%d CPU=%d\n",
		(unsigned int)READ_ONCE(c5_firmware_profile_active),
		(unsigned int)atomic_read(&c5_firmware_state),
		(unsigned int)atomic_read(&c5_firmware_prepared),
		(unsigned int)atomic_read(&c5_firmware_resumed),
		(unsigned int)atomic_read(&c5_firmware_returned),
		atomic_read(&c5_firmware_error),
		atomic_read(&c5_firmware_cpu));
}
static const struct kernel_param_ops c5_firmware_diag_ops = {
	.get = c5_firmware_diag_get,
};
module_param_cb(c5_firmware_ctx_diag, &c5_firmware_diag_ops, NULL, 0400);

/* One dependency-preserving comparison group: the legacy PM boot provider
 * registers a warm entry, while this PSCI port did not. Keep the existing
 * helper's ARM64/HLOS flag and target CPU0 only; other CPU boot paths and
 * PSCI arguments remain unchanged. This is not a demonstrated root cause.
 */
static int __init c5_cpu0_warm_init(void)
{
#ifdef CONFIG_QCOM_SCM
	phys_addr_t entry;
	u64 mpidr;
	u32 aff0, aff1, aff2;

	WRITE_ONCE(c5_cpu0_warm_ready, false);
	if (!READ_ONCE(c5_firmware_profile_active))
		return 0;
	mpidr = cpu_logical_map(0);
	/* Logical CPU0 need not occupy physical affinity 0/0/0. The fixed
	 * C5 DT includes CPU0 at 0x100. Select only the actual boot CPU,
	 * and reject affinities outside the existing SCM bitmap ABI.
	 */
	aff0 = MPIDR_AFFINITY_LEVEL(mpidr, 0);
	aff1 = MPIDR_AFFINITY_LEVEL(mpidr, 1);
	aff2 = MPIDR_AFFINITY_LEVEL(mpidr, 2);
	if (aff0 >= 32 || aff1 >= 32 || aff2 >= 32 ||
	    MPIDR_AFFINITY_LEVEL(mpidr, 3))
		return 0;
	/* Keep both firmware entry selectors consistent in each control. */
	entry = (READ_ONCE(c5_trial_mode) == 3 ||
		 READ_ONCE(c5_trial_mode) == 8 ||
		 READ_ONCE(c5_trial_mode) == 9) ? __pa_symbol(cpu_resume) :
		__pa_symbol(c5_cpu0_saved_mmu_resume);
	if (entry > 0xffffffffULL || (entry & 3) ||
	    scm_is_mc_boot_available() <= 0)
		return 0;
	if (!scm_set_boot_addr_mc(entry, 1U << aff0, 1U << aff1,
			      1U << aff2, SCM_FLAG_WARMBOOT_MC))
		WRITE_ONCE(c5_cpu0_warm_ready, true);
#endif
	return 0;
}
late_initcall(c5_cpu0_warm_init);

static int c5_cpu0_warm_ready_get(char *buffer, const struct kernel_param *kp)
{
	return scnprintf(buffer, PAGE_SIZE, "%u",
		(unsigned int)READ_ONCE(c5_cpu0_warm_ready));
}
static const struct kernel_param_ops c5_cpu0_warm_ready_ops = {
	.get = c5_cpu0_warm_ready_get,
};
module_param_cb(c5_cpu0_warm_ready, &c5_cpu0_warm_ready_ops, NULL, 0400);

/* Functional setup readiness only; never read this as a failed-stage log. */
static int c5_trial_ready_get(char *buffer, const struct kernel_param *kp)
{
	int mode = READ_ONCE(c5_trial_mode);
	bool ready = READ_ONCE(c5_firmware_profile_active) &&
		READ_ONCE(c5_cpu0_warm_ready) &&
		(mode == 3 || mode == 4 || mode == 5 || mode == 6 ||
		 mode == 7 || mode == 8) &&
		(mode != 7 || c5_query_smc_baseline_ready()) &&
		atomic_read(&c5_firmware_state) == 0;

	if (mode == 9)
		ready = READ_ONCE(c5_firmware_profile_active) &&
			READ_ONCE(c5_cpu0_warm_ready) &&
			atomic_read(&c5_repeat_inflight) == 0;
	return scnprintf(buffer, PAGE_SIZE, "%u\n", (unsigned int)ready);
}
static const struct kernel_param_ops c5_trial_ready_ops = {
	.get = c5_trial_ready_get,
};
module_param_cb(c5_trial_ready, &c5_trial_ready_ops, NULL, 0400);

/* Binary witness only: actual native resume, not a normal finisher return.
 * No failure context, error/stage record or new RAM sink is exposed here.
 */
static int c5_trial_success_get(char *buffer, const struct kernel_param *kp)
{
	return scnprintf(buffer, PAGE_SIZE, "%u",
		(unsigned int)(atomic_read(&c5_firmware_state) == 2 &&
		atomic_read(&c5_firmware_resumed) == 1 &&
		atomic_read(&c5_firmware_returned) == 0 &&
		(((READ_ONCE(c5_trial_mode) == 3 ||
		   READ_ONCE(c5_trial_mode) == 8) &&
		  READ_ONCE(c5_saved_mmu_completed) == 0) ||
		 ((READ_ONCE(c5_trial_mode) == 4 ||
		   READ_ONCE(c5_trial_mode) == 5) &&
		  READ_ONCE(c5_saved_mmu_completed) == 1))));
}
static const struct kernel_param_ops c5_trial_success_ops = {
	.get = c5_trial_success_get,
};
module_param_cb(c5_trial_success, &c5_trial_success_ops, NULL, 0400);

/* Independently atomic values; no multi-counter coherent snapshot claim. */
static int c5_ctx_diag_get(char *buffer, const struct kernel_param *kp)
{
	return scnprintf(buffer, PAGE_SIZE,
		"C5CTXSAVE1 ACTIVE=%u ABORTED=%u LAST=%08x\n",
		(unsigned int)READ_ONCE(c5_ctx_diag_active),
		(unsigned int)atomic_read(&c5_ctx_diag_aborted),
		(unsigned int)atomic_read(&c5_ctx_diag_last));
}
static const struct kernel_param_ops c5_ctx_diag_ops = {
	.get = c5_ctx_diag_get,
};
module_param_cb(c5_ctx_diag, &c5_ctx_diag_ops, NULL, 0400);
#endif

/*
 * This is allocated by cpu_suspend_init(), and used to store a pointer to
 * the 'struct sleep_stack_data' the contains a particular CPUs state.
 */
unsigned long *sleep_save_stash;

/*
 * This hook is provided so that cpu_suspend code can restore HW
 * breakpoints as early as possible in the resume path, before reenabling
 * debug exceptions. Code cannot be run from a CPU PM notifier since by the
 * time the notifier runs debug exceptions might have been enabled already,
 * with HW breakpoints registers content still in an unknown state.
 */
static int (*hw_breakpoint_restore)(unsigned int);
void __init cpu_suspend_set_dbg_restorer(int (*hw_bp_restore)(unsigned int))
{
	/* Prevent multiple restore hook initializations */
	if (WARN_ON(hw_breakpoint_restore))
		return;
	hw_breakpoint_restore = hw_bp_restore;
}

void notrace __cpu_suspend_exit(void)
{
	unsigned int cpu = smp_processor_id();

	/*
	 * We are resuming from reset with the idmap active in TTBR0_EL1.
	 * We must uninstall the idmap and restore the expected MMU
	 * state before we can possibly return to userspace.
	 */
	cpu_uninstall_idmap();

	/*
	 * PSTATE was not saved over suspend/resume, re-enable any detected
	 * features that might not have been set correctly.
	 */
	asm(ALTERNATIVE("nop", SET_PSTATE_PAN(1), ARM64_HAS_PAN,
			CONFIG_ARM64_PAN));
	uao_thread_switch(current);

	/*
	 * Restore HW breakpoint registers to sane values
	 * before debug exceptions are possibly reenabled
	 * through local_dbg_restore.
	 */
	if (hw_breakpoint_restore)
		hw_breakpoint_restore(cpu);

	/*
	 * On resume, firmware implementing dynamic mitigation will
	 * have turned the mitigation on. If the user has forcefully
	 * disabled it, make sure their wishes are obeyed.
	 */
	if (arm64_get_ssbd_state() == ARM64_SSBD_FORCE_DISABLE)
		arm64_set_ssbd_mitigation(false);
}

/*
 * cpu_suspend
 *
 * arg: argument to pass to the finisher function
 * fn: finisher function pointer
 *
 */
int cpu_suspend(unsigned long arg, int (*fn)(unsigned long))
{
	int ret = 0;
	unsigned long flags;
	struct sleep_stack_data state;

	/*
	 * From this point debug exceptions are disabled to prevent
	 * updates to mdscr register (saved and restored along with
	 * general purpose registers) from kernel debuggers.
	 */
	local_dbg_save(flags);

	/*
	 * Function graph tracer state gets incosistent when the kernel
	 * calls functions that never return (aka suspend finishers) hence
	 * disable graph tracing during their execution.
	 */
	pause_graph_tracing();

	if (__cpu_suspend_enter(&state)) {
		/* Call the suspend finisher, except for the exact-C5 diagnostic. */
#ifdef CONFIG_QTI_MPM_C5LTE
		if (c5_repeat_claim(arg)) {
			/* Repeat the accepted mode8 native preparation on every call. */
			cpu_install_idmap();
			flush_cache_all();
			ret = fn(arg);
			cpu_uninstall_idmap();
			c5_repeat_note_return(arg);
		} else if (c5_firmware_claim_once(arg)) {
			/* Matched real selected-state control. The native frame, idmap,
			 * cache publication and finisher remain shared. Only the
			 * declared target/entry group differs by immutable BOOT mode.
			 */
			cpu_install_idmap();
			flush_cache_all();
			ret = fn(arg);
			cpu_uninstall_idmap();
			c5_firmware_note_return(arg, ret);
		} else if (c5_ctx_abort_after_save(arg))
			ret = -ECANCELED;
		else
#endif
		ret = fn(arg);

		/*
		 * Never gets here, unless the suspend finisher fails.
		 * Successful cpu_suspend() should return from cpu_resume(),
		 * returning through this code path is considered an error
		 * If the return value is set to 0 force ret = -EOPNOTSUPP
		 * to make sure a proper error condition is propagated
		 */
		if (!ret)
			ret = -EOPNOTSUPP;
	} else {
		__cpu_suspend_exit();
#ifdef CONFIG_QTI_MPM_C5LTE
		c5_firmware_note_resume(arg);
#endif
	}

	unpause_graph_tracing();

	/*
	 * Restore pstate flags. OS lock and mdscr have been already
	 * restored, so from this point onwards, debugging is fully
	 * renabled if it was enabled when core started shutdown.
	 */
	local_dbg_restore(flags);
#ifdef CONFIG_QTI_MPM_C5LTE
	c5_no_smc_note_complete(arg, ret);
	c5_query_smc_note_complete(arg, ret);
	c5_repeat_note_complete(arg, ret);
#endif

	return ret;
}

static int __init cpu_suspend_init(void)
{
	/* ctx_ptr is an array of physical addresses */
	sleep_save_stash = kcalloc(mpidr_hash_size(), sizeof(*sleep_save_stash),
				   GFP_KERNEL);

	if (WARN_ON(!sleep_save_stash))
		return -ENOMEM;
#ifdef CONFIG_QTI_MPM_C5LTE
	c5_ctx_diag_profile_init();
#endif

	return 0;
}
early_initcall(cpu_suspend_init);
