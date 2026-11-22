/* SPDX-License-Identifier: GPL-2.0-only */
/* Recovery integration: register synchronously at the native probe boundary.
 * Keep the C5 PCNOC dependency and observational proc file; do not delay
 * frequency mitigation for diagnostic log collection.
 */
#ifdef CONFIG_C5_RECOVERY_ONLY
#include <linux/atomic.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/c5-cpu-pcnoc.h>

static atomic_t c5_dvfs_registration_state = ATOMIC_INIT(0);
static unsigned long c5_dvfs_cached_cpu0_khz;
static unsigned long c5_dvfs_cached_cpu4_khz;
static struct proc_dir_entry *c5_dvfs_state_entry;

static int c5_dvfs_state_show(struct seq_file *seq, void *unused)
{
	(void)unused;
	seq_printf(seq, "v=67 state=%d cpu0_khz=%lu cpu4_khz=%lu pcnoc=%d\n",
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

static int c5_register_cpufreq_driver(void)
{
	int ret;
	/* A missing dependency must defer the native probe, not silently skip
	 * registration. The caller owns PM-notifier cleanup on every error.
	 */
	if (c5_cpu_pcnoc_status() != 1)
		return -EPROBE_DEFER;
	if (atomic_cmpxchg(&c5_dvfs_registration_state, 0, 3) != 0)
		return -EBUSY;
	c5_dvfs_state_entry = proc_create("c5_dvfs_state", 0400, NULL,
					&c5_dvfs_state_fops);
	if (!c5_dvfs_state_entry) {
		atomic_set(&c5_dvfs_registration_state, 0);
		return -ENOMEM;
	}
	/* Before-registration software caches, not live physical CPU rates. */
	WRITE_ONCE(c5_dvfs_cached_cpu0_khz, clk_get_rate(cpu_clk[0]) / 1000);
	WRITE_ONCE(c5_dvfs_cached_cpu4_khz, clk_get_rate(cpu_clk[4]) / 1000);
	pr_info("C5DVFS v67 synchronous native policy registration begin\n");
	ret = cpufreq_register_driver(&msm_cpufreq_driver);
	pr_info("C5DVFS v67 synchronous native policy registration returned %d\n", ret);
	atomic_set(&c5_dvfs_registration_state, ret ? 5 : 4);
	if (ret) {
		proc_remove(c5_dvfs_state_entry);
		c5_dvfs_state_entry = NULL;
		atomic_set(&c5_dvfs_registration_state, 0);
	}
	return ret;
}
#else
static int c5_register_cpufreq_driver(void)
{
	return cpufreq_register_driver(&msm_cpufreq_driver);
}
#endif
