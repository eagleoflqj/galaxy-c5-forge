// SPDX-License-Identifier: GPL-2.0-only
/* Isolated C5 Venus/ION adapter. No old MSM-IOMMU compatibility stubs. */
#ifndef C5_VIDC_HOST_TEST
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/genalloc.h>
#include <linux/iommu.h>
#include <linux/msm_ion.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/init.h>
#include <soc/qcom/scm.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <asm/dma-iommu.h>
#include "msm_vidc_resources.h"
#include "c5-vidc-native.h"

#if !IS_ENABLED(CONFIG_ARM64_DMA_USE_IOMMU) || !IS_ENABLED(CONFIG_IOMMU_API)
#error "C5 VIDC requires the real native ARM DMA/IOMMU implementation"
#endif
#endif
#include "c5-vidc-layout.h"

struct c5_vidc_context {
	struct platform_device *child;
	struct dma_iommu_mapping *arm;
	struct gen_pool *pool[2];
	struct mutex lock;
	struct list_head mappings;
	struct list_head orphan;
	bool poisoned;
	bool revoked;
	bool attached;
};

struct c5_vidc_mapping {
	struct c5_vidc_context *ctx;
	struct dma_buf *buf;
	struct dma_buf_attachment *attachment;
	struct sg_table *sg;
	unsigned long iova, size, mapped;
	unsigned int pool;
	struct list_head link;
};

/* Retain the context and every backing DMA-buf if a short unmap is observed.
 * No range reuse, detach, page release or successful teardown is fabricated.
 */
static LIST_HEAD(c5_vidc_orphans);
static DEFINE_MUTEX(c5_vidc_orphan_lock);

static void c5_vidc_destroy_child(struct platform_device *child)
{
	/* Native of_platform_device_destroy is private. Mirror its cleanup only
	 * for this owned leaf, holding a device reference through unregister.
	 */
	struct device *dev = get_device(&child->dev);
	platform_device_unregister(child);
	/* of_dma_deconfigure is also private; its actual body is this call. */
	arch_teardown_dma_ops(dev);
	of_node_clear_flag(dev->of_node, OF_POPULATED);
	of_node_clear_flag(dev->of_node, OF_POPULATED_BUS);
	put_device(dev);
}

#include "c5-vidc-secure-pool.inc"

static int __init c5_vidc_secure_pool_startup(void)
{
	struct device_node *root, *node;
	struct resource resource;
	const char *model, *firmware;
	u32 id;
	int rc;

	if (!IS_ENABLED(CONFIG_C5_RECOVERY_ONLY) ||
	    !of_machine_is_compatible("qcom,msm8952"))
		return 0;
	root = of_find_node_by_path("/");
	if (!root)
		return -ENODEV;
	rc = of_property_read_string(root, "model", &model);
	if (!rc && strcmp(model, "Samsung C5LTE CHN OPEN PROJECT Rev09"))
		rc = -ENODEV;
	of_node_put(root);
	if (rc)
		return rc;
	/* The native v500 initialization must not already own another pool. */
	node = of_find_compatible_node(NULL, NULL, "qcom,qsmmu-v500");
	if (node) {
		of_node_put(node);
		return -EOPNOTSUPP;
	}
	node = of_find_node_by_path("/soc/qcom,iommu@1e00000");
	if (!node)
		return -ENODEV;
	rc = !of_device_is_available(node) ||
	    !of_device_is_compatible(node, "qcom,smmu-v2") ? -EINVAL :
		of_address_to_resource(node, 0, &resource);
	if (!rc && (resource.start != 0x1e00000 || resource_size(&resource) != 0x40000))
		rc = -EINVAL;
	if (!rc && of_property_count_u32_elems(node, "qcom,iommu-secure-id") != 1)
		rc = -EINVAL;
	if (!rc)
		rc = of_property_read_u32(node, "qcom,iommu-secure-id", &id);
	if (!rc && id != 17)
		rc = -EINVAL;
	of_node_put(node);
	if (rc)
		return rc;
	node = of_find_node_by_path("/soc/qcom,venus@1de0000");
	if (!node)
		return -ENODEV;
	rc = !of_device_is_available(node) ||
	    !of_device_is_compatible(node, "qcom,pil-tz-generic") ||
	    of_property_count_u32_elems(node, "qcom,pas-id") != 1 ? -EINVAL :
		of_property_read_u32(node, "qcom,pas-id", &id);
	if (!rc && id != 9)
		rc = -EINVAL;
	if (!rc)
		rc = of_property_read_string(node, "qcom,firmware-name", &firmware);
	if (!rc && strcmp(firmware, "venus"))
		rc = -EINVAL;
	of_node_put(node);
	if (rc)
		return rc;
	return c5_vidc_secure_pool_init();
}
subsys_initcall(c5_vidc_secure_pool_startup);

