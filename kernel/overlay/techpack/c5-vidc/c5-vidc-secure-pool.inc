/* SPDX-License-Identifier: GPL-2.0-only */
/* Accepted C5 secure-SMMU service initialization, not secure-codec support.
 * A handed-off allocation is never freed, even after an ambiguous failure.
 */
static int c5_vidc_secure_pool_result = -ENODEV;

static int c5_vidc_secure_pool_init(void)
{
	static DEFINE_MUTEX(lock);
	static bool attempted;
	static struct device dma_dev;
	static void *owned;
	struct scm_desc call = {0};
	u32 spare = 0, reply = 0, size_reply[2] = {0, 0};
	u32 request[3] = {0, 0, 0};
	int version;
	dma_addr_t address;
	void *buffer;
	unsigned long attrs = DMA_ATTR_NO_KERNEL_MAPPING;
	int rc;

	BUILD_BUG_ON(SCM_ARGS(1) != 1 || SCM_ARGS(2) != 2 ||
		SCM_ARGS(3, SCM_RW, SCM_VAL, SCM_VAL) != 0x23);
	mutex_lock(&lock);
	if (attempted) {
		rc = c5_vidc_secure_pool_result;
		goto out;
	}
	attempted = true;
	version = scm_get_feat_version(SCM_SVC_MP);
	/* Same300MiB virtual CP capacity and feature gate as accepted3.10. */
	if (version >= 0x00401001) {
		request[0] = 300U * 1024U * 1024U;
		if (!is_scm_armv8()) {
			rc = scm_call(SCM_SVC_MP, 5, request, 8, &reply, 4);
		} else {
			call.args[0] = request[0];
			call.args[1] = 0;
			call.arginfo = SCM_ARGS(2);
			rc = scm_call2(SCM_SIP_FNID(SCM_SVC_MP, 5), &call);
			reply = call.ret[0];
		}
		if (rc || reply) {
			rc = rc ? rc : -EINVAL;
			goto result;
		}
	}
	memset(&call, 0, sizeof(call));
	if (!is_scm_armv8()) {
		rc = scm_call(SCM_SVC_MP, 3, &spare, 4, size_reply, 8);
	} else {
		call.args[0] = 0;
		call.arginfo = SCM_ARGS(1);
		rc = scm_call2(SCM_SIP_FNID(SCM_SVC_MP, 3), &call);
		if (!rc && (call.ret[0] > 32U * 1024U * 1024U || call.ret[1]))
			rc = -EINVAL;
		size_reply[0] = call.ret[0];
		size_reply[1] = call.ret[1];
	}
	/* This is an allocation safety bound, not a guessed firmware size. */
	if (rc || size_reply[1] || !size_reply[0] || size_reply[0] > 32U * 1024U * 1024U) {
		rc = rc ? rc : -EINVAL;
		goto result;
	}
	arch_setup_dma_ops(&dma_dev, 0, DMA_BIT_MASK(32), NULL, false);
	rc = dma_coerce_mask_and_coherent(&dma_dev, DMA_BIT_MASK(32));
	if (rc)
		goto result;
	buffer = dma_alloc_attrs(&dma_dev, size_reply[0], &address, GFP_KERNEL, attrs);
	if (!buffer) {
		rc = -ENOMEM;
		goto result;
	}
	if (address > U32_MAX || size_reply[0] - 1 > U32_MAX - address ||
	    address != dma_to_phys(&dma_dev, address)) {
		dma_free_attrs(&dma_dev, size_reply[0], buffer, address, attrs);
		rc = -ERANGE;
		goto result;
	}
	request[0] = address;
	request[1] = size_reply[0];
	request[2] = 0;
	memset(&call, 0, sizeof(call));
	call.args[0] = address;
	call.args[1] = size_reply[0];
	call.args[2] = 0;
	call.arginfo = SCM_ARGS(3, SCM_RW, SCM_VAL, SCM_VAL);
	/* Firmware may retain this memory even if the transport reports failure. */
	owned = buffer;
	if (!is_scm_armv8()) {
		rc = scm_call(SCM_SVC_MP, 4, request, 12, &reply, 4);
	} else {
		rc = scm_call2(SCM_SIP_FNID(SCM_SVC_MP, 4), &call);
		reply = call.ret[0];
	}
	if (!rc && reply)
		rc = -EINVAL;
result:
	c5_vidc_secure_pool_result = rc;
	pr_info("c5_vidc: secure PTBL feature=%#x bytes=%u rc=%d retained=%u\n",
		(u32)version, size_reply[0], rc, !!owned);
out:
	mutex_unlock(&lock);
	return rc;
}

static int c5_vidc_secure_pool_status(void)
{
	return READ_ONCE(c5_vidc_secure_pool_result);
}
