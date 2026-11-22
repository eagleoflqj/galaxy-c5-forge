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
#ifndef __QCOM_MPM_H__
#define __QCOM_MPM_H__

#include <linux/irq.h>
#include <linux/device.h>

struct mpm_pin {
	int pin;
	irq_hw_number_t hwirq;
};

extern const struct mpm_pin mpm_msm8953_gic_chip_data[];
extern const struct mpm_pin mpm_msm8953_gpio_chip_data[];

extern const struct mpm_pin mpm_msm8937_gic_chip_data[];
extern const struct mpm_pin mpm_msm8937_gpio_chip_data[];

extern const struct mpm_pin mpm_msm8909_gic_chip_data[];
extern const struct mpm_pin mpm_msm8909_gpio_chip_data[];

extern const struct mpm_pin mpm_c5lte_gic_chip_data[];
extern const struct mpm_pin mpm_c5lte_gpio_chip_data[];

enum mpm_c5lte_irq_class {
	MPM_C5LTE_IRQ_UNLISTED,
	MPM_C5LTE_IRQ_MAPPED,
	MPM_C5LTE_IRQ_BYPASS,
};

extern const irq_hw_number_t mpm_c5lte_gic_bypass[];
extern const irq_hw_number_t mpm_c5lte_gpio_bypass[];
int mpm_c5lte_classify_irq(bool gpio, irq_hw_number_t hwirq);

#ifdef CONFIG_QTI_MPM_C5LTE
void mpm_c5lte_xo_activate(void);
int mpm_c5lte_xo_domain_init(struct irq_domain *domain, bool gpio);
void mpm_c5lte_xo_domain_remove(struct irq_domain *domain, bool gpio);
int mpm_c5lte_xo_validate(struct irq_domain *domain, irq_hw_number_t hwirq);
int mpm_c5lte_xo_update(struct irq_data *d, bool wake, bool on);
bool mpm_c5lte_xo_sleep_allowed(void);
#else
static inline bool mpm_c5lte_xo_sleep_allowed(void) { return true; }
#endif

#endif /* __QCOM_MPM_H__ */
