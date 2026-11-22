/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef C5_VIDC_NATIVE_H
#define C5_VIDC_NATIVE_H

#include <linux/types.h>

struct msm_vidc_platform_resources;
struct ion_client;
struct ion_handle;
struct c5_vidc_mapping;

int c5_vidc_native_init(struct msm_vidc_platform_resources *res);
int c5_vidc_native_ready(struct msm_vidc_platform_resources *res);
void c5_vidc_native_exit(struct msm_vidc_platform_resources *res);
int c5_vidc_native_map(struct msm_vidc_platform_resources *res,
	struct ion_client *client, struct ion_handle *handle, unsigned long flags,
	u32 buffer_type, unsigned long align, struct c5_vidc_mapping **cookie,
	unsigned long *address, unsigned long *size);
int c5_vidc_native_unmap(struct c5_vidc_mapping **cookie);
void c5_vidc_native_poison(struct msm_vidc_platform_resources *res);

#endif
