// SPDX-License-Identifier: GPL-2.0-only
/* Private C5 v2 query provider. No image selection or SEC_PM_DEBUG hook. */
#include <linux/debugfs.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <asm/arch_timer.h>
#include "c5_rpm_stats_format.h"

enum c5_rpm_kind { C5_RPM_STATS, C5_RPM_HEAP, C5_RPM_MASTER };

struct c5_rpm_state;
struct c5_rpm_endpoint {
	struct c5_rpm_state *state;
	enum c5_rpm_kind kind;
};

struct c5_rpm_state {
	struct kref ref;
	void __iomem *base;
	void __iomem *heap;
	u32 stride;
	u32 masters;
	char *names[32];
	struct c5_rpm_endpoint primary;
	struct c5_rpm_endpoint heap_endpoint;
	struct dentry *stats_dentry;
	struct dentry *heap_dentry;
	struct kobject *kobj;
	struct kobj_attribute attr;
	bool attr_created;
#ifdef CONFIG_C5_PM_RPM_MASTER_PRINT
	struct class *print_class;
	struct device *print_device;
	bool print_enable_attr;
	bool print_probe_attr;
#endif
};

struct c5_rpm_query {
	struct c5_rpm_state *state;
	struct mutex lock;
	enum c5_rpm_kind kind;
	u32 next;
	size_t length;
	char buf[2048];
};

/* Keep the firmware's legacy physical-counter time domain local to C5. */
static inline u64 c5_rpm_legacy_counter(void)
{
	isb();
	return read_sysreg(cntpct_el0);
}

static void c5_rpm_free(struct kref *ref)
{
	struct c5_rpm_state *s = container_of(ref, struct c5_rpm_state, ref);
	unsigned int i;

	if (s->heap)
		iounmap(s->heap);
	if (s->base)
		iounmap(s->base);
	for (i = 0; i < ARRAY_SIZE(s->names); i++)
		kfree(s->names[i]);
	kfree(s);
}

static void c5_rpm_read_stats(void __iomem *p, struct c5_rpm_v2 *r)
{
	r->stat_type = readl_relaxed(p + offsetof(struct c5_rpm_v2, stat_type));
	r->count = readl_relaxed(p + offsetof(struct c5_rpm_v2, count));
	memcpy_fromio(&r->last_entered_at, p + offsetof(struct c5_rpm_v2, last_entered_at), 8);
	memcpy_fromio(&r->last_exited_at, p + offsetof(struct c5_rpm_v2, last_exited_at), 8);
	memcpy_fromio(&r->accumulated, p + offsetof(struct c5_rpm_v2, accumulated), 8);
	r->client_votes = readl_relaxed(p + offsetof(struct c5_rpm_v2, client_votes));
}

static __always_inline void c5_rpm_read_master(void __iomem *p, struct c5_rpm_master_v2 *r)
{
	/* Preserve the old v2 field read order and 32/64-bit access widths. */
	r->shutdown_req = readq_relaxed(p + offsetof(struct c5_rpm_master_v2, shutdown_req));
	r->wakeup_ind = readq_relaxed(p + offsetof(struct c5_rpm_master_v2, wakeup_ind));
	r->bringup_req = readq_relaxed(p + offsetof(struct c5_rpm_master_v2, bringup_req));
	r->bringup_ack = readq_relaxed(p + offsetof(struct c5_rpm_master_v2, bringup_ack));
	r->last_sleep_transition_duration = readl_relaxed(p + offsetof(struct c5_rpm_master_v2, last_sleep_transition_duration));
	r->last_wake_transition_duration = readl_relaxed(p + offsetof(struct c5_rpm_master_v2, last_wake_transition_duration));
	r->count = readl_relaxed(p + offsetof(struct c5_rpm_master_v2, count));
	r->last_entered_at = readq_relaxed(p + offsetof(struct c5_rpm_master_v2, last_entered_at));
	r->last_exited_at = readq_relaxed(p + offsetof(struct c5_rpm_master_v2, last_exited_at));
	r->accumulated_duration = readq_relaxed(p + offsetof(struct c5_rpm_master_v2, accumulated_duration));
	r->wakeup_reason = readl_relaxed(p + offsetof(struct c5_rpm_master_v2, wakeup_reason));
	r->numshutdowns = readl_relaxed(p + offsetof(struct c5_rpm_master_v2, numshutdowns));
	r->active_cores = readl_relaxed(p + offsetof(struct c5_rpm_master_v2, active_cores));
}

