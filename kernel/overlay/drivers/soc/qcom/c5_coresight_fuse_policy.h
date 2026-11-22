/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef C5_CORESIGHT_FUSE_POLICY_H
#define C5_CORESIGHT_FUSE_POLICY_H
#include <linux/types.h>

bool c5_coresight_fuse_ready(void);
bool coresight_fuse_access_disabled(void);
bool coresight_fuse_apps_access_disabled(void);
bool coresight_fuse_nidnt_access_disabled(void);
bool coresight_fuse_qpdi_access_disabled(void);

struct c5_fuse_policy {
	u32 all_disabled;
	u32 apps_disabled;
};

/* Fixed C5 fuse-v2 masks from the original Qualcomm provider. */
static inline void c5_fuse_decode_v2(u32 config1,
				     struct c5_fuse_policy *out)
{
	out->all_disabled = !!(config1 & 0x0007c001U);
	out->apps_disabled = !!(config1 & 0x00043c01U);
}

static inline bool c5_fuse_nidnt_v2(u32 value)
{
	return !!(value & 0x08000000U);
}

static inline bool c5_fuse_qpdi_v2(u32 value)
{
	return !!(value & 0x00000004U);
}
#endif
