// SPDX-License-Identifier: GPL-2.0-only
/* C5 MUIC events -> native extcon and checked SM5705 source -> regulator. */
#include <linux/err.h>
#include <linux/extcon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/muic/muic.h>
#include <linux/muic/muic_notifier.h>

#include "c5-usb-role-core.h"

struct c5_usb_role_device {
	struct device *dev;
	struct extcon_dev *extcon;
	struct notifier_block muic;
	struct work_struct work;
	struct mutex lock;
	struct regulator_init_data regulator_init;
	struct c5_usb_source source;
	int pending_role;
	bool registered;
	bool stopping;
};

static const unsigned int c5_usb_cables[] = {
	EXTCON_USB, EXTCON_USB_HOST, EXTCON_NONE,
};

static int c5_usb_extcon_get(void *data, bool host)
{
	struct c5_usb_role_device *usb = data;

	return extcon_get_state(usb->extcon, host ? EXTCON_USB_HOST : EXTCON_USB);
}

static int c5_usb_extcon_set(void *data, bool host, bool on)
{
	struct c5_usb_role_device *usb = data;

	return extcon_set_state_sync(usb->extcon,
				     host ? EXTCON_USB_HOST : EXTCON_USB, on);
}

static void c5_usb_role_work(struct work_struct *work)
{
	struct c5_usb_role_device *usb = container_of(work,
						struct c5_usb_role_device, work);
	struct c5_usb_role_io io = {
		.data = usb, .get = c5_usb_extcon_get, .set = c5_usb_extcon_set,
	};
	int role, ret;

	mutex_lock(&usb->lock);
	if (usb->stopping) {
		mutex_unlock(&usb->lock);
		return;
	}
	role = usb->pending_role;
	mutex_unlock(&usb->lock);
	/* Native listeners may consult the regulator; do not hold our mutex. */
	ret = c5_usb_publish_role(&io, role);
	if (ret < 0)
		dev_err(usb->dev, "role notification failed: %d\n", ret);
}

static int c5_usb_muic_notify(struct notifier_block *nb, unsigned long action,
			       void *data)
{
	struct c5_usb_role_device *usb = container_of(nb,
						struct c5_usb_role_device, muic);
	int role;

	if (!data)
		return NOTIFY_DONE;
	role = c5_usb_role_from_muic(action, *(muic_attached_dev_t *)data);
	if (role < 0)
		return NOTIFY_DONE;
	mutex_lock(&usb->lock);
	if (!usb->stopping) {
		usb->pending_role = role;
		if (usb->registered)
			schedule_work(&usb->work);
	}
	mutex_unlock(&usb->lock);
	return NOTIFY_OK;
}

static int c5_usb_supply_get(void *unused)
{
	struct power_supply *psy;
	union power_supply_propval value;
	int ret;

	psy = power_supply_get_by_name("otg");
	if (!psy)
		return -ENODEV;
	ret = power_supply_get_property(psy, POWER_SUPPLY_PROP_ONLINE, &value);
	power_supply_put(psy);
	return ret < 0 ? ret : value.intval;
}

static int c5_usb_supply_set(void *unused, bool on)
{
	struct power_supply *psy;
	union power_supply_propval value = { .intval = on };
	int ret;

	psy = power_supply_get_by_name("otg");
	if (!psy)
		return -ENODEV;
	ret = power_supply_set_property(psy, POWER_SUPPLY_PROP_ONLINE, &value);
	power_supply_put(psy);
	return ret;
}

static const struct c5_usb_source_io c5_usb_supply_io = {
	.get = c5_usb_supply_get, .set = c5_usb_supply_set,
};

static int c5_usb_vbus_enable(struct regulator_dev *rdev)
{
	struct c5_usb_role_device *usb = rdev_get_drvdata(rdev);
	int ret;

	mutex_lock(&usb->lock);
	ret = usb->stopping ? -ESHUTDOWN :
		c5_usb_source_apply(&usb->source, &c5_usb_supply_io, true);
	mutex_unlock(&usb->lock);
	return ret;
}

static int c5_usb_vbus_disable(struct regulator_dev *rdev)
{
	struct c5_usb_role_device *usb = rdev_get_drvdata(rdev);
	int ret;

	mutex_lock(&usb->lock);
	ret = c5_usb_source_apply(&usb->source, &c5_usb_supply_io, false);
	mutex_unlock(&usb->lock);
	return ret;
}

static int c5_usb_vbus_is_enabled(struct regulator_dev *rdev)
{
	struct c5_usb_role_device *usb = rdev_get_drvdata(rdev);
	int ret;

	mutex_lock(&usb->lock);
	ret = c5_usb_supply_get(NULL);
	if (ret > 0 && !usb->source.owned)
		ret = -EBUSY;
	mutex_unlock(&usb->lock);
	return ret;
}