#ifdef CONFIG_C5_PM_RPM_MASTER_PRINT
#include "c5_rpm_master_print.inc"
#endif

static int c5_rpm_fill(struct c5_rpm_query *q)
{
	struct c5_rpm_state *s = q->state;
	int ret;
	size_t length = 0;
	unsigned int i;

	if (q->kind == C5_RPM_MASTER) {
		struct c5_rpm_master_v2 record;

		if (q->next >= s->masters)
			return 0;
		c5_rpm_read_master(s->base + (size_t)q->next * s->stride, &record);
		ret = c5_rpm_format_master(q->buf, sizeof(q->buf), s->names[q->next], &record);
		if (ret > 0)
			q->next++;
		return ret;
	}
	if (q->next)
		return 0;
	if (q->kind == C5_RPM_HEAP) {
		if (!s->heap)
			return -ENODEV;
		ret = snprintf(q->buf, sizeof(q->buf), "RPM FREE HEAP SPACE is 0x%x Bytes\n", readl_relaxed(s->heap));
		if (ret < 0 || ret >= sizeof(q->buf))
			return -EOVERFLOW;
		q->next = 1;
		return ret;
	}
	for (i = 0; i < 2; i++) {
		struct c5_rpm_v2 record;

		c5_rpm_read_stats(s->base + i * sizeof(record), &record);
		ret = c5_rpm_format_stats(q->buf + length, sizeof(q->buf) - length,
					  &record, c5_rpm_legacy_counter());
		if (ret < 0)
			return ret;
		length += ret;
	}
	q->next = 1;
	return length;
}

static int c5_rpm_open(struct inode *inode, struct file *file)
{
	struct c5_rpm_endpoint *endpoint = inode->i_private;
	struct c5_rpm_query *q;

	/* debugfs_create_file's full proxy protects this callback from removal. */
	q = kzalloc(sizeof(*q), GFP_KERNEL);
	if (!q)
		return -ENOMEM;
	kref_get(&endpoint->state->ref);
	q->state = endpoint->state;
	q->kind = endpoint->kind;
	mutex_init(&q->lock);
	file->private_data = q;
	return nonseekable_open(inode, file);
}

static ssize_t c5_rpm_read(struct file *file, char __user *buf,
			 size_t count, loff_t *position)
{
	struct c5_rpm_query *q = file->private_data;
	ssize_t ret;

	if (!count)
		return 0;
	if (*position < 0)
		return -EINVAL;
	mutex_lock(&q->lock);
	if (*position >= q->length) {
		ret = c5_rpm_fill(q);
		if (ret <= 0)
			goto out;
		q->length = ret;
		*position = 0;
	}
	ret = simple_read_from_buffer(buf, count, position, q->buf, q->length);
out:
	mutex_unlock(&q->lock);
	return ret;
}

static int c5_rpm_close(struct inode *inode, struct file *file)
{
	struct c5_rpm_query *q = file->private_data;

	file->private_data = NULL;
	kref_put(&q->state->ref, c5_rpm_free);
	kfree(q);
	return 0;
}

static const struct file_operations c5_rpm_fops = {
	.owner = THIS_MODULE,
	.open = c5_rpm_open,
	.read = c5_rpm_read,
	.release = c5_rpm_close,
	.llseek = no_llseek,
};

static ssize_t c5_rpm_sysfs_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	struct c5_rpm_state *s = container_of(attr, struct c5_rpm_state, attr);
	struct c5_rpm_query *q;
	int ret;

	q = kzalloc(sizeof(*q), GFP_KERNEL);
	if (!q)
		return -ENOMEM;
	q->state = s;
	q->kind = C5_RPM_STATS;
	ret = c5_rpm_fill(q);
	if (ret > PAGE_SIZE)
		ret = -EOVERFLOW;
	if (ret > 0)
		memcpy(buf, q->buf, ret);
	kfree(q);
	return ret;
}

