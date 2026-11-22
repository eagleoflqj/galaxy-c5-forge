/* Copyright (c) 2012-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/tracepoint.h>
#include <linux/coresight.h>
#include <trace/events/exception.h>

static int event_abort_enable;
static int event_abort_early_panic = 1;
static unsigned int c5_event_fault_mask;
static unsigned int c5_event_panic_mask;
static unsigned int c5_event_init_attempts;
static int c5_event_init_result = -EAGAIN;
static int c5_event_last_error;
static int c5_event_cleanup_error;
static DEFINE_MUTEX(c5_event_lock);
static int event_abort_set(const char *val, const struct kernel_param *kp);
static int event_abort_on_panic_set(const char *val, const struct kernel_param *kp);
module_param_call(event_abort_enable, event_abort_set, param_get_int,
		  &event_abort_enable, 0644);
module_param_call(event_abort_early_panic, event_abort_on_panic_set, param_get_int,
		  &event_abort_early_panic, 0644);

static int c5_event_reject(const char *value, const struct kernel_param *kp)
{
	return -EPERM;
}
static const struct kernel_param_ops c5_event_uint_ops = {
	.set = c5_event_reject, .get = param_get_uint,
};
static const struct kernel_param_ops c5_event_int_ops = {
	.set = c5_event_reject, .get = param_get_int,
};
module_param_cb(diag_init_attempts, &c5_event_uint_ops, &c5_event_init_attempts, 0400);
module_param_cb(diag_init_result, &c5_event_int_ops, &c5_event_init_result, 0400);
module_param_cb(diag_fault_mask, &c5_event_uint_ops, &c5_event_fault_mask, 0400);
module_param_cb(diag_panic_mask, &c5_event_uint_ops, &c5_event_panic_mask, 0400);
module_param_cb(diag_last_error, &c5_event_int_ops, &c5_event_last_error, 0400);
module_param_cb(diag_cleanup_error, &c5_event_int_ops, &c5_event_cleanup_error, 0400);

static void event_abort_user_fault(void *ignore,
				   struct task_struct *task,
				   unsigned long addr,
				   unsigned int fsr)
{
	coresight_abort();
	pr_debug("coresight_event: task_name: %s, addr: %lu, fsr:%u",
		(char *)task->comm, addr, fsr);
}

static void event_abort_undef_instr(void *ignore,
				    struct pt_regs *regs,
				    void *pc)
{
	if (user_mode(regs)) {
		coresight_abort();
		pr_debug("coresight_event: pc: %p", pc);
	}
}

static void event_abort_unhandled_abort(void *ignore,
					struct pt_regs *regs,
					unsigned long addr,
					unsigned int fsr)
{
	if (user_mode(regs)) {
		coresight_abort();
		pr_debug("coresight_event: addr: %lu, fsr:%u", addr, fsr);
	}
}

static void event_abort_kernel_panic(void *ignore, long state)
{
	coresight_abort();
}

/* Each membership bit denotes a successful registration owned by this file. */
static int c5_event_fault_one(unsigned int bit, bool enable)
{
	switch (bit) {
	case 0:
		return enable ? register_trace_user_fault(event_abort_user_fault, NULL) :
			unregister_trace_user_fault(event_abort_user_fault, NULL);
	case 1:
		return enable ? register_trace_undef_instr(event_abort_undef_instr, NULL) :
			unregister_trace_undef_instr(event_abort_undef_instr, NULL);
	case 2:
		return enable ? register_trace_unhandled_abort(event_abort_unhandled_abort, NULL) :
			unregister_trace_unhandled_abort(event_abort_unhandled_abort, NULL);
	default:
		return -EINVAL;
	}
}

