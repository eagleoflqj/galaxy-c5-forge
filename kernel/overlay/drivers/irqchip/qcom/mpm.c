/* Copyright (c) 2010-2018, The Linux Foundation. All rights reserved.
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

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/tick.h>
#include <linux/irqchip.h>
#include <linux/irqchip/arm-gic-v3.h>
#include <linux/irqdomain.h>
#include <linux/interrupt.h>
#include<linux/ktime.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/spinlock.h>
#include <linux/of_irq.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/cpu_pm.h>
#include <asm/arch_timer.h>
#include <soc/qcom/rpm-notifier.h>
#include <soc/qcom/lpm_levels.h>
#include <linux/c5-mpm-replay.h>
#include "mpm.h"
#define CREATE_TRACE_POINTS
#include "trace/events/mpm.h"

#define ARCH_TIMER_HZ (19200000)
#define MAX_MPM_PIN_PER_IRQ 2
#define CLEAR_INTR(reg, intr) (reg & ~(1U << intr))
#define ENABLE_INTR(reg, intr) (reg | (1U << intr))
#define CLEAR_TYPE(reg, type) (reg & ~(1U << type))
#define ENABLE_TYPE(reg, type) (reg | (1U << type))
#define MPM_REG_ENABLE 0
#define MPM_REG_FALLING_EDGE 1
#define MPM_REG_RISING_EDGE 2
#define MPM_REG_POLARITY 3
#define MPM_REG_STATUS 4

#define QCOM_MPM_REG_WIDTH  DIV_ROUND_UP(num_mpm_irqs, 32)
#define MPM_REGISTER(reg, index) ((reg * QCOM_MPM_REG_WIDTH + index + 2) * (4))

struct msm_mpm_device_data {
	struct device *dev;
	void __iomem *mpm_request_reg_base;
	void __iomem *mpm_ipc_reg;
	irq_hw_number_t ipc_irq;
	struct irq_domain *gic_chip_domain;
	struct irq_domain *gpio_chip_domain;
};

static int msm_pm_sleep_time_override;
static int num_mpm_irqs = 64;
module_param_named(sleep_time_override,
	msm_pm_sleep_time_override, int, 0664);
static struct msm_mpm_device_data msm_mpm_dev_data;
static unsigned int *mpm_to_irq;
static DEFINE_SPINLOCK(mpm_lock);

#ifdef CONFIG_QTI_MPM_C5LTE
static bool msm_mpm_is_c5_domain(struct irq_domain *domain)
{
	return domain && (domain->host_data == mpm_c5lte_gic_chip_data ||
			  domain->host_data == mpm_c5lte_gpio_chip_data);
}
#endif

#ifdef CONFIG_QTI_MPM_C5LTE
#define C5_MPM_WORDS 2
static u32 c5_mpm_enabled[C5_MPM_WORDS];
static u32 c5_mpm_wake[C5_MPM_WORDS];
static u32 c5_mpm_type[3][C5_MPM_WORDS];

/* mpm_lock protects both cached Linux intent and vMPM request publication. */
static void c5_mpm_cache_bit(u32 *word, unsigned int mask, bool on)
{
	if (on)
		*word |= BIT(mask);
	else
		*word &= ~BIT(mask);
}
#endif

static int msm_get_irq_pin(int mpm_pin, struct mpm_pin *mpm_data)
{
	int i = 0;

	if (!mpm_data)
		return -ENODEV;

	for (i = 0; mpm_data[i].pin >= 0; i++) {
		if (mpm_data[i].pin == mpm_pin)
			return mpm_to_irq[mpm_data[i].pin];
	}

	return -EINVAL;
}

static void msm_get_mpm_pin(struct irq_data *d, int *mpm_pin)
{
	struct mpm_pin *mpm_data = NULL;
	int i = 0, j = 0;

	if (!d || !d->domain->host_data)
		return;

	mpm_data = d->domain->host_data;

	for (i = 0; (mpm_data[i].pin >= 0) &&  (j < MAX_MPM_PIN_PER_IRQ); i++) {
		if (mpm_data[i].hwirq == d->hwirq) {
			mpm_pin[j] = mpm_data[i].pin;
			mpm_to_irq[mpm_data[i].pin] = d->irq;
			j++;
		}
	}
}

static inline uint32_t msm_mpm_read(unsigned int reg, unsigned int subreg_index)
{
	unsigned int offset = MPM_REGISTER(reg, subreg_index);

	return readl_relaxed(msm_mpm_dev_data.mpm_request_reg_base + offset);
}

static inline void msm_mpm_write(unsigned int reg,
					unsigned int subreg_index,
					uint32_t value)
{
	void __iomem *mpm_reg_base = msm_mpm_dev_data.mpm_request_reg_base;
	/*
	 * Add 2 to offset to account for the 64 bit timer in the vMPM
	 * mapping
	 */
	unsigned int offset = MPM_REGISTER(reg, subreg_index);
	u32 r_value;

	writel_relaxed(value, mpm_reg_base + offset);

#ifdef CONFIG_QTI_MPM_C5LTE
	/* C5's legacy vMPM protocol publishes a single write plus a barrier.
	 * Do not wait for equality in registers also updated by RPM firmware.
	 */
	if (msm_mpm_is_c5_domain(msm_mpm_dev_data.gic_chip_domain)) {
		wmb();
		return;
	}
#endif

	do {
		r_value = readl_relaxed(mpm_reg_base + offset);
		udelay(5);
	} while (r_value != value);
}

