// SPDX-License-Identifier: GPL-2.0-or-later
/* C5 secure VFS runtime ABI port, derived from Samsung/Validity vfs7xxx.c
 * (C) 2011-2013 Validity Sensors, Inc.; (C) 2013 Samsung Electronics.
 * Transfers, matching, templates and authentication remain inside the native TEE.
 * This file never maps/reads/writes QUP/BAM registers or exposes a raw SPI path.
 */
#include <linux/clk.h>
#include <linux/c5-fp-home-wake.h>
#include <linux/compat.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>
#include <linux/pid.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#define FP_RESET _IO('k', 2)
#define FP_CLK _IOW('k', 3, unsigned int)
#define FP_SIGNAL _IOW('k', 5, unsigned int)
#define FP_IRQ _IOW('k', 8, unsigned int)
#define FP_SUSPEND _IO('k', 9)
#define FP_POWER_ON _IO('k', 13)
#define FP_POWER_OFF _IO('k', 14)
#define FP_CLK_OFF _IO('k', 15)
#define FP_CONFIG _IO('k', 16)
#define FP_UNCONFIG _IO('k', 17)
#define FP_ORIENT _IOR('k', 18, unsigned int)
#define FP_BOOST _IOW('k', 19, unsigned int)
#define FP_TYPE _IOW('k', 20, unsigned int)
#define FP_LOCKSCREEN _IOW('k', 22, unsigned int)
#define FP_POWER_CONTROL _IOW('k', 23, unsigned int)

struct c5_fp {
	struct device *dev, *sysfs;
	struct miscdevice misc;
	struct kref refs;
	struct mutex lock;
	spinlock_t signal_lock;
	struct pid *signal_pid;
	int signal_nr, irq, sleep_gpio, drdy_gpio, ldo_gpio;
	struct clk *core, *iface;
	struct pinctrl *pins;
	struct pinctrl_state *idle, *sleep;
	struct wakeup_source *wake;
	struct notifier_block qos;
	atomic_t armed;
	u32 orientation, boost_khz;
	int sensor_type;
	u64 suspend_count, resume_count, home_wake_count, home_notify_attempts;
	bool opened, gone, powered, clocked, boost, lockscreen;
};
static struct class *fp_class;

/* Accepted native clock helper floors to a supported rate, never overspeeds. */
static long fp_floor_rate(struct clk *clk, unsigned long rate)
{
	long low, cur, guess = rate, step, direction = -1, best;
	int count = 10;

	cur = clk_round_rate(clk, rate);
	if (cur < 0 || cur == rate)
		return cur;
	low = clk_round_rate(clk, 0);
	if (low < 0)
		return low;
	if (low > rate)
		return -EINVAL;
	best = low;
	step = (rate - low) >> 1;
	while (count-- && step) {
		guess += step * direction;
		cur = clk_round_rate(clk, guess);
		if (cur < 0)
			return cur;
		if (cur <= rate && cur > best)
			best = cur;
		if ((cur > rate && direction > 0) || (cur < rate && direction < 0)) {
			direction = -direction;
			step >>= 1;
		}
	}
	return best;
}

static void fp_power(struct c5_fp *fp, bool on)
{
	gpio_set_value_cansleep(fp->ldo_gpio, on);
	if (!on)
		gpio_set_value_cansleep(fp->sleep_gpio, 0);
	fp->powered = on;
}

static void fp_clock_off(struct c5_fp *fp)
{
	if (!fp->clocked)
		return;
	clk_disable_unprepare(fp->iface);
	clk_disable_unprepare(fp->core);
	__pm_relax(fp->wake);
	fp->clocked = false;
}

static int fp_clock(struct c5_fp *fp, u16 value)
{
	unsigned long speed = value == 0 ? 9600000 :
		(value == 65535 ? 4800000 : min_t(unsigned long, value * 1000UL, 9600000));
	long rate;
	int ret;

	rate = fp_floor_rate(fp->core, speed);
	if (rate < 0)
		return rate;
	if (!fp->clocked) {
		ret = clk_prepare_enable(fp->core);
		if (ret)
			return ret;
		ret = clk_prepare_enable(fp->iface);
		if (ret) {
			clk_disable_unprepare(fp->core);
			return ret;
		}
		ret = clk_set_rate(fp->core, rate);
		if (ret) {
			clk_disable_unprepare(fp->iface);
			clk_disable_unprepare(fp->core);
			return ret;
		}
		fp->clocked = true;
		__pm_stay_awake(fp->wake);
	} else {
		ret = clk_set_rate(fp->core, rate);
		if (ret)
			return ret;
	}
	usleep_range(950, 1000);
	return 0;
}

