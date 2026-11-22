/*
 *  linux/init/main.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  GK 2/5/95  -  Changed to support mounting root fs via NFS
 *  Added initrd & change_root: Werner Almesberger & Hans Lermen, Feb '96
 *  Moan early if gcc is old, avoiding bogus kernels - Paul Gortmaker, May '96
 *  Simplified starting of init:  Michael A. Griffith <grif@acm.org>
 */

#define DEBUG		/* Enable initcall_debug */

#include <linux/c5-boot-trace.h>
#include <linux/types.h>
#include <linux/c5-start-trace.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/stackprotector.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/initrd.h>
#include <linux/bootmem.h>
#include <linux/acpi.h>
#include <linux/tty.h>
#include <linux/percpu.h>
#include <linux/kmod.h>
#include <linux/vmalloc.h>
#include <linux/kernel_stat.h>
#include <linux/start_kernel.h>
#include <linux/security.h>
#include <linux/smp.h>
#include <linux/profile.h>
#include <linux/rcupdate.h>
#include <linux/moduleparam.h>
#include <linux/kallsyms.h>
#include <linux/writeback.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/cgroup.h>
#include <linux/efi.h>
#include <linux/tick.h>
#include <linux/interrupt.h>
#include <linux/taskstats_kern.h>
#include <linux/delayacct.h>
#include <linux/unistd.h>
#include <linux/rmap.h>
#include <linux/mempolicy.h>
#include <linux/key.h>
#include <linux/buffer_head.h>
#include <linux/page_ext.h>
#include <linux/debug_locks.h>
#include <linux/debugobjects.h>
#include <linux/lockdep.h>
#include <linux/kmemleak.h>
#include <linux/pid_namespace.h>
#include <linux/device.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/idr.h>
#include <linux/kgdb.h>
#include <linux/ftrace.h>
#include <linux/async.h>
#include <linux/kmemcheck.h>
#include <linux/sfi.h>
#include <linux/shmem_fs.h>
#include <linux/slab.h>
#include <linux/perf_event.h>
#include <linux/ptrace.h>
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/sched_clock.h>
#include <linux/context_tracking.h>
#include <linux/random.h>
#include <linux/list.h>
#include <linux/integrity.h>
#include <linux/proc_ns.h>
#include <linux/io.h>
#include <linux/kaiser.h>
#include <linux/cache.h>

#include <asm/io.h>
#include <asm/bugs.h>
#include <asm/setup.h>
#include <asm/sections.h>
#include <asm/cacheflush.h>

static int kernel_init(void *);

extern void init_IRQ(void);
extern void fork_init(void);
extern void radix_tree_init(void);

/*
 * Debug helper: via this flag we know that we are in 'early bootup code'
 * where only the boot processor is running with IRQ disabled.  This means
 * two things - IRQ must not be enabled before the flag is cleared and some
 * operations which are not allowed with IRQ disabled are allowed while the
 * flag is set.
 */
bool early_boot_irqs_disabled __read_mostly;

enum system_states system_state __read_mostly;
EXPORT_SYMBOL(system_state);

/*
 * Boot command-line arguments
 */
#define MAX_INIT_ARGS CONFIG_INIT_ENV_ARG_LIMIT
#define MAX_INIT_ENVS CONFIG_INIT_ENV_ARG_LIMIT

extern void time_init(void);
/* Default late time init is NULL. archs can override this later. */
void (*__initdata late_time_init)(void);

/* Untouched command line saved by arch-specific code. */
char __initdata boot_command_line[COMMAND_LINE_SIZE];
/* Untouched saved command line (eg. for /proc) */
char *saved_command_line;
/* Command line for parameter parsing */
static char *static_command_line;
/* Command line for per-initcall parameter parsing */
static char *initcall_command_line;

static char *execute_command;
static char *ramdisk_execute_command;

/*
 * Used to generate warnings if static_key manipulation functions are used
 * before jump_label_init is called.
 */
bool static_key_initialized __read_mostly;
EXPORT_SYMBOL_GPL(static_key_initialized);

/*
 * If set, this is an indication to the drivers that reset the underlying
 * device before going ahead with the initialization otherwise driver might
 * rely on the BIOS and skip the reset operation.
 *
 * This is useful if kernel is booting in an unreliable environment.
 * For ex. kdump situation where previous kernel has crashed, BIOS has been
 * skipped and devices will be in unknown state.
 */
unsigned int reset_devices;
EXPORT_SYMBOL(reset_devices);

static int __init set_reset_devices(char *str)
{
	reset_devices = 1;
	return 1;
}

__setup("reset_devices", set_reset_devices);

static const char *argv_init[MAX_INIT_ARGS+2] = { "init", NULL, };
const char *envp_init[MAX_INIT_ENVS+2] = { "HOME=/", "TERM=linux", NULL, };
static const char *panic_later, *panic_param;

extern const struct obs_kernel_param __setup_start[], __setup_end[];

static bool __init obsolete_checksetup(char *line)
{
	const struct obs_kernel_param *p;
	bool had_early_param = false;

	p = __setup_start;
	do {
		int n = strlen(p->str);
		if (parameqn(line, p->str, n)) {
			if (p->early) {
				/* Already done in parse_early_param?
				 * (Needs exact match on param part).
				 * Keep iterating, as we can have early
				 * params and __setups of same names 8( */
				if (line[n] == '\0' || line[n] == '=')
					had_early_param = true;
			} else if (!p->setup_func) {
				pr_warn("Parameter %s is obsolete, ignored\n",
					p->str);
				return true;
			} else if (p->setup_func(line + n))
				return true;
		}
		p++;
	} while (p < __setup_end);

	return had_early_param;
}

/*
 * This should be approx 2 Bo*oMips to start (note initial shift), and will
 * still work even if initially too large, it will just take slightly longer
 */
