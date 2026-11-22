/* SPDX-License-Identifier: GPL-2.0-only */
/* C5 component experiment: conservative XO vote, not suspend validation.
 * Keep XO for the union of enabled and wake-enabled unlisted IRQs. This is
 * deliberately stricter than the legacy idle/suspend bitmap switch: no PM
 * notifier or suspend-abort race can open the gate using the wrong bitmap.
 * IRQ callbacks only change state; all sleeping clock operations run in work.
 */
#include <linux/bitmap.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include "mpm.h"

/* GICv2 architectural SPI/PPI range, not a guessed C5 interrupt count. */
#define C5_GIC_MAX_IRQS 1020
#define C5_GPIO_IRQS 134

struct c5_xo_domain {
	struct irq_domain *domain;
	unsigned int count;
	DECLARE_BITMAP(enabled, C5_GIC_MAX_IRQS);
	DECLARE_BITMAP(wake, C5_GIC_MAX_IRQS);
};

static struct c5_xo_domain c5_xo_domains[2];
static DEFINE_SPINLOCK(c5_xo_lock);
static struct clk *c5_xo_clk;
static bool c5_xo_active;
static bool c5_xo_worker_ready;
static bool c5_xo_ready;
static bool c5_xo_held;
static bool c5_xo_settled;
static bool c5_xo_fault;
static unsigned long c5_xo_generation;

static bool c5_xo_needed(void)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(c5_xo_domains); i++) {
		struct c5_xo_domain *s = &c5_xo_domains[i];

		if (!bitmap_empty(s->enabled, s->count) ||
		    !bitmap_empty(s->wake, s->count))
			return true;
	}
	return false;
}

static void c5_xo_reconcile(struct work_struct *work)
{
	unsigned long flags, generation;
	bool needed, held;
	int ret;

	for (;;) {
		spin_lock_irqsave(&c5_xo_lock, flags);
		generation = c5_xo_generation;
		needed = c5_xo_needed() || c5_xo_fault;
		held = c5_xo_held;
		spin_unlock_irqrestore(&c5_xo_lock, flags);

		if (needed && !held) {
			ret = clk_prepare_enable(c5_xo_clk);
			if (ret) {
				/* Fail closed; retry only on a subsequent state change. */
				pr_err("c5-mpm: XO vote failed: %d\n", ret);
				return;
			}
			held = true;
		} else if (!needed && held) {
			clk_disable_unprepare(c5_xo_clk);
			held = false;
		}

		spin_lock_irqsave(&c5_xo_lock, flags);
		c5_xo_held = held;
		c5_xo_settled = generation == c5_xo_generation;
		if (c5_xo_settled) {
			spin_unlock_irqrestore(&c5_xo_lock, flags);
			return;
		}
		spin_unlock_irqrestore(&c5_xo_lock, flags);
	}
}

static DECLARE_WORK(c5_xo_work, c5_xo_reconcile);

void mpm_c5lte_xo_activate(void)
{
	unsigned long flags;

	spin_lock_irqsave(&c5_xo_lock, flags);
	c5_xo_active = true;
	spin_unlock_irqrestore(&c5_xo_lock, flags);
}

int mpm_c5lte_xo_domain_init(struct irq_domain *domain, bool gpio)
{
	struct c5_xo_domain *s = &c5_xo_domains[gpio];
	unsigned int count;
	unsigned long flags;
	int ret = 0;

	/* Only the measured GICv2 and 134-pin C5 hierarchy is accepted. */
	if (!domain || (!gpio && !domain->parent))
		return -EINVAL;
	count = gpio ? C5_GPIO_IRQS : domain->parent->revmap_size;
	if ((!gpio && (count <= 331 || count > C5_GIC_MAX_IRQS)))
		return -EINVAL;
	spin_lock_irqsave(&c5_xo_lock, flags);
	if (s->domain)
		ret = -EBUSY;
	else {
		s->domain = domain;
		s->count = count;
	}
	spin_unlock_irqrestore(&c5_xo_lock, flags);
	return ret;
}

void mpm_c5lte_xo_domain_remove(struct irq_domain *domain, bool gpio)
{
	unsigned long flags;

	spin_lock_irqsave(&c5_xo_lock, flags);
	if (c5_xo_domains[gpio].domain == domain) {
		c5_xo_domains[gpio].domain = NULL;
		c5_xo_ready = false;
	}
	spin_unlock_irqrestore(&c5_xo_lock, flags);
}