static void fp_notify(struct c5_fp *fp)
{
	struct pid *pid;
	struct task_struct *task;
	unsigned long flags;
	int sig;

	spin_lock_irqsave(&fp->signal_lock, flags);
	pid = get_pid(fp->signal_pid);
	sig = fp->signal_nr;
	spin_unlock_irqrestore(&fp->signal_lock, flags);
	if (!pid)
		return;
	task = get_pid_task(pid, PIDTYPE_PID);
	if (task) {
		send_sig_info(sig, SEND_SIG_PRIV, task);
		put_task_struct(task);
	}
	put_pid(pid);
}

/* Caller holds fp->lock, as do signal registration and file release. */
static bool fp_has_signal(struct c5_fp *fp)
{
	unsigned long flags;
	bool registered;

	spin_lock_irqsave(&fp->signal_lock, flags);
	registered = fp->signal_pid && fp->signal_nr > 0;
	spin_unlock_irqrestore(&fp->signal_lock, flags);
	return registered;
}

static void fp_home_resume_notify(struct c5_fp *fp)
{
	if (!fp->gone && fp->opened && fp->lockscreen && fp_has_signal(fp) &&
	    c5_fp_wakeup_by_home()) {
		fp->home_wake_count++;
		/* Consume the HOME latch before testing DRDY, like the old helper. */
		if (atomic_read(&fp->armed) == 0) {
			fp->home_notify_attempts++;
			fp_notify(fp);
		}
	}
}

static irqreturn_t fp_irq(int irq, void *data)
{
	struct c5_fp *fp = data;

	if (gpio_get_value(fp->drdy_gpio) == 1 && atomic_cmpxchg(&fp->armed, 1, 0) == 1) {
		disable_irq_nosync(irq);
		fp_notify(fp);
	}
	return IRQ_HANDLED;
}

static int fp_set_irq(struct c5_fp *fp, bool on)
{
	int ret;

	if (!on) {
		if (atomic_cmpxchg(&fp->armed, 1, 0) == 1)
			disable_irq(fp->irq);
		else
			synchronize_irq(fp->irq);
		return pinctrl_select_state(fp->pins, fp->sleep);
	}
	ret = pinctrl_select_state(fp->pins, fp->idle);
	if (ret)
		return ret;
	if (atomic_cmpxchg(&fp->armed, 0, 1) == 0)
		enable_irq(fp->irq);
	/* Same native missed-edge check, atomically consume only one notification. */
	if (gpio_get_value(fp->drdy_gpio) == 1)
		fp_irq(fp->irq, fp);
	return 0;
}

static int fp_signal(struct c5_fp *fp, void __user *arg)
{
	struct { int pid, sig; } user;
	struct pid *pid, *old;
	struct task_struct *task;
	unsigned long flags;
	bool allowed;

	if (copy_from_user(&user, arg, sizeof(user)))
		return -EFAULT;
	if (user.pid <= 0 || user.sig <= 0 || user.sig > SIGRTMAX ||
	    user.sig == SIGKILL || user.sig == SIGSTOP)
		return -EINVAL;
	pid = find_get_pid(user.pid);
	if (!pid)
		return -ESRCH;
	task = get_pid_task(pid, PIDTYPE_PID);
	if (!task) {
		put_pid(pid);
		return -ESRCH;
	}
	allowed = same_thread_group(task, current);
	put_task_struct(task);
	if (!allowed) {
		put_pid(pid);
		return -EPERM;
	}
	spin_lock_irqsave(&fp->signal_lock, flags);
	old = fp->signal_pid;
	fp->signal_pid = pid;
	fp->signal_nr = user.sig;
	spin_unlock_irqrestore(&fp->signal_lock, flags);
	put_pid(old);
	return 0;
}