unsigned long loops_per_jiffy = (1<<12);
EXPORT_SYMBOL(loops_per_jiffy);

static int __init debug_kernel(char *str)
{
	console_loglevel = CONSOLE_LOGLEVEL_DEBUG;
	return 0;
}

static int __init quiet_kernel(char *str)
{
	console_loglevel = CONSOLE_LOGLEVEL_QUIET;
	return 0;
}

early_param("debug", debug_kernel);
early_param("quiet", quiet_kernel);

static int __init loglevel(char *str)
{
	int newlevel;

	/*
	 * Only update loglevel value when a correct setting was passed,
	 * to prevent blind crashes (when loglevel being set to 0) that
	 * are quite hard to debug
	 */
	if (get_option(&str, &newlevel)) {
		console_loglevel = newlevel;
		return 0;
	}

	return -EINVAL;
}

early_param("loglevel", loglevel);

/* Change NUL term back to "=", to make "param" the whole string. */
static int __init repair_env_string(char *param, char *val,
				    const char *unused, void *arg)
{
	if (val) {
		/* param=val or param="val"? */
		if (val == param+strlen(param)+1)
			val[-1] = '=';
		else if (val == param+strlen(param)+2) {
			val[-2] = '=';
			memmove(val-1, val, strlen(val)+1);
			val--;
		} else
			BUG();
	}
	return 0;
}

/* Anything after -- gets handed straight to init. */
static int __init set_init_arg(char *param, char *val,
			       const char *unused, void *arg)
{
	unsigned int i;

	if (panic_later)
		return 0;

	repair_env_string(param, val, unused, NULL);

	for (i = 0; argv_init[i]; i++) {
		if (i == MAX_INIT_ARGS) {
			panic_later = "init";
			panic_param = param;
			return 0;
		}
	}
	argv_init[i] = param;
	return 0;
}

/*
 * Unknown boot options get handed to init, unless they look like
 * unused parameters (modprobe will find them in /proc/cmdline).
 */
static int __init unknown_bootoption(char *param, char *val,
				     const char *unused, void *arg)
{
	repair_env_string(param, val, unused, NULL);

	/* Handle obsolete-style parameters */
	if (obsolete_checksetup(param))
		return 0;

	/* Unused module parameter. */
	if (strchr(param, '.') && (!val || strchr(param, '.') < val))
		return 0;

	if (panic_later)
		return 0;

	if (val) {
		/* Environment option */
		unsigned int i;
		for (i = 0; envp_init[i]; i++) {
			if (i == MAX_INIT_ENVS) {
				panic_later = "env";
				panic_param = param;
			}
			if (!strncmp(param, envp_init[i], val - param))
				break;
		}
		envp_init[i] = param;
	} else {
		/* Command line option */
		unsigned int i;
		for (i = 0; argv_init[i]; i++) {
			if (i == MAX_INIT_ARGS) {
				panic_later = "init";
				panic_param = param;
			}
		}
		argv_init[i] = param;
	}
	return 0;
}

static int __init init_setup(char *str)
{
	unsigned int i;

	execute_command = str;
	/*
	 * In case LILO is going to boot us with default command line,
	 * it prepends "auto" before the whole cmdline which makes
	 * the shell think it should execute a script with such name.
	 * So we ignore all arguments entered _before_ init=... [MJ]
	 */
	for (i = 1; i < MAX_INIT_ARGS; i++)
		argv_init[i] = NULL;
	return 1;
}
__setup("init=", init_setup);

static int __init rdinit_setup(char *str)
{
	unsigned int i;

	ramdisk_execute_command = str;
	/* See "auto" comment in init_setup */
	for (i = 1; i < MAX_INIT_ARGS; i++)
		argv_init[i] = NULL;
	return 1;
}
__setup("rdinit=", rdinit_setup);

#ifndef CONFIG_SMP
static const unsigned int setup_max_cpus = NR_CPUS;
static inline void setup_nr_cpu_ids(void) { }
static inline void smp_prepare_cpus(unsigned int maxcpus) { }
#endif

/*
 * We need to store the untouched command line for future reference.
 * We also need to store the touched command line since the parameter
 * parsing is performed in place, and we should allow a component to
 * store reference of name/value for future reference.
 */
static void __init setup_command_line(char *command_line)
{
	saved_command_line =
		memblock_virt_alloc(strlen(boot_command_line) + 1, 0);
	initcall_command_line =
		memblock_virt_alloc(strlen(boot_command_line) + 1, 0);
	static_command_line = memblock_virt_alloc(strlen(command_line) + 1, 0);
	strcpy(saved_command_line, boot_command_line);
	strcpy(static_command_line, command_line);
}

/*
 * We need to finalize in a non-__init function or else race conditions
 * between the root thread and the init thread may cause start_kernel to
 * be reaped by free_initmem before the root thread has proceeded to
 * cpu_idle.
 *
 * gcc-3.4 accidentally inlines this function, so use noinline.
 */

static __initdata DECLARE_COMPLETION(kthreadd_done);

