/* SPDX-License-Identifier: GPL-2.0-only */
/* Diagnostic only. Two schedulable phases expose progress before native
 * policy registration, without a HOST trigger or read-side mutation.
 */
#ifdef CONFIG_C5_RECOVERY_ONLY
#include <linux/workqueue.h>
#include <linux/atomic.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/c5-cpu-pcnoc.h>

/* 0=new,1=waiting,2=armed,3=registering,4=ready,5=native failure,6=queue failure */
static atomic_t c5_dvfs_registration_state = ATOMIC_INIT(0);
static unsigned long c5_dvfs_cached_cpu0_khz;
static unsigned long c5_dvfs_cached_cpu4_khz;
static struct proc_dir_entry *c5_dvfs_state_entry;

static int c5_dvfs_state_show(struct seq_file *seq, void *unused)
{
	(void)unused;
	seq_printf(seq, "v=65 state=%d cpu0_khz=%lu cpu4_khz=%lu pcnoc=%d\n",
		   atomic_read(&c5_dvfs_registration_state),
		   READ_ONCE(c5_dvfs_cached_cpu0_khz),
		   READ_ONCE(c5_dvfs_cached_cpu4_khz), c5_cpu_pcnoc_status());
	return 0;
}

static int c5_dvfs_state_open(struct inode *inode, struct file *file)
{
	(void)inode;
	return single_open(file, c5_dvfs_state_show, NULL);
}

static const struct file_operations c5_dvfs_state_fops = {
	.owner = THIS_MODULE,
	.open = c5_dvfs_state_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static void c5_dvfs_register_work(struct work_struct *work)
{
	int ret;
	(void)work;
	if (atomic_cmpxchg(&c5_dvfs_registration_state, 2, 3) != 2)
		return;
	if (c5_cpu_pcnoc_status() != 1) {
		pr_err("C5DVFS v65 PCNOC unavailable;no native registration\n");
		unregister_pm_notifier(&msm_cpufreq_pm_notifier);
		atomic_set(&c5_dvfs_registration_state, 6);
		return;
	}
	pr_info("C5DVFS v65 native policy registration begin\n");
	ret = cpufreq_register_driver(&msm_cpufreq_driver);
	pr_info("C5DVFS v65 native policy registration returned %d\n", ret);
	if (ret)
		unregister_pm_notifier(&msm_cpufreq_pm_notifier);
	atomic_set(&c5_dvfs_registration_state, ret ? 5 : 4);
}

static DECLARE_DELAYED_WORK(c5_dvfs_registration_work,
			    c5_dvfs_register_work);

static void c5_dvfs_arm_work(struct work_struct *work)
{
	(void)work;
	if (atomic_cmpxchg(&c5_dvfs_registration_state, 1, 2) != 1)
		return;
	/* Native CPU clock ops have no get_rate callback: these are cached
	 * software values, not direct PLL reads or proof of physical frequency.
	 */
	WRITE_ONCE(c5_dvfs_cached_cpu0_khz, clk_get_rate(cpu_clk[0]) / 1000);
	WRITE_ONCE(c5_dvfs_cached_cpu4_khz, clk_get_rate(cpu_clk[4]) / 1000);
	pr_info("C5DVFS v65 armed;native registration in 30000ms;cached kHz cpu0=%lu cpu4=%lu\n",
		READ_ONCE(c5_dvfs_cached_cpu0_khz),
		READ_ONCE(c5_dvfs_cached_cpu4_khz));
	if (!schedule_delayed_work(&c5_dvfs_registration_work,
				   msecs_to_jiffies(30000))) {
		atomic_set(&c5_dvfs_registration_state, 6);
		unregister_pm_notifier(&msm_cpufreq_pm_notifier);
		pr_err("C5DVFS v65 second queue failed;no native registration\n");
	}
}

static DECLARE_DELAYED_WORK(c5_dvfs_arming_work, c5_dvfs_arm_work);

static int c5_register_cpufreq_driver(void)
{
	if (atomic_cmpxchg(&c5_dvfs_registration_state, 0, 1) != 0)
		return -EBUSY;
	c5_dvfs_state_entry = proc_create("c5_dvfs_state", 0400, NULL,
					&c5_dvfs_state_fops);
	if (!c5_dvfs_state_entry) {
		atomic_set(&c5_dvfs_registration_state, 6);
		return -ENOMEM;
	}
	if (!schedule_delayed_work(&c5_dvfs_arming_work,
				   msecs_to_jiffies(120000))) {
		proc_remove(c5_dvfs_state_entry);
		c5_dvfs_state_entry = NULL;
		atomic_set(&c5_dvfs_registration_state, 6);
		return -EBUSY;
	}
	pr_info("C5DVFS v65 arm scheduled after 120000ms;not ready yet\n");
	return 0;
}
#else
static int c5_register_cpufreq_driver(void)
{
	return cpufreq_register_driver(&msm_cpufreq_driver);
}
#endif