static int fp_qos(struct notifier_block *nb, unsigned long event, void *data)
{
	struct c5_fp *fp = container_of(nb, struct c5_fp, qos);
	struct cpufreq_policy *policy = data;
	unsigned int floor;

	if (event == CPUFREQ_ADJUST && READ_ONCE(fp->boost)) {
		floor = min(fp->boost_khz, policy->max);
		cpufreq_verify_within_limits(policy, floor, policy->max);
	}
	return NOTIFY_OK;
}

static int fp_boost(struct c5_fp *fp, bool on)
{
	unsigned int cpu;
	int ret = 0, error;

	WRITE_ONCE(fp->boost, on);
	get_online_cpus();
	for_each_online_cpu(cpu) {
		error = cpufreq_update_policy(cpu);
		if (error && !ret)
			ret = error;
	}
	put_online_cpus();
	return ret;
}

static long fp_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct c5_fp *fp = file->private_data;
	void __user *ptr = (void __user *)arg;
	u32 value;
	u16 small;
	int ret = 0;

	mutex_lock(&fp->lock);
	if (fp->gone) {
		ret = -ENODEV;
		goto out;
	}
	switch (cmd) {
	case FP_RESET:
		gpio_set_value_cansleep(fp->sleep_gpio, 0);
		usleep_range(1000, 1100);
		gpio_set_value_cansleep(fp->sleep_gpio, 1);
		msleep(10);
		break;
	case FP_CLK:
	case FP_IRQ:
		/* Native ABI advertises sizeof(int) but reads exactly a u16. */
		if (copy_from_user(&small, ptr, sizeof(small))) { ret = -EFAULT; break; }
		ret = cmd == FP_CLK ? fp_clock(fp, small) : fp_set_irq(fp, small != 0);
		break;
	case FP_SIGNAL:
		ret = fp_signal(fp, ptr);
		break;
	case FP_SUSPEND:
		gpio_set_value_cansleep(fp->sleep_gpio, 0);
		break;
	case FP_POWER_ON:
		fp_power(fp, true);
		break;
	case FP_POWER_OFF:
		fp_power(fp, false);
		break;
	case FP_CLK_OFF:
		fp_clock_off(fp);
		break;
	case FP_CONFIG:
	case FP_UNCONFIG:
	case FP_POWER_CONTROL:
		/* Native secure vfs7xxx has these exact intentionally empty cases. */
		break;
	case FP_ORIENT:
		if (copy_to_user(ptr, &fp->orientation, sizeof(fp->orientation))) ret = -EFAULT;
		break;
	case FP_BOOST:
	case FP_TYPE:
	case FP_LOCKSCREEN:
		if (copy_from_user(&value, ptr, sizeof(value))) { ret = -EFAULT; break; }
		if (cmd == FP_TYPE) {
			/* Legacy invalid inputs reset UNKNOWN and still return success. */
			WRITE_ONCE(fp->sensor_type,
				   (int)value >= -1 && (int)value <= 3 ? (int)value : -1);
		} else if (cmd == FP_LOCKSCREEN) {
			/* The native vfs7xxx ABI treats every nonzero u32 as true. */
			WRITE_ONCE(fp->lockscreen, value != 0);
		} else {
			if (value > 1) { ret = -EINVAL; break; }
			ret = fp_boost(fp, value);
		}
		break;
	default:
		ret = -ENOTTY; /* Includes raw SPI messages; no nonsecure transfer ABI. */
	}
out:
	mutex_unlock(&fp->lock);
	return ret;
}

static void fp_free(struct kref *ref)
{
	struct c5_fp *fp = container_of(ref, struct c5_fp, refs);

	put_pid(fp->signal_pid);
	kfree(fp);
}

static int fp_open(struct inode *inode, struct file *file)
{
	struct c5_fp *fp = container_of(file->private_data, struct c5_fp, misc);
	int ret = 0;

	mutex_lock(&fp->lock);
	if (fp->gone) ret = -ENODEV;
	else if (fp->opened) ret = -EBUSY;
	else {
		fp_power(fp, true);
		msleep(100);
		fp->opened = true;
		kref_get(&fp->refs);
		file->private_data = fp;
		nonseekable_open(inode, file);
	}
	mutex_unlock(&fp->lock);
	return ret;
}