static noinline void __ref rest_init(void)
{
	int pid;

	c5_boot_trace_event(0x400, 0, 0, 0);
	rcu_scheduler_starting();
	c5_boot_trace_event(0x401, 0, 0, 0);
	/*
	 * We need to spawn init first so that it obtains pid 1, however
	 * the init task will end up wanting to create kthreads, which, if
	 * we schedule it before we create kthreadd, will OOPS.
	 */
	c5_boot_trace_event(0x402, 0, 0, 0);
	kernel_thread(kernel_init, NULL, CLONE_FS);
	c5_boot_trace_event(0x403, 0, 0, 0);
	c5_boot_trace_event(0x404, 0, 0, 0);
	numa_default_policy();
	c5_boot_trace_event(0x405, 0, 0, 0);
	pid = kernel_thread(kthreadd, NULL, CLONE_FS | CLONE_FILES);
	c5_boot_trace_event(0x406, 0, 0, 0);
	rcu_read_lock();
	c5_boot_trace_event(0x407, 0, 0, 0);
	kthreadd_task = find_task_by_pid_ns(pid, &init_pid_ns);
	c5_boot_trace_event(0x408, 0, 0, 0);
	rcu_read_unlock();
	c5_boot_trace_event(0x409, 0, 0, 0);
	c5_boot_trace_event(0x40a, 0, 0, 0);
	complete(&kthreadd_done);
	c5_boot_trace_event(0x40b, 0, 0, 0);

	/*
	 * The boot idle thread must execute schedule()
	 * at least once to get things moving:
	 */
	c5_boot_trace_event(0x40c, 0, 0, 0);
	init_idle_bootup_task(current);
	c5_boot_trace_event(0x40d, 0, 0, 0);
	c5_boot_trace_event(0x40e, 0, 0, 0);
	schedule_preempt_disabled();
	c5_boot_trace_event(0x40f, 0, 0, 0);
	/* Call into cpu_idle with preempt disabled */
	c5_boot_trace_event(0x410, 0, 0, 0);
	cpu_startup_entry(CPUHP_ONLINE);
	c5_boot_trace_event(0x411, 0, 0, 0);
}

/* Check for early params. */
static int __init do_early_param(char *param, char *val,
				 const char *unused, void *arg)
{
	const struct obs_kernel_param *p;

	for (p = __setup_start; p < __setup_end; p++) {
		if ((p->early && parameq(param, p->str)) ||
		    (strcmp(param, "console") == 0 &&
		     strcmp(p->str, "earlycon") == 0)
		) {
			if (p->setup_func(val) != 0)
				pr_warn("Malformed early option '%s'\n", param);
		}
	}
	/* We accept everything at this stage. */
	return 0;
}

void __init parse_early_options(char *cmdline)
{
	parse_args("early options", cmdline, NULL, 0, 0, 0, NULL,
		   do_early_param);
}

/* Arch code calls this early on, or if not, just before other parsing. */
void __init parse_early_param(void)
{
	static int done __initdata;
	static char tmp_cmdline[COMMAND_LINE_SIZE] __initdata;

	if (done)
		return;

	/* All fall through to do_early_param. */
	strlcpy(tmp_cmdline, boot_command_line, COMMAND_LINE_SIZE);
	parse_early_options(tmp_cmdline);
	done = 1;
}

void __init __weak smp_setup_processor_id(void)
{
}

# if THREAD_SIZE >= PAGE_SIZE
void __init __weak thread_stack_cache_init(void)
{
}
#endif

/*
 * Set up kernel memory allocators
 */
static void __init mm_init(void)
{
	/*
	 * page_ext requires contiguous pages,
	 * bigger than MAX_ORDER unless SPARSEMEM.
	 */
	c5_boot_trace_event(0x300, 0, 0, 0);
	page_ext_init_flatmem();
	c5_boot_trace_event(0x301, 0, 0, 0);
	c5_boot_trace_event(0x302, 0, 0, 0);
	mem_init();
	c5_boot_trace_event(0x303, 0, 0, 0);
	/* page_owner must be initialized after buddy is ready */
	c5_boot_trace_event(0x304, 0, 0, 0);
	page_ext_init_flatmem_late();
	c5_boot_trace_event(0x305, 0, 0, 0);
	c5_boot_trace_event(0x306, 0, 0, 0);
	kmem_cache_init();
	c5_boot_trace_event(0x307, 0, 0, 0);
	c5_boot_trace_event(0x308, 0, 0, 0);
	percpu_init_late();
	c5_boot_trace_event(0x309, 0, 0, 0);
	c5_boot_trace_event(0x30a, 0, 0, 0);
	pgtable_init();
	c5_boot_trace_event(0x30b, 0, 0, 0);
	c5_boot_trace_event(0x30c, 0, 0, 0);
	vmalloc_init();
	c5_boot_trace_event(0x30d, 0, 0, 0);
	c5_boot_trace_event(0x30e, 0, 0, 0);
	ioremap_huge_init();
	c5_boot_trace_event(0x30f, 0, 0, 0);
	c5_boot_trace_event(0x310, 0, 0, 0);
	kaiser_init();
	c5_boot_trace_event(0x311, 0, 0, 0);
}