static int c5_vidc_exact_policy(struct msm_vidc_platform_resources *res)
{
	struct iommu_info *ns = NULL;
	struct device_node *root;
	const char *hfi, *model;
	unsigned int i, found = 0;
	unsigned int secure_seen = 0;
	static const char * const secure_names[] = {
		"venus_sec_bitstream", "venus_sec_pixel", "venus_sec_non_pixel"
	};
	static const u32 secure_start[] = {0x4b000000, 0x25800000, 0x01000000};
	static const u32 secure_size[] = {0x12c00000, 0x25800000, 0x24800000};
	static const u32 secure_type[] = {0x241, 0x106, 0x480};
	int rc;

	if (!res || !res->pdev ||
	    !of_machine_is_compatible("qcom,msm8952") ||
	    of_property_read_string(res->pdev->dev.of_node, "qcom,hfi", &hfi) ||
	    strcmp(hfi, "venus") || res->use_non_secure_pil || res->ocmem_size)
		return -EOPNOTSUPP;
	root = of_find_node_by_path("/");
	if (!root)
		return -ENODEV;
	rc = of_property_read_string(root, "model", &model);
	if (!rc && strcmp(model, "Samsung C5LTE CHN OPEN PROJECT Rev09"))
		rc = -ENODEV;
	of_node_put(root);
	if (rc)
		return rc;
	if (res->iommu_group_set.count != 4 || !res->iommu_group_set.iommu_maps)
		return -EINVAL;
	for (i = 0; i < res->iommu_group_set.count; i++) {
		struct iommu_info *info = &res->iommu_group_set.iommu_maps[i];
		if (!info->is_secure) {
			ns = info;
			found++;
		} else {
			unsigned int n;
			if (!info->name || info->npartitions != 1)
				return -EINVAL;
			for (n = 0; n < ARRAY_SIZE(secure_names); n++)
				if (!strcmp(info->name, secure_names[n]))
					break;
			if (n == ARRAY_SIZE(secure_names) || (secure_seen & (1U << n)) ||
			    info->addr_range[0].start != secure_start[n] ||
			    info->addr_range[0].size != secure_size[n] ||
			    info->buffer_type[0] != secure_type[n])
				return -EINVAL;
			secure_seen |= 1U << n;
		}
	}
	if (found != 1 || secure_seen != 7 || !ns->name || strcmp(ns->name, "venus_ns") ||
	    ns->npartitions != 2 ||
	    ns->buffer_type[0] != C5_VIDC_DATA_MASK ||
	    ns->buffer_type[1] != C5_VIDC_QUEUE_MASK ||
	    ns->addr_range[0].start != C5_VIDC_DATA_START ||
	    ns->addr_range[0].size != C5_VIDC_DATA_SIZE ||
	    ns->addr_range[1].start != C5_VIDC_QUEUE_START ||
	    ns->addr_range[1].size != C5_VIDC_QUEUE_SIZE)
		return -EINVAL;
	return 0;
}

