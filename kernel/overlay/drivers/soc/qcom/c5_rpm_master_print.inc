/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/device.h>
#include <linux/spinlock.h>

struct c5_rpm_print_snapshot {
	u32 masters;
	char names[32][32];
	struct c5_rpm_master_v2 records[32];
};

static DEFINE_RAW_SPINLOCK(c5_rpm_print_lock);
static struct c5_rpm_state *c5_rpm_print_owner;
static bool c5_rpm_print_ready;
static int enable_master_info;
static u32 c5_rpm_print_calls;
static u32 c5_rpm_print_records;

int sec_print_masters_stats(void);

static void c5_rpm_print_emit(const char *name, const struct c5_rpm_master_v2 *r)
{
	unsigned int bit;

	pr_info("%s:-\n"
		"\tShutdown_req:0x%llX\n"
		"\tWakeup_ind:0x%llX\n"
		"\tBringup_req:0x%llX\n"
		"\tBringup_ack:0x%llX\n"
		"\tLast_sleep_transition_duration:0x%x\n"
		"\tLast_wake_transition_duration:0x%x\n"
		"\tCount:%d\n"
		"\tLast_entered_at:0x%llX\n"
		"\tLast_exited_at:0x%llX\n"
		"\tAccumulated_duration:0x%llX\n"
		"\tWakeup_reason:0x%x\n"
		"\tNumshutdowns:0x%x\n"
		"\tActive_cores:0x%x\n", name,
		(unsigned long long)r->shutdown_req, (unsigned long long)r->wakeup_ind,
		(unsigned long long)r->bringup_req, (unsigned long long)r->bringup_ack,
		r->last_sleep_transition_duration, r->last_wake_transition_duration,
		(int)r->count, (unsigned long long)r->last_entered_at,
		(unsigned long long)r->last_exited_at,
		(unsigned long long)r->accumulated_duration, r->wakeup_reason,
		r->numshutdowns, r->active_cores);
	/* The firmware bitmap is 32 bits; never inspect adjacent shutdown data. */
	for (bit = 0; bit < 32; bit++)
		if (r->active_cores & BIT(bit))
			pr_info(" Core%d Active", bit);
}

int sec_print_masters_stats(void)
{
	struct c5_rpm_print_snapshot *snapshot;
	struct c5_rpm_state *s;
	unsigned long flags;
	unsigned int i;
	bool enabled;

	raw_spin_lock_irqsave(&c5_rpm_print_lock, flags);
	enabled = c5_rpm_print_ready && enable_master_info == 1;
	raw_spin_unlock_irqrestore(&c5_rpm_print_lock, flags);
	if (!enabled)
		return 0;
	/* This API is called with IRQs disabled by the legacy suspend path. */
	snapshot = kzalloc(sizeof(*snapshot), GFP_ATOMIC);
	if (!snapshot)
		return -ENOMEM;
	raw_spin_lock_irqsave(&c5_rpm_print_lock, flags);
	s = c5_rpm_print_owner;
	if (!c5_rpm_print_ready || enable_master_info != 1 || !s) {
		raw_spin_unlock_irqrestore(&c5_rpm_print_lock, flags);
		kfree(snapshot);
		return 0;
	}
	snapshot->masters = s->masters;
	for (i = 0; i < s->masters; i++) {
		strlcpy(snapshot->names[i], s->names[i], sizeof(snapshot->names[i]));
		c5_rpm_read_master(s->base + (size_t)i * s->stride, &snapshot->records[i]);
	}
	raw_spin_unlock_irqrestore(&c5_rpm_print_lock, flags);
	/* No controller or name pointer survives publication protection. */
	pr_info("============ RPM MASTERS STATS ============");
	for (i = 0; i < snapshot->masters; i++) {
		c5_rpm_print_emit(snapshot->names[i], &snapshot->records[i]);
		pr_info(" ");
	}
	pr_info("===========================================");
	raw_spin_lock_irqsave(&c5_rpm_print_lock, flags);
	if (c5_rpm_print_calls != ~0U)
		c5_rpm_print_calls++;
	if (snapshot->masters > ~0U - c5_rpm_print_records)
		c5_rpm_print_records = ~0U;
	else
		c5_rpm_print_records += snapshot->masters;
	raw_spin_unlock_irqrestore(&c5_rpm_print_lock, flags);
	kfree(snapshot);
	return 0;
}

static int c5_rpm_print_state_get(char *buf, const struct kernel_param *kp)
{
	unsigned long flags;
	bool ready;
	int enabled;
	u32 calls, records;

	raw_spin_lock_irqsave(&c5_rpm_print_lock, flags);
	ready = c5_rpm_print_ready;
	enabled = enable_master_info;
	calls = c5_rpm_print_calls;
	records = c5_rpm_print_records;
	raw_spin_unlock_irqrestore(&c5_rpm_print_lock, flags);
	return scnprintf(buf, PAGE_SIZE,
		"C5_RPM_PRINT1 ready=%u enabled=%d calls=%u records=%u\n",
		ready, enabled, calls, records);
}