asmlinkage __visible void __init start_kernel(void)
{
	char *command_line;
	char *after_dashes;

	c5_start_trace(0x10);
	set_task_stack_end_magic(&init_task);
	c5_start_trace(0x11);
	smp_setup_processor_id();
	c5_start_trace(0x12);
	debug_objects_early_init();
	c5_start_trace(0x13);

	cgroup_init_early();
	c5_start_trace(0x14);

	local_irq_disable();
	early_boot_irqs_disabled = true;
	c5_start_trace(0x15);

	/*
	 * Interrupts are still disabled. Do necessary setups, then
	 * enable them.
	 */
	boot_cpu_init();
	c5_start_trace(0x16);
	page_address_init();
	c5_start_trace(0x17);
	pr_notice("%s", linux_banner);
	c5_start_trace(0x18);
	setup_arch(&command_line);
	c5_boot_trace_event(0x2f0, 0, 0, 0);
	c5_boot_trace_event(0x200, 0, 0, 0);
	mm_init_cpumask(&init_mm);
	c5_boot_trace_event(0x201, 0, 0, 0);
	c5_boot_trace_event(0x202, 0, 0, 0);
	setup_command_line(command_line);
	c5_boot_trace_event(0x203, 0, 0, 0);
	c5_boot_trace_event(0x204, 0, 0, 0);
	setup_nr_cpu_ids();
	c5_boot_trace_event(0x205, 0, 0, 0);
	c5_boot_trace_event(0x206, 0, 0, 0);
	setup_per_cpu_areas();
	c5_boot_trace_event(0x207, 0, 0, 0);
	c5_boot_trace_event(0x208, 0, 0, 0);
	smp_prepare_boot_cpu();	/* arch-specific boot-cpu hooks */
	c5_boot_trace_event(0x209, 0, 0, 0);
	c5_boot_trace_event(0x20a, 0, 0, 0);
	boot_cpu_hotplug_init();
	c5_boot_trace_event(0x20b, 0, 0, 0);

	c5_boot_trace_event(0x20c, 0, 0, 0);
	build_all_zonelists(NULL, NULL, false);
	c5_boot_trace_event(0x20d, 0, 0, 0);
	c5_boot_trace_event(0x20e, 0, 0, 0);
	page_alloc_init();
	c5_boot_trace_event(0x20f, 0, 0, 0);

	c5_boot_trace_event(0x210, 0, 0, 0);
	pr_notice("Kernel command line: %s\n", boot_command_line);
	c5_boot_trace_event(0x211, 0, 0, 0);
	/* parameters may set static keys */
	c5_boot_trace_event(0x212, 0, 0, 0);
	jump_label_init();
	c5_boot_trace_event(0x213, 0, 0, 0);
	c5_boot_trace_event(0x214, 0, 0, 0);
	parse_early_param();
	c5_boot_trace_event(0x215, 0, 0, 0);
	after_dashes = parse_args("Booting kernel",
				  static_command_line, __start___param,
				  __stop___param - __start___param,
				  -1, -1, NULL, &unknown_bootoption);
	if (!IS_ERR_OR_NULL(after_dashes))
		parse_args("Setting init args", after_dashes, NULL, 0, -1, -1,
			   NULL, set_init_arg);

	/*
	 * These use large bootmem allocations and must precede
	 * kmem_cache_init()
	 */
	c5_boot_trace_event(0x216, 0, 0, 0);
	setup_log_buf(0);
	c5_boot_trace_event(0x217, 0, 0, 0);
	c5_boot_trace_event(0x218, 0, 0, 0);
	pidhash_init();
	c5_boot_trace_event(0x219, 0, 0, 0);
	c5_boot_trace_event(0x21a, 0, 0, 0);
	vfs_caches_init_early();
	c5_boot_trace_event(0x21b, 0, 0, 0);
	c5_boot_trace_event(0x21c, 0, 0, 0);
	sort_main_extable();
	c5_boot_trace_event(0x21d, 0, 0, 0);
	c5_boot_trace_event(0x21e, 0, 0, 0);
	trap_init();
	c5_boot_trace_event(0x21f, 0, 0, 0);
	c5_boot_trace_event(0x220, 0, 0, 0);
	mm_init();
	c5_boot_trace_event(0x221, 0, 0, 0);

	/*
	 * Set up the scheduler prior starting any interrupts (such as the
	 * timer interrupt). Full topology setup happens at smp_init()
	 * time - but meanwhile we still have a functioning scheduler.
	 */
	c5_boot_trace_event(0x222, 0, 0, 0);
	sched_init();
	c5_boot_trace_event(0x223, 0, 0, 0);
	/*
	 * Disable preemption - early bootup scheduling is extremely
	 * fragile until we cpu_idle() for the first time.
	 */
	c5_boot_trace_event(0x224, 0, 0, 0);
	preempt_disable();
	c5_boot_trace_event(0x225, 0, 0, 0);
	if (WARN(!irqs_disabled(),
		 "Interrupts were enabled *very* early, fixing it\n"))
		local_irq_disable();
	c5_boot_trace_event(0x226, 0, 0, 0);
	idr_init_cache();
	c5_boot_trace_event(0x227, 0, 0, 0);

	/*
	 * Allow workqueue creation and work item queueing/cancelling
	 * early.  Work item execution depends on kthreads and starts after
	 * workqueue_init().
	 */
	c5_boot_trace_event(0x228, 0, 0, 0);
	workqueue_init_early();
	c5_boot_trace_event(0x229, 0, 0, 0);

	c5_boot_trace_event(0x22a, 0, 0, 0);
	rcu_init();
	c5_boot_trace_event(0x22b, 0, 0, 0);

	/* trace_printk() and trace points may be used after this */
	c5_boot_trace_event(0x22c, 0, 0, 0);
	trace_init();
	c5_boot_trace_event(0x22d, 0, 0, 0);

	c5_boot_trace_event(0x22e, 0, 0, 0);
	context_tracking_init();
	c5_boot_trace_event(0x22f, 0, 0, 0);
	c5_boot_trace_event(0x230, 0, 0, 0);
	radix_tree_init();
	c5_boot_trace_event(0x231, 0, 0, 0);
	/* init some links before init_ISA_irqs() */
	c5_boot_trace_event(0x232, 0, 0, 0);
	early_irq_init();
	c5_boot_trace_event(0x233, 0, 0, 0);
	c5_boot_trace_event(0x234, 0, 0, 0);
	init_IRQ();
	c5_boot_trace_event(0x235, 0, 0, 0);
	c5_boot_trace_event(0x236, 0, 0, 0);
	tick_init();
	c5_boot_trace_event(0x237, 0, 0, 0);
	c5_boot_trace_event(0x238, 0, 0, 0);
	rcu_init_nohz();
	c5_boot_trace_event(0x239, 0, 0, 0);
	c5_boot_trace_event(0x23a, 0, 0, 0);
	init_timers();
	c5_boot_trace_event(0x23b, 0, 0, 0);
	c5_boot_trace_event(0x23c, 0, 0, 0);
	hrtimers_init();
	c5_boot_trace_event(0x23d, 0, 0, 0);
	c5_boot_trace_event(0x23e, 0, 0, 0);
	softirq_init();
	c5_boot_trace_event(0x23f, 0, 0, 0);
	c5_boot_trace_event(0x240, 0, 0, 0);
	timekeeping_init();
	c5_boot_trace_event(0x241, 0, 0, 0);
	c5_boot_trace_event(0x242, 0, 0, 0);
	time_init();
	c5_boot_trace_event(0x243, 0, 0, 0);

	/*
	 * For best initial stack canary entropy, prepare it after:
	 * - setup_arch() for any UEFI RNG entropy and boot cmdline access
	 * - timekeeping_init() for ktime entropy used in random_init()
	 * - time_init() for making random_get_entropy() work on some platforms
	 * - random_init() to initialize the RNG from from early entropy sources
	 */
	c5_boot_trace_event(0x244, 0, 0, 0);
	random_init(command_line);
	c5_boot_trace_event(0x245, 0, 0, 0);
	c5_boot_trace_event(0x246, 0, 0, 0);
	boot_init_stack_canary();
	c5_boot_trace_event(0x247, 0, 0, 0);

	c5_boot_trace_event(0x248, 0, 0, 0);
	sched_clock_postinit();
	c5_boot_trace_event(0x249, 0, 0, 0);
	c5_boot_trace_event(0x24a, 0, 0, 0);
	printk_nmi_init();
	c5_boot_trace_event(0x24b, 0, 0, 0);
	c5_boot_trace_event(0x24c, 0, 0, 0);
	perf_event_init();
	c5_boot_trace_event(0x24d, 0, 0, 0);
	c5_boot_trace_event(0x24e, 0, 0, 0);
	profile_init();
	c5_boot_trace_event(0x24f, 0, 0, 0);
	c5_boot_trace_event(0x250, 0, 0, 0);
	call_function_init();
	c5_boot_trace_event(0x251, 0, 0, 0);
	c5_boot_trace_event(0x252, 0, 0, 0);
	WARN(!irqs_disabled(), "Interrupts were enabled early\n");
	c5_boot_trace_event(0x253, 0, 0, 0);
	early_boot_irqs_disabled = false;
	c5_boot_trace_event(0x254, 0, 0, 0);
	local_irq_enable();
	c5_boot_trace_event(0x255, 0, 0, 0);

	c5_boot_trace_event(0x256, 0, 0, 0);
	kmem_cache_init_late();
	c5_boot_trace_event(0x257, 0, 0, 0);

	/*
	 * HACK ALERT! This is early. We're enabling the console before
	 * we've done PCI setups etc, and console_init() must be aware of
	 * this. But we do want output early, in case something goes wrong.
	 */
	c5_boot_trace_event(0x258, 0, 0, 0);
	console_init();
	c5_boot_trace_event(0x259, 0, 0, 0);
	if (panic_later)
		panic("Too many boot %s vars at `%s'", panic_later,
		      panic_param);

	c5_boot_trace_event(0x25a, 0, 0, 0);
	lockdep_info();
	c5_boot_trace_event(0x25b, 0, 0, 0);

	/*
	 * Need to run this when irqs are enabled, because it wants
	 * to self-test [hard/soft]-irqs on/off lock inversion bugs
	 * too:
	 */
	c5_boot_trace_event(0x25c, 0, 0, 0);
	locking_selftest();
	c5_boot_trace_event(0x25d, 0, 0, 0);

#ifdef CONFIG_BLK_DEV_INITRD
	if (initrd_start && !initrd_below_start_ok &&
	    page_to_pfn(virt_to_page((void *)initrd_start)) < min_low_pfn) {
		pr_crit("initrd overwritten (0x%08lx < 0x%08lx) - disabling it.\n",
		    page_to_pfn(virt_to_page((void *)initrd_start)),
		    min_low_pfn);
		initrd_start = 0;
	}
#endif
	c5_boot_trace_event(0x25e, 0, 0, 0);
	page_ext_init();
	c5_boot_trace_event(0x25f, 0, 0, 0);
	c5_boot_trace_event(0x260, 0, 0, 0);
	debug_objects_mem_init();
	c5_boot_trace_event(0x261, 0, 0, 0);
	c5_boot_trace_event(0x262, 0, 0, 0);
	kmemleak_init();
	c5_boot_trace_event(0x263, 0, 0, 0);
	c5_boot_trace_event(0x264, 0, 0, 0);
	setup_per_cpu_pageset();
	c5_boot_trace_event(0x265, 0, 0, 0);
	c5_boot_trace_event(0x266, 0, 0, 0);
	numa_policy_init();
	c5_boot_trace_event(0x267, 0, 0, 0);
	if (late_time_init)
		late_time_init();
	c5_boot_trace_event(0x268, 0, 0, 0);
	sched_clock_init();
	c5_boot_trace_event(0x269, 0, 0, 0);
	c5_boot_trace_event(0x26a, 0, 0, 0);
	calibrate_delay();
	c5_boot_trace_event(0x26b, 0, 0, 0);
	c5_boot_trace_event(0x26c, 0, 0, 0);
	pidmap_init();
	c5_boot_trace_event(0x26d, 0, 0, 0);
	c5_boot_trace_event(0x26e, 0, 0, 0);
	anon_vma_init();
	c5_boot_trace_event(0x26f, 0, 0, 0);
	c5_boot_trace_event(0x270, 0, 0, 0);
	acpi_early_init();
	c5_boot_trace_event(0x271, 0, 0, 0);
#ifdef CONFIG_X86
	if (efi_enabled(EFI_RUNTIME_SERVICES))
		efi_enter_virtual_mode();
#endif
#ifdef CONFIG_X86_ESPFIX64
	/* Should be run before the first non-init thread is created */
	c5_boot_trace_event(0x272, 0, 0, 0);
	init_espfix_bsp();
	c5_boot_trace_event(0x273, 0, 0, 0);
#endif
	c5_boot_trace_event(0x274, 0, 0, 0);
	thread_stack_cache_init();
	c5_boot_trace_event(0x275, 0, 0, 0);
	c5_boot_trace_event(0x276, 0, 0, 0);
	cred_init();
	c5_boot_trace_event(0x277, 0, 0, 0);
	c5_boot_trace_event(0x278, 0, 0, 0);
	fork_init();
	c5_boot_trace_event(0x279, 0, 0, 0);
	c5_boot_trace_event(0x27a, 0, 0, 0);
	proc_caches_init();
	c5_boot_trace_event(0x27b, 0, 0, 0);
	c5_boot_trace_event(0x27c, 0, 0, 0);
	buffer_init();
	c5_boot_trace_event(0x27d, 0, 0, 0);
	c5_boot_trace_event(0x27e, 0, 0, 0);
	key_init();
	c5_boot_trace_event(0x27f, 0, 0, 0);
	c5_boot_trace_event(0x280, 0, 0, 0);
	security_init();
	c5_boot_trace_event(0x281, 0, 0, 0);
	c5_boot_trace_event(0x282, 0, 0, 0);
	dbg_late_init();
	c5_boot_trace_event(0x283, 0, 0, 0);
	c5_boot_trace_event(0x284, 0, 0, 0);
	vfs_caches_init();
	c5_boot_trace_event(0x285, 0, 0, 0);
	c5_boot_trace_event(0x286, 0, 0, 0);
	pagecache_init();
	c5_boot_trace_event(0x287, 0, 0, 0);
	c5_boot_trace_event(0x288, 0, 0, 0);
	signals_init();
	c5_boot_trace_event(0x289, 0, 0, 0);
	c5_boot_trace_event(0x28a, 0, 0, 0);
	proc_root_init();
	c5_boot_trace_event(0x28b, 0, 0, 0);
	c5_boot_trace_event(0x28c, 0, 0, 0);
	nsfs_init();
	c5_boot_trace_event(0x28d, 0, 0, 0);
	c5_boot_trace_event(0x28e, 0, 0, 0);
	cpuset_init();
	c5_boot_trace_event(0x28f, 0, 0, 0);
	c5_boot_trace_event(0x290, 0, 0, 0);
	cgroup_init();
	c5_boot_trace_event(0x291, 0, 0, 0);
	c5_boot_trace_event(0x292, 0, 0, 0);
	taskstats_init_early();
	c5_boot_trace_event(0x293, 0, 0, 0);
	c5_boot_trace_event(0x294, 0, 0, 0);
	delayacct_init();
	c5_boot_trace_event(0x295, 0, 0, 0);

	c5_boot_trace_event(0x296, 0, 0, 0);
	check_bugs();
	c5_boot_trace_event(0x297, 0, 0, 0);

	c5_boot_trace_event(0x298, 0, 0, 0);
	acpi_subsystem_init();
	c5_boot_trace_event(0x299, 0, 0, 0);
	c5_boot_trace_event(0x29a, 0, 0, 0);
	sfi_init_late();
	c5_boot_trace_event(0x29b, 0, 0, 0);

	if (efi_enabled(EFI_RUNTIME_SERVICES)) {
		efi_late_init();
		efi_free_boot_services();
	}

	c5_boot_trace_event(0x29c, 0, 0, 0);
	ftrace_init();
	c5_boot_trace_event(0x29d, 0, 0, 0);

	/* Do the rest non-__init'ed, we're now alive */
	c5_boot_trace_event(0x29e, 0, 0, 0);
	rest_init();
	c5_boot_trace_event(0x29f, 0, 0, 0);

	prevent_tail_call_optimization();
}

