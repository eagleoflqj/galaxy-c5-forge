/* SPDX-License-Identifier: GPL-2.0-only */
/* One pre-auth observation of our own exact Venus firmware RAM. No register
 * access, protection change, payload output or skipped native authentication.
 */
static int c5_venus_hash_ram(struct shash_desc *hash, const u8 __iomem *ram,
			   size_t bytes, u8 *scratch, u8 digest[32])
{
	size_t offset = 0;
	int rc = crypto_shash_init(hash);

	while (!rc && offset < bytes) {
		size_t chunk = min_t(size_t, bytes - offset, 4096);

		memcpy_fromio(scratch, ram + offset, chunk);
		rc = crypto_shash_update(hash, scratch, chunk);
		offset += chunk;
	}
	if (rc)
		return rc;
	return crypto_shash_final(hash, digest);
}

static int c5_venus_check_staged(struct pil_desc *desc, const struct firmware *fw)
{
	static DEFINE_MUTEX(lock);
	static bool observed;
	static int cached;
	static const u8 metadata_sha[32] = {
		0x51,0xa4,0x86,0x70,0xa1,0xad,0xc1,0xd8,
		0x6b,0x48,0x1b,0x76,0x59,0xf2,0x5d,0x8e,
		0xe9,0xed,0x82,0x79,0xc9,0xe4,0xde,0xb9,
		0x46,0x70,0x2e,0x35,0x07,0xeb,0xae,0x37,
	};
	static const struct { size_t offset, file, memory; } segment[] = {
		{ 0, 0xf0890, 0xf0890 },
		{ 0xf1000, 0x15c0, 0x407000 },
		{ 0x4ff000, 0x20, 0x20 },
	};
	struct pil_priv *priv = desc->priv;
	struct pil_map_fw_info info = {
		.attrs = desc->attrs, .region = priv->region,
		.base_addr = priv->region_start, .dev = desc->dev,
	};
	struct crypto_shash *tfm;
	struct shash_desc *hash = NULL;
	u8 *scratch = NULL;
	u8 __iomem *ram = NULL;
	u8 digest[32];
	u32 pas;
	unsigned int i;
	int rc = 0;

	if (!IS_ENABLED(CONFIG_C5_RECOVERY_ONLY) || !IS_ENABLED(CONFIG_C5_VIDC) ||
	    strcmp(desc->name, "venus") || strcmp(desc->fw_name, "venus") ||
	    !of_device_is_compatible(desc->dev->of_node, "qcom,pil-tz-generic") ||
	    of_property_read_u32(desc->dev->of_node, "qcom,pas-id", &pas) || pas != 9)
		return 0;
	mutex_lock(&lock);
	if (observed) {
		rc = cached;
		goto out_unlock;
	}
	observed = true;
	if (desc->subsys_vmid || desc->map_data || !priv->region ||
	    priv->region_start != 0x8e500000 || priv->region_end != 0x8ea00000 ||
	    priv->region_size != 0x500000 || fw->size != 37532) {
		rc = -EINVAL;
		goto out_result;
	}
	tfm = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(tfm)) {
		rc = PTR_ERR(tfm);
		goto out_result;
	}
	if (crypto_shash_descsize(tfm) > 4096) {
		rc = -EOVERFLOW;
		goto out_crypto;
	}
	hash = kzalloc(sizeof(*hash) + crypto_shash_descsize(tfm), GFP_KERNEL);
	scratch = kmalloc(4096, GFP_KERNEL);
	if (!hash || !scratch) {
		rc = -ENOMEM;
		goto out_free;
	}
	hash->tfm = tfm;
	hash->flags = 0;
	rc = crypto_shash_digest(hash, fw->data, fw->size, digest);
	if (rc)
		goto out_free;
	if (memcmp(digest, metadata_sha, sizeof(digest))) {
		rc = -EBADMSG;
		goto out_free;
	}
	ram = desc->map_fw_mem(priv->region_start, 0x500000, &info);
	if (!ram) {
		rc = -ENOMEM;
		goto out_free;
	}
	for (i = 0; i < ARRAY_SIZE(segment); i++) {
		size_t offset = segment[i].file;
		bool zero_tail = true;

		rc = c5_venus_hash_ram(hash, ram + segment[i].offset,
				segment[i].file, scratch, digest);
		if (rc)
			break;
		/* These three hashes belong to this exact, hash-pinned MDT. */
		if (memcmp(digest, fw->data + 316 + 32 * i, sizeof(digest))) {
			rc = -EBADMSG;
			dev_err(desc->dev, "C5 Venus stage segment=%u SHA256_MISMATCH\n", i + 2);
			break;
		}
		while (offset < segment[i].memory) {
			size_t chunk = min_t(size_t, segment[i].memory - offset, 4096);

			memcpy_fromio(scratch, ram + segment[i].offset + offset, chunk);
			if (memchr_inv(scratch, 0, chunk)) {
				zero_tail = false;
				break;
			}
			offset += chunk;
		}
		dev_info(desc->dev,
			"C5 Venus stage segment=%u bytes=%zu SHA256_MATCH=1 zero_tail=%u\n",
			i + 2, segment[i].file, zero_tail);
		if (!zero_tail) {
			rc = -EBADMSG;
			break;
		}
	}
	desc->unmap_fw_mem(ram, 0x500000, &info);
out_free:
	kfree(scratch);
	kfree(hash);
out_crypto:
	crypto_free_shash(tfm);
out_result:
	cached = rc;
	dev_info(desc->dev, "C5 Venus pre-auth staged check rc=%d; native auth retained\n", rc);
out_unlock:
	mutex_unlock(&lock);
	return rc;
}
