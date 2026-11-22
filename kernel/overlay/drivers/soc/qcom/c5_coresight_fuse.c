/* SPDX-License-Identifier: GPL-2.0-only */
/* C5 fuse-v2 policy. Primary queries use RAM; optional old APIs stay lazy. */
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include "c5_coresight_fuse_policy.h"
#if defined(CONFIG_C5_LEGACY_CORESIGHT) && defined(CONFIG_CORESIGHT_FUSE)
#include <linux/coresight.h>
#include <linux/of_coresight.h>
#endif

struct c5_fuse_maps {
	void __iomem *primary;
	void __iomem *nidnt;
	void __iomem *qpdi;
#if defined(CONFIG_C5_LEGACY_CORESIGHT) && defined(CONFIG_CORESIGHT_FUSE)
	struct coresight_device *csdev;
#endif
};

static DEFINE_MUTEX(c5_fuse_lock);
static struct c5_fuse_maps __rcu *c5_fuse_active;
static struct c5_fuse_policy c5_fuse_policy;
static unsigned int c5_fuse_attempts;
static unsigned int c5_fuse_ready;
static unsigned int c5_fuse_version = 2;
static unsigned int c5_fuse_nidnt_present;
static unsigned int c5_fuse_qpdi_present;
static int c5_fuse_result = -EAGAIN;

static int c5_fuse_reject(const char *value, const struct kernel_param *kp)
{
	return -EPERM;
}

static int c5_fuse_uint_get(char *buffer, const struct kernel_param *kp)
{
	(void)smp_load_acquire(&c5_fuse_ready);
	return param_get_uint(buffer, kp);
}

static int c5_fuse_int_get(char *buffer, const struct kernel_param *kp)
{
	(void)smp_load_acquire(&c5_fuse_ready);
	return param_get_int(buffer, kp);
}

static const struct kernel_param_ops c5_fuse_uint_ops = {
	.set = c5_fuse_reject,
	.get = c5_fuse_uint_get,
};
static const struct kernel_param_ops c5_fuse_int_ops = {
	.set = c5_fuse_reject,
	.get = c5_fuse_int_get,
};
module_param_cb(init_attempts, &c5_fuse_uint_ops, &c5_fuse_attempts, 0400);
module_param_cb(init_result, &c5_fuse_int_ops, &c5_fuse_result, 0400);
module_param_cb(ready, &c5_fuse_uint_ops, &c5_fuse_ready, 0400);
module_param_cb(version, &c5_fuse_uint_ops, &c5_fuse_version, 0400);
module_param_cb(all_disabled, &c5_fuse_uint_ops,
		&c5_fuse_policy.all_disabled, 0400);
module_param_cb(apps_disabled, &c5_fuse_uint_ops,
		&c5_fuse_policy.apps_disabled, 0400);
module_param_cb(nidnt_present, &c5_fuse_uint_ops, &c5_fuse_nidnt_present, 0400);
module_param_cb(qpdi_present, &c5_fuse_uint_ops, &c5_fuse_qpdi_present, 0400);

bool c5_coresight_fuse_ready(void)
{
	return smp_load_acquire(&c5_fuse_ready) != 0;
}
EXPORT_SYMBOL(c5_coresight_fuse_ready);

bool coresight_fuse_access_disabled(void)
{
	if (!c5_coresight_fuse_ready())
		return true;
	return READ_ONCE(c5_fuse_policy.all_disabled) != 0;
}
EXPORT_SYMBOL(coresight_fuse_access_disabled);

bool coresight_fuse_apps_access_disabled(void)
{
	if (!c5_coresight_fuse_ready())
		return true;
	return READ_ONCE(c5_fuse_policy.apps_disabled) != 0;
}
EXPORT_SYMBOL(coresight_fuse_apps_access_disabled);