/* Call all constructor functions linked into the kernel. */
static void __init do_ctors(void)
{
#ifdef CONFIG_CONSTRUCTORS
	ctor_fn_t *fn = (ctor_fn_t *) __ctors_start;

	for (; fn < (ctor_fn_t *) __ctors_end; fn++)
		(*fn)();
#endif
}

bool initcall_debug;
core_param(initcall_debug, initcall_debug, bool, 0644);

#ifdef CONFIG_KALLSYMS
struct blacklist_entry {
	struct list_head next;
	char *buf;
};

static __initdata_or_module LIST_HEAD(blacklisted_initcalls);

static int __init initcall_blacklist(char *str)
{
	char *str_entry;
	struct blacklist_entry *entry;

	/* str argument is a comma-separated list of functions */
	do {
		str_entry = strsep(&str, ",");
		if (str_entry) {
			pr_debug("blacklisting initcall %s\n", str_entry);
			entry = alloc_bootmem(sizeof(*entry));
			entry->buf = alloc_bootmem(strlen(str_entry) + 1);
			strcpy(entry->buf, str_entry);
			list_add(&entry->next, &blacklisted_initcalls);
		}
	} while (str_entry);

	return 1;
}

static bool __init_or_module initcall_blacklisted(initcall_t fn)
{
	struct blacklist_entry *entry;
	char fn_name[KSYM_SYMBOL_LEN];
	unsigned long addr;

	if (list_empty(&blacklisted_initcalls))
		return false;

	addr = (unsigned long) dereference_function_descriptor(fn);
	sprint_symbol_no_offset(fn_name, addr);

	/*
	 * fn will be "function_name [module_name]" where [module_name] is not
	 * displayed for built-in init functions.  Strip off the [module_name].
	 */
	strreplace(fn_name, ' ', '\0');

	list_for_each_entry(entry, &blacklisted_initcalls, next) {
		if (!strcmp(fn_name, entry->buf)) {
			pr_debug("initcall %s blacklisted\n", fn_name);
			return true;
		}
	}

	return false;
}
#else
static int __init initcall_blacklist(char *str)
{
	pr_warn("initcall_blacklist requires CONFIG_KALLSYMS\n");
	return 0;
}

