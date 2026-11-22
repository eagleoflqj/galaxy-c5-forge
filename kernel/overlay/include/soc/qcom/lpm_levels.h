/* Copyright (c) 2018, The Linux Foundation. All rights reserved.
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
#ifndef __SOC_QCOM_LPM_LEVEL_H__
#define __SOC_QCOM_LPM_LEVEL_H__

struct system_pm_ops {
	int (*enter)(struct cpumask *mask);
	void (*exit)(bool success);
	int (*update_wakeup)(bool);
	bool (*sleep_allowed)(void);
	/* Optional atomic entry that publishes the wakeup before platform IPC.
	 * Providers using this hook must not need a later update_wakeup call.
	 */
	int (*enter_with_wakeup)(struct cpumask *mask, bool from_idle);
};

uint32_t register_system_pm_ops(struct system_pm_ops *pm_ops);

#ifdef CONFIG_QTI_MPM_C5LTE
void c5_firmware_system_scope(bool active);
#endif
#endif