bool coresight_fuse_nidnt_access_disabled(void)
{
	struct c5_fuse_maps *maps;
	bool disabled = true;

	if (!c5_coresight_fuse_ready())
		return true;
	rcu_read_lock();
	maps = rcu_dereference(c5_fuse_active);
	if (maps)
		disabled = maps->nidnt ?
			c5_fuse_nidnt_v2(__raw_readl(maps->nidnt)) : false;
	rcu_read_unlock();
	return disabled;
}
EXPORT_SYMBOL(coresight_fuse_nidnt_access_disabled);

bool coresight_fuse_qpdi_access_disabled(void)
{
	struct c5_fuse_maps *maps;
	bool disabled = true;

	if (!c5_coresight_fuse_ready())
		return true;
	rcu_read_lock();
	maps = rcu_dereference(c5_fuse_active);
	if (maps)
		disabled = maps->qpdi ?
			c5_fuse_qpdi_v2(__raw_readl(maps->qpdi)) : false;
	rcu_read_unlock();
	return disabled;
}
EXPORT_SYMBOL(coresight_fuse_qpdi_access_disabled);

#if defined(CONFIG_C5_LEGACY_CORESIGHT) && defined(CONFIG_CORESIGHT_FUSE)
static unsigned int c5_fuse_core_registered;
static int c5_fuse_core_result = -EAGAIN;
module_param_cb(core_registered, &c5_fuse_uint_ops, &c5_fuse_core_registered, 0400);
module_param_cb(core_result, &c5_fuse_int_ops, &c5_fuse_core_result, 0400);

static int c5_fuse_attach_core(struct platform_device *pdev,
			       struct c5_fuse_maps *maps)
{
	struct coresight_desc desc = { 0 };
	struct coresight_platform_data *pdata;

	pdata = of_get_coresight_platform_data(&pdev->dev, pdev->dev.of_node);
	if (IS_ERR_OR_NULL(pdata)) {
		c5_fuse_core_result = pdata ? PTR_ERR(pdata) : -ENODEV;
		return c5_fuse_core_result;
	}
	desc.type = CORESIGHT_DEV_TYPE_NONE;
	desc.pdata = pdata;
	desc.dev = &pdev->dev;
	desc.owner = THIS_MODULE;
	maps->csdev = coresight_register(&desc);
	if (IS_ERR_OR_NULL(maps->csdev)) {
		c5_fuse_core_result = maps->csdev ? PTR_ERR(maps->csdev) : -ENODEV;
		maps->csdev = NULL;
		return c5_fuse_core_result;
	}
	c5_fuse_core_result = 0;
	c5_fuse_core_registered = 1;
	return 0;
}

static void c5_fuse_detach_core(struct c5_fuse_maps *maps)
{
	if (maps->csdev) {
		c5_fuse_core_registered = 0;
		c5_fuse_core_result = -ENODEV;
		coresight_unregister(maps->csdev);
		maps->csdev = NULL;
	}
}
#else
static inline int c5_fuse_attach_core(struct platform_device *pdev,
				      struct c5_fuse_maps *maps)
{
	return 0;
}
static inline void c5_fuse_detach_core(struct c5_fuse_maps *maps) { }
#endif

static void c5_fuse_free(struct c5_fuse_maps *maps)
{
	if (!maps)
		return;
	c5_fuse_detach_core(maps);
	if (maps->qpdi)
		iounmap(maps->qpdi);
	if (maps->nidnt)
		iounmap(maps->nidnt);
	if (maps->primary)
		iounmap(maps->primary);
	kfree(maps);
}

static int c5_fuse_resource(struct platform_device *pdev, const char *name,
			    resource_size_t address, resource_size_t bytes,
			    bool required, struct resource **out)
{
	struct resource *res;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, name);
	if (!res) {
		*out = NULL;
		return required ? -ENODEV : 0;
	}
	if (res->start != address || resource_size(res) != bytes)
		return -ERANGE;
	*out = res;
	return 0;
}