static bool __init_or_module initcall_blacklisted(initcall_t fn)
{
	return false;
}
#endif
__setup("initcall_blacklist=", initcall_blacklist);

static int __init_or_module do_one_initcall_debug(initcall_t fn)
{
	ktime_t calltime, delta, rettime;
	unsigned long long duration;
	int ret;

	printk(KERN_DEBUG "calling  %pF @ %i\n", fn, task_pid_nr(current));
	calltime = ktime_get();
	ret = fn();
	rettime = ktime_get();
	delta = ktime_sub(rettime, calltime);
	duration = (unsigned long long) ktime_to_ns(delta) >> 10;
	printk(KERN_DEBUG "initcall %pF returned %d after %lld usecs\n",
		 fn, ret, duration);

	return ret;
}

int __init_or_module do_one_initcall(initcall_t fn)
{
	int count = preempt_count();
	int ret;
	char msgbuf[64];

	if (initcall_blacklisted(fn))
		return -EPERM;

	c5_boot_trace_event(0x7000, (unsigned long)fn, 0, 0);
	if (c5_boot_trace_native_logs())
		pr_info("C5BOOT initcall start %pF\n", fn);
	if (initcall_debug)
		ret = do_one_initcall_debug(fn);
	else
		ret = fn();
	c5_boot_trace_event(0x7001, (unsigned long)fn, (u64)(s64)ret, 0);
	if (c5_boot_trace_native_logs())
		pr_info("C5BOOT initcall end %pF ret=%d\n", fn, ret);

	msgbuf[0] = 0;

	if (preempt_count() != count) {
		sprintf(msgbuf, "preemption imbalance ");
		preempt_count_set(count);
	}
	if (irqs_disabled()) {
		strlcat(msgbuf, "disabled interrupts ", sizeof(msgbuf));
		local_irq_enable();
	}
	WARN(msgbuf[0], "initcall %pF returned with %s\n", fn, msgbuf);

	add_latent_entropy();
	return ret;
}