static inline void msm_mpm_enable_irq(struct irq_data *d, bool on)
{
	int mpm_pin[MAX_MPM_PIN_PER_IRQ] = {-1, -1};
	unsigned long flags;
	int i = 0;
	u32 enable;
	unsigned int index, mask;
	unsigned int reg;

	reg = MPM_REG_ENABLE;
#ifdef CONFIG_QTI_MPM_C5LTE
	if (msm_mpm_is_c5_domain(d->domain))
		mpm_c5lte_xo_update(d, false, on);
#endif
	msm_get_mpm_pin(d, mpm_pin);
	for (i = 0; i < MAX_MPM_PIN_PER_IRQ; i++) {
		if (mpm_pin[i] < 0)
			return;

		index = mpm_pin[i]/32;
		mask = mpm_pin[i]%32;
		spin_lock_irqsave(&mpm_lock, flags);
		enable = msm_mpm_read(reg, index);
#ifdef CONFIG_QTI_MPM_C5LTE
		if (msm_mpm_is_c5_domain(d->domain) && index < C5_MPM_WORDS)
			c5_mpm_cache_bit(&c5_mpm_enabled[index], mask, on);
#endif

		if (on)
			enable = ENABLE_INTR(enable, mask);
		else
			enable = CLEAR_INTR(enable, mask);

		msm_mpm_write(reg, index, enable);
		spin_unlock_irqrestore(&mpm_lock, flags);
	}
}

static void msm_mpm_program_set_type(bool set, unsigned int reg,
					unsigned int index, unsigned int mask)
{
	u32 type;

	type = msm_mpm_read(reg, index);
	if (set)
		type = ENABLE_TYPE(type, mask);
	else
		type = CLEAR_TYPE(type, mask);

#ifdef CONFIG_QTI_MPM_C5LTE
	if (msm_mpm_is_c5_domain(msm_mpm_dev_data.gic_chip_domain) &&
	    index < C5_MPM_WORDS && reg >= MPM_REG_FALLING_EDGE &&
	    reg <= MPM_REG_POLARITY)
		c5_mpm_cache_bit(&c5_mpm_type[reg - MPM_REG_FALLING_EDGE][index],
				 mask, set);
#endif
	msm_mpm_write(reg, index, type);
}

static void msm_mpm_set_type(struct irq_data *d,
					unsigned int flowtype)
{
	int mpm_pin[MAX_MPM_PIN_PER_IRQ] = {-1, -1};
	unsigned long flags;
	int i = 0;
	unsigned int index, mask;
	unsigned int reg = 0;

	msm_get_mpm_pin(d, mpm_pin);
	for (i = 0; i < MAX_MPM_PIN_PER_IRQ; i++) {
		if (mpm_pin[i] < 0)
			return;

		index = mpm_pin[i]/32;
		mask = mpm_pin[i]%32;

		spin_lock_irqsave(&mpm_lock, flags);
		reg = MPM_REG_RISING_EDGE;
		if (flowtype & IRQ_TYPE_EDGE_RISING)
			msm_mpm_program_set_type(1, reg, index, mask);
		else
			msm_mpm_program_set_type(0, reg, index, mask);

		reg = MPM_REG_FALLING_EDGE;
		if (flowtype & IRQ_TYPE_EDGE_FALLING)
			msm_mpm_program_set_type(1, reg, index, mask);
		else
			msm_mpm_program_set_type(0, reg, index, mask);

		reg = MPM_REG_POLARITY;
		if (flowtype & IRQ_TYPE_LEVEL_HIGH)
			msm_mpm_program_set_type(1, reg, index, mask);
		else
			msm_mpm_program_set_type(0, reg, index, mask);
		spin_unlock_irqrestore(&mpm_lock, flags);
	}
}

static void msm_mpm_gpio_chip_mask(struct irq_data *d)
{
	msm_mpm_enable_irq(d, false);
}

static void msm_mpm_gpio_chip_unmask(struct irq_data *d)
{
	msm_mpm_enable_irq(d, true);
}

static int msm_mpm_gpio_chip_set_type(struct irq_data *d, unsigned int type)
{
	msm_mpm_set_type(d, type);
	return 0;
}

static void msm_mpm_gic_chip_mask(struct irq_data *d)
{
	msm_mpm_enable_irq(d, false);
	irq_chip_mask_parent(d);
}

static void msm_mpm_gic_chip_unmask(struct irq_data *d)
{
	msm_mpm_enable_irq(d, true);
	irq_chip_unmask_parent(d);
}

static int msm_mpm_gic_chip_set_type(struct irq_data *d, unsigned int type)
{
	msm_mpm_set_type(d, type);
	return irq_chip_set_type_parent(d, type);
}

static struct irq_chip msm_mpm_gic_chip = {
	.name		= "mpm-gic",
	.irq_eoi	= irq_chip_eoi_parent,
	.irq_mask	= msm_mpm_gic_chip_mask,
	.irq_disable	= msm_mpm_gic_chip_mask,
	.irq_unmask	= msm_mpm_gic_chip_unmask,
	.irq_retrigger	= irq_chip_retrigger_hierarchy,
	.irq_set_type	= msm_mpm_gic_chip_set_type,
	.flags		= IRQCHIP_MASK_ON_SUSPEND | IRQCHIP_SKIP_SET_WAKE,
	.irq_set_affinity	= irq_chip_set_affinity_parent,
};