static const struct kernel_param_ops c5_rpm_print_state_ops = {
	.get = c5_rpm_print_state_get,
};
module_param_cb(c5_rpm_print_state, &c5_rpm_print_state_ops, NULL, 0400);

static ssize_t masters_info_enable_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	unsigned long flags;
	int enabled;

	raw_spin_lock_irqsave(&c5_rpm_print_lock, flags);
	enabled = enable_master_info;
	raw_spin_unlock_irqrestore(&c5_rpm_print_lock, flags);
	return scnprintf(buf, PAGE_SIZE, "%d\n", enabled);
}

static ssize_t masters_info_enable_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	unsigned long flags;
	const char *digit = skip_spaces(buf);
	int next, rc;

	/* Keep the unsigned first-digit prefix without parsing or overflowing a suffix. */
	if (*digit < '0' || *digit > '9')
		return -EINVAL;
	next = *digit - '0';
	raw_spin_lock_irqsave(&c5_rpm_print_lock, flags);
	if (!c5_rpm_print_ready || !c5_rpm_print_owner)
		rc = -ENODEV;
	else {
		enable_master_info = next;
		rc = 0;
	}
	raw_spin_unlock_irqrestore(&c5_rpm_print_lock, flags);
	return rc ? rc : size;
}

static ssize_t c5_print_probe_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int rc = sec_print_masters_stats();

	if (rc)
		return rc;
	return c5_rpm_print_state_get(buf, NULL);
}

static DEVICE_ATTR(masters_info_enable, 0664, masters_info_enable_show, masters_info_enable_store);
static DEVICE_ATTR(c5_print_probe, 0400, c5_print_probe_show, NULL);

static void c5_rpm_master_print_unpublish(struct c5_rpm_state *s)
{
	unsigned long flags;

	/* Drain all MMIO snapshots before the existing owner reference is dropped. */
	raw_spin_lock_irqsave(&c5_rpm_print_lock, flags);
	if (c5_rpm_print_owner == s) {
		c5_rpm_print_ready = false;
		c5_rpm_print_owner = NULL;
		enable_master_info = 0;
	}
	raw_spin_unlock_irqrestore(&c5_rpm_print_lock, flags);
	/* kernfs removal waits outside the lock used by its callbacks. */
	if (s->print_probe_attr) {
		device_remove_file(s->print_device, &dev_attr_c5_print_probe);
		s->print_probe_attr = false;
	}
	if (s->print_enable_attr) {
		device_remove_file(s->print_device, &dev_attr_masters_info_enable);
		s->print_enable_attr = false;
	}
	if (s->print_device) {
		device_unregister(s->print_device);
		s->print_device = NULL;
	}
	if (s->print_class) {
		class_destroy(s->print_class);
		s->print_class = NULL;
	}
}

static int c5_rpm_master_print_publish(struct c5_rpm_state *s)
{
	unsigned long flags;
	unsigned int i;
	int rc;

	if (!s->base || !s->masters || s->masters > 32)
		return -EINVAL;
	for (i = 0; i < s->masters; i++)
		if (!s->names[i] || strnlen(s->names[i], 32) == 32)
			return -EINVAL;
	raw_spin_lock_irqsave(&c5_rpm_print_lock, flags);
	if (c5_rpm_print_owner) {
		raw_spin_unlock_irqrestore(&c5_rpm_print_lock, flags);
		return -EBUSY;
	}
	c5_rpm_print_owner = s;
	c5_rpm_print_ready = false;
	enable_master_info = 0;
	raw_spin_unlock_irqrestore(&c5_rpm_print_lock, flags);
	s->print_class = class_create(THIS_MODULE, "sec_rpmstats");
	if (IS_ERR(s->print_class)) {
		rc = PTR_ERR(s->print_class);
		s->print_class = NULL;
		goto fail;
	}
	s->print_device = device_create(s->print_class, NULL, 0, s, "sec_masters_stats");
	if (IS_ERR(s->print_device)) {
		rc = PTR_ERR(s->print_device);
		s->print_device = NULL;
		goto fail;
	}
	rc = device_create_file(s->print_device, &dev_attr_masters_info_enable);
	if (rc)
		goto fail;
	s->print_enable_attr = true;
	rc = device_create_file(s->print_device, &dev_attr_c5_print_probe);
	if (rc)
		goto fail;
	s->print_probe_attr = true;
	raw_spin_lock_irqsave(&c5_rpm_print_lock, flags);
	c5_rpm_print_ready = true;
	raw_spin_unlock_irqrestore(&c5_rpm_print_lock, flags);
	return 0;
fail:
	c5_rpm_master_print_unpublish(s);
	return rc;
}
