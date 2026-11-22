// SPDX-License-Identifier: GPL-2.0-only
/* C5 DC-motor branch of samsung_vib: regulator-only, 3.3 V at the original
 * maximum intensity. Do not use the other boards' raw PWM clock registers.
 * Preserve the installed HAL's timed_output/vibrator/enable interface.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/pm_wakeup.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include "timed_output.h"

struct c5_vibrator {
	struct timed_output_dev output;
	struct regulator *supply;
	struct wakeup_source wake;
	struct mutex command_lock;
	struct mutex state_lock;
	struct delayed_work stop;
	unsigned long deadline;
	bool active;
};

static int c5_vibrator_off(struct c5_vibrator *vib)
{
	int ret = 0;
	if (vib->active) {
		ret = regulator_disable(vib->supply);
		if (ret)
			return ret;
		vib->active = false;
	}
	__pm_relax(&vib->wake);
	return ret;
}

static void c5_vibrator_stop(struct work_struct *work)
{
	struct c5_vibrator *vib = container_of(to_delayed_work(work), struct c5_vibrator, stop);
	mutex_lock(&vib->state_lock);
	if (c5_vibrator_off(vib))
		pr_err("c5-vibrator: regulator disable failed\n");
	mutex_unlock(&vib->state_lock);
}

static void c5_vibrator_enable(struct timed_output_dev *output, int ms)
{
	struct c5_vibrator *vib = container_of(output, struct c5_vibrator, output);
	int ret = 0;
	ms = clamp(ms, 0, 10000);
	mutex_lock(&vib->command_lock);
	cancel_delayed_work_sync(&vib->stop);
	mutex_lock(&vib->state_lock);
	if (!ms) {
		ret = c5_vibrator_off(vib);
	} else {
		if (!vib->active) {
			ret = regulator_set_voltage(vib->supply, 3300000, 3300000);
			if (!ret)
				ret = regulator_enable(vib->supply);
			if (!ret) {
				vib->active = true;
				__pm_stay_awake(&vib->wake);
			}
		}
		if (!ret) {
			vib->deadline = jiffies + msecs_to_jiffies(ms);
			schedule_delayed_work(&vib->stop, msecs_to_jiffies(ms));
		}
	}
	if (ret)
		pr_err("c5-vibrator: supply transition failed: %d\n", ret);
	mutex_unlock(&vib->state_lock);
	mutex_unlock(&vib->command_lock);
}

static int c5_vibrator_remaining(struct timed_output_dev *output)
{
	struct c5_vibrator *vib = container_of(output, struct c5_vibrator, output);
	unsigned long now;
	int ms = 0;
	mutex_lock(&vib->state_lock);
	now = jiffies;
	if (vib->active && time_after(vib->deadline, now))
		ms = jiffies_to_msecs(vib->deadline - now);
	mutex_unlock(&vib->state_lock);
	return ms;
}

static int c5_vibrator_probe(struct platform_device *pdev)
{
	struct c5_vibrator *vib;
	const char *name;
	int ret;
	vib = devm_kzalloc(&pdev->dev, sizeof(*vib), GFP_KERNEL);
	if (!vib)
		return -ENOMEM;
	ret = of_property_read_string(pdev->dev.of_node, "samsung,vib-regulator", &name);
	if (ret)
		return ret;
	vib->supply = devm_regulator_get(&pdev->dev, name);
	if (IS_ERR(vib->supply))
		return PTR_ERR(vib->supply);
	mutex_init(&vib->command_lock);
	mutex_init(&vib->state_lock);
	INIT_DELAYED_WORK(&vib->stop, c5_vibrator_stop);
	wakeup_source_init(&vib->wake, "c5-vibrator");
	vib->output.name = "vibrator";
	vib->output.enable = c5_vibrator_enable;
	vib->output.get_time = c5_vibrator_remaining;
	platform_set_drvdata(pdev, vib);
	ret = timed_output_dev_register(&vib->output);
	if (ret)
		wakeup_source_trash(&vib->wake);
	return ret;
}

static int c5_vibrator_suspend(struct device *dev)
{
	struct c5_vibrator *vib = dev_get_drvdata(dev);
	c5_vibrator_enable(&vib->output, 0);
	return vib->active ? -EBUSY : 0;
}

static void c5_vibrator_shutdown(struct platform_device *pdev)
{
	c5_vibrator_suspend(&pdev->dev);
}

static int c5_vibrator_remove(struct platform_device *pdev)
{
	struct c5_vibrator *vib = platform_get_drvdata(pdev);
	timed_output_dev_unregister(&vib->output);
	cancel_delayed_work_sync(&vib->stop);
	wakeup_source_trash(&vib->wake);
	return 0;
}

static SIMPLE_DEV_PM_OPS(c5_vibrator_pm, c5_vibrator_suspend, NULL);
static const struct of_device_id c5_vibrator_ids[] = {{ .compatible = "samsung_vib" }, {}};
MODULE_DEVICE_TABLE(of, c5_vibrator_ids);
static struct platform_driver c5_vibrator_driver = {
	.probe = c5_vibrator_probe,
	.remove = c5_vibrator_remove,
	.shutdown = c5_vibrator_shutdown,
	.driver = { .name = "c5-vibrator", .of_match_table = c5_vibrator_ids, .pm = &c5_vibrator_pm },
};
module_platform_driver(c5_vibrator_driver);
MODULE_LICENSE("GPL v2");