static struct irq_chip msm_mpm_gpio_chip = {
	.name		= "mpm-gpio",
	.irq_mask	= msm_mpm_gpio_chip_mask,
	.irq_disable	= msm_mpm_gpio_chip_mask,
	.irq_unmask	= msm_mpm_gpio_chip_unmask,
	.irq_set_type	= msm_mpm_gpio_chip_set_type,
	.flags		= IRQCHIP_MASK_ON_SUSPEND | IRQCHIP_SKIP_SET_WAKE,
	.irq_retrigger          = irq_chip_retrigger_hierarchy,
};

#ifdef CONFIG_QTI_MPM_C5LTE
static struct irq_chip msm_mpm_c5_gic_chip;
static struct irq_chip msm_mpm_c5_gpio_chip;

static int msm_mpm_c5_set_wake(struct irq_data *d, unsigned int on)
{
	int pins[MAX_MPM_PIN_PER_IRQ] = {-1, -1};
	unsigned long flags;
	unsigned int i, index;
	int ret;

	/* Unlisted IRQs still use the original conservative XO policy. */
	ret = mpm_c5lte_xo_update(d, true, !!on);
	if (ret)
		return ret;
	msm_get_mpm_pin(d, pins);
	spin_lock_irqsave(&mpm_lock, flags);
	for (i = 0; i < MAX_MPM_PIN_PER_IRQ && pins[i] >= 0; i++) {
		index = pins[i] / 32;
		if (index < C5_MPM_WORDS)
			c5_mpm_cache_bit(&c5_mpm_wake[index], pins[i] % 32, !!on);
	}
	spin_unlock_irqrestore(&mpm_lock, flags);
	return 0;
}
#endif

#ifdef CONFIG_QTI_MPM_C5LTE
static void c5_mpm_forget_irq(struct irq_data *d)
{
	int pins[MAX_MPM_PIN_PER_IRQ] = {-1, -1};
	unsigned long flags;
	unsigned int i, index;

	msm_get_mpm_pin(d, pins);
	spin_lock_irqsave(&mpm_lock, flags);
	for (i = 0; i < MAX_MPM_PIN_PER_IRQ && pins[i] >= 0; i++) {
		index = pins[i] / 32;
		if (index < C5_MPM_WORDS) {
			c5_mpm_cache_bit(&c5_mpm_enabled[index], pins[i] % 32, false);
			c5_mpm_cache_bit(&c5_mpm_wake[index], pins[i] % 32, false);
		}
	}
	spin_unlock_irqrestore(&mpm_lock, flags);
}
#endif

static void msm_mpm_chip_free(struct irq_domain *domain, unsigned int virq,
			      unsigned int nr_irqs)
{
#ifdef CONFIG_QTI_MPM_C5LTE
	unsigned int i;

	if (msm_mpm_is_c5_domain(domain))
		for (i = 0; i < nr_irqs; i++) {
			struct irq_data *d = irq_domain_get_irq_data(domain, virq + i);

			if (d) {
				c5_mpm_forget_irq(d);
				mpm_c5lte_xo_update(d, false, false);
				mpm_c5lte_xo_update(d, true, false);
			}
		}
#endif
	irq_domain_free_irqs_common(domain, virq, nr_irqs);
}

static int msm_mpm_gpio_chip_translate(struct irq_domain *d,
		struct irq_fwspec *fwspec,
		unsigned long *hwirq,
		unsigned int *type)
{
	if (is_of_node(fwspec->fwnode)) {
		if (fwspec->param_count != 2)
			return -EINVAL;
		*hwirq = fwspec->param[0];
		*type = fwspec->param[1];
		return 0;
	}
	return -EINVAL;
}

static int msm_mpm_gpio_chip_alloc(struct irq_domain *domain,
		unsigned int virq,
		unsigned int nr_irqs,
		void *data)
{
	int ret = 0;
	struct irq_fwspec *fwspec = data;
	irq_hw_number_t hwirq;
	unsigned int type = IRQ_TYPE_NONE;
	struct irq_chip *chip = &msm_mpm_gpio_chip;

	ret = msm_mpm_gpio_chip_translate(domain, fwspec, &hwirq, &type);
	if (ret)
		return ret;

#ifdef CONFIG_QTI_MPM_C5LTE
	if (msm_mpm_is_c5_domain(domain)) {
		if (nr_irqs != 1 || (type & ~IRQ_TYPE_SENSE_MASK))
			return -EINVAL;
		ret = mpm_c5lte_xo_validate(domain, hwirq);
		if (ret)
			return ret;
		chip = &msm_mpm_c5_gpio_chip;
	}
#endif

	irq_domain_set_hwirq_and_chip(domain, virq, hwirq,
				chip, NULL);

	return 0;
}

static const struct irq_domain_ops msm_mpm_gpio_chip_domain_ops = {
	.translate	= msm_mpm_gpio_chip_translate,
	.alloc		= msm_mpm_gpio_chip_alloc,
	.free		= msm_mpm_chip_free,
};