int mpm_c5lte_xo_validate(struct irq_domain *domain, irq_hw_number_t hwirq)
{
	unsigned int i;

	/* Domains/counts are published before allocation and remain built-in. */
	for (i = 0; i < ARRAY_SIZE(c5_xo_domains); i++)
		if (c5_xo_domains[i].domain == domain)
			return hwirq < c5_xo_domains[i].count ? 0 : -EINVAL;
	return -ENODEV;
}

int mpm_c5lte_xo_update(struct irq_data *d, bool wake, bool on)
{
	struct c5_xo_domain *s;
	unsigned long *bits, flags;
	unsigned int i;
	int ret = -EINVAL;

	spin_lock_irqsave(&c5_xo_lock, flags);
	for (i = 0; i < ARRAY_SIZE(c5_xo_domains); i++) {
		s = &c5_xo_domains[i];
		if (s->domain != d->domain)
			continue;
		if (d->hwirq >= s->count)
			break;
		ret = 0;
		if (mpm_c5lte_classify_irq(i, d->hwirq) !=
		    MPM_C5LTE_IRQ_UNLISTED)
			goto out;
		bits = wake ? s->wake : s->enabled;
		if (!!test_bit(d->hwirq, bits) == on)
			goto out;
		if (on)
			__set_bit(d->hwirq, bits);
		else
			__clear_bit(d->hwirq, bits);
		break;
	}
	if (ret)
		c5_xo_fault = true; /* Unexpected domain/IRQ cannot enable sleep. */
	c5_xo_generation++;
	c5_xo_settled = false;
	if (c5_xo_worker_ready)
		schedule_work(&c5_xo_work);
out:
	spin_unlock_irqrestore(&c5_xo_lock, flags);
	return ret;
}

bool mpm_c5lte_xo_sleep_allowed(void)
{
	unsigned long flags;
	bool allowed;

	spin_lock_irqsave(&c5_xo_lock, flags);
	allowed = !c5_xo_active || (c5_xo_ready && c5_xo_settled &&
		 !c5_xo_fault && !c5_xo_held && !c5_xo_needed());
	spin_unlock_irqrestore(&c5_xo_lock, flags);
	return allowed;
}

static int c5_xo_probe(struct platform_device *pdev)
{
	struct clk *clk;
	struct device_node *node;
	struct irq_domain *domain;
	unsigned int i;
	unsigned long flags;
	int ret;

	/* Early IRQ setup must never acquire a sleepable/deferred clock. */
	if (!c5_xo_active || !c5_xo_domains[0].domain ||
	    !c5_xo_domains[1].domain)
		return -EPROBE_DEFER;
	if (c5_xo_worker_ready)
		return -EBUSY;
	/* IRQCHIP_DECLARE marks its nodes OF_POPULATED. A separate policy
	 * node must reference the exact early domains; do not clear that flag.
	 */
	for (i = 0; i < ARRAY_SIZE(c5_xo_domains); i++) {
		node = of_parse_phandle(pdev->dev.of_node,
				i ? "qcom,mpm-gpio-domain" : "qcom,mpm-gic-domain", 0);
		if (!node)
			return -EINVAL;
		domain = irq_find_host(node);
		of_node_put(node);
		if (domain != c5_xo_domains[i].domain)
			return domain ? -EINVAL : -EPROBE_DEFER;
	}
	clk = devm_clk_get(&pdev->dev, "xo");
	if (IS_ERR(clk))
		return PTR_ERR(clk);
	ret = clk_prepare_enable(clk);
	if (ret)
		return ret;
	spin_lock_irqsave(&c5_xo_lock, flags);
	c5_xo_clk = clk;
	c5_xo_held = true;
	c5_xo_worker_ready = true;
	c5_xo_ready = true;
	c5_xo_settled = false;
	schedule_work(&c5_xo_work);
	spin_unlock_irqrestore(&c5_xo_lock, flags);
	return 0;
}

/* Built-in static IRQ domains: no manual unbind/module-unload contract. */
static const struct of_device_id c5_xo_match[] = {
	{ .compatible = "qcom,mpm-c5lte-xo" },
	{ },
};

static struct platform_driver c5_xo_driver = {
	.probe = c5_xo_probe,
	.driver = {
		.name = "c5-mpm-xo",
		.of_match_table = c5_xo_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(c5_xo_driver);
