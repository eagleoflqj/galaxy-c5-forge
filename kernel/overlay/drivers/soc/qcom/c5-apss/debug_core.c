/* Copyright (c) 2014, 2016, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/debugfs.h>
#include <linux/ctype.h>
#include <linux/cpu.h>
#include <linux/c5-apss-core.h>
#include <linux/msm-core-interface.h>
#include <linux/mutex.h>

#define MAX_PSTATES C5_APSS_DEBUG_MAX_PSTATES
#define C5_APSS_DEBUG_INPUT_MAX 128
#define NUM_OF_PENTRY 3 /* number of variables for ptable node */
#define NUM_OF_EENTRY 2 /* number of variables for enable node */

enum arg_offset {
	CPU_OFFSET,
	FREQ_OFFSET,
	POWER_OFFSET,
};

struct core_debug {
	int cpu;
	struct cpu_pstate_pwr *head;
	int enabled;
	int len;
};

static DEFINE_PER_CPU(struct core_debug, c_dgfs);
static DEFINE_MUTEX(c5_apss_debug_lock);
static struct debugfs_blob_wrapper help_msg = {
	.data =
"MSM CORE Debug-FS Support\n"
"\n"
"Hierarchy schema\n"
"/sys/kernel/debug/msm_core\n"
"  /help        - Static help text\n"
"  /ptable      - write to p-state table\n"
"  /enable      - enable the written p-state table\n"
"  /ptable_dump - Dump the debug ptable\n"
"\n"
"Usage\n"
" Input test frequency and power information in ptable:\n"
" echo \"0 300000 120\" > ptable\n"
" format: <cpu> <frequency in khz> <power>\n"
"\n"
" Enable the ptable for the cpu:\n"
" echo \"0 1\" > enable\n"
" format: <cpu> <1 to enable, 0 to disable>\n"
" Note: Writing 0 to disable will reset/clear the ptable\n"
"\n"
" Dump the entire ptable:\n"
" cat ptable\n"
" -----  CPU0 - Enabled ---------\n"
"     Freq       Power\n"
"     700000       120\n"
"-----  CPU0 - Live numbers -----\n"
"   Freq       Power\n"
"   300000      218\n"
" -----  CPU1 - Written ---------\n"
"     Freq       Power\n"
"     700000       120\n"
" Ptable dump will dump the status of the table as well\n"
" It shows:\n"
" Enabled -> for a cpu that debug ptable enabled\n"
" Written -> for a cpu that has debug ptable values written\n"
"            but not enabled\n"
"\n",

};

static int add_to_ptable(unsigned int *arg)
{
	struct core_debug *node;
	unsigned int cpu = arg[CPU_OFFSET];
	u32 freq = arg[FREQ_OFFSET], power = arg[POWER_OFFSET];
	int i;

	if (cpu >= nr_cpu_ids || !cpu_possible(cpu))
		return -EINVAL;
	if (!freq || !power)
		return -EINVAL;
	node = &per_cpu(c_dgfs, cpu);
	if (!node->head) {
		node->head = kcalloc(MAX_PSTATES + 1, sizeof(*node->head), GFP_KERNEL);
		if (!node->head)
			return -ENOMEM;
	}
	for (i = 0; i < node->len; i++) {
		if (node->head[i].freq == freq) {
			node->head[i].power = power;
			return node->enabled ? c5_apss_debug_override(cpu, node->head, node->len, true) : 0;
		}
	}
	if (node->len >= MAX_PSTATES)
		return -ENOSPC;
	for (i = node->len; i > 0 && node->head[i - 1].freq > freq; i--)
		node->head[i] = node->head[i - 1];
	node->head[i].freq = freq;
	node->head[i].power = power;
	node->len++;
	memset(&node->head[node->len], 0, sizeof(*node->head));
	return node->enabled ? c5_apss_debug_override(cpu, node->head, node->len, true) : 0;
}

static int split_ptable_args(char *line, unsigned int *arg, uint32_t n)
{
	char *token;
	unsigned int i = 0;
	int ret;

	while ((token = strsep(&line, " \t\r\n")) != NULL) {
		if (!*token)
			continue;
		if (i == n)
			return -EINVAL;
		ret = kstrtouint(token, 10, &arg[i]);
		if (ret)
			return ret;
		i++;
	}
	if (i != n)
		return -EINVAL;
	return 0;
}