static int msm_mpm_gic_chip_translate(struct irq_domain *d,
					struct irq_fwspec *fwspec,
					unsigned long *hwirq,
					unsigned int *type)
{
	if (is_of_node(fwspec->fwnode)) {
		if (fwspec->param_count < 3)
			return -EINVAL;

		switch (fwspec->param[0]) {
		case 0:			/* SPI */
			*hwirq = fwspec->param[1] + 32;
			break;
		case 1:			/* PPI */
			*hwirq = fwspec->param[1] + 16;
			break;
		case GIC_IRQ_TYPE_LPI:	/* LPI */
			*hwirq = fwspec->param[1];
			break;
		default:
			return -EINVAL;
		}

		*type = fwspec->param[2] & IRQ_TYPE_SENSE_MASK;
		return 0;
	}

	if (is_fwnode_irqchip(fwspec->fwnode)) {
		if (fwspec->param_count != 2)
			return -EINVAL;

		*hwirq = fwspec->param[0];
		*type = fwspec->param[1];
		return 0;
	}

	return -EINVAL;
}

static int msm_mpm_gic_chip_alloc(struct irq_domain *domain,
					unsigned int virq,
					unsigned int nr_irqs,
					void *data)
{
	struct irq_fwspec *fwspec = data;
	struct irq_fwspec parent_fwspec;
	irq_hw_number_t hwirq;
	unsigned int type;
	int  ret;
	struct irq_chip *chip = &msm_mpm_gic_chip;

#ifdef CONFIG_QTI_MPM_C5LTE
	if (msm_mpm_is_c5_domain(domain) &&
	    (fwspec->param_count != 3 || fwspec->param[0] > 1 ||
	     (fwspec->param[0] == 0 && fwspec->param[1] >= 988)))
		return -EINVAL;
#endif
	ret = msm_mpm_gic_chip_translate(domain, fwspec, &hwirq, &type);
	if (ret)
		return ret;

#ifdef CONFIG_QTI_MPM_C5LTE
	if (msm_mpm_is_c5_domain(domain)) {
		/* The measured C5 uses GICv2, not LPIs or multi-IRQ allocations. */
		if (nr_irqs != 1 || fwspec->param_count != 3 ||
		    fwspec->param[0] > 1 ||
		    (fwspec->param[0] == 1 && fwspec->param[1] >= 16))
			return -EINVAL;
		ret = mpm_c5lte_xo_validate(domain, hwirq);
		if (ret)
			return ret;
		chip = &msm_mpm_c5_gic_chip;
	}
#endif
	parent_fwspec = *fwspec;
	parent_fwspec.fwnode = domain->parent->fwnode;
	ret = irq_domain_alloc_irqs_parent(domain, virq, nr_irqs,
					 &parent_fwspec);
	if (ret)
		return ret;
	irq_domain_set_hwirq_and_chip(domain, virq, hwirq,
						chip, NULL);

	return 0;
}

static const struct irq_domain_ops msm_mpm_gic_chip_domain_ops = {
	.translate	= msm_mpm_gic_chip_translate,
	.alloc		= msm_mpm_gic_chip_alloc,
	.free		= msm_mpm_chip_free,
};

static inline void msm_mpm_send_interrupt(void)
{
	writel_relaxed(2, msm_mpm_dev_data.mpm_ipc_reg);
	/* Ensure the write is complete before returning. */
	wmb();
}

static inline void msm_mpm_timer_write(uint32_t *expiry)
{
	writel_relaxed(expiry[0], msm_mpm_dev_data.mpm_request_reg_base);
	writel_relaxed(expiry[1], msm_mpm_dev_data.mpm_request_reg_base + 0x4);
}

static void msm_mpm_enter_sleep(struct cpumask *cpumask)
{
	/* Publish the request before notifying RPM, as in the C5 baseline. */
	wmb();
	msm_mpm_send_interrupt();
	irq_set_affinity(msm_mpm_dev_data.ipc_irq, cpumask);
}

static int msm_get_apps_irq(unsigned int mpm_irq)
{
	struct mpm_pin *mpm_pin = NULL;
	int apps_irq;

	mpm_pin = msm_mpm_dev_data.gic_chip_domain ?
		msm_mpm_dev_data.gic_chip_domain->host_data : NULL;
	apps_irq = msm_get_irq_pin(mpm_irq, mpm_pin);
	if (apps_irq >= 0)
		return apps_irq;

	mpm_pin = msm_mpm_dev_data.gpio_chip_domain ?
		msm_mpm_dev_data.gpio_chip_domain->host_data : NULL;
	return  msm_get_irq_pin(mpm_irq, mpm_pin);

}

static void system_pm_exit_sleep(bool success)
{
	msm_rpm_exit_sleep();
}

static cycle_t us_to_ticks(uint64_t sleep_val)
{
	uint64_t sec, fraction;
	cycle_t wakeup;

	sec = sleep_val;
	do_div(sec, USEC_PER_SEC);
	/* Divide after scaling the remainder, not before: preserve subsecond
	 * ticks without overflowing the full microsecond input multiplication.
	 * Caller uses a positive int-seconds override, bounded below U64_MAX.
	 */
	fraction = (sleep_val - sec * USEC_PER_SEC) * ARCH_TIMER_HZ;
	do_div(fraction, USEC_PER_SEC);
	wakeup = sec * ARCH_TIMER_HZ + fraction;

	if (sleep_val)
		wakeup += arch_counter_get_cntvct();
	else
		wakeup = (~0ULL);

	return wakeup;
}

