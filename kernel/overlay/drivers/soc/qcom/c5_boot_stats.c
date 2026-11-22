/* Copyright (c) 2013-2014,2019 The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/smp.h>
#include <soc/qcom/boot_stats.h>

static DEFINE_MUTEX(boot_stats_lock);
static void __iomem *mpm_counter_base;
static phys_addr_t mpm_counter_pa;
static u32 mpm_counter_freq;
struct boot_stats __iomem *boot_stats;

u32 bootloader_start;
u32 bootloader_end;
u32 bootloader_display;
u32 bootloader_load_kernel;

/* Preserve the initialization outcome after early printk buffers wrap. */
static u32 c5_boot_kpi[6];
static unsigned int c5_init_attempts;
static unsigned int c5_init_ready;
static unsigned int c5_init_stage;
static int c5_init_result = -EAGAIN;

static int c5_cached_reject(const char *val, const struct kernel_param *kp)
{
	/* parse_one() requires a callable setter even for read-only parameters. */
	return -EPERM;
}

static int c5_cached_uint_get(char *buf, const struct kernel_param *kp)
{
	/* Match the completed cache publication in boot_stats_init(). */
	(void)smp_load_acquire(&c5_init_ready);
	return param_get_uint(buf, kp);
}

static int c5_cached_int_get(char *buf, const struct kernel_param *kp)
{
	(void)smp_load_acquire(&c5_init_ready);
	return param_get_int(buf, kp);
}

static const struct kernel_param_ops c5_cached_uint_ops = {
	.set = c5_cached_reject,
	.get = c5_cached_uint_get,
};

static const struct kernel_param_ops c5_cached_int_ops = {
	.set = c5_cached_reject,
	.get = c5_cached_int_get,
};

/* Reject every sysfs/command-line write. Reads only inspect cached RAM. */
module_param_cb(init_attempts, &c5_cached_uint_ops, &c5_init_attempts, 0400);
module_param_cb(init_result, &c5_cached_int_ops, &c5_init_result, 0400);
module_param_cb(ready, &c5_cached_uint_ops, &c5_init_ready, 0400);
module_param_cb(init_stage, &c5_cached_uint_ops, &c5_init_stage, 0400);
module_param_cb(kpi_start, &c5_cached_uint_ops, &c5_boot_kpi[0], 0400);
module_param_cb(kpi_end, &c5_cached_uint_ops, &c5_boot_kpi[1], 0400);
module_param_cb(kpi_display, &c5_cached_uint_ops, &c5_boot_kpi[2], 0400);
module_param_cb(kpi_load_kernel, &c5_cached_uint_ops, &c5_boot_kpi[3], 0400);
module_param_cb(kpi_mpm_timestamp, &c5_cached_uint_ops, &c5_boot_kpi[4], 0400);
module_param_cb(kpi_mpm_frequency, &c5_cached_uint_ops, &c5_boot_kpi[5], 0400);

static int map_boot_resources(void __iomem **counter)
{
	struct device_node *np;
	struct resource resource;
	struct boot_stats __iomem *snapshot;
	void __iomem *mapped_counter;
	u32 freq;
	int ret;

	np = of_find_compatible_node(NULL, NULL, "qcom,msm-imem-boot_stats");
	if (!np)
		return -ENODEV;
	c5_init_stage = 2;
	ret = of_address_to_resource(np, 0, &resource);
	if (ret || resource_type(&resource) != IORESOURCE_MEM ||
	    resource.end < resource.start ||
	    (resource.start & (sizeof(u32) - 1)) ||
	    resource_size(&resource) < sizeof(*snapshot)) {
		of_node_put(np);
		return -EINVAL;
	}
	c5_init_stage = 3;
	snapshot = of_iomap(np, 0);
	of_node_put(np);
	if (!snapshot)
		return -ENOMEM;
	c5_init_stage = 4;

	np = of_find_compatible_node(NULL, NULL, "qcom,mpm2-sleep-counter");
	if (!np) {
		ret = -ENODEV;
		goto unmap_snapshot;
	}
	c5_init_stage = 5;
	ret = of_property_read_u32(np, "clock-frequency", &freq);
	if (ret || freq != TIMER_KHZ) {
		ret = -EINVAL;
		goto put_counter_node;
	}
	c5_init_stage = 6;
	ret = of_address_to_resource(np, 0, &resource);
	if (ret || resource_type(&resource) != IORESOURCE_MEM ||
	    resource.end < resource.start ||
	    (resource.start & (sizeof(u32) - 1)) ||
	    resource_size(&resource) < sizeof(u32)) {
		ret = -EINVAL;
		goto put_counter_node;
	}
	c5_init_stage = 7;
	mapped_counter = of_iomap(np, 0);
	if (!mapped_counter) {
		ret = -ENOMEM;
		goto put_counter_node;
	}
	c5_init_stage = 8;
	of_node_put(np);

	boot_stats = snapshot;
	mpm_counter_pa = resource.start;
	mpm_counter_freq = freq;
	*counter = mapped_counter;
	return 0;

put_counter_node:
	of_node_put(np);
unmap_snapshot:
	iounmap(snapshot);
	return ret;
}

