/*
 * MSM8952 performance L1/L2 clock-gating workaround.
 * Derived from drivers/cpuidle/lpm-workarounds.c:
 * Copyright (c) 2014-2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License.
 */
#include <linux/cpu.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/msm_thermal.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

#define C5_L2_POWER_STATUS 0x0b011018
#define C5_L1_L2_GCC      0x0b011088
#define C5_L2_ACTIVE      0x200
#define C5_GATE_WAIT_MS   5000

struct c5_lpm_wa {
	struct device *dev;
	void __iomem *l2_status;
	void __iomem *gcc;
	struct device_clnt_data *client;
	struct delayed_work work;
	struct workqueue_struct *wq;
	struct mutex lock;
	struct kobject *kobj;
	cpumask_t target;
	unsigned long deadline;
	bool requested;
	bool enabled;
};

/* The fixed legacy sysfs ABI has no per-device argument. */
static struct c5_lpm_wa *c5_wa;

/* Pure prerequisite gate; the caller holds the CPU-hotplug read lock. */
static bool c5_gate_may_write(bool requested, bool target_offline,
			    u32 l2_status)
{
	return requested && target_offline && !(l2_status & C5_L2_ACTIVE);
}

static int c5_release_request(struct c5_lpm_wa *wa)
{
	union device_request req;
	int ret;

	HOTPLUG_NO_MITIGATION(&req.offline_mask);
	ret = devmgr_client_request_mitigation(wa->client,
					     HOTPLUG_MITIGATION_REQ, &req);
	if (!ret)
		wa->requested = false;
	return ret;
}

static void c5_gate_work(struct work_struct *work)
{
	struct c5_lpm_wa *wa = container_of(to_delayed_work(work),
						struct c5_lpm_wa, work);
	bool offline;
	u32 status = C5_L2_ACTIVE;

	mutex_lock(&wa->lock);
	if (!wa->requested)
		goto unlock;
	get_online_cpus();
	offline = !cpumask_intersects(&wa->target, cpu_online_mask);
	if (offline)
		status = readl(wa->l2_status);
	if (c5_gate_may_write(wa->requested, offline, status)) {
		/* Same single register/value and quiescence prerequisite as 3.10.
		 * No voltage, frequency, timer, reset or power-state write.
		 */
		writel(0, wa->gcc);
		wa->enabled = readl(wa->gcc) == 0;
		put_online_cpus();
		if (c5_release_request(wa))
			dev_err(wa->dev, "Cannot release own hotplug request\n");
		goto unlock;
	}
	put_online_cpus();
	if (time_before(jiffies, wa->deadline)) {
		queue_delayed_work(wa->wq, &wa->work, msecs_to_jiffies(20));
	} else {
		/* Never retain an endless retry loop or our mitigation request. */
		if (c5_release_request(wa))
			dev_err(wa->dev, "Cannot release timed-out hotplug request\n");
	}
unlock:
	mutex_unlock(&wa->lock);
}

static ssize_t c5_gating_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	struct c5_lpm_wa *wa = READ_ONCE(c5_wa);

	if (!wa)
		return -ENODEV;
	/* Current functional setting only, not a failure/phase history. */
	return scnprintf(buf, PAGE_SIZE, "%u\n", READ_ONCE(wa->enabled));
}

static ssize_t c5_gating_store(struct kobject *kobj,
			     struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	struct c5_lpm_wa *wa = READ_ONCE(c5_wa);
	union device_request req;
	int value, ret;

	if (!wa)
		return -ENODEV;
	ret = kstrtoint(buf, 10, &value);
	if (ret || value != 1)
		return -EINVAL;
	mutex_lock(&wa->lock);
	if (wa->enabled || wa->requested) {
		ret = 0;
		goto unlock;
	}
	cpumask_copy(&req.offline_mask, &wa->target);
	ret = devmgr_client_request_mitigation(wa->client,
					     HOTPLUG_MITIGATION_REQ, &req);
	if (ret) {
		/* An implementation may update its request before reporting an
		 * unavailable worker; cancel only this client's request.
		 */
		c5_release_request(wa);
		goto unlock;
	}
	wa->requested = true;
	wa->deadline = jiffies + msecs_to_jiffies(C5_GATE_WAIT_MS);
	queue_delayed_work(wa->wq, &wa->work, 0);
unlock:
	mutex_unlock(&wa->lock);
	return ret ? ret : count;
}

static struct kobj_attribute c5_gating_attr =
	__ATTR(dynamic_clock_gating, 0644, c5_gating_show, c5_gating_store);