extern initcall_t __initcall_start[];
extern initcall_t __initcall0_start[];
extern initcall_t __initcall1_start[];
extern initcall_t __initcall2_start[];
extern initcall_t __initcall3_start[];
extern initcall_t __initcall4_start[];
extern initcall_t __initcall5_start[];
extern initcall_t __initcall6_start[];
extern initcall_t __initcall7_start[];
extern initcall_t __initcall_end[];

static initcall_t *initcall_levels[] __initdata = {
	__initcall0_start,
	__initcall1_start,
	__initcall2_start,
	__initcall3_start,
	__initcall4_start,
	__initcall5_start,
	__initcall6_start,
	__initcall7_start,
	__initcall_end,
};

/* Keep these in sync with initcalls in include/linux/init.h */
static char *initcall_level_names[] __initdata = {
	"early",
	"core",
	"postcore",
	"arch",
	"subsys",
	"fs",
	"device",
	"late",
};

static void __init do_initcall_level(int level)
{
	initcall_t *fn;

	strcpy(initcall_command_line, saved_command_line);
	parse_args(initcall_level_names[level],
		   initcall_command_line, __start___param,
		   __stop___param - __start___param,
		   level, level,
		   NULL, &repair_env_string);

	for (fn = initcall_levels[level]; fn < initcall_levels[level+1]; fn++)
		do_one_initcall(*fn);
}

static void __init do_initcalls(void)
{
	int level;

	for (level = 0; level < ARRAY_SIZE(initcall_levels) - 1; level++)
		do_initcall_level(level);
}

/*
 * Ok, the machine is now initialized. None of the devices
 * have been touched yet, but the CPU subsystem is up and
 * running, and memory and process management works.
 *
 * Now we can finally start doing some real work..
 */
static void __init do_basic_setup(void)
{
	c5_boot_trace_event(0x500, 0, 0, 0);
	cpuset_init_smp();
	c5_boot_trace_event(0x501, 0, 0, 0);
	c5_boot_trace_event(0x502, 0, 0, 0);
	shmem_init();
	c5_boot_trace_event(0x503, 0, 0, 0);
	c5_boot_trace_event(0x504, 0, 0, 0);
	driver_init();
	c5_boot_trace_event(0x505, 0, 0, 0);
	c5_boot_trace_event(0x506, 0, 0, 0);
	init_irq_proc();
	c5_boot_trace_event(0x507, 0, 0, 0);
	c5_boot_trace_event(0x508, 0, 0, 0);
	do_ctors();
	c5_boot_trace_event(0x509, 0, 0, 0);
	c5_boot_trace_event(0x50a, 0, 0, 0);
	usermodehelper_enable();
	c5_boot_trace_event(0x50b, 0, 0, 0);
	c5_boot_trace_event(0x50c, 0, 0, 0);
	do_initcalls();
	c5_boot_trace_event(0x50d, 0, 0, 0);
}

static void __init do_pre_smp_initcalls(void)
{
	initcall_t *fn;

	for (fn = __initcall_start; fn < __initcall0_start; fn++)
		do_one_initcall(*fn);
}

/*
 * This function requests modules which should be loaded by default and is
 * called twice right after initrd is mounted and right before init is
 * exec'd.  If such modules are on either initrd or rootfs, they will be
 * loaded before control is passed to userland.
 */
void __init load_default_modules(void)
{
	load_default_elevator_module();
}

static int run_init_process(const char *init_filename)
{
	argv_init[0] = init_filename;
	return do_execve(getname_kernel(init_filename),
		(const char __user *const __user *)argv_init,
		(const char __user *const __user *)envp_init);
}

