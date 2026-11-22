/* SPDX-License-Identifier: GPL-2.0-only */
/* Legacy SSC ABI: Copyright (c) 2012-2014, The Linux Foundation.
 * C5 timer-only integration: physical reads through the existing native frame.
 */
#include <linux/cdev.h>
#include <linux/compat.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/msm_dsps.h>

#ifdef CONFIG_COMPAT
#define C5_SSC_READ_SLOW_TIMER32 _IOR(DSPS_IOCTL_MAGIC, 3, compat_uint_t)
#endif

struct c5_ssc_timer {
	struct device dev;
	struct cdev cdev;
	struct class *class;
	struct mutex lock;
	bool stopping;
	bool devt_allocated;
};

static DEFINE_MUTEX(c5_ssc_registry_lock);
/* Held through the final open-file/cdev release, including after remove. */
static struct c5_ssc_timer *c5_ssc_active;

static int c5_ssc_backend_status(void)
{
	bool ready = smp_load_acquire(&c5_counter_ref_ready);

	if (!c5_counter_ref_profile() || !ready || !arch_counter_base ||
	    c5_counter_ref_base != arch_counter_base)
		return -ENODEV;
	if (!(READ_ONCE(c5_counter_ref_access) & CNTACR_RPCT))
		return -EACCES;
	if (READ_ONCE(arch_timer_rate) != C5_COUNTER_REF_RATE)
		return -ERANGE;
	return 0;
}

static int c5_ssc_read_slow_timer(u32 *value)
{
	u64 physical;
	int ret = c5_ssc_backend_status();

	if (ret)
		return ret;
	/* These are ordered reads of the global physical frame, on any CPU. */
	isb();
	if (!c5_counter_ref_read_phys(&physical))
		return -EIO;
	mb();
	physical <<= 4;
	do_div(physical, 9375);
	*value = (u32)physical;
	return 0;
}

static void c5_ssc_state_release(struct device *dev)
{
	struct c5_ssc_timer *ssc = container_of(dev, struct c5_ssc_timer, dev);

	if (ssc->devt_allocated)
		unregister_chrdev_region(dev->devt, 1);
	if (ssc->class)
		class_destroy(ssc->class);
	mutex_lock(&c5_ssc_registry_lock);
	if (c5_ssc_active == ssc)
		c5_ssc_active = NULL;
	mutex_unlock(&c5_ssc_registry_lock);
	kfree(ssc);
}

static int c5_ssc_open(struct inode *inode, struct file *file)
{
	struct c5_ssc_timer *ssc = container_of(inode->i_cdev,
					     struct c5_ssc_timer, cdev);
	int ret = 0;

	/* cdev_device_add's parent reference protects this lookup. */
	mutex_lock(&ssc->lock);
	if (ssc->stopping)
		ret = -ENODEV;
	else {
		get_device(&ssc->dev);
		file->private_data = ssc;
	}
	mutex_unlock(&ssc->lock);
	return ret;
}

static int c5_ssc_release(struct inode *inode, struct file *file)
{
	struct c5_ssc_timer *ssc = file->private_data;

	file->private_data = NULL;
	if (ssc)
		put_device(&ssc->dev);
	return 0;
}

static long c5_ssc_ioctl(struct file *file, unsigned int cmd,
			unsigned long arg)
{
	struct c5_ssc_timer *ssc = file->private_data;
	u32 value;
	int ret;

	switch (cmd) {
	case DSPS_IOCTL_READ_SLOW_TIMER:
#ifdef CONFIG_COMPAT
	case C5_SSC_READ_SLOW_TIMER32:
#endif
		break;
	default:
		return -EINVAL;
	}
	if (!ssc)
		return -ENODEV;
	mutex_lock(&ssc->lock);
	ret = ssc->stopping ? -ENODEV : c5_ssc_read_slow_timer(&value);
	mutex_unlock(&ssc->lock);
	if (ret)
		return ret;
	/* Preserve the old native8/compat4 command geometry, both copying u32. */
	return put_user(value, (u32 __user *)arg);
}

static const struct file_operations c5_ssc_fops = {
	.owner = THIS_MODULE,
	.open = c5_ssc_open,
	.release = c5_ssc_release,
	.unlocked_ioctl = c5_ssc_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = c5_ssc_ioctl,
#endif
};

static int c5_ssc_probe(struct platform_device *pdev)
{
	struct c5_ssc_timer *ssc;
	int ret = c5_ssc_backend_status();

	if (ret)
		return ret;
	mutex_lock(&c5_ssc_registry_lock);
	if (c5_ssc_active) {
		mutex_unlock(&c5_ssc_registry_lock);
		return -EBUSY;
	}
	ssc = kzalloc(sizeof(*ssc), GFP_KERNEL);
	if (!ssc) {
		mutex_unlock(&c5_ssc_registry_lock);
		return -ENOMEM;
	}
	mutex_init(&ssc->lock);
	device_initialize(&ssc->dev);
	ssc->dev.release = c5_ssc_state_release;
	ssc->stopping = true;
	c5_ssc_active = ssc;
	mutex_unlock(&c5_ssc_registry_lock);

	ssc->class = class_create(THIS_MODULE, "ssc");
	if (IS_ERR(ssc->class)) {
		ret = PTR_ERR(ssc->class);
		ssc->class = NULL;
		goto put;
	}
	ssc->dev.class = ssc->class;
	ret = alloc_chrdev_region(&ssc->dev.devt, 0, 1, "sensors");
	if (ret)
		goto put;
	ssc->devt_allocated = true;
	ret = dev_set_name(&ssc->dev, "sensors");
	if (ret)
		goto put;
	cdev_init(&ssc->cdev, &c5_ssc_fops);
	ssc->cdev.owner = THIS_MODULE;
	ret = cdev_device_add(&ssc->cdev, &ssc->dev);
	if (ret)
		goto put;
	platform_set_drvdata(pdev, ssc);
	mutex_lock(&ssc->lock);
	ssc->stopping = false;
	mutex_unlock(&ssc->lock);
	return 0;
put:
	/* Publication may have been attempted; open remains refused until success. */
	put_device(&ssc->dev);
	return ret;
}

static int c5_ssc_remove(struct platform_device *pdev)
{
	struct c5_ssc_timer *ssc = platform_get_drvdata(pdev);

	if (!ssc)
		return 0;
	mutex_lock(&ssc->lock);
	ssc->stopping = true;
	mutex_unlock(&ssc->lock);
	cdev_device_del(&ssc->cdev, &ssc->dev);
	platform_set_drvdata(pdev, NULL);
	/* Existing file/cdev references keep storage, class and dev_t alive. */
	put_device(&ssc->dev);
	return 0;
}

static const struct of_device_id c5_ssc_match[] = {
	{ .compatible = "qcom,msm-ssc-sensors" },
	{ }
};
MODULE_DEVICE_TABLE(of, c5_ssc_match);

static struct platform_driver c5_ssc_driver = {
	.driver = {
		.name = "sensors-ssc",
		.owner = THIS_MODULE,
		.of_match_table = c5_ssc_match,
	},
	.probe = c5_ssc_probe,
	.remove = c5_ssc_remove,
};

static int __init c5_ssc_init(void)
{
	return platform_driver_register(&c5_ssc_driver);
}
module_init(c5_ssc_init);

static void __exit c5_ssc_exit(void)
{
	platform_driver_unregister(&c5_ssc_driver);
}
module_exit(c5_ssc_exit);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("C5 legacy SSC physical slow-timer interface");