static void c5_rpm_unpublish(struct c5_rpm_state *s)
{
#ifdef CONFIG_C5_PM_RPM_MASTER_PRINT
	c5_rpm_master_print_unpublish(s);
#endif
	/* Do not hold a callback's mutex while removal waits for active users. */
	if (s->attr_created)
		sysfs_remove_file(s->kobj, &s->attr.attr);
	if (s->kobj)
		kobject_put(s->kobj);
	debugfs_remove(s->heap_dentry);
	debugfs_remove(s->stats_dentry);
	kref_put(&s->ref, c5_rpm_free);
}

static int c5_rpm_memory_resource(struct resource *res, unsigned int minimum,
				unsigned int alignment)
{
	if (!res || resource_type(res) != IORESOURCE_MEM ||
	    res->end < res->start || res->start > 0xffffffffULL ||
	    (res->start & (alignment - 1)) || resource_size(res) < minimum ||
	    res->end > 0xffffffffULL)
		return -EINVAL;
	return 0;
}

static int c5_rpm_publish(struct c5_rpm_state *s, bool master)
{
	int ret;

	s->primary.state = s;
	s->primary.kind = master ? C5_RPM_MASTER : C5_RPM_STATS;
	s->stats_dentry = debugfs_create_file(master ? "rpm_master_stats" : "rpm_stats",
					    0444, NULL, &s->primary, &c5_rpm_fops);
	if (IS_ERR_OR_NULL(s->stats_dentry)) {
		ret = s->stats_dentry ? PTR_ERR(s->stats_dentry) : -ENOMEM;
		s->stats_dentry = NULL;
		return ret;
	}
	if (master)
		return 0;
	if (s->heap) {
		s->heap_endpoint.state = s;
		s->heap_endpoint.kind = C5_RPM_HEAP;
		s->heap_dentry = debugfs_create_file("rpm_heap", 0444, NULL,
						  &s->heap_endpoint, &c5_rpm_fops);
		if (IS_ERR_OR_NULL(s->heap_dentry)) {
			ret = s->heap_dentry ? PTR_ERR(s->heap_dentry) : -ENOMEM;
			s->heap_dentry = NULL;
			return ret;
		}
	}
	s->kobj = kobject_create_and_add("system_sleep", power_kobj);
	if (!s->kobj)
		return -ENOMEM;
	sysfs_attr_init(&s->attr.attr);
	s->attr.attr.name = "stats";
	s->attr.attr.mode = 0444;
	s->attr.show = c5_rpm_sysfs_show;
	ret = sysfs_create_file(s->kobj, &s->attr.attr);
	if (!ret)
		s->attr_created = true;
	return ret;
}

static int c5_rpm_stats_probe(struct platform_device *pdev)
{
	struct c5_rpm_state *s;
	struct resource *res, *offset_res, *heap_res;
	void __iomem *offset_map;
	u64 address;
	u32 version, offset = 0;
	int ret;

	ret = of_property_read_u32(pdev->dev.of_node, "qcom,sleep-stats-version", &version);
	if (ret || version != 2)
		return -EINVAL;
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "phys_addr_base");
	ret = c5_rpm_memory_resource(res, 2 * sizeof(struct c5_rpm_v2), 4);
	if (ret)
		return ret;
	offset_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "offset_addr");
	if (offset_res) {
		ret = c5_rpm_memory_resource(offset_res, 4, 4);
		if (ret)
			return ret;
		offset_map = ioremap_nocache(offset_res->start, 4);
		if (!offset_map)
			return -ENOMEM;
		offset = readl_relaxed(offset_map);
		iounmap(offset_map);
	}
	ret = c5_rpm_stats_extent(res->start, resource_size(res), offset, 2, &address);
	if (ret)
		return ret;
	heap_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "heap_phys_addrbase");
	if (heap_res) {
		ret = c5_rpm_memory_resource(heap_res, 4, 4);
		if (ret)
			return ret;
	}
	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	kref_init(&s->ref);
	s->base = ioremap_nocache(address, resource_size(res));
	if (!s->base) {
		ret = -ENOMEM;
		goto fail;
	}
	if (heap_res) {
		s->heap = ioremap_nocache(heap_res->start, 4);
		if (!s->heap) {
			ret = -ENOMEM;
			goto fail;
		}
	}
	ret = c5_rpm_publish(s, false);
	if (ret)
		goto fail;
	platform_set_drvdata(pdev, s);
	return 0;