int c5_vidc_native_init(struct msm_vidc_platform_resources *res)
{
	struct c5_vidc_context *ctx;
	struct device_node *node, *smmu;
	struct of_phandle_args args;
	u32 cb;
	static const u32 sid[] = {0x800, 0x804, 0x807, 0x808};
	static const u32 mask[] = {3, 1, 0, 3};
	int rc, i;

	rc = c5_vidc_exact_policy(res);
	if (rc)
		return rc;
	if (res->native_context)
		return -EBUSY;
	node = of_get_child_by_name(res->pdev->dev.of_node, "c5-native-context");
	if (!node)
		return -ENODEV;
	if (!of_device_is_compatible(node, "qcom,c5-vidc-context") ||
	    of_get_child_count(node) ||
	    of_property_read_bool(node, "qcom,secure-context") ||
	    of_property_count_u32_elems(node, "qcom,fixed-context-bank") != 1 ||
	    of_property_read_u32(node, "qcom,fixed-context-bank", &cb) || cb != 22 ||
	    of_count_phandle_with_args(node, "iommus", "#iommu-cells") != 4) {
		rc = -EINVAL;
		goto put_node;
	}
	smmu = of_find_node_by_path("/soc/qcom,iommu@1e00000");
	if (!smmu) {
		rc = -ENODEV;
		goto put_node;
	}
	for (i = 0; i < ARRAY_SIZE(sid); i++) {
		rc = of_parse_phandle_with_args(node, "iommus", "#iommu-cells", i, &args);
		if (!rc) {
			if (args.np != smmu || args.args_count != 2 ||
			    args.args[0] != sid[i] || args.args[1] != mask[i])
				rc = -EINVAL;
			of_node_put(args.np);
		}
		if (rc)
			break;
	}
	of_node_put(smmu);
	if (rc)
		goto put_node;
	rc = c5_vidc_secure_pool_status();
	if (rc)
		goto put_node;
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		rc = -ENOMEM;
		goto put_node;
	}
	mutex_init(&ctx->lock);
	INIT_LIST_HEAD(&ctx->mappings);
	INIT_LIST_HEAD(&ctx->orphan);
	/* Creating this exact child runs native of_iommu_configure/fwspec parsing. */
	ctx->child = of_platform_device_create(node, NULL, &res->pdev->dev);
	if (!ctx->child) {
		rc = -ENODEV;
		goto free_ctx;
	}
	ctx->child->dev.coherent_dma_mask = DMA_BIT_MASK(32);
	ctx->child->dev.dma_mask = &ctx->child->dev.coherent_dma_mask;
	for (i = 0; i < 2; i++) {
		unsigned long start = i ? C5_VIDC_QUEUE_START : C5_VIDC_DATA_START;
		unsigned long size = i ? C5_VIDC_QUEUE_SIZE : C5_VIDC_DATA_SIZE;
		ctx->pool[i] = gen_pool_create(PAGE_SHIFT, -1);
		if (!ctx->pool[i]) {
			rc = -ENOMEM;
			goto free_pool;
		}
		rc = gen_pool_add(ctx->pool[i], start, size, -1);
		if (rc)
			goto free_pool;
	}
	ctx->arm = arm_iommu_create_mapping(&platform_bus_type,
		C5_VIDC_DATA_START, C5_VIDC_DATA_SIZE + C5_VIDC_QUEUE_SIZE);
	if (IS_ERR_OR_NULL(ctx->arm)) {
		rc = ctx->arm ? PTR_ERR(ctx->arm) : -ENODEV;
		goto free_pool;
	}
	/* Defer hardware attach until HFI has enabled all Venus clocks. */
	/* Only our two gen_pools allocate buffer IOVAs. Do not dma_map_sg here:
	 * its independent ARM bitmap allocator would overlap the same aperture.
	 */
	res->native_context = ctx;
	of_node_put(node);
	return 0;
free_pool:
	for (i = 0; i < 2; i++)
		if (ctx->pool[i])
			gen_pool_destroy(ctx->pool[i]);
	c5_vidc_destroy_child(ctx->child);
free_ctx:
	kfree(ctx);
put_node:
	of_node_put(node);
	return rc;
}

int c5_vidc_native_ready(struct msm_vidc_platform_resources *res)
{
	struct c5_vidc_context *ctx = res ? res->native_context : NULL;
	int rc = 0;

	if (!ctx)
		return -ENODEV;
	/* Called by HFI only after its real regulator/clock enable sequence. */
	mutex_lock(&ctx->lock);
	if (ctx->poisoned) {
		rc = -ENODEV;
	} else if (!ctx->attached) {
		rc = arm_iommu_attach_device(&ctx->child->dev, ctx->arm);
		if (!rc) {
			ctx->attached = true;
			dev_info(&ctx->child->dev,
				"c5_vidc: NS attach at powered HFI boundary\n");
		}
	}
	mutex_unlock(&ctx->lock);
	return rc;
}

static void c5_vidc_drop_dma_buf(struct c5_vidc_mapping *m)
{
	if (m->sg)
		dma_buf_unmap_attachment(m->attachment, m->sg, DMA_BIDIRECTIONAL);
	if (m->attachment)
		dma_buf_detach(m->buf, m->attachment);
	dma_buf_put(m->buf);
	kfree(m);
}