static int system_pm_update_wakeup(bool from_idle)
{
	uint64_t wake_time;
	uint32_t lo = ~0U, hi = ~0U;
	cycle_t wakeup;

	if (unlikely(!from_idle && msm_pm_sleep_time_override > 0)) {
		wake_time = (u64)msm_pm_sleep_time_override * USEC_PER_SEC;
		wakeup = us_to_ticks(wake_time);
	} else {
		/* Read the hardware to get the most accurate value */

		arch_timer_mem_get_cval(&lo, &hi);
		wakeup = lo;
		wakeup |= ((uint64_t)(hi) << 32);
	}

	msm_mpm_timer_write((uint32_t *)&wakeup);
	trace_mpm_wakeup_time(from_idle, wakeup, arch_counter_get_cntvct());

	return 0;
}

static int system_pm_enter_sleep(struct cpumask *mask)
{
	int ret = 0;

	/* Recheck after level selection, before entering RPM sleep. */
	if (!mpm_c5lte_xo_sleep_allowed())
		return -EBUSY;
	ret = msm_rpm_enter_sleep(0, mask);
	if (ret)
		return ret;
	msm_mpm_enter_sleep(mask);
	return ret;
}

#ifdef CONFIG_QTI_MPM_C5LTE
static atomic_t c5_timer_handoff_armed = ATOMIC_INIT(0);
static bool c5_timer_handoff_active;

/* UNSELECTED C5 compatibility draft: one synchronous replay owner per
 * successful enter_with_wakeup transaction. The caller serializes system
 * entry/exit with bc_timer_lock; mpm_lock protects this intent and masks.
 * These are functional ownership fields, never failure/site records.
 */
static bool c5_mpm_sync_exit_armed;
static bool c5_mpm_sync_exit_from_idle;

static bool c5_mpm_sync_exit_available(void)
{
	unsigned long flags;
	bool available;

	spin_lock_irqsave(&mpm_lock, flags);
	available = !c5_mpm_sync_exit_armed;
	spin_unlock_irqrestore(&mpm_lock, flags);
	return available;
}

static int c5_mpm_sync_exit_arm(bool from_idle)
{
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&mpm_lock, flags);
	if (c5_mpm_sync_exit_armed)
		ret = -EBUSY;
	else {
		c5_mpm_sync_exit_from_idle = from_idle;
		c5_mpm_sync_exit_armed = true;
	}
	spin_unlock_irqrestore(&mpm_lock, flags);
	return ret;
}

static bool c5_mpm_sync_exit_snapshot(u32 pending[C5_MPM_WORDS])
{
	unsigned long flags;
	unsigned int i;
	bool armed = false;

	if (!msm_mpm_is_c5_domain(msm_mpm_dev_data.gic_chip_domain) ||
	    num_mpm_irqs != C5_MPM_WORDS * 32)
		return false;
	spin_lock_irqsave(&mpm_lock, flags);
	if (c5_mpm_sync_exit_armed) {
		for (i = 0; i < C5_MPM_WORDS; i++)
			pending[i] = msm_mpm_read(MPM_REG_STATUS, i) &
				(c5_mpm_sync_exit_from_idle ? c5_mpm_enabled[i] :
				 c5_mpm_wake[i]);
		c5_mpm_sync_exit_armed = false;
		armed = true;
	}
	spin_unlock_irqrestore(&mpm_lock, flags);
	/* No descriptor/chip API may run while mpm_lock is held. */
	return armed;
}

/* Restore the C5 baseline's separate idle-enabled and suspend-wake sets.
 * Firmware may update vMPM memory, so publish cached Linux intent at entry.
 * No provider for a different SoC/domain opts into this fixed-size protocol.
 */
static int c5_mpm_publish_request(bool from_idle)
{
	unsigned long flags;
	unsigned int i;

	if (!msm_mpm_is_c5_domain(msm_mpm_dev_data.gic_chip_domain) ||
	    num_mpm_irqs != C5_MPM_WORDS * 32)
		return -EINVAL;
	spin_lock_irqsave(&mpm_lock, flags);
	for (i = 0; i < C5_MPM_WORDS; i++) {
		msm_mpm_write(MPM_REG_ENABLE, i,
			from_idle ? c5_mpm_enabled[i] : c5_mpm_wake[i]);
		msm_mpm_write(MPM_REG_FALLING_EDGE, i, c5_mpm_type[0][i]);
		msm_mpm_write(MPM_REG_RISING_EDGE, i, c5_mpm_type[1][i]);
		msm_mpm_write(MPM_REG_POLARITY, i, c5_mpm_type[2][i]);
		msm_mpm_write(MPM_REG_STATUS, i, 0);
	}
	spin_unlock_irqrestore(&mpm_lock, flags);
	return 0;
}