static int c5_lpm_wa_probe(struct platform_device *pdev)
{
	struct c5_lpm_wa *wa;
	struct resource *status, *gcc;
	struct kobject *parent;
	u32 mask, non_boot;
	int ret, cpu;

	if (!of_machine_is_compatible("qcom,msm8952-mtp") || c5_wa ||
	    !of_property_read_bool(pdev->dev.of_node,
				   "qcom,lpm-wa-dynamic-clock-gating") ||
	    of_property_read_bool(pdev->dev.of_node, "qcom,l1-l2-gcc-secure"))
		return -ENODEV;
	status = platform_get_resource_byname(pdev, IORESOURCE_MEM, "l2-pwr-sts");
	gcc = platform_get_resource_byname(pdev, IORESOURCE_MEM, "l1-l2-gcc");
	if (!status || !gcc || status->start != C5_L2_POWER_STATUS ||
	    gcc->start != C5_L1_L2_GCC || resource_size(status) != 4 ||
	    resource_size(gcc) != 4)
		return -EINVAL;
	if (of_property_read_u32(pdev->dev.of_node, "qcom,cpu-offline-mask", &mask) ||
	    of_property_read_u32(pdev->dev.of_node, "qcom,non-boot-cpu-index", &non_boot) ||
	    mask != 0xf || non_boot != 4 || num_possible_cpus() != 8)
		return -EINVAL;
	wa = devm_kzalloc(&pdev->dev, sizeof(*wa), GFP_KERNEL);
	if (!wa)
		return -ENOMEM;
	wa->dev = &pdev->dev;
	mutex_init(&wa->lock);
	wa->l2_status = devm_ioremap(&pdev->dev, status->start, 4);
	wa->gcc = devm_ioremap(&pdev->dev, gcc->start, 4);
	if (!wa->l2_status || !wa->gcc)
		return -ENOMEM;
	for (cpu = 0; cpu < non_boot; cpu++)
		cpumask_set_cpu(cpu, &wa->target);
	wa->client = devmgr_register_mitigation_client(&pdev->dev,
						     HOTPLUG_DEVICE, NULL);
	if (IS_ERR_OR_NULL(wa->client))
		return -EPROBE_DEFER;
	INIT_DELAYED_WORK(&wa->work, c5_gate_work);
	wa->wq = alloc_workqueue("c5-lpm-wa", WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
	if (!wa->wq) {
		ret = -ENOMEM;
		goto unregister;
	}
	parent = kset_find_obj(module_kset, "lpm_levels");
	if (!parent) {
		ret = -EPROBE_DEFER;
		goto destroy;
	}
	wa->kobj = kobject_create_and_add("lpm_workarounds", parent);
	kobject_put(parent);
	if (!wa->kobj) {
		ret = -ENOMEM;
		goto destroy;
	}
	/* Publish only after client, locks and worker are ready. */
	platform_set_drvdata(pdev, wa);
	WRITE_ONCE(c5_wa, wa);
	ret = sysfs_create_file(wa->kobj, &c5_gating_attr.attr);
	if (!ret)
		return 0;
	WRITE_ONCE(c5_wa, NULL);
	kobject_put(wa->kobj);
destroy:
	destroy_workqueue(wa->wq);
unregister:
	devmgr_unregister_mitigation_client(&pdev->dev, wa->client);
	return ret;
}

static int c5_lpm_wa_remove(struct platform_device *pdev)
{
	struct c5_lpm_wa *wa = platform_get_drvdata(pdev);

	sysfs_remove_file(wa->kobj, &c5_gating_attr.attr);
	cancel_delayed_work_sync(&wa->work);
	mutex_lock(&wa->lock);
	if (wa->requested)
		c5_release_request(wa);
	mutex_unlock(&wa->lock);
	WRITE_ONCE(c5_wa, NULL);
	kobject_put(wa->kobj);
	destroy_workqueue(wa->wq);
	devmgr_unregister_mitigation_client(&pdev->dev, wa->client);
	return 0;
}

static const struct of_device_id c5_lpm_wa_match[] = {
	{ .compatible = "qcom,lpm-workarounds" },
	{ }
};
static struct platform_driver c5_lpm_wa_driver = {
	.probe = c5_lpm_wa_probe,
	.remove = c5_lpm_wa_remove,
	.driver = {
		.name = "c5-lpm-workarounds",
		.of_match_table = c5_lpm_wa_match,
	},
};
static int __init c5_lpm_wa_init(void)
{
	return platform_driver_register(&c5_lpm_wa_driver);
}
late_initcall(c5_lpm_wa_init);
MODULE_LICENSE("GPL v2");