static int c5_fuse_probe(struct platform_device *pdev)
{
	struct resource *primary, *nidnt, *qpdi;
	struct c5_fuse_maps *maps = NULL;
	struct c5_fuse_policy policy;
	u32 config0, config1;
	int ret;

	mutex_lock(&c5_fuse_lock);
	if (rcu_access_pointer(c5_fuse_active)) {
		ret = -EBUSY;
		goto out;
	}
	c5_fuse_attempts++;
	c5_fuse_result = -EINPROGRESS;
	ret = c5_fuse_resource(pdev, "fuse-base", 0x5e01c, 8,
			       true, &primary);
	if (ret)
		goto failed;
	ret = c5_fuse_resource(pdev, "nidnt-fuse-base", 0x58040, 4,
			       false, &nidnt);
	if (ret)
		goto failed;
	ret = c5_fuse_resource(pdev, "qpdi-fuse-base", 0x5e00c, 4,
			       false, &qpdi);
	if (ret)
		goto failed;

	maps = kzalloc(sizeof(*maps), GFP_KERNEL);
	if (!maps) {
		ret = -ENOMEM;
		goto failed;
	}
	/* The two existing fuse nodes share read-only words; do not claim them. */
	maps->primary = ioremap(primary->start, resource_size(primary));
	if (!maps->primary) {
		ret = -ENOMEM;
		goto failed;
	}
	if (nidnt) {
		maps->nidnt = ioremap(nidnt->start, resource_size(nidnt));
		if (!maps->nidnt) {
			ret = -ENOMEM;
			goto failed;
		}
	}
	if (qpdi) {
		maps->qpdi = ioremap(qpdi->start, resource_size(qpdi));
		if (!maps->qpdi) {
			ret = -ENOMEM;
			goto failed;
		}
	}

	/* Match the original v2 primary order. Optional words stay unread. */
	config0 = __raw_readl(maps->primary);
	config1 = __raw_readl(maps->primary + 4);
	(void)config0;
	c5_fuse_decode_v2(config1, &policy);
	ret = c5_fuse_attach_core(pdev, maps);
	if (ret)
		goto failed;
	c5_fuse_policy = policy;
	c5_fuse_nidnt_present = !!maps->nidnt;
	c5_fuse_qpdi_present = !!maps->qpdi;
	c5_fuse_result = 0;
	platform_set_drvdata(pdev, maps);
	rcu_assign_pointer(c5_fuse_active, maps);
	smp_store_release(&c5_fuse_ready, 1);
	ret = 0;
	goto out;
failed:
	c5_fuse_free(maps);
	c5_fuse_result = ret;
out:
	mutex_unlock(&c5_fuse_lock);
	return ret;
}

static int c5_fuse_remove(struct platform_device *pdev)
{
	struct c5_fuse_maps *maps;

	mutex_lock(&c5_fuse_lock);
	maps = platform_get_drvdata(pdev);
	if (maps && rcu_access_pointer(c5_fuse_active) == maps) {
		smp_store_release(&c5_fuse_ready, 0);
		rcu_assign_pointer(c5_fuse_active, NULL);
		c5_fuse_result = -ENODEV;
		/* Optional readers finish before any mapping is released. */
		synchronize_rcu();
		c5_fuse_free(maps);
		platform_set_drvdata(pdev, NULL);
	}
	mutex_unlock(&c5_fuse_lock);
	return 0;
}

static const struct of_device_id c5_fuse_match[] = {
	{ .compatible = "arm,coresight-fuse-v2" },
	{},
};
MODULE_DEVICE_TABLE(of, c5_fuse_match);
static struct platform_driver c5_fuse_driver = {
	.probe = c5_fuse_probe,
	.remove = c5_fuse_remove,
	.driver = {
		.name = "coresight-fuse",
		.of_match_table = c5_fuse_match,
	},
};
module_platform_driver(c5_fuse_driver);
MODULE_DESCRIPTION("C5 read-only fuse-v2 policy and cached primary diagnostics");
MODULE_LICENSE("GPL v2");