static int c5_system_pm_enter_with_wakeup(struct cpumask *mask, bool from_idle)
{
	int ret;

	if (!c5_mpm_sync_exit_available() || !mpm_c5lte_xo_sleep_allowed())
		return -EBUSY;
	ret = msm_rpm_enter_sleep(0, mask);
	if (ret)
		return ret;
	/* Publish C5's request before notifying RPM, matching the 3.10 order.
	 * Preserve the 3.10 protocol order without deferring the IPC past
	 * entry consumers such as CPU hotplug.
	 */
	ret = system_pm_update_wakeup(from_idle);
	if (ret) {
		msm_rpm_exit_sleep();
		return ret;
	}
	ret = c5_mpm_publish_request(from_idle);
	if (ret) {
		msm_rpm_exit_sleep();
		return ret;
	}
	ret = c5_mpm_sync_exit_arm(from_idle);
	if (ret) {
		msm_rpm_exit_sleep();
		return ret;
	}
	atomic_inc(&c5_timer_handoff_armed);
	msm_mpm_enter_sleep(mask);
	return 0;
}
#endif

static bool system_pm_sleep_allowed(void)
{
	return mpm_c5lte_xo_sleep_allowed() && !msm_rpm_waiting_for_ack();
}

static struct system_pm_ops pm_ops = {
	.enter = system_pm_enter_sleep,
	.exit = system_pm_exit_sleep,
	.update_wakeup = system_pm_update_wakeup,
	.sleep_allowed = system_pm_sleep_allowed,
};

#ifdef CONFIG_QTI_MPM_C5LTE
static int c5_timer_handoff_stats_get(char *buffer, const struct kernel_param *kp)
{
	(void)kp;
	return scnprintf(buffer, PAGE_SIZE, "C5HANDOFF1 ACTIVE=%u ARMED=%u\n",
		(unsigned int)READ_ONCE(c5_timer_handoff_active),
		(unsigned int)atomic_read(&c5_timer_handoff_armed));
}

static const struct kernel_param_ops c5_timer_handoff_stats_ops = {
	.get = c5_timer_handoff_stats_get,
};
module_param_cb(c5_timer_handoff_stats, &c5_timer_handoff_stats_ops, NULL, 0400);
#endif


#ifdef CONFIG_QTI_MPM_C5LTE
static atomic_t c5_replay_ok = ATOMIC_INIT(0);
static atomic_t c5_replay_error = ATOMIC_INIT(0);

static int c5_replay_stats_get(char *buffer, const struct kernel_param *kp)
{
	(void)kp;
	/* Aggregate completed calls only; no reset, payload or IRQ identities. */
	return scnprintf(buffer, PAGE_SIZE, "C5REPLAY1 OK=%u ERR=%u\n",
		(unsigned int)atomic_read(&c5_replay_ok),
		(unsigned int)atomic_read(&c5_replay_error));
}

static const struct kernel_param_ops c5_replay_stats_ops = {
	.get = c5_replay_stats_get,
};
module_param_cb(c5_replay_stats, &c5_replay_stats_ops, NULL, 0400);
#endif

static int msm_mpm_replay_irq(unsigned int irq)
{
	int ret = irq_set_irqchip_state(irq, IRQCHIP_STATE_PENDING, true);

#ifdef CONFIG_QTI_MPM_C5LTE
	/* Only the measured C5 GPIO hierarchy lacks the hardware callback. */
	if (ret == -EINVAL &&
	    msm_mpm_is_c5_domain(msm_mpm_dev_data.gpio_chip_domain) &&
	    irq_domain_get_irq_data(msm_mpm_dev_data.gpio_chip_domain, irq)) {
		ret = c5_irq_replay_pending(irq);
		if (ret)
			atomic_inc(&c5_replay_error);
		else
			atomic_inc(&c5_replay_ok);
	}
#endif
	return ret;
}

#ifdef CONFIG_QTI_MPM_C5LTE
static void c5_system_pm_sync_exit_sleep(bool success)
{
	u32 pending[C5_MPM_WORDS];
	unsigned int i, bit;
	int apps_irq;
	struct irq_desc *desc;

	(void)success;
	/* Preserve the old RPM-unmask then synchronous-MPM-replay order. */
	msm_rpm_exit_sleep();
	if (!c5_mpm_sync_exit_snapshot(pending))
		return;
	for (i = 0; i < C5_MPM_WORDS; i++) {
		for (bit = 0; bit < 32; bit++) {
			if (!(pending[i] & BIT(bit)))
				continue;
			apps_irq = msm_get_apps_irq(i * 32 + bit);
			desc = apps_irq > 0 ? irq_to_desc(apps_irq) : NULL;
			if (desc && !irqd_is_level_type(&desc->irq_data))
				msm_mpm_replay_irq(apps_irq);
		}
	}
	/* Shared STATUS remains untouched, as in the old synchronous exit.
	 * The existing next successful C5 entry clears it before notifying RPM.
	 */
}
#endif

/*
 * Triggered by RPM when system resumes from deep sleep
 */
