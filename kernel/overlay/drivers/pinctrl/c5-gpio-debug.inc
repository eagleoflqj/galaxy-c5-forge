/* SPDX-License-Identifier: GPL-2.0-only */
/* Included after native GP helpers. No implicit sampling at initialization. */
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#define C5_GPIO_DEBUG_MAX_PINS 134U

struct c5_gpio_debug_row {
	u16 pin;
	u8 func, drv, pull, dir, value;
};

struct c5_gpio_debug_snapshot {
	unsigned int count;
	struct c5_gpio_debug_row rows[C5_GPIO_DEBUG_MAX_PINS];
};

static DEFINE_RAW_SPINLOCK(c5_gpio_debug_lock);
static struct platform_device *c5_gpio_debug_owner;
static bool c5_gpio_debug_ready;
static unsigned int c5_gpio_debug_samples;

static bool c5_gpio_debug_excluded(unsigned int pin)
{
	/* Fitted C5 secure SPI0 fingerprint, MST data pins and SPI8 eSE. */
	return pin <= 3 || pin == MST_GPIO_D_MINUS || pin == MST_GPIO_D_PLUS ||
		(pin >= 96 && pin <= 99);
}

static int c5_gpio_debug_claim(struct platform_device *pdev)
{
	unsigned long flags;
	int rc = 0;

	raw_spin_lock_irqsave(&c5_gpio_debug_lock, flags);
	if (c5_gpio_debug_owner)
		rc = -EBUSY;
	else {
		c5_gpio_debug_owner = pdev;
		c5_gpio_debug_ready = false;
	}
	raw_spin_unlock_irqrestore(&c5_gpio_debug_lock, flags);
	return rc;
}

static void c5_gpio_debug_release(struct platform_device *pdev)
{
	unsigned long flags;

	/* Also waits for every in-progress register snapshot before devres teardown. */
	raw_spin_lock_irqsave(&c5_gpio_debug_lock, flags);
	if (c5_gpio_debug_owner == pdev) {
		c5_gpio_debug_ready = false;
		c5_gpio_debug_owner = NULL;
	}
	raw_spin_unlock_irqrestore(&c5_gpio_debug_lock, flags);
}

static void c5_gpio_debug_publish(struct platform_device *pdev)
{
	struct msm_pintype_info *pinfo = &tlmm_pininfo[0];
	struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	unsigned long flags;
	u64 end = 0;
	bool valid = false;

	if (res && pinfo->pintype_data && !IS_ERR_OR_NULL(pinfo->reg_base) &&
	    pinfo->num_pins && pinfo->num_pins <= C5_GPIO_DEBUG_MAX_PINS &&
	    pinfo->pintype_data->gp_reg_size >= 8) {
		end = (u64)pinfo->pintype_data->reg_base_offset +
			(u64)(pinfo->num_pins - 1) *
			pinfo->pintype_data->gp_reg_size + 8;
		valid = end <= resource_size(res);
	}
	raw_spin_lock_irqsave(&c5_gpio_debug_lock, flags);
	if (c5_gpio_debug_owner == pdev)
		c5_gpio_debug_ready = valid;
	raw_spin_unlock_irqrestore(&c5_gpio_debug_lock, flags);
}

static int c5_gpio_debug_snapshot(struct c5_gpio_debug_snapshot *snapshot)
{
	struct msm_pintype_info *pinfo = &tlmm_pininfo[0];
	struct c5_gpio_debug_row *row;
	unsigned long flags;
	unsigned int pin, cfg, inout;
	int rc = 0;

	snapshot->count = 0;
	raw_spin_lock_irqsave(&c5_gpio_debug_lock, flags);
	if (!c5_gpio_debug_ready) {
		rc = -ENODEV;
		goto out;
	}
	for (pin = 0; pin < pinfo->num_pins; pin++) {
		/* This precedes register-address construction and both MMIO reads. */
		if (c5_gpio_debug_excluded(pin))
			continue;
		cfg = readl_relaxed(TLMM_GP_CFG(pinfo, pin));
		inout = readl_relaxed(TLMM_GP_INOUT(pinfo, pin));
		row = &snapshot->rows[snapshot->count++];
		row->pin = pin;
		row->pull = cfg & 3;
		row->func = (cfg >> 2) & 15;
		row->drv = (cfg >> 6) & 7;
		row->dir = (cfg & BIT(9)) ? GPIOMUX_OUT_HIGH : GPIOMUX_IN;
		if (row->func == GPIOMUX_FUNC_GPIO && row->dir)
			row->dir = (inout & BIT(1)) ? GPIOMUX_OUT_HIGH : GPIOMUX_OUT_LOW;
		row->value = row->dir ? (inout & BIT(1)) : (inout & BIT(0));
	}
	if (c5_gpio_debug_samples != ~0U)
		c5_gpio_debug_samples++;
out:
	raw_spin_unlock_irqrestore(&c5_gpio_debug_lock, flags);
	return rc;
}

