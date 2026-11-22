/* Copyright (c) 2013-2014, 2016-2017, The Linux Foundation. All rights
 * reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/random.h>
#include <linux/io.h>

#include <soc/qcom/scm.h>

#include <asm/cacheflush.h>

#ifdef CONFIG_C5_EARLY_RANDOM_LEGACY
#include <linux/moduleparam.h>
#endif

#define TZ_SVC_CRYPTO	10
#define PRNG_CMD_ID	0x01

struct tz_prng_data {
	uint8_t		*out_buf;
	uint32_t	out_buf_sz;
} __packed;

DEFINE_SCM_BUFFER(common_scm_buf)
#define RANDOM_BUFFER_SIZE	PAGE_SIZE
char random_buffer[RANDOM_BUFFER_SIZE] __aligned(PAGE_SIZE);

#ifdef CONFIG_C5_EARLY_RANDOM_LEGACY
/* Cached completion metadata only; never expose seed bytes or addresses. */
static struct {
	unsigned int calls, path;
	int status;
	unsigned int request, reply_class, mixed, done;
} c5_early_random_health;

static int c5_early_random_get(char *buffer, const struct kernel_param *kp)
{
	(void)kp;
	return scnprintf(buffer, PAGE_SIZE,
		"V=1 CALLS=%u PATH=%u STATUS=%d REQUEST=%u REPLY=%u MIXED=%u DONE=%u",
		c5_early_random_health.calls, c5_early_random_health.path,
		c5_early_random_health.status, c5_early_random_health.request,
		c5_early_random_health.reply_class, c5_early_random_health.mixed,
		c5_early_random_health.done);
}

static const struct kernel_param_ops c5_early_random_ops = {
	.get = c5_early_random_get,
};
module_param_cb(c5_rng_health, &c5_early_random_ops, NULL, 0400);
#endif

void __init init_random_pool(void)
{
	struct tz_prng_data data;
	int ret;
#ifdef CONFIG_C5_EARLY_RANDOM_LEGACY
	u32 resp = 0;
	struct scm_desc desc = { 0 };
#else
	u32 resp;
	struct scm_desc desc;
#endif
	u64 bytes_received;

#ifdef CONFIG_C5_EARLY_RANDOM_LEGACY
	c5_early_random_health.calls++;
	c5_early_random_health.path = 0;
	c5_early_random_health.status = 0;
	c5_early_random_health.request = SZ_512;
	c5_early_random_health.reply_class = 0;
	c5_early_random_health.mixed = 0;
	c5_early_random_health.done = 0;
#endif

	data.out_buf = (uint8_t *) virt_to_phys(random_buffer);
	desc.args[0] = (unsigned long) data.out_buf;
	desc.args[1] = data.out_buf_sz = SZ_512;
	desc.arginfo = SCM_ARGS(2, SCM_RW, SCM_VAL);

	dmac_flush_range(random_buffer, random_buffer + RANDOM_BUFFER_SIZE);

	if (!is_scm_armv8()) {
#ifdef CONFIG_C5_EARLY_RANDOM_LEGACY
		c5_early_random_health.path = 1;
#endif
		ret = scm_call_noalloc(TZ_SVC_CRYPTO, PRNG_CMD_ID, &data,
				sizeof(data), &resp, sizeof(resp),
				common_scm_buf,
				SCM_BUFFER_SIZE(common_scm_buf));
		bytes_received = resp;
	} else {
#ifdef CONFIG_C5_EARLY_RANDOM_LEGACY
		c5_early_random_health.path = 2;
#endif
		ret = scm_call2(SCM_SIP_FNID(TZ_SVC_CRYPTO, PRNG_CMD_ID),
					&desc);
		bytes_received = desc.ret[0];
	}

#ifdef CONFIG_C5_EARLY_RANDOM_LEGACY
	c5_early_random_health.status = ret;
	/* 0: no successful reply; 1: zero; 2: requested length; 3: other. */
	if (!ret)
		c5_early_random_health.reply_class = !bytes_received ? 1 :
			bytes_received == SZ_512 ? 2 : 3;
#endif
	if (!ret) {
#ifdef CONFIG_C5_EARLY_RANDOM_LEGACY
		/* Preserve the original C5 caller's status-only PRNG contract. */
		dmac_inv_range(random_buffer, random_buffer + RANDOM_BUFFER_SIZE);
		add_device_randomness(random_buffer, SZ_512);
		c5_early_random_health.mixed = SZ_512;
#else
		if (bytes_received != SZ_512)
			pr_warn("Did not receive the expected number of bytes from PRNG: %llu\n",
				bytes_received);

		dmac_inv_range(random_buffer, random_buffer +
						RANDOM_BUFFER_SIZE);
		bytes_received = (bytes_received <= RANDOM_BUFFER_SIZE) ?
					bytes_received : RANDOM_BUFFER_SIZE;
		add_bootloader_randomness(random_buffer, bytes_received);
#endif
	}
#ifdef CONFIG_C5_EARLY_RANDOM_LEGACY
	c5_early_random_health.done = 1;
#endif
}