int c5_vidc_native_map(struct msm_vidc_platform_resources *res,
	struct ion_client *client, struct ion_handle *handle, unsigned long flags,
	u32 type, unsigned long align, struct c5_vidc_mapping **cookie,
	unsigned long *address, unsigned long *size)
{
	struct c5_vidc_context *ctx = res ? res->native_context : NULL;
	struct c5_vidc_mapping *m;
	struct scatterlist *sg;
	struct genpool_data_align allocation;
	unsigned long total = 0, at;
	unsigned int i;
	int rc, pool;

	if (!ctx || !client || !handle || !cookie || *cookie || !address || !size ||
	    !c5_vidc_align_valid(align))
		return -EINVAL;
	if (!READ_ONCE(ctx->attached))
		return -ENODEV;
	pool = c5_vidc_pool_for(type, !!(flags & SMEM_SECURE));
	if (pool < 0)
		return pool;
	m = kzalloc(sizeof(*m), GFP_KERNEL);
	if (!m)
		return -ENOMEM;
	m->ctx = ctx;
	m->pool = pool;
	INIT_LIST_HEAD(&m->link);
	m->buf = ion_share_dma_buf(client, handle);
	if (IS_ERR_OR_NULL(m->buf)) {
		rc = m->buf ? PTR_ERR(m->buf) : -EINVAL;
		kfree(m);
		return rc;
	}
	/* A private ION share guarantees the reviewed physical-SG exporter. */
	if (!m->buf->size || m->buf->size > U32_MAX ||
	    (m->buf->size & (PAGE_SIZE - 1))) {
		rc = -EINVAL;
		goto drop;
	}
	m->attachment = dma_buf_attach(m->buf, &ctx->child->dev);
	if (IS_ERR(m->attachment)) {
		rc = PTR_ERR(m->attachment);
		m->attachment = NULL;
		goto drop;
	}
	m->sg = dma_buf_map_attachment(m->attachment, DMA_BIDIRECTIONAL);
	if (IS_ERR_OR_NULL(m->sg)) {
		rc = m->sg ? PTR_ERR(m->sg) : -EINVAL;
		m->sg = NULL;
		goto drop;
	}
	if (!m->sg->sgl || !m->sg->orig_nents || m->sg->nents != m->sg->orig_nents) {
		rc = -EINVAL;
		goto drop;
	}
	for_each_sg(m->sg->sgl, sg, m->sg->orig_nents, i) {
		if (!sg->length || (sg_phys(sg) & (PAGE_SIZE - 1)) ||
		    (sg->length & (PAGE_SIZE - 1)) ||
		    sg->length > m->buf->size - total) {
			rc = -EINVAL;
			goto drop;
		}
		total += sg->length;
	}
	if (total != m->buf->size) {
		rc = -EINVAL;
		goto drop;
	}
	m->size = total;
	allocation.align = align;
	mutex_lock(&ctx->lock);
	if (ctx->poisoned || !ctx->attached) {
		rc = -EIO;
		goto unlock_drop;
	}
	m->iova = gen_pool_alloc_algo(ctx->pool[pool], total,
		gen_pool_first_fit_align, &allocation);
	if (!m->iova) {
		rc = -ENOMEM;
		goto unlock_drop;
	}
	if (!c5_vidc_extent_valid(m->iova, total,
		pool ? C5_VIDC_QUEUE_START : C5_VIDC_DATA_START,
		pool ? C5_VIDC_QUEUE_SIZE : C5_VIDC_DATA_SIZE)) {
		gen_pool_free(ctx->pool[pool], m->iova, m->size);
		rc = -ERANGE;
		goto unlock_drop;
	}
	for_each_sg(m->sg->sgl, sg, m->sg->orig_nents, i) {
		for (at = 0; at < sg->length; at += PAGE_SIZE) {
			unsigned long va = m->iova + m->mapped;
			rc = iommu_map(ctx->arm->domain, va, sg_phys(sg) + at,
				PAGE_SIZE, IOMMU_READ | IOMMU_WRITE);
			if (rc) {
				/* One smallest-page operation: retain everything on a
				 * residual failed-page PTE or a short prefix unmap.
				 */
				if (iommu_iova_to_phys(ctx->arm->domain, va) ||
				    (m->mapped && iommu_unmap(ctx->arm->domain,
					m->iova, m->mapped) != m->mapped)) {
					ctx->poisoned = true;
					list_add_tail(&m->link, &ctx->mappings);
					pr_err("c5_vidc: failed map cleanup; DMA quarantined\n");
					mutex_unlock(&ctx->lock);
					return -EIO;
				}
				gen_pool_free(ctx->pool[pool], m->iova, m->size);
				goto unlock_drop;
			}
			m->mapped += PAGE_SIZE;
		}
	}
	at = m->iova;
	for_each_sg(m->sg->sgl, sg, m->sg->orig_nents, i) {
		sg_dma_address(sg) = at;
		sg_dma_len(sg) = sg->length;
		at += sg->length;
	}
	dma_sync_sg_for_device(&ctx->child->dev, m->sg->sgl,
		m->sg->orig_nents, DMA_BIDIRECTIONAL);
	list_add_tail(&m->link, &ctx->mappings);
	*cookie = m;
	*address = m->iova;
	*size = m->size;
	mutex_unlock(&ctx->lock);
	return 0;
unlock_drop:
	mutex_unlock(&ctx->lock);
drop:
	c5_vidc_drop_dma_buf(m);
	return rc;
}

