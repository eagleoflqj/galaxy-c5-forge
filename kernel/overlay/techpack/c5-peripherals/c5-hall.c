// SPDX-License-Identifier: GPL-2.0-only
/* C5's legacy "hall" DT binding: the original driver labels GPIO high OPEN.
 * Translate to the standard input ABI SW_LID (one means closed), without
 * depending on Samsung's missing global sec_key device. No factory controls.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/pinctrl/consumer.h>

struct c5_hall {
	struct input_dev *input;
	int gpio;
	int irq;
	bool wake_armed;
};

static irqreturn_t c5_hall_report(int irq, void *data)
{
	struct c5_hall *hall = data;
	input_report_switch(hall->input, SW_LID, !gpio_get_value_cansleep(hall->gpio));
	input_sync(hall->input);
	return IRQ_HANDLED;
}

static int c5_hall_probe(struct platform_device *pdev)
{
	struct c5_hall *hall;
	struct pinctrl *pins;
	int ret;

	hall = devm_kzalloc(&pdev->dev, sizeof(*hall), GFP_KERNEL);
	if (!hall)
		return -ENOMEM;
	hall->gpio = of_get_named_gpio(pdev->dev.of_node, "hall,gpio_flip_cover", 0);
	if (hall->gpio < 0)
		return hall->gpio;
	pins = devm_pinctrl_get_select(&pdev->dev, "hall_gpio_default");
	if (IS_ERR(pins))
		return PTR_ERR(pins);
	ret = devm_gpio_request_one(&pdev->dev, hall->gpio, GPIOF_IN, "c5-hall");
	if (ret)
		return ret;
	hall->irq = gpio_to_irq(hall->gpio);
	if (hall->irq < 0)
		return hall->irq;
	hall->input = devm_input_allocate_device(&pdev->dev);
	if (!hall->input)
		return -ENOMEM;
	hall->input->name = "hall";
	hall->input->phys = "c5-hall/input0";
	hall->input->id.bustype = BUS_HOST;
	input_set_capability(hall->input, EV_SW, SW_LID);
	ret = input_register_device(hall->input);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, hall);
	ret = devm_request_threaded_irq(&pdev->dev, hall->irq, NULL, c5_hall_report,
		IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING | IRQF_ONESHOT, "c5-hall", hall);
	if (ret)
		return ret;
	c5_hall_report(hall->irq, hall);
	return device_init_wakeup(&pdev->dev, true);
}

static int c5_hall_suspend(struct device *dev)
{
	struct c5_hall *hall = dev_get_drvdata(dev);
	int ret;
	if (!device_may_wakeup(dev))
		return 0;
	ret = enable_irq_wake(hall->irq);
	if (!ret)
		hall->wake_armed = true;
	return ret;
}

static int c5_hall_resume(struct device *dev)
{
	struct c5_hall *hall = dev_get_drvdata(dev);
	if (hall->wake_armed) {
		disable_irq_wake(hall->irq);
		hall->wake_armed = false;
	}
	c5_hall_report(hall->irq, hall);
	return 0;
}

static int c5_hall_remove(struct platform_device *pdev)
{
	c5_hall_resume(&pdev->dev);
	device_init_wakeup(&pdev->dev, false);
	return 0;
}

static SIMPLE_DEV_PM_OPS(c5_hall_pm, c5_hall_suspend, c5_hall_resume);
static const struct of_device_id c5_hall_ids[] = {{ .compatible = "hall" }, {}};
MODULE_DEVICE_TABLE(of, c5_hall_ids);
static struct platform_driver c5_hall_driver = {
	.probe = c5_hall_probe,
	.remove = c5_hall_remove,
	.driver = { .name = "c5-hall", .of_match_table = c5_hall_ids, .pm = &c5_hall_pm },
};
module_platform_driver(c5_hall_driver);
MODULE_LICENSE("GPL v2");