static irqreturn_t msm_mpm_irq(int irq, void *dev_id)
{
	unsigned long pending;
	uint32_t value[3];
	int i, k, apps_irq;
	unsigned int mpm_irq;
	struct irq_desc *desc = NULL;
	unsigned int reg = MPM_REG_ENABLE;

#ifdef CONFIG_QTI_MPM_C5LTE
	/* Old C5 summary is ack-only; synchronous exit owns its replay. */
	if (msm_mpm_is_c5_domain(msm_mpm_dev_data.gic_chip_domain))
		return IRQ_HANDLED;
#endif
	for (i = 0; i < QCOM_MPM_REG_WIDTH; i++) {
		value[i] = msm_mpm_read(reg, i);
		trace_mpm_wakeup_enable_irqs(i, value[i]);
	}

	for (i = 0; i < QCOM_MPM_REG_WIDTH; i++) {
		pending = msm_mpm_read(MPM_REG_STATUS, i);
		pending &= (unsigned long)value[i];

		trace_mpm_wakeup_pending_irqs(i, pending);
		for_each_set_bit(k, &pending, 32) {
			mpm_irq = 32 * i + k;
			apps_irq = msm_get_apps_irq(mpm_irq);
			desc = apps_irq > 0 ?
				irq_to_desc(apps_irq) : NULL;

			if (desc && !irqd_is_level_type(&desc->irq_data))
				msm_mpm_replay_irq(apps_irq);

		}

		msm_mpm_write(MPM_REG_STATUS, i, 0);
	}
	return IRQ_HANDLED;
}

static int msm_mpm_init(struct device_node *node)
{
	struct msm_mpm_device_data *dev = &msm_mpm_dev_data;
	int ret = 0;
	int irq;

	dev->mpm_request_reg_base = of_iomap_by_name(node, "vmpm");
	if (!dev->mpm_request_reg_base) {
		pr_err("Unable to iomap\n");
		ret = -EADDRNOTAVAIL;
		goto reg_base_err;
	}

	dev->mpm_ipc_reg = of_iomap_by_name(node, "ipc");
	if (!dev->mpm_ipc_reg) {
		pr_err("Unable to iomap IPC register\n");
		ret = -EADDRNOTAVAIL;
		goto ipc_reg_err;
	}

	irq = of_irq_get(node, 0);
	if (irq <= 0) {
		pr_err("no IRQ resource info\n");
		ret = irq ? irq : -ENXIO;
		goto ipc_irq_err;
	}
	dev->ipc_irq = irq;

	ret = request_irq(dev->ipc_irq, msm_mpm_irq,
		IRQF_TRIGGER_RISING | IRQF_NO_SUSPEND, "mpm",
		msm_mpm_irq);
	if (ret) {
		pr_err("request_irq failed errno: %d\n", ret);
		goto ipc_irq_err;
	}

	ret = irq_set_irq_wake(dev->ipc_irq, 1);
	if (ret) {
		pr_err("failed to set wakeup irq %lu: %d\n",
			dev->ipc_irq, ret);
		goto set_wake_irq_err;
	}

#ifdef CONFIG_QTI_MPM_C5LTE
	/* Opt in only after the exact C5 GIC domain has been initialized. */
	if (msm_mpm_is_c5_domain(dev->gic_chip_domain)) {
		pm_ops.enter_with_wakeup = c5_system_pm_enter_with_wakeup;
		pm_ops.exit = c5_system_pm_sync_exit_sleep;
	}
#endif
	ret = register_system_pm_ops(&pm_ops);
	if (!ret) {
#ifdef CONFIG_QTI_MPM_C5LTE
		WRITE_ONCE(c5_timer_handoff_active,
			pm_ops.enter_with_wakeup == c5_system_pm_enter_with_wakeup);
#endif
		return 0;
	}
	irq_set_irq_wake(dev->ipc_irq, 0);

set_wake_irq_err:
	free_irq(dev->ipc_irq, msm_mpm_irq);
ipc_irq_err:
	iounmap(dev->mpm_ipc_reg);
	dev->mpm_ipc_reg = NULL;
ipc_reg_err:
	iounmap(dev->mpm_request_reg_base);
	dev->mpm_request_reg_base = NULL;
reg_base_err:
	return ret;
}

static const struct of_device_id mpm_gic_chip_data_table[] = {
#ifdef CONFIG_QTI_MPM_C5LTE
	{
		.compatible = "qcom,mpm-gic-c5lte",
		.data = mpm_c5lte_gic_chip_data,
	},
#endif
	{
		.compatible = "qcom,mpm-gic-msm8953",
		.data = mpm_msm8953_gic_chip_data,
	},
	{
		.compatible = "qcom,mpm-gic-msm8937",
		.data = mpm_msm8937_gic_chip_data,
	},
	{
		.compatible = "qcom,mpm-gic-msm8909",
		.data = mpm_msm8909_gic_chip_data,
	},
	{}
};

MODULE_DEVICE_TABLE(of, mpm_gic_chip_data_table);

static const struct of_device_id mpm_gpio_chip_data_table[] = {
#ifdef CONFIG_QTI_MPM_C5LTE
	{
		.compatible = "qcom,mpm-gpio-c5lte",
		.data = mpm_c5lte_gpio_chip_data,
	},
#endif
	{
		.compatible = "qcom,mpm-gpio-msm8953",
		.data = mpm_msm8953_gpio_chip_data,
	},
	{
		.compatible = "qcom,mpm-gpio-msm8937",
		.data = mpm_msm8937_gpio_chip_data,
	},
	{
		.compatible = "qcom,mpm-gpio-msm8909",
		.data = mpm_msm8909_gpio_chip_data,
	},
	{}
};

MODULE_DEVICE_TABLE(of, mpm_gpio_chip_data_table);