fail:
	c5_rpm_unpublish(s);
	return ret;
}

static int c5_rpm_master_probe(struct platform_device *pdev)
{
	struct c5_rpm_state *s;
	struct resource *res;
	u32 version, stride;
	int masters, ret, i;

	if (of_property_read_u32(pdev->dev.of_node, "qcom,master-stats-version", &version) || version != 2 ||
	    of_property_read_u32(pdev->dev.of_node, "qcom,master-offset", &stride))
		return -EINVAL;
	masters = of_property_count_strings(pdev->dev.of_node, "qcom,masters");
	if (masters <= 0 || masters > 32)
		return -EINVAL;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	ret = c5_rpm_memory_resource(res, sizeof(struct c5_rpm_master_v2), 8);
	if (ret)
		return ret;
	ret = c5_rpm_master_extent(res->start, resource_size(res), stride, masters);
	if (ret)
		return ret;
	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	kref_init(&s->ref);
	s->stride = stride;
	s->masters = masters;
	for (i = 0; i < masters; i++) {
		const char *name;

		ret = of_property_read_string_index(pdev->dev.of_node, "qcom,masters", i, &name);
		if (ret || !*name || strnlen(name, 32) == 32) {
			ret = -EINVAL;
			goto fail;
		}
		s->names[i] = kstrdup(name, GFP_KERNEL);
		if (!s->names[i]) {
			ret = -ENOMEM;
			goto fail;
		}
	}
	s->base = ioremap_nocache(res->start, resource_size(res));
	if (!s->base) {
		ret = -ENOMEM;
		goto fail;
	}
	ret = c5_rpm_publish(s, true);
	if (ret)
		goto fail;
#ifdef CONFIG_C5_PM_RPM_MASTER_PRINT
	ret = c5_rpm_master_print_publish(s);
	if (ret)
		goto fail;
#endif
	platform_set_drvdata(pdev, s);
	return 0;
fail:
	c5_rpm_unpublish(s);
	return ret;
}

static int c5_rpm_remove(struct platform_device *pdev)
{
	struct c5_rpm_state *s = platform_get_drvdata(pdev);

	platform_set_drvdata(pdev, NULL);
	if (s)
		c5_rpm_unpublish(s);
	return 0;
}

static const struct of_device_id c5_rpm_stats_match[] = {
	{ .compatible = "qcom,rpm-stats" }, {}
};
static const struct of_device_id c5_rpm_master_match[] = {
	{ .compatible = "qcom,rpm-master-stats" }, {}
};
static struct platform_driver c5_rpm_stats_driver = {
	.probe = c5_rpm_stats_probe,
	.remove = c5_rpm_remove,
	.driver = { .name = "msm_rpm_stat", .of_match_table = c5_rpm_stats_match },
};
static struct platform_driver c5_rpm_master_driver = {
	.probe = c5_rpm_master_probe,
	.remove = c5_rpm_remove,
	.driver = { .name = "msm_rpm_master_stats", .of_match_table = c5_rpm_master_match },
};

static int __init c5_rpm_init(void)
{
	int ret;

	BUILD_BUG_ON(sizeof(struct c5_rpm_v2) != 48);
	BUILD_BUG_ON(sizeof(struct c5_rpm_master_v2) != 80);
	ret = platform_driver_register(&c5_rpm_stats_driver);
	if (ret)
		return ret;
	ret = platform_driver_register(&c5_rpm_master_driver);
	if (ret)
		platform_driver_unregister(&c5_rpm_stats_driver);
	return ret;
}

static void __exit c5_rpm_exit(void)
{
	platform_driver_unregister(&c5_rpm_master_driver);
	platform_driver_unregister(&c5_rpm_stats_driver);
}
module_init(c5_rpm_init);
module_exit(c5_rpm_exit);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("C5 v2 RPM statistical query compatibility provider");
