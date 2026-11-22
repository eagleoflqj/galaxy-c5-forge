// SPDX-License-Identifier: GPL-2.0-only
/* Restore the opt-in MSM8952 CPU PCNOC floor from the accepted C5 3.10
 * clock driver. No frequency/voltage table or native protection is changed.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/msm-bus.h>
#include <linux/c5-cpu-pcnoc.h>
#include <dt-bindings/msm/msm-bus-ids.h>

static struct msm_bus_paths c5_cpu_bw_table[] = {
	{
		.vectors = (struct msm_bus_vectors[]) {
			{ .src = MSM_BUS_PNOC_INT_2,
			  .dst = MSM_BUS_SLAVE_MESSAGE_RAM, .ib = 0, .ab = 0 },
		},
		.num_paths = 1,
	},
	{
		.vectors = (struct msm_bus_vectors[]) {
			{ .src = MSM_BUS_PNOC_INT_2,
			  .dst = MSM_BUS_SLAVE_MESSAGE_RAM,
			  .ib = 50 * 4 * 1000000UL, .ab = 0 },
		},
		.num_paths = 1,
	},
};

static struct msm_bus_scale_pdata c5_cpu_bus_data = {
	.usecase = c5_cpu_bw_table,
	.active_only = 1,
	.num_usecases = ARRAY_SIZE(c5_cpu_bw_table),
	.name = "cpu_pcnoc",
};

/* A successful vote is held for this built-in recovery boot's lifetime. */
static u32 c5_cpu_bus_client;
static int c5_cpu_bus_status;

int c5_cpu_pcnoc_status(void)
{
	return READ_ONCE(c5_cpu_bus_status);
}

static int __init c5_cpu_pcnoc_init(void)
{
	struct device_node *node;
	struct platform_device *pdev;
	int ret;

	if (c5_cpu_pcnoc_status() || c5_cpu_bus_client)
		return -EBUSY;
	node = of_find_compatible_node(NULL, NULL, "qcom,cpu-clock-8939");
	if (!node) {
		WRITE_ONCE(c5_cpu_bus_status, -ENODEV);
		return -ENODEV;
	}
	if (!of_property_read_bool(node, "qcom,cpu-pcnoc-vote")) {
		of_node_put(node);
		WRITE_ONCE(c5_cpu_bus_status, -EOPNOTSUPP);
		return 0;
	}
	pdev = of_find_device_by_node(node);
	of_node_put(node);
	if (!pdev) {
		WRITE_ONCE(c5_cpu_bus_status, -ENODEV);
		return -ENODEV;
	}
	ret = READ_ONCE(pdev->dev.driver) ? 0 : -ENODEV;
	put_device(&pdev->dev);
	if (ret) {
		WRITE_ONCE(c5_cpu_bus_status, ret);
		return ret;
	}
	c5_cpu_bus_client = msm_bus_scale_register_client(&c5_cpu_bus_data);
	if (!c5_cpu_bus_client) {
		WRITE_ONCE(c5_cpu_bus_status, -ENODEV);
		pr_err("C5PCNOC register failed;native DVFS must not start\n");
		return -ENODEV;
	}
	ret = msm_bus_scale_client_update_request(c5_cpu_bus_client, 1);
	if (ret) {
		msm_bus_scale_client_update_request(c5_cpu_bus_client, 0);
		msm_bus_scale_unregister_client(c5_cpu_bus_client);
		c5_cpu_bus_client = 0;
		WRITE_ONCE(c5_cpu_bus_status, ret);
		pr_err("C5PCNOC vote failed ret=%d;native DVFS must not start\n", ret);
		return ret;
	}
	WRITE_ONCE(c5_cpu_bus_status, 1);
	pr_info("C5PCNOC applied src=%u dst=%u ib=200000000 active_only=1\n",
		MSM_BUS_PNOC_INT_2, MSM_BUS_SLAVE_MESSAGE_RAM);
	return 0;
}
/* Bus fabric registers at fs_initcall;wait until device probing completes. */
late_initcall(c5_cpu_pcnoc_init);