static ssize_t msm_core_ptable_write(struct file *file,
		const char __user *ubuf, size_t len, loff_t *offp)
{
	char *kbuf;
	int ret;
	unsigned int arg[NUM_OF_PENTRY];

	if (!len)
		return 0;
	if (len > C5_APSS_DEBUG_INPUT_MAX)
		return -E2BIG;
	kbuf = kzalloc(len + 1, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;
	if (copy_from_user(kbuf, ubuf, len)) {
		ret = -EFAULT;
		goto done;
	}
	if (memchr(kbuf, '\0', len)) {
		ret = -EINVAL;
		goto done;
	}
	ret = split_ptable_args(kbuf, arg, NUM_OF_PENTRY);
	if (ret)
		goto done;
	mutex_lock(&c5_apss_debug_lock);
	ret = add_to_ptable(arg);
	mutex_unlock(&c5_apss_debug_lock);
	if (!ret)
		ret = len;
 done:
	kfree(kbuf);
	return ret;
}

static void print_table(struct seq_file *m, struct cpu_pstate_pwr *c_n,
		int len)
{
	int i;

	seq_puts(m, "   Freq       Power\n");
	for (i = 0; i < len; i++)
		seq_printf(m, "  %d       %u\n", c_n[i].freq,
				c_n[i].power);

}

static int msm_core_ptable_read(struct seq_file *m, void *data)
{
	struct cpu_pstate_pwr *table;
	struct cpu_pwr_stats snapshot;
	struct core_debug *node;
	int cpu, ret = 0;

	table = kcalloc(MAX_NUM_FREQ + 1, sizeof(*table), GFP_KERNEL);
	if (!table)
		return -ENOMEM;
	mutex_lock(&c5_apss_debug_lock);
	for_each_possible_cpu(cpu) {
		node = &per_cpu(c_dgfs, cpu);
		if (node->head) {
			seq_printf(m, "-----  CPU%d - %s - Debug -------\n", cpu,
				   node->enabled ? "Enabled" : "Written");
			print_table(m, node->head, node->len);
		}
		ret = c5_apss_debug_snapshot(cpu, &snapshot, table, MAX_NUM_FREQ);
		if (ret)
			break;
		if (snapshot.ptable) {
			seq_printf(m, "--- CPU%d - Live numbers at %ldC---\n", cpu, snapshot.temp);
			print_table(m, table, snapshot.len);
		}
	}
	mutex_unlock(&c5_apss_debug_lock);
	kfree(table);
	return ret;
}

static ssize_t msm_core_enable_write(struct file *file,
		const char __user *ubuf, size_t len, loff_t *offp)
{
	struct core_debug *node;
	char *kbuf;
	int ret;
	unsigned int arg[NUM_OF_EENTRY], cpu;

	if (!len)
		return 0;
	if (len > C5_APSS_DEBUG_INPUT_MAX)
		return -E2BIG;
	kbuf = kzalloc(len + 1, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;
	if (copy_from_user(kbuf, ubuf, len)) {
		ret = -EFAULT;
		goto done;
	}
	if (memchr(kbuf, '\0', len)) {
		ret = -EINVAL;
		goto done;
	}
	ret = split_ptable_args(kbuf, arg, NUM_OF_EENTRY);
	if (ret)
		goto done;
	cpu = arg[CPU_OFFSET];
	if (cpu >= nr_cpu_ids || !cpu_possible(cpu) || arg[FREQ_OFFSET] > 1) {
		ret = -EINVAL;
		goto done;
	}
	mutex_lock(&c5_apss_debug_lock);
	node = &per_cpu(c_dgfs, cpu);
	if (arg[FREQ_OFFSET]) {
		if (!node->head || !node->len) {
			ret = -ENODATA;
			goto unlock;
		}
		ret = c5_apss_debug_override(cpu, node->head, node->len, true);
		if (ret)
			goto unlock;
		node->enabled = 1;
	} else {
		ret = c5_apss_debug_override(cpu, NULL, 0, false);
		if (ret)
			goto unlock;
		node->enabled = 0;
		node->len = 0;
		if (node->head)
			memset(node->head, 0, sizeof(*node->head) * (MAX_PSTATES + 1));
	}
	ret = len;
 unlock:
	mutex_unlock(&c5_apss_debug_lock);
 done:
	kfree(kbuf);
	return ret;
}

static const struct file_operations msm_core_enable_ops = {
	.write = msm_core_enable_write,
};

static int msm_core_dump_open(struct inode *inode, struct file *file)
{
	return single_open(file, msm_core_ptable_read, inode->i_private);
}

static const struct file_operations msm_core_ptable_ops = {
	.open = msm_core_dump_open,
	.read = seq_read,
	.write = msm_core_ptable_write,
	.llseek = seq_lseek,
	.release = single_release,
};

int msm_core_debug_init(void)
{
	struct dentry *dir, *file;
	int cpu, ret;

	if (!get_cpu_pwr_stats())
		return -ENODEV;
	dir = debugfs_create_dir("msm_core", NULL);
	if (IS_ERR_OR_NULL(dir))
		return dir ? PTR_ERR(dir) : -ENOMEM;
	file = debugfs_create_file("enable", S_IRUSR | S_IRGRP | S_IWUSR | S_IWGRP,
		dir, NULL, &msm_core_enable_ops);
	if (IS_ERR_OR_NULL(file))
		goto failed;
	file = debugfs_create_file("ptable", S_IRUSR | S_IRGRP | S_IWUSR | S_IWGRP,
		dir, NULL, &msm_core_ptable_ops);
	if (IS_ERR_OR_NULL(file))
		goto failed;
	help_msg.size = strlen(help_msg.data);
	file = debugfs_create_blob("help", S_IRUGO, dir, &help_msg);
	if (IS_ERR_OR_NULL(file))
		goto failed;
	for_each_possible_cpu(cpu)
		per_cpu(c_dgfs, cpu).cpu = cpu;
	return 0;
 failed:
	ret = file ? PTR_ERR(file) : -ENOMEM;
	debugfs_remove_recursive(dir);
	return ret;
}
late_initcall(msm_core_debug_init);