int c5_vidc_native_unmap(struct c5_vidc_mapping **cookie)
{
	struct c5_vidc_mapping *m;
	struct c5_vidc_context *ctx;
	if (!cookie || !*cookie)
		return -EINVAL;
	m = *cookie;
	ctx = m->ctx;
	mutex_lock(&ctx->lock);
	if (ctx->poisoned) {
		*cookie = NULL;
		mutex_unlock(&ctx->lock);
		return -EIO;
	}
	/* Keep the attachment/pages alive if any stale PTE might remain. */
	dma_sync_sg_for_cpu(&ctx->child->dev, m->sg->sgl,
		m->sg->orig_nents, DMA_BIDIRECTIONAL);
	if (iommu_unmap(ctx->arm->domain, m->iova, m->mapped) != m->mapped) {
		ctx->poisoned = true;
		*cookie = NULL;
		pr_err("c5_vidc: short unmap; DMA backing and range quarantined\n");
		mutex_unlock(&ctx->lock);
		return -EIO;
	}
	gen_pool_free(ctx->pool[m->pool], m->iova, m->size);
	list_del(&m->link);
	*cookie = NULL;
	mutex_unlock(&ctx->lock);
	c5_vidc_drop_dma_buf(m);
	return 0;
}

void c5_vidc_native_poison(struct msm_vidc_platform_resources *res)
{
	struct c5_vidc_context *ctx = res ? res->native_context : NULL;
	struct c5_vidc_mapping *m;
	if (!ctx)
		return;
	mutex_lock(&ctx->lock);
	if (ctx->revoked) {
		mutex_unlock(&ctx->lock);
		return;
	}
	ctx->poisoned = true;
	ctx->revoked = true;
	/* A failed firmware stop must not turn a VB2 return into physical-page
	 * reuse while the codec may still be running. Revoke every known PTE,
	 * retain ALL backing references/ranges even if revocation succeeds,
	 * and refuse future mappings until a new boot. This is a codec failure,
	 * not a successful stop/recovery or an excuse to detach into bypass.
	 */
	list_for_each_entry(m, &ctx->mappings, link) {
		if (m->mapped && iommu_unmap(ctx->arm->domain, m->iova,
			m->mapped) != m->mapped)
			pr_err("c5_vidc: stop-error revocation incomplete; backing retained\n");
	}
	mutex_unlock(&ctx->lock);
}

void c5_vidc_native_exit(struct msm_vidc_platform_resources *res)
{
	struct c5_vidc_context *ctx = res ? res->native_context : NULL;
	if (!ctx)
		return;
	mutex_lock(&ctx->lock);
	if (ctx->poisoned || !list_empty(&ctx->mappings)) {
		ctx->poisoned = true;
		mutex_lock(&c5_vidc_orphan_lock);
		if (list_empty(&ctx->orphan))
			list_add_tail(&ctx->orphan, &c5_vidc_orphans);
		mutex_unlock(&c5_vidc_orphan_lock);
		res->native_context = NULL;
		mutex_unlock(&ctx->lock);
		pr_err("c5_vidc: live/poisoned context retained; teardown refused\n");
		return;
	}
	if (ctx->attached)
		arm_iommu_detach_device(&ctx->child->dev);
	arm_iommu_release_mapping(ctx->arm);
	gen_pool_destroy(ctx->pool[0]);
	gen_pool_destroy(ctx->pool[1]);
	c5_vidc_destroy_child(ctx->child);
	res->native_context = NULL;
	mutex_unlock(&ctx->lock);
	kfree(ctx);
}
