/* SPDX-License-Identifier: GPL-2.0-only */
/* Checked SM5705 operation-table backend. Included by sm5705_charger_oper.c. */
#include <linux/mutex.h>

static DEFINE_MUTEX(c5_sm5705_oper_lock);

/* Caller holds c5_sm5705_oper_lock. Preserve the existing table/write order. */
static int sm5705_charger_oper_change_state(unsigned char new_status)
{
	struct sm5705_charger_oper_table_info *next;
	int i, ret;

	for (i = 0; i < oper_info.max_table_num; i++)
		if (sm5705_charger_operation_mode_table[i].status == new_status)
			break;
	if (i == oper_info.max_table_num)
		return -EINVAL;
	next = &sm5705_charger_operation_mode_table[i];
	if (oper_info.suspend_mode) {
		oper_info.current_table.status = new_status;
		return 0;
	}

	if (next->BST_OUT != oper_info.current_table.BST_OUT) {
		ret = sm5705_charger_oper_set_BSTOUT(oper_info.i2c, next->BST_OUT);
		if (ret < 0)
			return ret;
		oper_info.current_table.BST_OUT = next->BST_OUT;
	}
	if (next->OTG_CURRENT != oper_info.current_table.OTG_CURRENT) {
		ret = sm5705_charger_oper_set_OTG_CURRENT(oper_info.i2c,
						       next->OTG_CURRENT);
		if (ret < 0)
			return ret;
		oper_info.current_table.OTG_CURRENT = next->OTG_CURRENT;
	}
	if (sm5705_call_fg_device_id() < 5 &&
	    oper_info.current_table.oper_mode == SM5705_CHARGER_OP_MODE_USB_OTG &&
	    next->oper_mode == SM5705_CHARGER_OP_MODE_CHG_ON) {
		ret = sm5705_charger_oper_set_mode(oper_info.i2c,
						 SM5705_CHARGER_OP_MODE_SUSPEND);
		if (ret < 0)
			return ret;
		oper_info.current_table.oper_mode = SM5705_CHARGER_OP_MODE_SUSPEND;
		msleep(100);
	}
	if (next->oper_mode != oper_info.current_table.oper_mode) {
		ret = sm5705_charger_oper_set_mode(oper_info.i2c, next->oper_mode);
		if (ret < 0)
			return ret;
		oper_info.current_table.oper_mode = next->oper_mode;
	}
	oper_info.current_table.status = new_status;
	return 0;
}

static int c5_sm5705_oper_push_locked(int event_type, bool enable)
{
	unsigned char new_status;
	int ret;

	if (!oper_info.i2c)
		return -ENOENT;
	if (event_type == SM5705_CHARGER_OP_EVENT_SUSPEND_MODE) {
		if (enable) {
			if (oper_info.suspend_mode)
				return 0;
			ret = sm5705_charger_oper_set_mode(oper_info.i2c,
						 SM5705_CHARGER_OP_MODE_SUSPEND);
			if (ret < 0)
				return ret;
			oper_info.current_table.oper_mode = SM5705_CHARGER_OP_MODE_SUSPEND;
			oper_info.suspend_mode = true;
			return 0;
		}
		if (!oper_info.suspend_mode)
			return 0;
		oper_info.suspend_mode = false;
		ret = sm5705_charger_oper_change_state(oper_info.current_table.status);
		if (ret < 0)
			oper_info.suspend_mode = true;
		return ret;
	}
	if (event_type < 0 || event_type > SM5705_CHARGER_OP_EVENT_VBUS)
		return -EINVAL;
	new_status = _update_status(event_type, enable);
	return sm5705_charger_oper_change_state(new_status);
}

int sm5705_charger_oper_push_event(int event_type, bool enable)
{
	int ret;

	mutex_lock(&c5_sm5705_oper_lock);
	ret = c5_sm5705_oper_push_locked(event_type, enable);
	mutex_unlock(&c5_sm5705_oper_lock);
	return ret;
}
EXPORT_SYMBOL(sm5705_charger_oper_push_event);

int sm5705_charger_oper_table_init(struct i2c_client *i2c)
{
	struct sm5705_charger_oper_table_info initial = {
		.status = make_OP_STATUS(0, 0, 0, 0, 0, 0),
		.oper_mode = SM5705_CHARGER_OP_MODE_CHG_ON,
		.BST_OUT = BST_OUT_4500mV,
		.OTG_CURRENT = OTG_CURRENT_500mA,
	};
	int ret;

	if (!i2c)
		return -EINVAL;
	mutex_lock(&c5_sm5705_oper_lock);
	/* A failed initialization must never publish a usable controller. */
	oper_info.i2c = NULL;
	ret = sm5705_charger_oper_set_mode(i2c, initial.oper_mode);
	if (ret < 0)
		goto out;
	ret = sm5705_charger_oper_set_BSTOUT(i2c, initial.BST_OUT);
	if (ret < 0)
		goto out;
	ret = sm5705_charger_oper_set_OTG_CURRENT(i2c, initial.OTG_CURRENT);
	if (ret < 0)
		goto out;
	oper_info.current_table = initial;
	oper_info.max_table_num = ARRAY_SIZE(sm5705_charger_operation_mode_table);
	oper_info.suspend_mode = false;
	oper_info.i2c = i2c;
	ret = 0;
out:
	mutex_unlock(&c5_sm5705_oper_lock);
	return ret;
}
EXPORT_SYMBOL(sm5705_charger_oper_table_init);

int sm5705_charger_oper_get_current_status(void)
{
	int value;

	mutex_lock(&c5_sm5705_oper_lock);
	value = oper_info.current_table.status;
	mutex_unlock(&c5_sm5705_oper_lock);
	return value;
}
EXPORT_SYMBOL(sm5705_charger_oper_get_current_status);

int sm5705_charger_oper_get_current_op_mode(void)
{
	int value;

	mutex_lock(&c5_sm5705_oper_lock);
	value = oper_info.current_table.oper_mode;
	mutex_unlock(&c5_sm5705_oper_lock);
	return value;
}
EXPORT_SYMBOL(sm5705_charger_oper_get_current_op_mode);

/* A sourcing request must not succeed merely because it was deferred. */
int c5_sm5705_oper_set_otg(bool enable)
{
	int ret;

	mutex_lock(&c5_sm5705_oper_lock);
	if (!oper_info.i2c)
		ret = -ENOENT;
	else if (enable && (oper_info.suspend_mode ||
		 (!(oper_info.current_table.status & (1 << SM5705_CHARGER_OP_EVENT_OTG)) &&
		  (oper_info.current_table.status & (1 << SM5705_CHARGER_OP_EVENT_VBUS)))))
		ret = -EBUSY;
	else
		ret = c5_sm5705_oper_push_locked(SM5705_CHARGER_OP_EVENT_OTG, enable);
	mutex_unlock(&c5_sm5705_oper_lock);
	return ret;
}

int c5_sm5705_oper_otg_enabled(void)
{
	int value;

	mutex_lock(&c5_sm5705_oper_lock);
	if (!oper_info.i2c)
		value = -ENOENT;
	else
		value = !oper_info.suspend_mode &&
			!!(oper_info.current_table.status &
			   (1 << SM5705_CHARGER_OP_EVENT_OTG));
	mutex_unlock(&c5_sm5705_oper_lock);
	return value;
}