static const struct regulator_ops c5_usb_vbus_ops = {
	.enable = c5_usb_vbus_enable,
	.disable = c5_usb_vbus_disable,
	.is_enabled = c5_usb_vbus_is_enabled,
};

/* Enable-only adapter; no voltage/current setter or invented measurement. */
static const struct regulator_desc c5_usb_vbus_desc = {
	.name = "c5-usb-vbus", .owner = THIS_MODULE,
	.type = REGULATOR_VOLTAGE, .ops = &c5_usb_vbus_ops,
};

static void c5_usb_role_stop(struct c5_usb_role_device *usb)
{
	struct c5_usb_role_io io = {
		.data = usb, .get = c5_usb_extcon_get, .set = c5_usb_extcon_set,
	};
	int ret;

	mutex_lock(&usb->lock);
	usb->stopping = true;
	mutex_unlock(&usb->lock);
	if (usb->registered) {
		muic_notifier_unregister(&usb->muic);
		usb->registered = false;
	}
	cancel_work_sync(&usb->work);
	ret = c5_usb_publish_role(&io, C5_USB_NONE);
	if (ret < 0)
		dev_err(usb->dev, "role shutdown notification failed: %d\n", ret);
	mutex_lock(&usb->lock);
	if (usb->source.owned) {
		ret = c5_usb_source_apply(&usb->source, &c5_usb_supply_io, false);
		if (ret < 0)
			dev_err(usb->dev, "owned VBUS shutdown failed: %d\n", ret);
	}
	mutex_unlock(&usb->lock);
}

static int c5_usb_role_probe(struct platform_device *pdev)
{
	struct c5_usb_role_device *usb;
	struct regulator_config config = { };
	struct regulator_dev *rdev;
	int ret;

	ret = c5_usb_supply_get(NULL);
	if (ret == -ENODEV || ret == -ENOENT)
		return -EPROBE_DEFER;
	if (ret < 0)
		return ret;
	if (ret)
		return -EBUSY;
	usb = devm_kzalloc(&pdev->dev, sizeof(*usb), GFP_KERNEL);
	if (!usb)
		return -ENOMEM;
	usb->dev = &pdev->dev;
	mutex_init(&usb->lock);
	INIT_WORK(&usb->work, c5_usb_role_work);
	usb->extcon = devm_extcon_dev_allocate(&pdev->dev, c5_usb_cables);
	if (IS_ERR(usb->extcon))
		return PTR_ERR(usb->extcon);
	usb->regulator_init.constraints.name = "c5-usb-vbus";
	usb->regulator_init.constraints.valid_ops_mask = REGULATOR_CHANGE_STATUS;
	config.dev = &pdev->dev;
	config.of_node = pdev->dev.of_node;
	config.driver_data = usb;
	config.init_data = &usb->regulator_init;
	rdev = devm_regulator_register(&pdev->dev, &c5_usb_vbus_desc, &config);
	if (IS_ERR(rdev))
		return PTR_ERR(rdev);
	platform_set_drvdata(pdev, usb);
	/* Registration invokes the callback synchronously; publish only after success. */
	ret = muic_notifier_register(&usb->muic, c5_usb_muic_notify,
				     MUIC_NOTIFY_DEV_USB);
	if (ret < 0)
		return ret;
	/* Publish the supplier only after its regulator and notifier are ready. */
	ret = devm_extcon_dev_register(&pdev->dev, usb->extcon);
	if (ret < 0) {
		muic_notifier_unregister(&usb->muic);
		return ret;
	}
	mutex_lock(&usb->lock);
	usb->registered = true;
	schedule_work(&usb->work);
	mutex_unlock(&usb->lock);
	return 0;
}

static int c5_usb_role_remove(struct platform_device *pdev)
{
	c5_usb_role_stop(platform_get_drvdata(pdev));
	return 0;
}

static void c5_usb_role_shutdown(struct platform_device *pdev)
{
	c5_usb_role_stop(platform_get_drvdata(pdev));
}

static const struct of_device_id c5_usb_role_match[] = {
	{ .compatible = "samsung,c5-usb-role" }, { },
};
MODULE_DEVICE_TABLE(of, c5_usb_role_match);

static struct platform_driver c5_usb_role_driver = {
	.probe = c5_usb_role_probe,
	.remove = c5_usb_role_remove,
	.shutdown = c5_usb_role_shutdown,
	.driver = { .name = "c5-usb-role", .of_match_table = c5_usb_role_match },
};
module_platform_driver(c5_usb_role_driver);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("C5 native MUIC role and checked OTG source adapter");