static int fp_release(struct inode *inode, struct file *file)
{
	struct c5_fp *fp = file->private_data;
	struct pid *old;
	unsigned long flags;

	mutex_lock(&fp->lock);
	if (!fp->gone) {
		fp_set_irq(fp, false);
		fp_clock_off(fp);
		fp_boost(fp, false);
		fp_power(fp, false);
	}
	spin_lock_irqsave(&fp->signal_lock, flags);
	old = fp->signal_pid;
	fp->signal_pid = NULL;
	fp->signal_nr = 0;
	spin_unlock_irqrestore(&fp->signal_lock, flags);
	put_pid(old);
	fp->opened = false;
	mutex_unlock(&fp->lock);
	kref_put(&fp->refs, fp_free);
	return 0;
}

#ifdef CONFIG_COMPAT
static long fp_compat(struct file *file, unsigned int cmd, unsigned long arg)
{
	return fp_ioctl(file, cmd, (unsigned long)compat_ptr(arg));
}
#endif
static const struct file_operations fp_fops = {
	.owner = THIS_MODULE, .open = fp_open, .release = fp_release,
	.unlocked_ioctl = fp_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = fp_compat,
#endif
	.llseek = no_llseek,
};

static ssize_t type_check_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct c5_fp *fp = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n", READ_ONCE(fp->sensor_type));
}
static ssize_t vendor_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "SYNAPTICS\n");
}
static ssize_t name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "VIPER\n");
}
static ssize_t home_wake_diag_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct c5_fp *fp = dev_get_drvdata(dev);
	struct c5_fp_home_state keys;
	ssize_t size;

	mutex_lock(&fp->lock);
	c5_fp_home_snapshot(&keys);
	size = scnprintf(buf, PAGE_SIZE,
		"OPENED=%u LOCKSCREEN=%u ARMED=%u REGISTERED=%u "
		"SUSPENDS=%llu RESUMES=%llu HOME_MATCHES=%llu NOTIFY_ATTEMPTS=%llu "
		"KEY_SUSPENDS=%llu HOME_IRQS=%llu HOME_TAKEN=%llu "
		"KEY_SUSPENDED=%u HOME_PENDING=%u\n",
		fp->opened, fp->lockscreen, atomic_read(&fp->armed), fp_has_signal(fp),
		(unsigned long long)fp->suspend_count,
		(unsigned long long)fp->resume_count,
		(unsigned long long)fp->home_wake_count,
		(unsigned long long)fp->home_notify_attempts,
		(unsigned long long)keys.suspends,
		(unsigned long long)keys.home_irqs,
		(unsigned long long)keys.taken, keys.suspended, keys.pending_home);
	mutex_unlock(&fp->lock);
	return size;
}
static DEVICE_ATTR(home_wake_diag, 0400, home_wake_diag_show, NULL);
static DEVICE_ATTR_RO(type_check);
static DEVICE_ATTR_RO(vendor);
static DEVICE_ATTR_RO(name);
static struct attribute *fp_attrs[] = {
	&dev_attr_type_check.attr, &dev_attr_vendor.attr, &dev_attr_name.attr,
	&dev_attr_home_wake_diag.attr, NULL
};
ATTRIBUTE_GROUPS(fp);

static void fp_cleanup(void *data)
{
	struct c5_fp *fp = data;

	mutex_lock(&fp->lock);
	fp->gone = true;
	fp_set_irq(fp, false);
	free_irq(fp->irq, fp);
	fp_clock_off(fp);
	fp_boost(fp, false);
	fp_power(fp, false);
	mutex_unlock(&fp->lock);
	cpufreq_unregister_notifier(&fp->qos, CPUFREQ_POLICY_NOTIFIER);
	wakeup_source_unregister(fp->wake);
	kref_put(&fp->refs, fp_free);
}

