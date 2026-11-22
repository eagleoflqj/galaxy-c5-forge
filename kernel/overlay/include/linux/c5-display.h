/* SPDX-License-Identifier: GPL-2.0-only */
/* Shared declaration for the original C5 Samsung display resume consumer. */
#ifndef _LINUX_C5_DISPLAY_H
#define _LINUX_C5_DISPLAY_H

#include <linux/mutex.h>

struct device;
struct device_node;
struct mdss_dsi_ctrl_pdata;

#define C5_PANEL_EXTRA_POWER_MAX 4

/* Per-controller ownership; GPIO devres belongs to the actual DSI device. */
struct c5_panel_extra_power {
	struct mutex lock;
	int gpio[C5_PANEL_EXTRA_POWER_MAX];
	bool initialized;
	bool ready;
};

int c5_panel_extra_power_init(struct device *dev, struct device_node *np,
		struct mdss_dsi_ctrl_pdata *ctrl);
void mdss_samsung_resume_event(int irq);

#endif
