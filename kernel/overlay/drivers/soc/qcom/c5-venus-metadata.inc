/* SPDX-License-Identifier: GPL-2.0-only */
/* Preserve the accepted C5 PAS metadata placement, not an auth bypass.
 * This is a separate physical DMA allocation, as in the accepted 3.10
 * pil_init_image_trusted. Native DMA ops must be initialized explicitly:
 * an uninitialized temporary device selects dummy_dma_ops on ARM64 4.9.
 */
static int c5_venus_init_metadata(struct pil_desc *pil, const u8 *metadata,
				size_t size)
{
	struct device dma_dev = {0};
	struct scm_desc desc = {0};
	struct { u32 proc; u32 image_addr; } request;
	unsigned long attrs = DMA_ATTR_STRONGLY_ORDERED;
	dma_addr_t address;
	u32 scm_ret = 0;
	void *buffer;
	int rc;

	if (!size || size > 65536)
		return -EINVAL;
	arch_setup_dma_ops(&dma_dev, 0, DMA_BIT_MASK(32), NULL, false);
	rc = dma_coerce_mask_and_coherent(&dma_dev, DMA_BIT_MASK(32));
	if (rc)
		return rc;
	rc = scm_pas_enable_bw();
	if (rc)
		return rc;
	buffer = dma_alloc_attrs(&dma_dev, size, &address, GFP_KERNEL, attrs);
	if (!buffer) {
		rc = -ENOMEM;
		goto out_bw;
	}
	/* Match the old PAS interface, which takes a 32-bit physical address. */
	if (address > U32_MAX || address != dma_to_phys(&dma_dev, address)) {
		rc = -ERANGE;
		goto out_free;
	}
	memcpy(buffer, metadata, size);
	request.proc = 9;
	request.image_addr = address;
	if (!is_scm_armv8()) {
		rc = scm_call(SCM_SVC_PIL, PAS_INIT_IMAGE_CMD, &request,
				sizeof(request), &scm_ret, sizeof(scm_ret));
	} else {
		desc.args[0] = request.proc;
		desc.args[1] = address;
		desc.arginfo = SCM_ARGS(2, SCM_VAL, SCM_RW);
		rc = scm_call2(SCM_SIP_FNID(SCM_SVC_PIL, PAS_INIT_IMAGE_CMD),
				&desc);
		scm_ret = desc.ret[0];
	}
out_free:
	dma_free_attrs(&dma_dev, size, buffer, address, attrs);
out_bw:
	scm_pas_disable_bw();
	dev_info_ratelimited(pil->dev,
		"C5 Venus separate PAS metadata: bytes=%zu rc=%d tz=%u\n",
		size, rc, scm_ret);
	if (rc)
		return rc;
	return scm_ret;
}