static int try_to_run_init_process(const char *init_filename)
{
	int ret;

	ret = run_init_process(init_filename);

	if (ret && ret != -ENOENT) {
		pr_err("Starting init: %s exists but couldn't execute it (error %d)\n",
		       init_filename, ret);
	}

	return ret;
}

static noinline void __init kernel_init_freeable(void);

#if defined(CONFIG_DEBUG_RODATA) || defined(CONFIG_DEBUG_SET_MODULE_RONX)
bool rodata_enabled __ro_after_init = true;
static int __init set_debug_rodata(char *str)
{
	if (strtobool(str, &rodata_enabled))
		pr_warn("Invalid option string for rodata: '%s'\n", str);
	return 1;
}
__setup("rodata=", set_debug_rodata);
#endif

#ifdef CONFIG_DEBUG_RODATA
static void mark_readonly(void)
{
	if (rodata_enabled)
		mark_rodata_ro();
	else
		pr_info("Kernel memory protection disabled.\n");
}
#else
static inline void mark_readonly(void)
{
	pr_warn("This architecture does not have kernel memory protection.\n");
}
#endif

static int __ref kernel_init(void *unused)
{
	int ret;

	kernel_init_freeable();
	/* need to finish all async __init code before freeing the memory */
	async_synchronize_full();
	c5_boot_trace_stop();
	free_initmem();
	mark_readonly();
	system_state = SYSTEM_RUNNING;
	numa_default_policy();

	rcu_end_inkernel_boot();

	if (ramdisk_execute_command) {
		ret = run_init_process(ramdisk_execute_command);
		if (!ret)
			return 0;
		pr_err("Failed to execute %s (error %d)\n",
		       ramdisk_execute_command, ret);
	}

	/*
	 * We try each of these until one succeeds.
	 *
	 * The Bourne shell can be used instead of init if we are
	 * trying to recover a really broken machine.
	 */
	if (execute_command) {
		ret = run_init_process(execute_command);
		if (!ret)
			return 0;
		panic("Requested init %s failed (error %d).",
		      execute_command, ret);
	}
	if (!try_to_run_init_process("/sbin/init") ||
	    !try_to_run_init_process("/etc/init") ||
	    !try_to_run_init_process("/bin/init") ||
	    !try_to_run_init_process("/bin/sh"))
		return 0;

	panic("No working init found.  Try passing init= option to kernel. "
	      "See Linux Documentation/init.txt for guidance.");
}

static noinline void __init kernel_init_freeable(void)
{
	/*
	 * Wait until kthreadd is all set-up.
	 */
	c5_boot_trace_event(0x600, 0, 0, 0);
	wait_for_completion(&kthreadd_done);
	c5_boot_trace_event(0x601, 0, 0, 0);

	/* Now the scheduler is fully set up and can do blocking allocations */
	gfp_allowed_mask = __GFP_BITS_MASK;

	/*
	 * init can allocate pages on any node
	 */
	c5_boot_trace_event(0x602, 0, 0, 0);
	set_mems_allowed(node_states[N_MEMORY]);
	c5_boot_trace_event(0x603, 0, 0, 0);
	/*
	 * init can run on any cpu.
	 */
	c5_boot_trace_event(0x604, 0, 0, 0);
	set_cpus_allowed_ptr(current, cpu_all_mask);
	c5_boot_trace_event(0x605, 0, 0, 0);

	cad_pid = get_pid(task_pid(current));

	c5_boot_trace_event(0x606, 0, 0, 0);
	smp_prepare_cpus(setup_max_cpus);
	c5_boot_trace_event(0x607, 0, 0, 0);

	c5_boot_trace_event(0x608, 0, 0, 0);
	workqueue_init();
	c5_boot_trace_event(0x609, 0, 0, 0);

	c5_boot_trace_event(0x60a, 0, 0, 0);
	do_pre_smp_initcalls();
	c5_boot_trace_event(0x60b, 0, 0, 0);
	c5_boot_trace_event(0x60c, 0, 0, 0);
	lockup_detector_init();
	c5_boot_trace_event(0x60d, 0, 0, 0);

	c5_boot_trace_event(0x60e, 0, 0, 0);
	smp_init();
	c5_boot_trace_event(0x60f, 0, 0, 0);
	c5_boot_trace_event(0x610, 0, 0, 0);
	sched_init_smp();
	c5_boot_trace_event(0x611, 0, 0, 0);

	c5_boot_trace_event(0x612, 0, 0, 0);
	page_alloc_init_late();
	c5_boot_trace_event(0x613, 0, 0, 0);

	c5_boot_trace_event(0x614, 0, 0, 0);
	do_basic_setup();
	c5_boot_trace_event(0x615, 0, 0, 0);

	/* Open the /dev/console on the rootfs, this should never fail */
	if (sys_open((const char __user *) "/dev/console", O_RDWR, 0) < 0)
		pr_err("Warning: unable to open an initial console.\n");

	(void) sys_dup(0);
	(void) sys_dup(0);
	/*
	 * check if there is an early userspace init.  If yes, let it do all
	 * the work
	 */

	if (!ramdisk_execute_command)
		ramdisk_execute_command = "/init";

	if (sys_access((const char __user *) ramdisk_execute_command, 0) != 0) {
		ramdisk_execute_command = NULL;
		prepare_namespace();
	}

	/*
	 * Ok, we have completed the initial bootup, and
	 * we're essentially up and running. Get rid of the
	 * initmem segments and start the user-mode stuff..
	 *
	 * rootfs is available now, try loading the public keys
	 * and default modules
	 */

	c5_boot_trace_event(0x616, 0, 0, 0);
	integrity_load_keys();
	c5_boot_trace_event(0x617, 0, 0, 0);
	c5_boot_trace_event(0x618, 0, 0, 0);
	load_default_modules();
	c5_boot_trace_event(0x619, 0, 0, 0);
}