static int c5_event_fault_set(bool enable)
{
	unsigned int before = c5_event_fault_mask;
	int bit, ret, first = 0;
	if (enable) {
		for (bit = 0; bit < 3; bit++) {
			if (c5_event_fault_mask & BIT(bit))
				continue;
			ret = c5_event_fault_one(bit, true);
			if (ret)
				goto rollback;
			c5_event_fault_mask |= BIT(bit);
		}
		return 0;
	}
	for (bit = 2; bit >= 0; bit--) {
		if (!(c5_event_fault_mask & BIT(bit)))
			continue;
		ret = c5_event_fault_one(bit, false);
		if (!ret)
			c5_event_fault_mask &= ~BIT(bit);
		else if (!first)
			first = ret;
	}
	return first;
rollback:
	first = ret;
	for (bit--; bit >= 0; bit--) {
		if (!(c5_event_fault_mask & BIT(bit)) || (before & BIT(bit)))
			continue;
		ret = c5_event_fault_one(bit, false);
		if (!ret)
			c5_event_fault_mask &= ~BIT(bit);
		else if (!c5_event_cleanup_error)
			c5_event_cleanup_error = ret;
	}
	return first;
}

static int c5_event_panic_one(unsigned int bit, bool enable)
{
	if (!bit)
		return enable ? register_trace_kernel_panic(event_abort_kernel_panic, NULL) :
			unregister_trace_kernel_panic(event_abort_kernel_panic, NULL);
	if (bit == 1)
		return enable ? register_trace_kernel_panic_late(event_abort_kernel_panic, NULL) :
			unregister_trace_kernel_panic_late(event_abort_kernel_panic, NULL);
	return -EINVAL;
}

static int c5_event_panic_set(bool early)
{
	unsigned int want = early ? 0 : 1, other = early ? 1 : 0;
	bool added = false;
	int ret, cleanup;
	if (!(c5_event_panic_mask & BIT(want))) {
		ret = c5_event_panic_one(want, true);
		if (ret)
			return ret;
		c5_event_panic_mask |= BIT(want);
		added = true;
	}
	if (c5_event_panic_mask & BIT(other)) {
		ret = c5_event_panic_one(other, false);
		if (ret) {
			if (added) {
				cleanup = c5_event_panic_one(want, false);
				if (!cleanup)
					c5_event_panic_mask &= ~BIT(want);
				else
					c5_event_cleanup_error = cleanup;
			}
			return ret;
		}
		c5_event_panic_mask &= ~BIT(other);
	}
	return 0;
}

static int event_abort_set(const char *val, const struct kernel_param *kp)
{
	int ret, next;
	ret = kstrtoint(val, 0, &next);
	if (ret)
		return ret;
	mutex_lock(&c5_event_lock);
	c5_event_cleanup_error = 0;
	ret = c5_event_fault_set(next != 0);
	if (!ret)
		event_abort_enable = next;
	c5_event_last_error = ret;
	mutex_unlock(&c5_event_lock);
	return ret;
}

static int event_abort_on_panic_set(const char *val, const struct kernel_param *kp)
{
	int ret, next;
	ret = kstrtoint(val, 0, &next);
	if (ret)
		return ret;
	mutex_lock(&c5_event_lock);
	c5_event_cleanup_error = 0;
	ret = c5_event_panic_set(next != 0);
	if (!ret)
		event_abort_early_panic = next;
	c5_event_last_error = ret;
	mutex_unlock(&c5_event_lock);
	return ret;
}

static int __init event_init(void)
{
	int ret;
	mutex_lock(&c5_event_lock);
	c5_event_init_attempts++;
	c5_event_cleanup_error = 0;
	ret = c5_event_panic_set(event_abort_early_panic != 0);
	c5_event_init_result = ret;
	c5_event_last_error = ret;
	mutex_unlock(&c5_event_lock);
	if (ret)
		pr_err("coresight_event: panic registration failed: %d\n", ret);
	/* Preserve original nonfatal init; diagnostics distinguish failed setup. */
	return 0;
}
module_init(event_init);

static void __exit event_exit(void)
{
	int bit, ret;
	mutex_lock(&c5_event_lock);
	c5_event_cleanup_error = 0;
	c5_event_last_error = c5_event_fault_set(false);
	for (bit = 1; bit >= 0; bit--) {
		if (!(c5_event_panic_mask & BIT(bit)))
			continue;
		ret = c5_event_panic_one(bit, false);
		if (!ret)
			c5_event_panic_mask &= ~BIT(bit);
		else if (!c5_event_last_error)
			c5_event_last_error = ret;
	}
	mutex_unlock(&c5_event_lock);
}
module_exit(event_exit);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("C5 generic CoreSight abort events with owned membership");