static int __init mpm_gic_chip_init(struct device_node *node,
					struct device_node *parent)
{
	struct irq_domain *parent_domain;
	const struct of_device_id *id;
	int ret;
	bool c5 = of_device_is_compatible(node, "qcom,mpm-gic-c5lte");

#ifdef CONFIG_QTI_MPM_C5LTE
	if (c5)
		mpm_c5lte_xo_activate(); /* Failure must leave the sleep gate closed. */
#endif

	if (!parent) {
		pr_err("%s(): no parent for mpm-gic\n", node->full_name);
		return -ENXIO;
	}

	parent_domain = irq_find_host(parent);
	if (!parent_domain) {
		pr_err("unable to obtain gic parent domain\n");
		return -ENXIO;
	}

	of_property_read_u32(node, "qcom,num-mpm-irqs", &num_mpm_irqs);
	if (c5 && num_mpm_irqs != 64)
		return -EINVAL;

	mpm_to_irq = kcalloc(num_mpm_irqs, sizeof(*mpm_to_irq), GFP_KERNEL);
	if (!mpm_to_irq)
		return -ENOMEM;

	id = of_match_node(mpm_gic_chip_data_table, node);
	if (!id) {
		pr_err("can not find mpm_gic_data_table of_node\n");
		ret = -ENODEV;
		goto mpm_map_err;
	}

	msm_mpm_dev_data.gic_chip_domain = irq_domain_add_hierarchy(
			parent_domain, 0, num_mpm_irqs, node,
			&msm_mpm_gic_chip_domain_ops, (void *)id->data);
	if (!msm_mpm_dev_data.gic_chip_domain) {
		pr_err("gic domain add failed\n");
		ret = -ENOMEM;
		goto mpm_map_err;
	}

	msm_mpm_dev_data.gic_chip_domain->name = "qcom,mpm-gic";

#ifdef CONFIG_QTI_MPM_C5LTE
	if (c5) {
		msm_mpm_c5_gic_chip = msm_mpm_gic_chip;
		msm_mpm_c5_gic_chip.flags &= ~IRQCHIP_SKIP_SET_WAKE;
		msm_mpm_c5_gic_chip.irq_set_wake = msm_mpm_c5_set_wake;
		ret = mpm_c5lte_xo_domain_init(msm_mpm_dev_data.gic_chip_domain,
					      false);
		if (ret)
			goto domain_err;
	}
#endif

	ret = msm_mpm_init(node);
	if (!ret)
		return ret;
#ifdef CONFIG_QTI_MPM_C5LTE
	if (c5)
		mpm_c5lte_xo_domain_remove(msm_mpm_dev_data.gic_chip_domain, false);
domain_err:
#endif
	irq_domain_remove(msm_mpm_dev_data.gic_chip_domain);
	msm_mpm_dev_data.gic_chip_domain = NULL;

mpm_map_err:
	kfree(mpm_to_irq);
	mpm_to_irq = NULL;
	return ret;
}

IRQCHIP_DECLARE(mpm_gic_chip, "qcom,mpm-gic", mpm_gic_chip_init);

static int __init mpm_gpio_chip_init(struct device_node *node,
					struct device_node *parent)
{
	const struct of_device_id *id;
#ifdef CONFIG_QTI_MPM_C5LTE
	int ret;

	if (of_device_is_compatible(node, "qcom,mpm-gpio-c5lte")) {
		mpm_c5lte_xo_activate();
		/* Force early ordering and reject a half-initialized C5 MPM. */
		if (!parent || !msm_mpm_dev_data.gic_chip_domain ||
		    irq_find_host(parent) != msm_mpm_dev_data.gic_chip_domain ||
		    !msm_mpm_is_c5_domain(msm_mpm_dev_data.gic_chip_domain) ||
		    !msm_mpm_dev_data.mpm_request_reg_base)
			return -ENXIO;
	}
#endif

	id = of_match_node(mpm_gpio_chip_data_table, node);
	if (!id) {
		pr_err("match_table not found for mpm-gpio\n");
		return -ENODEV;
	}

	msm_mpm_dev_data.gpio_chip_domain = irq_domain_create_linear(
			of_node_to_fwnode(node), num_mpm_irqs,
			&msm_mpm_gpio_chip_domain_ops, (void *)id->data);

	if (!msm_mpm_dev_data.gpio_chip_domain)
		return -ENOMEM;

	msm_mpm_dev_data.gpio_chip_domain->name = "qcom,mpm-gpio";

#ifdef CONFIG_QTI_MPM_C5LTE
	if (msm_mpm_is_c5_domain(msm_mpm_dev_data.gpio_chip_domain)) {
		mpm_c5lte_xo_activate();
		msm_mpm_c5_gpio_chip = msm_mpm_gpio_chip;
		msm_mpm_c5_gpio_chip.flags &= ~IRQCHIP_SKIP_SET_WAKE;
		msm_mpm_c5_gpio_chip.irq_set_wake = msm_mpm_c5_set_wake;
		ret = mpm_c5lte_xo_domain_init(msm_mpm_dev_data.gpio_chip_domain,
					      true);
		if (ret) {
			irq_domain_remove(msm_mpm_dev_data.gpio_chip_domain);
			msm_mpm_dev_data.gpio_chip_domain = NULL;
			return ret;
		}
	}
#endif

	return 0;
}

IRQCHIP_DECLARE(mpm_gpio_chip, "qcom,mpm-gpio", mpm_gpio_chip_init);