static void print_boot_stats(void __iomem *counter)
{
	/* Preserve the old Samsung snapshot and subsequent KPI read order. */
	bootloader_start = readl_relaxed(&boot_stats->bootloader_start);
	bootloader_end = readl_relaxed(&boot_stats->bootloader_end);
	bootloader_display = readl_relaxed(&boot_stats->bootloader_display);
	bootloader_load_kernel = readl_relaxed(&boot_stats->bootloader_load_kernel);

	pr_info("KPI: Bootloader start count = %u\n",
		c5_boot_kpi[0] = readl_relaxed(&boot_stats->bootloader_start));
	pr_info("KPI: Bootloader end count = %u\n",
		c5_boot_kpi[1] = readl_relaxed(&boot_stats->bootloader_end));
	pr_info("KPI: Bootloader display count = %u\n",
		c5_boot_kpi[2] = readl_relaxed(&boot_stats->bootloader_display));
	pr_info("KPI: Bootloader load kernel count = %u\n",
		c5_boot_kpi[3] = readl_relaxed(&boot_stats->bootloader_load_kernel));
	pr_info("KPI: Kernel MPM timestamp = %u\n",
		c5_boot_kpi[4] = readl_relaxed(counter));
	pr_info("KPI: Kernel MPM Clock frequency = %u\n",
		c5_boot_kpi[5] = mpm_counter_freq);
}

static void release_boot_snapshot(void)
{
	if (boot_stats) {
		iounmap(boot_stats);
		boot_stats = NULL;
	}
}

int boot_stats_init(void)
{
	void __iomem *counter = NULL;
	int ret = 0;

	mutex_lock(&boot_stats_lock);
	if (smp_load_acquire(&mpm_counter_base))
		goto out;
	c5_init_attempts++;
	c5_init_stage = 1;
	ret = map_boot_resources(&counter);
	if (ret) {
		c5_init_result = ret;
		goto out;
	}
	print_boot_stats(counter);
	c5_init_stage = 9;
	release_boot_snapshot();
	c5_init_stage = 10;
	/* The old get_boot_stat_time() requires this mapping for kernel life. */
	smp_store_release(&mpm_counter_base, counter);
	c5_init_result = 0;
	c5_init_stage = 11;
	smp_store_release(&c5_init_ready, 1);
out:
	mutex_unlock(&boot_stats_lock);
	return ret;
}

int boot_stats_exit(void)
{
	mutex_lock(&boot_stats_lock);
	release_boot_snapshot();
	/* Retain the MPM mapping used by the old fast, non-sleeping getter. */
	mutex_unlock(&boot_stats_lock);
	return 0;
}

unsigned int get_boot_stat_time(void)
{
	void __iomem *counter = smp_load_acquire(&mpm_counter_base);

	return counter ? readl_relaxed(counter) : 0;
}

phys_addr_t msm_timer_get_pa(void)
{
	return smp_load_acquire(&mpm_counter_base) ? mpm_counter_pa : 0;
}

unsigned long long int msm_timer_get_sclk_ticks(void)
{
	void __iomem *counter = smp_load_acquire(&mpm_counter_base);
	u32 before, after;
	unsigned int retry, sample;

	if (!counter)
		return 0;
	for (retry = 0; retry < 3; retry++) {
		before = __raw_readl_no_log(counter);
		for (sample = 0; sample < 10; sample++) {
			udelay(1);
			after = __raw_readl_no_log(counter);
			if (before == after)
				break;
			before = after;
		}
		if (sample == 10)
			return 0;
		if (after)
			return after;
		udelay((USEC_PER_SEC / TIMER_KHZ) / 2);
	}
	return 0;
}