static int fp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct c5_fp *fp;
	int vendor, ret;
	u32 control;

	if (!dev->parent || !dev->parent->of_node ||
	    !of_device_is_compatible(dev->parent->of_node, "qcom,spi-qup-v2"))
		return -ENODEV;
	if (of_property_read_u32(dev->of_node, "vfsspi-ldocontrol", &control) || control != 1)
		return -EINVAL;
	fp = kzalloc(sizeof(*fp), GFP_KERNEL);
	if (!fp)
		return -ENOMEM;
	fp->dev = dev;
	fp->sensor_type = -1;
	kref_init(&fp->refs);
	mutex_init(&fp->lock);
	spin_lock_init(&fp->signal_lock);
	atomic_set(&fp->armed, 0);
	platform_set_drvdata(pdev, fp);
	fp->core = devm_clk_get(dev->parent, "core_clk");
	fp->iface = devm_clk_get(dev->parent, "iface_clk");
	if (IS_ERR(fp->core) || IS_ERR(fp->iface)) {
		ret = IS_ERR(fp->core) ? PTR_ERR(fp->core) : PTR_ERR(fp->iface);
		goto err_free;
	}
	fp->pins = devm_pinctrl_get(dev);
	if (IS_ERR(fp->pins)) { ret = PTR_ERR(fp->pins); goto err_free; }
	fp->idle = pinctrl_lookup_state(fp->pins, "idle");
	fp->sleep = pinctrl_lookup_state(fp->pins, "sleep");
	if (IS_ERR(fp->idle) || IS_ERR(fp->sleep)) {
		ret = IS_ERR(fp->idle) ? PTR_ERR(fp->idle) : PTR_ERR(fp->sleep); goto err_free;
	}
	fp->sleep_gpio = of_get_named_gpio(dev->of_node, "vfsspi-sleepPin", 0);
	fp->drdy_gpio = of_get_named_gpio(dev->of_node, "vfsspi-drdyPin", 0);
	fp->ldo_gpio = of_get_named_gpio(dev->of_node, "vfsspi-ldoPin", 0);
	vendor = of_get_named_gpio(dev->of_node, "vfsspi-vendorPin", 0);
	if (fp->sleep_gpio < 0 || fp->drdy_gpio < 0 || fp->ldo_gpio < 0 || vendor < 0) {
		ret = min(min(fp->sleep_gpio, fp->drdy_gpio), min(fp->ldo_gpio, vendor)); goto err_free;
	}
	ret = devm_gpio_request_one(dev, fp->ldo_gpio, GPIOF_OUT_INIT_LOW, "vfsspi_ldo");
	if (ret) goto err_free;
	ret = devm_gpio_request_one(dev, fp->sleep_gpio, GPIOF_OUT_INIT_LOW, "vfsspi_sleep");
	if (ret) goto err_free;
	ret = devm_gpio_request_one(dev, fp->drdy_gpio, GPIOF_IN, "vfsspi_drdy");
	if (ret) goto err_free;
	ret = devm_gpio_request_one(dev, vendor, GPIOF_IN, "vfsspi_vendor");
	if (ret) goto err_free;
	/* Native dualization: power, wait10ms, read vendor; zero selects Viper. */
	fp_power(fp, true);
	msleep(10);
	ret = gpio_get_value_cansleep(vendor);
	fp_power(fp, false);
	dev_info(dev, "C5 fingerprint vendor-select=%d (0=Viper,1=Egis)\n", ret);
	if (ret != 0) { ret = ret < 0 ? ret : -ENODEV; goto err_free; }
	fp->irq = gpio_to_irq(fp->drdy_gpio);
	if (fp->irq < 0) { ret = fp->irq; goto err_free; }
	of_property_read_u32(dev->of_node, "vfsspi-orient", &fp->orientation);
	of_property_read_u32(dev->of_node, "vfsspi-min_cpufeq_limit", &fp->boost_khz);
	fp->wake = wakeup_source_register("vfsspi_wake_lock");
	if (!fp->wake) { ret = -ENOMEM; goto err_free; }
	fp->qos.notifier_call = fp_qos;
	ret = cpufreq_register_notifier(&fp->qos, CPUFREQ_POLICY_NOTIFIER);
	if (ret) goto err_wake;
	ret = request_irq(fp->irq, fp_irq, IRQF_TRIGGER_RISING, "vfsspi_irq", fp);
	if (ret) goto err_qos;
	disable_irq(fp->irq);
	ret = devm_add_action_or_reset(dev, fp_cleanup, fp);
	if (ret) goto err_irq_only;
	fp->misc.minor = MISC_DYNAMIC_MINOR;
	fp->misc.name = "vfsspi";
	fp->misc.fops = &fp_fops;
	fp->misc.parent = dev;
	fp->misc.mode = 0660;
	ret = misc_register(&fp->misc);
	if (ret) return ret;
	fp->sysfs = device_create_with_groups(fp_class, dev, 0, fp, fp_groups, "fingerprint");
	if (IS_ERR(fp->sysfs)) {
		ret = PTR_ERR(fp->sysfs); misc_deregister(&fp->misc); return ret;
	}
	dev_info(dev, "C5 secure vfsspi runtime ready; TEE transfer/authentication unchanged\n");
	return 0;