static const char * const c5_gpio_debug_func[] = {
	"GPIO", "Func_1", "Func_2", "Func_3", "Func_4", "Func_5", "Func_6", "Func_7",
	"Func_8", "Func_9", "Func_a", "Func_b", "Func_c", "Func_d", "Func_e", "Func_f",
};
static const char * const c5_gpio_debug_drv[] = {
	"DRV_2mA", "DRV_4mA", "DRV_6mA", "DRV_8mA", "DRV_10mA", "DRV_12mA", "DRV_14mA", "DRV_16mA",
};
static const char * const c5_gpio_debug_pull[] = {
	"PULL_NONE", "PULL_DOWN", "PULL_KEEPER", "PULL_UP",
};
static const char * const c5_gpio_debug_dir[] = { "IN", "OUT_HIGH", "OUT_LOW" };
static const char * const c5_gpio_debug_out[] = { "VAL_LOW", "NULL", "VAL_HIGH" };
static const char * const c5_gpio_debug_in[] = { "VAL_LOW", "VAL_HIGH" };

static int c5_gpio_debug_print(struct seq_file *m)
{
	struct c5_gpio_debug_snapshot *snapshot;
	const struct c5_gpio_debug_row *row;
	const char *value;
	unsigned int i;
	int rc;

	/* The legacy print helper can be called with IRQs disabled. */
	snapshot = kzalloc(sizeof(*snapshot), GFP_ATOMIC);
	if (!snapshot)
		return -ENOMEM;
	rc = c5_gpio_debug_snapshot(snapshot);
	if (rc)
		goto out;
	for (i = 0; i < snapshot->count; i++) {
		row = &snapshot->rows[i];
		value = row->dir ? c5_gpio_debug_out[row->value] : c5_gpio_debug_in[row->value];
		if (IS_ERR_OR_NULL(m))
			pr_info("GPIO[%u] \t%s \t%s \t%s \t%s \t%s\n", row->pin,
				c5_gpio_debug_func[row->func], c5_gpio_debug_dir[row->dir],
				c5_gpio_debug_pull[row->pull], c5_gpio_debug_drv[row->drv], value);
		else
			seq_printf(m, "GPIO[%u] \t%s \t%s \t%s \t%s \t%s\n", row->pin,
				c5_gpio_debug_func[row->func], c5_gpio_debug_dir[row->dir],
				c5_gpio_debug_pull[row->pull], c5_gpio_debug_drv[row->drv], value);
	}
out:
	kfree(snapshot);
	return rc;
}

void msm_gpio_print_enabled(void)
{
	int rc = c5_gpio_debug_print(NULL);

	if (rc)
		pr_info("gpiomux snapshot unavailable: %d\n", rc);
}

static int c5_gpio_debug_show(struct seq_file *m, void *unused)
{
	return c5_gpio_debug_print(m);
}

static int c5_gpio_debug_open(struct inode *inode, struct file *file)
{
	return single_open(file, c5_gpio_debug_show, inode->i_private);
}

static const struct file_operations c5_gpio_debug_operations = {
	.open = c5_gpio_debug_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init c5_gpio_debug_init(void)
{
	struct dentry *entry;

	entry = debugfs_create_file("gpiomux", 0444, NULL, NULL, &c5_gpio_debug_operations);
	return IS_ERR_OR_NULL(entry) ? (entry ? PTR_ERR(entry) : -ENOMEM) : 0;
}
late_initcall(c5_gpio_debug_init);

static int c5_gpio_debug_state_get(char *buf, const struct kernel_param *kp)
{
	unsigned long flags;
	unsigned int samples;
	bool ready;

	raw_spin_lock_irqsave(&c5_gpio_debug_lock, flags);
	ready = c5_gpio_debug_ready;
	samples = c5_gpio_debug_samples;
	raw_spin_unlock_irqrestore(&c5_gpio_debug_lock, flags);
	return scnprintf(buf, PAGE_SIZE, "C5_GPIO_DEBUG1 ready=%u samples=%u\n", ready, samples);
}

static const struct kernel_param_ops c5_gpio_debug_state_ops = {
	.get = c5_gpio_debug_state_get,
};
module_param_cb(c5_gpio_debug_state, &c5_gpio_debug_state_ops, NULL, 0400);
