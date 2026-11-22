/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _MDSS_C5_LEGACY_H
#define _MDSS_C5_LEGACY_H

/* Accepted C5 LP64 userspace layout, not the enlarged 4.9 PP extension. */
struct c5_mdp_pa_mem_col_cfg {
	uint32_t color_adjust_p0, color_adjust_p1;
	uint32_t hue_region, sat_region, val_region;
	uint32_t color_adjust_p2, blend_gain;
	uint8_t sat_hold, val_hold;
};

struct c5_mdp_pa_v2_data {
	uint32_t flags, global_hue_adj, global_sat_adj;
	uint32_t global_val_adj, global_cont_adj;
	struct c5_mdp_pa_mem_col_cfg skin_cfg, sky_cfg, fol_cfg;
	uint32_t six_zone_len, six_zone_thresh;
	uint32_t *six_zone_curve_p0, *six_zone_curve_p1;
	uint32_t six_zone_adj_p0, six_zone_adj_p1;
	uint8_t six_zone_sat_hold, six_zone_val_hold;
};

struct c5_mdp_igc_lut_data {
	uint32_t block, len, ops;
	uint32_t *c0_c1_data, *c2_data;
};

struct c5_mdp_hist_lut_data {
	uint32_t block, ops, len;
	uint32_t *data;
};

struct c5_mdp_overlay_pp_params {
	uint32_t config_ops;
	struct mdp_csc_cfg csc_cfg;
	struct mdp_qseed_cfg qseed_cfg[2];
	struct mdp_pa_cfg pa_cfg;
	struct c5_mdp_pa_v2_data pa_v2_cfg;
	struct c5_mdp_igc_lut_data igc_cfg;
	struct mdp_sharp_cfg sharp_cfg;
	struct mdp_histogram_cfg hist_cfg;
	struct c5_mdp_hist_lut_data hist_lut_cfg;
};

struct c5_mdp_overlay {
	struct msmfb_img src;
	struct mdp_rect src_rect;
	struct mdp_rect dst_rect;
	uint32_t z_order, is_fg, alpha, blend_op, transp_mask, flags;
	uint32_t pipe_type, id;
	uint8_t priority;
	uint32_t user_data[6];
	uint32_t bg_color;
	uint8_t horz_deci, vert_deci;
	struct c5_mdp_overlay_pp_params overlay_pp_cfg;
	struct mdp_scale_data scale;
	uint8_t color_space;
	uint32_t frame_rate;
};

static inline int c5_mdp_overlay_to_native(struct mdp_overlay *to,
		const struct c5_mdp_overlay *from)
{
	BUILD_BUG_ON(sizeof(struct c5_mdp_overlay) != 840);
	BUILD_BUG_ON(sizeof(struct c5_mdp_overlay_pp_params) != 440);
	BUILD_BUG_ON(offsetof(struct mdp_overlay, overlay_pp_cfg) != 112);
	BUILD_BUG_ON(offsetof(struct c5_mdp_overlay, overlay_pp_cfg) != 112);
	/* Nested PP payloads also changed ABI. Do not interpret or drop requests. */
	if (from->overlay_pp_cfg.config_ops || (from->flags & MDP_OVERLAY_PP_CFG_EN))
		return -EOPNOTSUPP;
	/* Never import legacy padding as new configuration or trust new pointers. */
	memset(to, 0, sizeof(*to));
	memcpy(to, from, offsetof(struct c5_mdp_overlay, overlay_pp_cfg));
	to->scale = from->scale;
	to->color_space = from->color_space;
	to->frame_rate = from->frame_rate;
	return 0;
}

static inline void c5_mdp_overlay_from_native(struct c5_mdp_overlay *to,
		const struct mdp_overlay *from)
{
	memset(to, 0, sizeof(*to));
	memcpy(to, from, offsetof(struct c5_mdp_overlay, overlay_pp_cfg));
	to->scale = from->scale;
	to->color_space = from->color_space;
	to->frame_rate = from->frame_rate;
}

#ifndef C5_MDP_HOST_LAYOUT_TEST
#include <linux/of.h>
#include <linux/compat.h>

static inline bool mdss_c5_legacy_overlay(struct msm_fb_data_type *mfd)
{
	const char *model;

	return mfd && mfd->index == 0 &&
		of_machine_is_compatible("qcom,msm8952") &&
		!of_property_read_string(of_root, "model", &model) &&
		!strcmp(model, "Samsung C5LTE CHN OPEN PROJECT Rev09");
}

static inline bool mdss_c5_legacy_command(u32 cmd)
{
	return cmd == MSMFB_OVERLAY_PREPARE || cmd == MSMFB_BUFFER_SYNC ||
		cmd == MSMFB_DISPLAY_COMMIT || cmd == MSMFB_OVERLAY_PLAY ||
		cmd == MSMFB_OVERLAY_UNSET;
}
#endif
#endif