err_irq_only:
	return ret;
err_qos:
	cpufreq_unregister_notifier(&fp->qos, CPUFREQ_POLICY_NOTIFIER);
err_wake:
	wakeup_source_unregister(fp->wake);
err_free:
	kref_put(&fp->refs, fp_free);
	return ret;
}

static int fp_remove(struct platform_device *pdev)
{
	struct c5_fp *fp = platform_get_drvdata(pdev);

	device_unregister(fp->sysfs);
	misc_deregister(&fp->misc);
	return 0; /* Managed cleanup marks gone before its GPIO resources disappear. */
}

static int fp_suspend(struct device *dev)
{
	struct c5_fp *fp = dev_get_drvdata(dev);

	mutex_lock(&fp->lock);
	if (!fp->gone) {
		fp->suspend_count++;
		fp_power(fp, false);
	}
	mutex_unlock(&fp->lock);
	return 0;
}
static int fp_resume(struct device *dev)
{
	struct c5_fp *fp = dev_get_drvdata(dev);

	mutex_lock(&fp->lock);
	if (!fp->gone) {
		fp->resume_count++;
		fp_home_resume_notify(fp);
		if (fp->opened) fp_power(fp, true);
	}
	mutex_unlock(&fp->lock);
	return 0;
}
static SIMPLE_DEV_PM_OPS(fp_pm, fp_suspend, fp_resume);
static const struct of_device_id fp_match[] = { { .compatible = "vfsspi,vfs7xxx" }, {} };
static struct platform_driver fp_driver = {
	.probe = fp_probe, .remove = fp_remove,
	.driver = { .name = "c5-vfsspi-secure", .of_match_table = fp_match, .pm = &fp_pm },
};

/* The accepted secure SPI probe returns before register/BAM initialization.
 * Publish only that exact controller's children, never a normal-world SPI master.
 */
static int fp_host_probe(struct platform_device *pdev)
{
	struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	struct device_node *child;

	if (!res || res->start != 0x078b5000 || resource_size(res) != 0x600)
		return -ENODEV;
	child = of_get_compatible_child(pdev->dev.of_node, "vfsspi,vfs7xxx");
	if (!child)
		return -ENODEV;
	of_node_put(child);
	return of_platform_populate(pdev->dev.of_node, NULL, NULL, &pdev->dev);
}
static int fp_host_remove(struct platform_device *pdev)
{
	of_platform_depopulate(&pdev->dev);
	return 0;
}
static const struct of_device_id fp_host_match[] = { { .compatible = "qcom,spi-qup-v2" }, {} };
static struct platform_driver fp_host = {
	.probe = fp_host_probe, .remove = fp_host_remove,
	.driver = { .name = "c5-secure-spi-host", .of_match_table = fp_host_match },
};
static int __init fp_init(void)
{
	int ret;

	fp_class = class_create(THIS_MODULE, "fingerprint");
	if (IS_ERR(fp_class)) return PTR_ERR(fp_class);
	ret = platform_driver_register(&fp_driver);
	if (ret) goto err_class;
	ret = platform_driver_register(&fp_host);
	if (!ret) return 0;
	platform_driver_unregister(&fp_driver);
err_class:
	class_destroy(fp_class);
	return ret;
}
device_initcall(fp_init);
MODULE_DESCRIPTION("C5 VFS secure runtime control only; native TEE owns biometrics");
MODULE_LICENSE("GPL");
