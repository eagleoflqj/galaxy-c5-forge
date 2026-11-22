/* Copyright (c) 2012-2017, The Linux Foundation. All rights reserved.
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

#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/wait.h>
#include <linux/of_platform.h>
#include <linux/regulator/consumer.h>
#include <linux/regmap.h>
#include <linux/of.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/hwmon.h>
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/spmi.h>
#include <linux/platform_device.h>
#include <linux/of_irq.h>
#include <linux/interrupt.h>
#include <linux/completion.h>
#include <linux/hwmon-sysfs.h>
#include <linux/qpnp/qpnp-adc.h>
#include <linux/platform_device.h>

/* QPNP IADC register definition */
#define QPNP_IADC_REVISION1				0x0
#define QPNP_IADC_REVISION2				0x1
#define QPNP_IADC_REVISION3				0x2
#define QPNP_IADC_REVISION4				0x3
#define QPNP_IADC_PERPH_TYPE				0x4
#define QPNP_IADC_PERH_SUBTYPE				0x5

#define QPNP_IADC_SUPPORTED_REVISION2			1

#define QPNP_STATUS1					0x8
#define QPNP_STATUS1_OP_MODE				4
#define QPNP_STATUS1_MULTI_MEAS_EN			BIT(3)
#define QPNP_STATUS1_MEAS_INTERVAL_EN_STS		BIT(2)
#define QPNP_STATUS1_REQ_STS				BIT(1)
#define QPNP_STATUS1_EOC				BIT(0)
#define QPNP_STATUS1_REQ_STS_EOC_MASK			0x3
#define QPNP_STATUS2					0x9
#define QPNP_STATUS2_CONV_SEQ_STATE_SHIFT		4
#define QPNP_STATUS2_FIFO_NOT_EMPTY_FLAG		BIT(1)
#define QPNP_STATUS2_CONV_SEQ_TIMEOUT_STS		BIT(0)
#define QPNP_CONV_TIMEOUT_ERR				2

#define QPNP_IADC_MODE_CTL				0x40
#define QPNP_OP_MODE_SHIFT				4
#define QPNP_USE_BMS_DATA				BIT(4)
#define QPNP_VADC_SYNCH_EN				BIT(2)
#define QPNP_OFFSET_RMV_EN				BIT(1)
#define QPNP_ADC_TRIM_EN				BIT(0)
#define QPNP_IADC_EN_CTL1				0x46
#define QPNP_IADC_ADC_EN				BIT(7)
#define QPNP_ADC_CH_SEL_CTL				0x48
#define QPNP_ADC_DIG_PARAM				0x50
#define QPNP_ADC_CLK_SEL_MASK				0x3
#define QPNP_ADC_DEC_RATIO_SEL_MASK			0xc
#define QPNP_ADC_DIG_DEC_RATIO_SEL_SHIFT		2

#define QPNP_CONV_REQ					0x52
#define QPNP_CONV_REQ_SET				BIT(7)
#define QPNP_CONV_SEQ_CTL				0x54
#define QPNP_CONV_SEQ_HOLDOFF_SHIFT			4
#define QPNP_CONV_SEQ_TRIG_CTL				0x55
#define QPNP_FAST_AVG_CTL				0x5a

#define QPNP_M0_LOW_THR_LSB				0x5c
#define QPNP_M0_LOW_THR_MSB				0x5d
#define QPNP_M0_HIGH_THR_LSB				0x5e
#define QPNP_M0_HIGH_THR_MSB				0x5f
#define QPNP_M1_LOW_THR_LSB				0x69
#define QPNP_M1_LOW_THR_MSB				0x6a
#define QPNP_M1_HIGH_THR_LSB				0x6b
#define QPNP_M1_HIGH_THR_MSB				0x6c

#define QPNP_DATA0					0x60
#define QPNP_DATA1					0x61
#define QPNP_CONV_TIMEOUT_ERR				2

#define QPNP_IADC_SEC_ACCESS				0xD0
#define QPNP_IADC_SEC_ACCESS_DATA			0xA5
#define QPNP_IADC_MSB_OFFSET				0xF2
#define QPNP_IADC_LSB_OFFSET				0xF3
#define QPNP_IADC_NOMINAL_RSENSE			0xF4
#define QPNP_IADC_ATE_GAIN_CALIB_OFFSET			0xF5
#define QPNP_INT_TEST_VAL				0xE1

#define QPNP_IADC_ADC_CH_SEL_CTL			0x48
#define QPNP_IADC_ADC_CHX_SEL_SHIFT			3

#define QPNP_IADC_ADC_DIG_PARAM				0x50
#define QPNP_IADC_CLK_SEL_SHIFT				1
#define QPNP_IADC_DEC_RATIO_SEL				3

#define QPNP_IADC_CONV_REQUEST				0x52
#define QPNP_IADC_CONV_REQ				BIT(7)

#define QPNP_IADC_DATA0					0x60
#define QPNP_IADC_DATA1					0x61

#define QPNP_ADC_CONV_TIME_MIN				2000
#define QPNP_ADC_CONV_TIME_MAX				2100
#define QPNP_ADC_ERR_COUNT				20

#define QPNP_ADC_GAIN_NV				17857
#define QPNP_OFFSET_CALIBRATION_SHORT_CADC_LEADS_IDEAL	0
#define QPNP_IADC_INTERNAL_RSENSE_N_OHMS_FACTOR		10000000
#define QPNP_IADC_NANO_VOLTS_FACTOR			1000000
#define QPNP_IADC_CALIB_SECONDS				300000
#define QPNP_IADC_RSENSE_LSB_N_OHMS_PER_BIT		15625
#define QPNP_IADC_DIE_TEMP_CALIB_OFFSET			5000

#define QPNP_RAW_CODE_16_BIT_MSB_MASK			0xff00
#define QPNP_RAW_CODE_16_BIT_LSB_MASK			0xff
#define QPNP_BIT_SHIFT_8				8
#define QPNP_RSENSE_MSB_SIGN_CHECK			0x80
#define QPNP_ADC_COMPLETION_TIMEOUT			HZ
#define SMBB_BAT_IF_TRIM_CNST_RDS_MASK			0x7
#define SMBB_BAT_IF_TRIM_CNST_RDS_MASK_CONST_0		0
#define SMBB_BAT_IF_TRIM_CNST_RDS_MASK_CONST_2		2
#define QPNP_IADC1_USR_TRIM2_ADC_FULLSCALE1_CONST	127
#define QPNP_IADC_RSENSE_DEFAULT_VALUE			7800000
#define QPNP_IADC_RSENSE_DEFAULT_TYPEB_GF		9000000
#define QPNP_IADC_RSENSE_DEFAULT_TYPEB_SMIC		9700000

struct qpnp_iadc_comp {
	bool	ext_rsense;
	u8	id;
	u8	sys_gain;
	u8	revision_dig_major;
	u8	revision_ana_minor;
};

struct qpnp_iadc_chip {
	struct device				*dev;
	struct qpnp_adc_drv			*adc;
	int32_t					rsense;
	bool					external_rsense;
	bool					default_internal_rsense;
	struct device				*iadc_hwmon;
	struct list_head			list;
	int64_t					die_temp;
	struct delayed_work			iadc_work;
	bool					iadc_mode_sel;
	struct qpnp_iadc_comp			iadc_comp;
	struct qpnp_vadc_chip			*vadc_dev;
	struct work_struct			trigger_completion_work;
	bool					skip_auto_calibrations;
	bool					iadc_poll_eoc;
	/* Legacy handles are borrowed. Caller binding lifetime is serialized. */
	struct mutex op_lock;
	wait_queue_head_t api_wait;
	unsigned int active_ops;
	bool ready;
	bool stopping;
	bool calibrated;
	bool conversion_on;
	bool irq_requested;
	bool irq_wake_enabled;
	bool wakeup_owned;
	struct device *vadc_supplier;
	bool vadc_locked;
	unsigned int attrs_created;
	int restore_error;
	u16					batt_id_trim_cnst_rds;
	int					rds_trim_default_type;
	int					max_channels_available;
	bool					rds_trim_default_check;
	int32_t					rsense_workaround_value;
	struct sensor_device_attribute		sens_attr[0];
};

LIST_HEAD(qpnp_iadc_device_list);

enum qpnp_iadc_rsense_rds_workaround {
	QPNP_IADC_RDS_DEFAULT_TYPEA,
	QPNP_IADC_RDS_DEFAULT_TYPEB,
	QPNP_IADC_RDS_DEFAULT_TYPEC,
};

static int32_t qpnp_iadc_read_reg(struct qpnp_iadc_chip *iadc,
						uint32_t reg, u8 *data)
{
	int rc;
	uint val;

	if (reg > 0xff || (u32)iadc->adc->offset + reg > 0xffff)
		return -EINVAL;

	rc = regmap_read(iadc->adc->regmap, (iadc->adc->offset + reg), &val);
	if (rc < 0) {
		pr_err("qpnp iadc read reg %d failed with %d\n", reg, rc);
		return rc;
	}
	*data = (u8)val;

	return 0;
}

static int32_t qpnp_iadc_write_reg(struct qpnp_iadc_chip *iadc,
						uint32_t reg, u8 data)
{
	int rc;
	u8 *buf;

	if (reg > 0xff || (u32)iadc->adc->offset + reg > 0xffff)
		return -EINVAL;

	buf = &data;
	rc = regmap_write(iadc->adc->regmap, (iadc->adc->offset + reg), *buf);
	if (rc < 0) {
		pr_err("qpnp iadc write reg %d failed with %d\n", reg, rc);
		return rc;
	}

	return 0;
}

static DEFINE_MUTEX(c5_iadc_registry_lock);

/* No reference is returned by qpnp_get_iadc: the legacy API has no put.
 * Consumers must serialize their borrowed handle's lifetime with IADC
 * unbind. Per-call admission drains entered/queued operations only and does
 * not make a stale pointer valid. VADC private-state use is separately
 * serialized with its native driver-core device mutex and fresh lookup;
 * holding the supplier device reference alone would not pin devm state.
 */
static void c5_iadc_api_end(struct qpnp_iadc_chip *iadc)
{
	struct device *dev = iadc->dev;

	if (iadc->vadc_locked) {
		iadc->vadc_locked = false;
		device_unlock(iadc->vadc_supplier);
	}
	mutex_unlock(&iadc->op_lock);
	mutex_lock(&c5_iadc_registry_lock);
	WARN_ON(!iadc->active_ops);
	iadc->active_ops--;
	if (!iadc->active_ops)
		wake_up_all(&iadc->api_wait);
	mutex_unlock(&c5_iadc_registry_lock);
	/* No access to iadc after making the last operation visible as drained. */
	put_device(dev);
}

static int c5_iadc_api_begin(struct qpnp_iadc_chip *want, bool needs_vadc)
{
	struct qpnp_iadc_chip *iadc;
	bool found = false;
	struct qpnp_vadc_chip *fresh;

	mutex_lock(&c5_iadc_registry_lock);
	list_for_each_entry(iadc, &qpnp_iadc_device_list, list) {
		if (iadc == want && iadc->ready && !iadc->stopping) {
			/* Count before waiting on op_lock: remove also drains waiters. */
			iadc->active_ops++;
			get_device(iadc->dev);
			found = true;
			break;
		}
	}
	mutex_unlock(&c5_iadc_registry_lock);
	if (!found)
		return -EPROBE_DEFER;
	mutex_lock(&want->op_lock);
	if (READ_ONCE(want->stopping)) {
		c5_iadc_api_end(want);
		return -EPROBE_DEFER;
	}
	if (needs_vadc) {
		/* Reference pins this device object only. The real dev->mutex below
		 * excludes native remove AND devres_release_all for the entire use
		 * of the freshly looked-up VADC private pointer.
		 */
		device_lock(want->vadc_supplier);
		want->vadc_locked = true;
		if (READ_ONCE(want->stopping) || !want->vadc_supplier->driver ||
		    !dev_get_drvdata(want->vadc_supplier)) {
			c5_iadc_api_end(want);
			return -EPROBE_DEFER;
		}
		fresh = qpnp_get_vadc(want->dev, "iadc");
		if (IS_ERR(fresh) || fresh != dev_get_drvdata(want->vadc_supplier)) {
			c5_iadc_api_end(want);
			return -EPROBE_DEFER;
		}
		want->vadc_dev = fresh;
	}
	want->restore_error = 0;
	return 0;
}

static int c5_iadc_channel_index(struct qpnp_iadc_chip *iadc,
				enum qpnp_iadc_channels channel)
{
	int i;

	if ((int)channel < 0 || channel >= IADC_MUX_NUM)
		return -EINVAL;
	for (i = 0; i < iadc->max_channels_available; i++)
		if (iadc->adc->adc_channels[i].channel_num == channel)
			return i;
	return -EINVAL;
}

#define C5_IADC_S64_MAX ((s64)0x7fffffffffffffffLL)
#define C5_IADC_S64_MIN (-C5_IADC_S64_MAX - 1)
static int c5_iadc_mul(s64 a, s64 b, s64 *out)
{
	if (!a || !b) {
		*out = 0;
		return 0;
	}
	if ((a == -1 && b == C5_IADC_S64_MIN) ||
	    (b == -1 && a == C5_IADC_S64_MIN))
		return -ERANGE;
	if (a > 0 ? (b > 0 ? a > C5_IADC_S64_MAX / b :
			b < C5_IADC_S64_MIN / a) :
		    (b > 0 ? a < C5_IADC_S64_MIN / b :
			 a < C5_IADC_S64_MAX / b))
		return -ERANGE;
	*out = a * b;
	return 0;
}

static int c5_iadc_add(s64 a, s64 b, s64 *out)
{
	if ((b > 0 && a > C5_IADC_S64_MAX - b) ||
	    (b < 0 && a < C5_IADC_S64_MIN - b))
		return -ERANGE;
	*out = a + b;
	return 0;
}


static int c5_iadc_sub(s64 a, s64 b, s64 *out)
{
	if ((b > 0 && a < C5_IADC_S64_MIN + b) ||
	    (b < 0 && a > C5_IADC_S64_MAX + b))
		return -ERANGE;
	*out = a - b;
	return 0;
}

static int c5_iadc_div(s64 n, s64 d, s64 *out)
{
	if (!d)
		return -EINVAL;
	if (n == C5_IADC_S64_MIN && d == -1)
		return -ERANGE;
	*out = div64_s64(n, d);
	return 0;
}

static void c5_iadc_queue_auto(struct qpnp_iadc_chip *iadc)
{
	mutex_lock(&c5_iadc_registry_lock);
	if (iadc->ready && !iadc->stopping)
		schedule_delayed_work(&iadc->iadc_work,
			round_jiffies_relative(msecs_to_jiffies(QPNP_IADC_CALIB_SECONDS)));
	mutex_unlock(&c5_iadc_registry_lock);
}

static void qpnp_iadc_trigger_completion(struct work_struct *work)
{
	struct qpnp_iadc_chip *iadc = container_of(work,
			struct qpnp_iadc_chip, trigger_completion_work);

	/* Keep completion functional until admitted conversions have drained.
	 * IRQ synchronization then cancel_work_sync precede state release.
	 */
	complete(&iadc->adc->adc_rslt_completion);
}

static irqreturn_t qpnp_iadc_isr(int irq, void *dev_id)
{
	struct qpnp_iadc_chip *iadc = dev_id;

	schedule_work(&iadc->trigger_completion_work);

	return IRQ_HANDLED;
}

static int32_t qpnp_iadc_enable(struct qpnp_iadc_chip *iadc, bool state)
{
	int rc;

	/* A failed enable write has unknown hardware effect; still try safeoff. */
	if (state)
		iadc->conversion_on = true;
	rc = qpnp_iadc_write_reg(iadc, QPNP_IADC_EN_CTL1,
				state ? QPNP_IADC_ADC_EN : 0);
	if (!rc && !state)
		iadc->conversion_on = false;
	if (rc && !state && !iadc->restore_error)
		iadc->restore_error = rc;
	return rc;
}

static int32_t qpnp_iadc_status_debug(struct qpnp_iadc_chip *dev)
{
	int rc = 0;
	u8 mode = 0, status1 = 0, chan = 0, dig = 0, en = 0;

	rc = qpnp_iadc_read_reg(dev, QPNP_IADC_MODE_CTL, &mode);
	if (rc < 0) {
		pr_err("mode ctl register read failed with %d\n", rc);
		return rc;
	}

	rc = qpnp_iadc_read_reg(dev, QPNP_ADC_DIG_PARAM, &dig);
	if (rc < 0) {
		pr_err("digital param read failed with %d\n", rc);
		return rc;
	}

	rc = qpnp_iadc_read_reg(dev, QPNP_IADC_ADC_CH_SEL_CTL, &chan);
	if (rc < 0) {
		pr_err("channel read failed with %d\n", rc);
		return rc;
	}

	rc = qpnp_iadc_read_reg(dev, QPNP_STATUS1, &status1);
	if (rc < 0) {
		pr_err("status1 read failed with %d\n", rc);
		return rc;
	}

	rc = qpnp_iadc_read_reg(dev, QPNP_IADC_EN_CTL1, &en);
	if (rc < 0) {
		pr_err("en read failed with %d\n", rc);
		return rc;
	}

	pr_debug("EOC not set with status:%x, dig:%x, ch:%x, mode:%x, en:%x\n",
			status1, dig, chan, mode, en);

	rc = qpnp_iadc_enable(dev, false);
	if (rc < 0) {
		pr_err("IADC disable failed with %d\n", rc);
		return rc;
	}

	return 0;
}

static int32_t qpnp_iadc_read_conversion_result(struct qpnp_iadc_chip *iadc,
								uint16_t *data)
{
	uint8_t rslt_lsb, rslt_msb;
	uint16_t rslt;
	int32_t rc;

	rc = qpnp_iadc_read_reg(iadc, QPNP_IADC_DATA0, &rslt_lsb);
	if (rc < 0) {
		pr_err("qpnp adc result read failed with %d\n", rc);
		return rc;
	}

	rc = qpnp_iadc_read_reg(iadc, QPNP_IADC_DATA1, &rslt_msb);
	if (rc < 0) {
		pr_err("qpnp adc result read failed with %d\n", rc);
		return rc;
	}

	rslt = (rslt_msb << 8) | rslt_lsb;
	*data = rslt;

	return 0;
}

#define QPNP_IADC_PM8026_2_REV2	4
#define QPNP_IADC_PM8026_2_REV3	2

#define QPNP_COEFF_1					969000
#define QPNP_COEFF_2					32
#define QPNP_COEFF_3_TYPEA				1700000
#define QPNP_COEFF_3_TYPEB				1000000
#define QPNP_COEFF_4					100
#define QPNP_COEFF_5					15
#define QPNP_COEFF_6					100000
#define QPNP_COEFF_7					21
#define QPNP_COEFF_8					100000000
#define QPNP_COEFF_9					38
#define QPNP_COEFF_10					40
#define QPNP_COEFF_11					7
#define QPNP_COEFF_12					11
#define QPNP_COEFF_13					37
#define QPNP_COEFF_14					39
#define QPNP_COEFF_15					9
#define QPNP_COEFF_16					11
#define QPNP_COEFF_17					851200
#define QPNP_COEFF_18					296500
#define QPNP_COEFF_19					222400
#define QPNP_COEFF_20					813800
#define QPNP_COEFF_21					1059100
#define QPNP_COEFF_22					5000000
#define QPNP_COEFF_23					3722500
#define QPNP_COEFF_24					84
#define QPNP_COEFF_25					33
#define QPNP_COEFF_26					22
#define QPNP_COEFF_27					53
#define QPNP_COEFF_28					48

static int32_t qpnp_iadc_comp(int64_t *result, struct qpnp_iadc_chip *iadc,
							int64_t die_temp)
{
	int64_t temp_var = 0, sys_gain_coeff = 0, old;
	int32_t coeff_a = 0, coeff_b = 0;
	int rc;
	s64 intermediate;
	int version = 0;

	version = qpnp_adc_get_revid_version(iadc->dev);
	if (version == -EINVAL)
		return 0;

	old = *result;
	rc = c5_iadc_mul(*result, 1000000, result);
	if (rc)
		return rc;

	if (iadc->iadc_comp.sys_gain > 127)
		sys_gain_coeff = -QPNP_COEFF_6 *
				(iadc->iadc_comp.sys_gain - 128);
	else
		sys_gain_coeff = QPNP_COEFF_6 *
				iadc->iadc_comp.sys_gain;

	switch (version) {
	case QPNP_REV_ID_8941_3_1:
		switch (iadc->iadc_comp.id) {
		case COMP_ID_GF:
			if (!iadc->iadc_comp.ext_rsense) {
				/* internal rsense */
				coeff_a = QPNP_COEFF_2;
				coeff_b = -QPNP_COEFF_3_TYPEA;
			} else {
				if (*result < 0) {
					/* charge */
					coeff_a = QPNP_COEFF_5;
					coeff_b = QPNP_COEFF_6;
				} else {
					/* discharge */
					coeff_a = -QPNP_COEFF_7;
					coeff_b = QPNP_COEFF_6;
				}
			}
			break;
		case COMP_ID_TSMC:
		default:
			if (!iadc->iadc_comp.ext_rsense) {
				/* internal rsense */
				coeff_a = QPNP_COEFF_2;
				coeff_b = -QPNP_COEFF_3_TYPEB;
			} else {
				if (*result < 0) {
					/* charge */
					coeff_a = QPNP_COEFF_5;
					coeff_b = QPNP_COEFF_6;
				} else {
					/* discharge */
					coeff_a = -QPNP_COEFF_7;
					coeff_b = QPNP_COEFF_6;
				}
			}
			break;
		}
		break;
	case QPNP_REV_ID_8026_2_1:
	case QPNP_REV_ID_8026_2_2:
		/* pm8026 rev 2.1 and 2.2 */
		switch (iadc->iadc_comp.id) {
		case COMP_ID_GF:
			if (!iadc->iadc_comp.ext_rsense) {
				/* internal rsense */
				if (*result < 0) {
					/* charge */
					coeff_a = 0;
					coeff_b = 0;
				} else {
					coeff_a = QPNP_COEFF_25;
					coeff_b = 0;
				}
			} else {
				if (*result < 0) {
					/* charge */
					coeff_a = 0;
					coeff_b = 0;
				} else {
					/* discharge */
					coeff_a = 0;
					coeff_b = 0;
				}
			}
			break;
		case COMP_ID_TSMC:
		default:
			if (!iadc->iadc_comp.ext_rsense) {
				/* internal rsense */
				if (*result < 0) {
					/* charge */
					coeff_a = 0;
					coeff_b = 0;
				} else {
					coeff_a = QPNP_COEFF_26;
					coeff_b = 0;
				}
			} else {
				if (*result < 0) {
					/* charge */
					coeff_a = 0;
					coeff_b = 0;
				} else {
					/* discharge */
					coeff_a = 0;
					coeff_b = 0;
				}
			}
			break;
		}
		break;
	case QPNP_REV_ID_8026_1_0:
		/* pm8026 rev 1.0 */
		switch (iadc->iadc_comp.id) {
		case COMP_ID_GF:
			if (!iadc->iadc_comp.ext_rsense) {
				/* internal rsense */
				if (*result < 0) {
					/* charge */
					coeff_a = QPNP_COEFF_9;
					coeff_b = -QPNP_COEFF_17;
				} else {
					coeff_a = QPNP_COEFF_10;
					coeff_b = QPNP_COEFF_18;
				}
			} else {
				if (*result < 0) {
					/* charge */
					coeff_a = -QPNP_COEFF_11;
					coeff_b = 0;
				} else {
					/* discharge */
					coeff_a = -QPNP_COEFF_17;
					coeff_b = -QPNP_COEFF_19;
				}
			}
			break;
		case COMP_ID_TSMC:
		default:
			if (!iadc->iadc_comp.ext_rsense) {
				/* internal rsense */
				if (*result < 0) {
					/* charge */
					coeff_a = QPNP_COEFF_13;
					coeff_b = -QPNP_COEFF_20;
				} else {
					coeff_a = QPNP_COEFF_14;
					coeff_b = QPNP_COEFF_21;
				}
			} else {
				if (*result < 0) {
					/* charge */
					coeff_a = -QPNP_COEFF_15;
					coeff_b = 0;
				} else {
					/* discharge */
					coeff_a = -QPNP_COEFF_12;
					coeff_b = -QPNP_COEFF_19;
				}
			}
			break;
		}
		break;
	case QPNP_REV_ID_8110_1_0:
		/* pm8110 rev 1.0 */
		switch (iadc->iadc_comp.id) {
		case COMP_ID_GF:
			if (!iadc->iadc_comp.ext_rsense) {
				/* internal rsense */
				if (*result < 0) {
					/* charge */
					coeff_a = QPNP_COEFF_24;
					coeff_b = -QPNP_COEFF_22;
				} else {
					coeff_a = QPNP_COEFF_24;
					coeff_b = -QPNP_COEFF_23;
				}
			}
			break;
		case COMP_ID_SMIC:
		default:
			if (!iadc->iadc_comp.ext_rsense) {
				/* internal rsense */
				if (*result < 0) {
					/* charge */
					coeff_a = QPNP_COEFF_24;
					coeff_b = -QPNP_COEFF_22;
				} else {
					coeff_a = QPNP_COEFF_24;
					coeff_b = -QPNP_COEFF_23;
				}
			}
			break;
		}
		break;
	case QPNP_REV_ID_8110_2_0:
		if (c5_iadc_add(die_temp, -25000, &die_temp))
			return -ERANGE;
		/* pm8110 rev 2.0 */
		switch (iadc->iadc_comp.id) {
		case COMP_ID_GF:
			if (!iadc->iadc_comp.ext_rsense) {
				/* internal rsense */
				if (*result < 0) {
					/* charge */
					coeff_a = 0;
					coeff_b = 0;
				} else {
					coeff_a = QPNP_COEFF_27;
					coeff_b = 0;
				}
			}
			break;
		case COMP_ID_SMIC:
		default:
			if (!iadc->iadc_comp.ext_rsense) {
				/* internal rsense */
				if (*result < 0) {
					/* charge */
					coeff_a = 0;
					coeff_b = 0;
				} else {
					coeff_a = QPNP_COEFF_28;
					coeff_b = 0;
				}
			}
			break;
		}
		break;
	default:
	case QPNP_REV_ID_8026_2_0:
		/* pm8026 rev 1.0 */
		coeff_a = 0;
		coeff_b = 0;
		break;
	}

	if (c5_iadc_mul(coeff_a, die_temp, &intermediate) ||
	    c5_iadc_add(intermediate, coeff_b, &temp_var))
		return -ERANGE;
	temp_var = div64_s64(temp_var, QPNP_COEFF_4);
	if (temp_var == C5_IADC_S64_MIN ||
	    c5_iadc_add(1000000, -temp_var, &intermediate) ||
	    c5_iadc_mul(1000, intermediate, &temp_var))
		return -ERANGE;

	if (!iadc->iadc_comp.ext_rsense) {
		/* internal rsense */
		if (c5_iadc_mul(*result, 1000, &intermediate))
			return -ERANGE;
		if (c5_iadc_div(intermediate, temp_var, result))
			return temp_var ? -ERANGE : -EINVAL;
	}

	if (iadc->iadc_comp.ext_rsense) {
		/* external rsense */
		sys_gain_coeff = (1000000 +
			div64_s64(sys_gain_coeff, QPNP_COEFF_4));
		if (c5_iadc_mul(temp_var, sys_gain_coeff, &intermediate))
			return -ERANGE;
		temp_var = div64_s64(intermediate, 1000000);
		if (c5_iadc_mul(*result, 1000, &intermediate))
			return -ERANGE;
		if (c5_iadc_div(intermediate, temp_var, result))
			return temp_var ? -ERANGE : -EINVAL;
	}
	pr_debug("%lld compensated into %lld, a: %d, b: %d, sys_gain: %lld\n",
			old, *result, coeff_a, coeff_b, sys_gain_coeff);

	return 0;
}

static int32_t c5_qpnp_iadc_comp_result_locked(struct qpnp_iadc_chip *iadc, int64_t *result)
{
	return qpnp_iadc_comp(result, iadc, iadc->die_temp);
}

static int qpnp_iadc_rds_trim_update_check(struct qpnp_iadc_chip *iadc)
{
	int rc = 0;
	u8 trim2_val = 0;
	uint smbb_batt_trm_data = 0;
	u8 smbb_batt_trm_cnst_rds = 0;

	if (!iadc->rds_trim_default_check) {
		pr_debug("No internal rds trim check needed\n");
		return 0;
	}

	rc = qpnp_iadc_read_reg(iadc, QPNP_IADC_NOMINAL_RSENSE, &trim2_val);
	if (rc < 0) {
		pr_err("qpnp adc trim2_fullscale1 reg read failed %d\n", rc);
		return rc;
	}

	rc = regmap_read(iadc->adc->regmap, iadc->batt_id_trim_cnst_rds,
			 &smbb_batt_trm_data);
	if (rc < 0) {
		pr_err("batt_id trim_cnst rds reg read failed %d\n", rc);
		return rc;
	}

	smbb_batt_trm_cnst_rds = (u8)smbb_batt_trm_data &
				SMBB_BAT_IF_TRIM_CNST_RDS_MASK;

	pr_debug("n_trim:0x%x smb_trm:0x%02x\n", trim2_val, smbb_batt_trm_data);

	if (iadc->rds_trim_default_type == QPNP_IADC_RDS_DEFAULT_TYPEA) {

		if ((smbb_batt_trm_cnst_rds ==
				SMBB_BAT_IF_TRIM_CNST_RDS_MASK_CONST_2) &&
		(trim2_val == QPNP_IADC1_USR_TRIM2_ADC_FULLSCALE1_CONST)) {
			iadc->rsense_workaround_value =
					QPNP_IADC_RSENSE_DEFAULT_VALUE;
			iadc->default_internal_rsense = true;
		}
	} else if (iadc->rds_trim_default_type ==
						QPNP_IADC_RDS_DEFAULT_TYPEB) {
		if ((smbb_batt_trm_cnst_rds >=
				SMBB_BAT_IF_TRIM_CNST_RDS_MASK_CONST_2) &&
		(trim2_val == QPNP_IADC1_USR_TRIM2_ADC_FULLSCALE1_CONST)) {
			iadc->rsense_workaround_value =
					QPNP_IADC_RSENSE_DEFAULT_VALUE;
				iadc->default_internal_rsense = true;
		} else if ((smbb_batt_trm_cnst_rds <
				SMBB_BAT_IF_TRIM_CNST_RDS_MASK_CONST_2) &&
			(trim2_val ==
				QPNP_IADC1_USR_TRIM2_ADC_FULLSCALE1_CONST)) {
			if (iadc->iadc_comp.id == COMP_ID_GF) {
				iadc->rsense_workaround_value =
					QPNP_IADC_RSENSE_DEFAULT_TYPEB_GF;
				iadc->default_internal_rsense = true;
			} else if (iadc->iadc_comp.id == COMP_ID_SMIC) {
				iadc->rsense_workaround_value =
					QPNP_IADC_RSENSE_DEFAULT_TYPEB_SMIC;
				iadc->default_internal_rsense = true;
			}
		}
	} else if (iadc->rds_trim_default_type == QPNP_IADC_RDS_DEFAULT_TYPEC) {

		if ((smbb_batt_trm_cnst_rds >
				SMBB_BAT_IF_TRIM_CNST_RDS_MASK_CONST_0) &&
		(smbb_batt_trm_cnst_rds <=
				SMBB_BAT_IF_TRIM_CNST_RDS_MASK_CONST_2) &&
		(trim2_val == QPNP_IADC1_USR_TRIM2_ADC_FULLSCALE1_CONST)) {
			iadc->rsense_workaround_value =
					QPNP_IADC_RSENSE_DEFAULT_VALUE;
			iadc->default_internal_rsense = true;
		}
	}

	return 0;
}

static int32_t qpnp_iadc_comp_info(struct qpnp_iadc_chip *iadc)
{
	int rc = 0;

	rc = qpnp_iadc_read_reg(iadc, QPNP_INT_TEST_VAL, &iadc->iadc_comp.id);
	if (rc < 0) {
		pr_err("qpnp adc comp id failed with %d\n", rc);
		return rc;
	}

	rc = qpnp_iadc_read_reg(iadc, QPNP_IADC_REVISION2,
					&iadc->iadc_comp.revision_dig_major);
	if (rc < 0) {
		pr_err("qpnp adc revision2 read failed with %d\n", rc);
		return rc;
	}

	rc = qpnp_iadc_read_reg(iadc, QPNP_IADC_REVISION3,
					&iadc->iadc_comp.revision_ana_minor);
	if (rc < 0) {
		pr_err("qpnp adc revision3 read failed with %d\n", rc);
		return rc;
	}

	rc = qpnp_iadc_read_reg(iadc, QPNP_IADC_ATE_GAIN_CALIB_OFFSET,
						&iadc->iadc_comp.sys_gain);
	if (rc < 0) {
		pr_err("full scale read failed with %d\n", rc);
		return rc;
	}

	if (iadc->external_rsense)
		iadc->iadc_comp.ext_rsense = true;

	pr_debug("fab id = %u, revision_dig_major = %u, revision_ana_minor = %u sys gain = %u, external_rsense = %d\n",
			iadc->iadc_comp.id,
			iadc->iadc_comp.revision_dig_major,
			iadc->iadc_comp.revision_ana_minor,
			iadc->iadc_comp.sys_gain,
			iadc->iadc_comp.ext_rsense);
	return rc;
}

static int32_t qpnp_iadc_configure(struct qpnp_iadc_chip *iadc,
			enum qpnp_iadc_channels channel, uint16_t *raw_code,
			uint32_t mode_sel)
{
	u8 status = 0, mode = QPNP_ADC_TRIM_EN;
	u16 raw = 0;
	unsigned int count = 0;
	int rc, cleanup;

	/* Only a real prior failed safeoff can leave this set. */
	if (iadc->conversion_on) {
		rc = qpnp_iadc_enable(iadc, false);
		if (rc)
			return rc;
	}
	if (iadc->iadc_mode_sel)
		mode |= QPNP_VADC_SYNCH_EN;
	rc = qpnp_iadc_write_reg(iadc, QPNP_IADC_MODE_CTL, mode);
	if (rc)
		return rc;
	rc = qpnp_iadc_write_reg(iadc, QPNP_IADC_ADC_CH_SEL_CTL, channel);
	if (rc)
		return rc;
	rc = qpnp_iadc_write_reg(iadc, QPNP_ADC_DIG_PARAM,
			iadc->adc->amux_prop->decimation << QPNP_IADC_DEC_RATIO_SEL);
	if (rc)
		return rc;
	rc = qpnp_iadc_write_reg(iadc, QPNP_FAST_AVG_CTL,
				iadc->adc->amux_prop->fast_avg_setup);
	if (rc)
		return rc;
	if (!iadc->iadc_poll_eoc)
		reinit_completion(&iadc->adc->adc_rslt_completion);
	rc = qpnp_iadc_enable(iadc, true);
	if (rc)
		goto out;
	rc = qpnp_iadc_write_reg(iadc, QPNP_CONV_REQ, QPNP_IADC_CONV_REQ);
	if (rc)
		goto out;
	if (iadc->iadc_poll_eoc) {
		while (status != QPNP_STATUS1_EOC) {
			rc = qpnp_iadc_read_reg(iadc, QPNP_STATUS1, &status);
			if (rc)
				goto out;
			status &= QPNP_STATUS1_REQ_STS_EOC_MASK;
			usleep_range(QPNP_ADC_CONV_TIME_MIN, QPNP_ADC_CONV_TIME_MAX);
			if (++count > QPNP_ADC_ERR_COUNT) {
				qpnp_iadc_status_debug(iadc);
				rc = -EINVAL;
				goto out;
			}
		}
	} else if (!wait_for_completion_timeout(&iadc->adc->adc_rslt_completion,
					      QPNP_ADC_COMPLETION_TIMEOUT)) {
		rc = qpnp_iadc_read_reg(iadc, QPNP_STATUS1, &status);
		if (rc)
			goto out;
		if ((status & QPNP_STATUS1_REQ_STS_EOC_MASK) != QPNP_STATUS1_EOC) {
			qpnp_iadc_status_debug(iadc);
			rc = -EINVAL;
			goto out;
		}
	}
	rc = qpnp_iadc_read_conversion_result(iadc, &raw);
out:
	cleanup = iadc->conversion_on ? qpnp_iadc_enable(iadc, false) : 0;
	if (!rc)
		rc = cleanup;
	if (!rc)
		*raw_code = raw;
	return rc;
}

#define IADC_CENTER	0xC000
#define IADC_READING_RESOLUTION_N	542535
#define IADC_READING_RESOLUTION_D	100000
static int32_t qpnp_convert_raw_offset_voltage(struct qpnp_iadc_calib *calib)
{
	s64 numerator;

	if (calib->gain_raw <= calib->offset_raw) {
		pr_err("raw offset errors! raw_gain:0x%x and raw_offset:0x%x\n",
			calib->gain_raw, calib->offset_raw);
		return -EINVAL;
	}

	numerator = calib->offset_raw - IADC_CENTER;
	numerator *= IADC_READING_RESOLUTION_N;
	calib->offset_uv = div_s64(numerator,
						IADC_READING_RESOLUTION_D);

	numerator = calib->gain_raw - calib->offset_raw;
	numerator *= IADC_READING_RESOLUTION_N;

	calib->gain_uv = div_s64(numerator,
						IADC_READING_RESOLUTION_D);

	pr_debug("gain_uv:%d offset_uv:%d\n",
			calib->gain_uv, calib->offset_uv);
	return 0;
}

#define IADC_IDEAL_RAW_GAIN	3291
static int32_t c5_qpnp_iadc_calibrate_for_trim_locked(struct qpnp_iadc_chip *iadc,
							bool batfet_closed)
{
	uint8_t rslt_lsb, rslt_msb;
	int32_t rc = 0, version = 0;
	uint16_t raw_data;
	uint32_t mode_sel = 0;
	bool iadc_offset_ch_batfet_check;
	struct qpnp_iadc_calib next = iadc->adc->calib;


	mutex_lock(&iadc->adc->adc_lock);

	if (iadc->iadc_poll_eoc) {
		pr_debug("acquiring iadc eoc wakelock\n");
		pm_stay_awake(iadc->dev);
	}

	iadc->adc->amux_prop->decimation = DECIMATION_TYPE1;
	iadc->adc->amux_prop->fast_avg_setup = ADC_FAST_AVG_SAMPLE_1;

	rc = qpnp_iadc_configure(iadc, GAIN_CALIBRATION_17P857MV,
						&raw_data, mode_sel);
	if (rc < 0) {
		pr_err("qpnp adc result read failed with %d\n", rc);
		goto fail;
	}

	next.gain_raw = raw_data;

	/*
	 * there is a features on PM8941 in the BMS where if the batfet is
	 * opened the BMS reads from INTERNAL_RSENSE (channel 0) actually go to
	 * OFFSET_CALIBRATION_CSP_CSN (channel 5). Hence if batfet is opened
	 * we have to calibrate based on OFFSET_CALIBRATION_CSP_CSN even for
	 * internal rsense.
	 */
	version = qpnp_adc_get_revid_version(iadc->dev);
	if ((version == QPNP_REV_ID_8941_3_1) ||
			(version == QPNP_REV_ID_8941_3_0) ||
			(version == QPNP_REV_ID_8941_2_0))
		iadc_offset_ch_batfet_check = true;
	else
		iadc_offset_ch_batfet_check = false;

	if ((iadc_offset_ch_batfet_check && !batfet_closed) ||
						(iadc->external_rsense)) {
		/* external offset calculation */
		rc = qpnp_iadc_configure(iadc, OFFSET_CALIBRATION_CSP_CSN,
						&raw_data, mode_sel);
		if (rc < 0) {
			pr_err("qpnp adc result read failed with %d\n", rc);
			goto fail;
		}
	} else {
		/* internal offset calculation */
		rc = qpnp_iadc_configure(iadc, OFFSET_CALIBRATION_CSP2_CSN2,
						&raw_data, mode_sel);
		if (rc < 0) {
			pr_err("qpnp adc result read failed with %d\n", rc);
			goto fail;
		}
	}

	next.offset_raw = raw_data;
	if (rc < 0) {
		pr_err("qpnp adc offset/gain calculation failed\n");
		goto fail;
	}

	if (iadc->iadc_comp.revision_dig_major == QPNP_IADC_PM8026_2_REV2
		&& iadc->iadc_comp.revision_ana_minor ==
						QPNP_IADC_PM8026_2_REV3) {
		if (next.offset_raw > 0xffff - IADC_IDEAL_RAW_GAIN) {
			rc = -ERANGE;
			goto fail;
		}
		next.gain_raw = next.offset_raw + IADC_IDEAL_RAW_GAIN;
	}

	pr_debug("raw gain:0x%x, raw offset:0x%x\n",
		next.gain_raw, next.offset_raw);

	rc = qpnp_convert_raw_offset_voltage(&next);
	if (rc < 0) {
		pr_err("qpnp raw_voltage conversion failed\n");
		goto fail;
	}

	rslt_msb = (raw_data & QPNP_RAW_CODE_16_BIT_MSB_MASK) >>
							QPNP_BIT_SHIFT_8;
	rslt_lsb = raw_data & QPNP_RAW_CODE_16_BIT_LSB_MASK;

	pr_debug("trim values:lsb:0x%x and msb:0x%x\n", rslt_lsb, rslt_msb);

	rc = qpnp_iadc_write_reg(iadc, QPNP_IADC_SEC_ACCESS,
					QPNP_IADC_SEC_ACCESS_DATA);
	if (rc < 0) {
		pr_err("qpnp iadc configure error for sec access\n");
		goto fail;
	}

	rc = qpnp_iadc_write_reg(iadc, QPNP_IADC_MSB_OFFSET,
						rslt_msb);
	if (rc < 0) {
		pr_err("qpnp iadc configure error for MSB write\n");
		goto fail;
	}

	rc = qpnp_iadc_write_reg(iadc, QPNP_IADC_SEC_ACCESS,
					QPNP_IADC_SEC_ACCESS_DATA);
	if (rc < 0) {
		pr_err("qpnp iadc configure error for sec access\n");
		goto fail;
	}

	rc = qpnp_iadc_write_reg(iadc, QPNP_IADC_LSB_OFFSET,
						rslt_lsb);
	if (rc < 0) {
		pr_err("qpnp iadc configure error for LSB write\n");
		goto fail;
	}
	if (!rc) {
		iadc->adc->calib = next;
		iadc->calibrated = true;
	}
fail:
	if (rc)
		iadc->calibrated = false;
	if (iadc->iadc_poll_eoc) {
		pr_debug("releasing iadc eoc wakelock\n");
		pm_relax(iadc->dev);
	}
	mutex_unlock(&iadc->adc->adc_lock);
	return rc;
}

static void qpnp_iadc_work(struct work_struct *work)
{
	struct qpnp_iadc_chip *iadc = container_of(work,
				struct qpnp_iadc_chip, iadc_work.work);

	if (!c5_iadc_api_begin(iadc, false)) {
		if (!iadc->skip_auto_calibrations)
			c5_qpnp_iadc_calibrate_for_trim_locked(iadc, true);
		c5_iadc_api_end(iadc);
	}
	c5_iadc_queue_auto(iadc);
}

static int32_t qpnp_iadc_version_check(struct qpnp_iadc_chip *iadc)
{
	uint8_t revision;
	int rc;

	rc = qpnp_iadc_read_reg(iadc, QPNP_IADC_REVISION2, &revision);
	if (rc < 0) {
		pr_err("qpnp adc result read failed with %d\n", rc);
		return rc;
	}

	if (revision < QPNP_IADC_SUPPORTED_REVISION2) {
		pr_err("IADC Version not supported\n");
		return -EINVAL;
	}

	return 0;
}

struct qpnp_iadc_chip *qpnp_get_iadc(struct device *dev, const char *name)
{
	struct qpnp_iadc_chip *iadc, *result = ERR_PTR(-EPROBE_DEFER);
	struct device_node *node;
	char property[QPNP_MAX_PROP_NAME_LEN];
	int len;

	if (!dev || !dev->of_node || !name)
		return ERR_PTR(-EINVAL);
	len = snprintf(property, sizeof(property), "qcom,%s-iadc", name);
	if (len < 0 || len >= sizeof(property))
		return ERR_PTR(-EINVAL);
	node = of_parse_phandle(dev->of_node, property, 0);
	if (!node)
		return ERR_PTR(-ENODEV);
	mutex_lock(&c5_iadc_registry_lock);
	list_for_each_entry(iadc, &qpnp_iadc_device_list, list) {
		if (iadc->ready && !iadc->stopping &&
		    iadc->adc->pdev->dev.of_node == node) {
			result = iadc;
			break;
		}
	}
	mutex_unlock(&c5_iadc_registry_lock);
	of_node_put(node);
	return result;
}
EXPORT_SYMBOL(qpnp_get_iadc);

static int32_t c5_qpnp_iadc_get_rsense_locked(struct qpnp_iadc_chip *iadc, int32_t *rsense)
{
	uint8_t	rslt_rsense = 0;
	int32_t	rc = 0, sign_bit = 0;


	if (iadc->external_rsense) {
		*rsense = iadc->rsense;
	} else if (iadc->default_internal_rsense) {
		*rsense = iadc->rsense_workaround_value;
	} else {

		rc = qpnp_iadc_read_reg(iadc, QPNP_IADC_NOMINAL_RSENSE,
							&rslt_rsense);
		if (rc < 0) {
			pr_err("qpnp adc rsense read failed with %d\n", rc);
			return rc;
		}

		pr_debug("rsense:0%x\n", rslt_rsense);

		if (rslt_rsense & QPNP_RSENSE_MSB_SIGN_CHECK)
			sign_bit = 1;

		rslt_rsense &= ~QPNP_RSENSE_MSB_SIGN_CHECK;

		if (sign_bit)
			*rsense = QPNP_IADC_INTERNAL_RSENSE_N_OHMS_FACTOR -
			(rslt_rsense * QPNP_IADC_RSENSE_LSB_N_OHMS_PER_BIT);
		else
			*rsense = QPNP_IADC_INTERNAL_RSENSE_N_OHMS_FACTOR +
			(rslt_rsense * QPNP_IADC_RSENSE_LSB_N_OHMS_PER_BIT);
	}
	pr_debug("rsense value is %d\n", *rsense);

	if (*rsense <= 0)
		pr_err("incorrect rsens value:%d rslt_rsense:%d\n",
				*rsense, rslt_rsense);

	return *rsense > 0 ? rc : -EINVAL;
}

static int32_t qpnp_check_pmic_temp(struct qpnp_iadc_chip *iadc)
{
	struct qpnp_vadc_result sample;
	s64 difference;
	int rc;

	rc = qpnp_vadc_read(iadc->vadc_dev, DIE_TEMP, &sample);
	if (rc)
		return rc;
	if (c5_iadc_sub(sample.physical, iadc->die_temp, &difference) ||
	    difference == C5_IADC_S64_MIN)
		return -ERANGE;
	if (difference < 0)
		difference = -difference;
	if (difference > QPNP_IADC_DIE_TEMP_CALIB_OFFSET) {
		iadc->die_temp = sample.physical;
		if (!iadc->skip_auto_calibrations)
			return c5_qpnp_iadc_calibrate_for_trim_locked(iadc, true);
	}
	return 0;
}

static int32_t c5_qpnp_iadc_read_locked(struct qpnp_iadc_chip *iadc,
		enum qpnp_iadc_channels channel, struct qpnp_iadc_result *result)
{
	struct qpnp_iadc_result next;
	s64 result_current;
	int index, rc, rsense, divisor, num, sign;
	u16 raw;

	index = c5_iadc_channel_index(iadc, channel);
	if (index < 0)
		return index;
	if (!iadc->calibrated ||
	    iadc->adc->calib.gain_raw <= iadc->adc->calib.offset_raw)
		return -EINVAL;
	rc = qpnp_check_pmic_temp(iadc);
	if (rc)
		return rc;
	mutex_lock(&iadc->adc->adc_lock);
	iadc->adc->amux_prop->decimation = iadc->adc->adc_channels[index].adc_decimation;
	iadc->adc->amux_prop->fast_avg_setup = iadc->adc->adc_channels[index].fast_avg_setup;
	if (iadc->iadc_poll_eoc)
		pm_stay_awake(iadc->dev);
	rc = qpnp_iadc_configure(iadc, channel, &raw, 0);
	if (rc)
		goto out;
	rc = c5_qpnp_iadc_get_rsense_locked(iadc, &rsense);
	if (rc)
		goto out;
	divisor = rsense / 1000;
	if (!divisor) {
		rc = -EINVAL;
		goto out;
	}
	num = (int)raw - iadc->adc->calib.offset_raw;
	sign = num < 0;
	if (sign)
		num = -num;
	next.result_uv = (num * QPNP_ADC_GAIN_NV) /
		(iadc->adc->calib.gain_raw - iadc->adc->calib.offset_raw);
	result_current = (s64)next.result_uv * QPNP_IADC_NANO_VOLTS_FACTOR;
	do_div(result_current, divisor);
	if (sign) {
		next.result_uv = -next.result_uv;
		if (result_current == C5_IADC_S64_MIN) {
		rc = -ERANGE;
		goto out;
	}
	result_current = -result_current;
	}
	if (result_current == C5_IADC_S64_MIN) {
		rc = -ERANGE;
		goto out;
	}
	result_current = -result_current;
	rc = c5_qpnp_iadc_comp_result_locked(iadc, &result_current);
	if (rc)
		goto out;
	if (result_current == C5_IADC_S64_MIN) {
		rc = -ERANGE;
		goto out;
	}
	result_current = -result_current;
	if (result_current > 2147483647LL || result_current < -2147483648LL) {
		rc = -ERANGE;
		goto out;
	}
	next.result_ua = result_current;
	*result = next;
out:
	if (iadc->iadc_poll_eoc)
		pm_relax(iadc->dev);
	mutex_unlock(&iadc->adc->adc_lock);
	return rc;
}

static int32_t c5_qpnp_iadc_get_gain_and_offset_locked(struct qpnp_iadc_chip *iadc,
					struct qpnp_iadc_calib *result)
{
	int rc;


	if (!iadc->calibrated)
		return -EINVAL;
	rc = qpnp_check_pmic_temp(iadc);
	if (rc) {
		pr_err("Error checking pmic therm temp\n");
		return rc;
	}

	mutex_lock(&iadc->adc->adc_lock);
	result->gain_raw = iadc->adc->calib.gain_raw;
	result->ideal_gain_nv = QPNP_ADC_GAIN_NV;
	result->gain_uv = iadc->adc->calib.gain_uv;
	result->offset_raw = iadc->adc->calib.offset_raw;
	result->ideal_offset_uv =
				QPNP_OFFSET_CALIBRATION_SHORT_CADC_LEADS_IDEAL;
	result->offset_uv = iadc->adc->calib.offset_uv;
	pr_debug("raw gain:0%x, raw offset:0%x\n",
			result->gain_raw, result->offset_raw);
	pr_debug("gain_uv:%d offset_uv:%d\n",
			result->gain_uv, result->offset_uv);
	mutex_unlock(&iadc->adc->adc_lock);

	return 0;
}

static int c5_qpnp_iadc_skip_calibration_locked(struct qpnp_iadc_chip *iadc)
{
	iadc->skip_auto_calibrations = true;
	return 0;
}

static int c5_qpnp_iadc_resume_calibration_locked(struct qpnp_iadc_chip *iadc)
{
	iadc->skip_auto_calibrations = false;
	return 0;
}

static int32_t c5_qpnp_iadc_vadc_sync_read_locked(struct qpnp_iadc_chip *iadc,
	enum qpnp_iadc_channels i_channel, struct qpnp_iadc_result *i_result,
	enum qpnp_vadc_channels v_channel, struct qpnp_vadc_result *v_result)
{
	struct qpnp_iadc_result next_i;
	struct qpnp_vadc_result next_v;
	s64 result_current;
	int index, rc, cleanup, rsense, divisor, num, sign;
	u16 raw;
	bool requested = false;

	memcpy(&next_v, v_result, sizeof(next_v));
	/* Reject invalid IADC input before acquiring the shared VADC transaction. */
	index = c5_iadc_channel_index(iadc, i_channel);
	if (index < 0)
		return index;
	if (!iadc->calibrated ||
	    iadc->adc->calib.gain_raw <= iadc->adc->calib.offset_raw)
		return -EINVAL;
	mutex_lock(&iadc->adc->adc_lock);
	if (iadc->iadc_poll_eoc)
		pm_stay_awake(iadc->dev);
	iadc->iadc_mode_sel = true;
	rc = qpnp_vadc_iadc_sync_request(iadc->vadc_dev, v_channel);
	if (rc)
		goto out;
	requested = true;
	iadc->adc->amux_prop->decimation = iadc->adc->adc_channels[index].adc_decimation;
	iadc->adc->amux_prop->fast_avg_setup = iadc->adc->adc_channels[index].fast_avg_setup;
	rc = qpnp_iadc_configure(iadc, i_channel, &raw, 0);
	if (rc)
		goto complete;
	rc = c5_qpnp_iadc_get_rsense_locked(iadc, &rsense);
	if (rc)
		goto complete;
	divisor = rsense / 1000;
	if (!divisor) {
		rc = -EINVAL;
		goto complete;
	}
	num = (int)raw - iadc->adc->calib.offset_raw;
	sign = num < 0;
	if (sign)
		num = -num;
	next_i.result_uv = (num * QPNP_ADC_GAIN_NV) /
		(iadc->adc->calib.gain_raw - iadc->adc->calib.offset_raw);
	result_current = (s64)next_i.result_uv * QPNP_IADC_NANO_VOLTS_FACTOR;
	do_div(result_current, divisor);
	if (sign) {
		next_i.result_uv = -next_i.result_uv;
		if (result_current == C5_IADC_S64_MIN) {
		rc = -ERANGE;
		goto complete;
	}
	result_current = -result_current;
	}
	if (result_current == C5_IADC_S64_MIN) {
		rc = -ERANGE;
		goto complete;
	}
	result_current = -result_current;
	rc = c5_qpnp_iadc_comp_result_locked(iadc, &result_current);
	if (rc)
		goto complete;
	if (result_current == C5_IADC_S64_MIN) {
		rc = -ERANGE;
		goto complete;
	}
	result_current = -result_current;
	if (result_current > 2147483647LL || result_current < -2147483648LL) {
		rc = -ERANGE;
		goto complete;
	}
	next_i.result_ua = result_current;
complete:
	/* Exactly one completion for every successful request, even on failure. */
	if (requested) {
		cleanup = qpnp_vadc_iadc_sync_complete_request(iadc->vadc_dev,
							v_channel, &next_v);
		if (cleanup && !iadc->restore_error)
			iadc->restore_error = cleanup;
		if (!rc)
			rc = cleanup;
	}
	if (!rc) {
		*i_result = next_i;
		*v_result = next_v;
	}
out:
	iadc->iadc_mode_sel = false;
	if (iadc->iadc_poll_eoc)
		pm_relax(iadc->dev);
	mutex_unlock(&iadc->adc->adc_lock);
	return rc;
}


/* Public C ABI: all handles borrowed, no new caller-owned reference or put API. */
int32_t qpnp_iadc_comp_result(struct qpnp_iadc_chip *iadc, int64_t *result)
{
	s64 next;
	int rc;

	if (!result)
		return -EINVAL;
	rc = c5_iadc_api_begin(iadc, false);
	if (rc)
		return rc;
	next = *result;
	rc = c5_qpnp_iadc_comp_result_locked(iadc, &next);
	if (!rc)
		*result = next;
	c5_iadc_api_end(iadc);
	return rc;
}
EXPORT_SYMBOL(qpnp_iadc_comp_result);

int32_t qpnp_iadc_calibrate_for_trim(struct qpnp_iadc_chip *iadc, bool batfet_closed)
{
	int rc = c5_iadc_api_begin(iadc, false);

	if (rc)
		return rc;
	rc = c5_qpnp_iadc_calibrate_for_trim_locked(iadc, batfet_closed);
	c5_iadc_api_end(iadc);
	return rc;
}
EXPORT_SYMBOL(qpnp_iadc_calibrate_for_trim);

int32_t qpnp_iadc_get_rsense(struct qpnp_iadc_chip *iadc, int32_t *rsense)
{
	s32 next;
	int rc;

	if (!rsense)
		return -EINVAL;
	rc = c5_iadc_api_begin(iadc, false);
	if (rc)
		return rc;
	rc = c5_qpnp_iadc_get_rsense_locked(iadc, &next);
	if (!rc)
		*rsense = next;
	c5_iadc_api_end(iadc);
	return rc;
}
EXPORT_SYMBOL(qpnp_iadc_get_rsense);

int32_t qpnp_iadc_read(struct qpnp_iadc_chip *iadc,
		enum qpnp_iadc_channels channel, struct qpnp_iadc_result *result)
{
	int rc;

	if (!result)
		return -EINVAL;
	rc = c5_iadc_api_begin(iadc, true);
	if (rc)
		return rc;
	rc = c5_qpnp_iadc_read_locked(iadc, channel, result);
	c5_iadc_api_end(iadc);
	return rc;
}
EXPORT_SYMBOL(qpnp_iadc_read);

int32_t qpnp_iadc_get_gain_and_offset(struct qpnp_iadc_chip *iadc,
				    struct qpnp_iadc_calib *result)
{
	int rc;

	if (!result)
		return -EINVAL;
	rc = c5_iadc_api_begin(iadc, true);
	if (rc)
		return rc;
	rc = c5_qpnp_iadc_get_gain_and_offset_locked(iadc, result);
	c5_iadc_api_end(iadc);
	return rc;
}
EXPORT_SYMBOL(qpnp_iadc_get_gain_and_offset);

int qpnp_iadc_skip_calibration(struct qpnp_iadc_chip *iadc)
{
	int rc = c5_iadc_api_begin(iadc, false);

	if (rc)
		return rc;
	rc = c5_qpnp_iadc_skip_calibration_locked(iadc);
	c5_iadc_api_end(iadc);
	return rc;
}
EXPORT_SYMBOL(qpnp_iadc_skip_calibration);

int qpnp_iadc_resume_calibration(struct qpnp_iadc_chip *iadc)
{
	int rc = c5_iadc_api_begin(iadc, false);

	if (rc)
		return rc;
	rc = c5_qpnp_iadc_resume_calibration_locked(iadc);
	c5_iadc_api_end(iadc);
	return rc;
}
EXPORT_SYMBOL(qpnp_iadc_resume_calibration);

int32_t qpnp_iadc_vadc_sync_read(struct qpnp_iadc_chip *iadc,
	enum qpnp_iadc_channels i_channel, struct qpnp_iadc_result *i_result,
	enum qpnp_vadc_channels v_channel, struct qpnp_vadc_result *v_result)
{
	int rc;

	if (!i_result || !v_result)
		return -EINVAL;
	rc = c5_iadc_api_begin(iadc, true);
	if (rc)
		return rc;
	rc = c5_qpnp_iadc_vadc_sync_read_locked(iadc, i_channel, i_result,
						     v_channel, v_result);
	c5_iadc_api_end(iadc);
	return rc;
}
EXPORT_SYMBOL(qpnp_iadc_vadc_sync_read);


static ssize_t qpnp_iadc_show(struct device *dev,
			    struct device_attribute *devattr, char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct qpnp_iadc_chip *iadc = dev_get_drvdata(dev);
	struct qpnp_iadc_result result;
	int rc = qpnp_iadc_read(iadc, attr->index, &result);

	if (rc)
		return rc;
	return scnprintf(buf, PAGE_SIZE, "%d\n", result.result_ua);
}

static struct sensor_device_attribute qpnp_adc_attr =
	SENSOR_ATTR(NULL, 0444, qpnp_iadc_show, NULL, 0);


static int c5_iadc_named_reg(struct device_node *node, const char *name, u32 *value)
{
	int index, address_cells, size_cells;

	if (!of_find_property(node, "reg-names", NULL))
		return -ENOENT;
	index = of_property_match_string(node, "reg-names", name);
	if (index == -ENODATA)
		return -ENOENT;
	if (index < 0)
		return -EINVAL;
	address_cells = of_n_addr_cells(node);
	size_cells = of_n_size_cells(node);
	if (address_cells != 1 || size_cells < 0 || size_cells > 1)
		return -EINVAL;
	return of_property_read_u32_index(node, "reg",
			index * (address_cells + size_cells), value);
}

static int c5_iadc_binding(struct platform_device *pdev, u32 *base, u32 *trim)
{
	struct device_node *node = pdev->dev.of_node, *child;
	u32 named, scalar, channel, value;
	int a, b, count = 0;

	if (!node || !pdev->dev.parent || (!pdev->dev.parent->bus || !pdev->dev.parent->bus->name || strcmp(pdev->dev.parent->bus->name, "spmi")))
		return -EINVAL;
	a = c5_iadc_named_reg(node, "iadc-base", base);
	if (a == -ENOENT)
		a = of_property_read_u32(node, "reg", base);
	if (a || *base > 0xff00)
		return -EINVAL;
	a = c5_iadc_named_reg(node, "batt-id-trim-cnst-rds", &named);
	b = of_find_property(node, "batt-id-trim-cnst-rds", NULL) ?
		of_property_read_u32(node, "batt-id-trim-cnst-rds", &scalar) : -ENOENT;
	if ((a && a != -ENOENT) || (b && b != -ENOENT) ||
	    (a && b) || (!a && !b && named != scalar))
		return -EINVAL;
	*trim = !b ? scalar : named;
	if (*trim > 0xffff)
		return -EINVAL;
	for_each_child_of_node(node, child) {
		if (of_property_read_u32(child, "reg", &channel) || channel >= IADC_MUX_NUM ||
		    of_property_read_u32(child, "qcom,decimation", &value) || value > DECIMATION_TYPE4 ||
		    of_property_read_u32(child, "qcom,fast-avg-setup", &value) || value > ADC_FAST_AVG_SAMPLE_256) {
			of_node_put(child);
			return -EINVAL;
		}
		count++;
	}
	return count ? count : -EINVAL;
}

static void c5_iadc_remove_attrs(struct qpnp_iadc_chip *iadc)
{
	while (iadc->attrs_created) {
		iadc->attrs_created--;
		device_remove_file(iadc->dev, &iadc->sens_attr[iadc->attrs_created].dev_attr);
	}
}

static int c5_iadc_drain_irq(struct qpnp_iadc_chip *iadc)
{
	int rc = 0;

	if (iadc->irq_wake_enabled) {
		rc = disable_irq_wake(iadc->adc->adc_irq_eoc);
		iadc->irq_wake_enabled = false;
	}
	if (iadc->irq_requested) {
		disable_irq(iadc->adc->adc_irq_eoc);
		devm_free_irq(iadc->dev, iadc->adc->adc_irq_eoc, iadc);
		iadc->irq_requested = false;
	}
	cancel_work_sync(&iadc->trigger_completion_work);
	return rc;
}

static void c5_iadc_release_supplies(struct qpnp_iadc_chip *iadc)
{
	/* Common metadata helper owns separate regulator_get handles, even if a
	 * later supply failed. IADC never regulator_enable's these handles.
	 */
	if (!IS_ERR_OR_NULL(iadc->adc->hkadc_ldo))
		regulator_put(iadc->adc->hkadc_ldo);
	if (!IS_ERR_OR_NULL(iadc->adc->hkadc_ldo_ok))
		regulator_put(iadc->adc->hkadc_ldo_ok);
	iadc->adc->hkadc_ldo = NULL;
	iadc->adc->hkadc_ldo_ok = NULL;
}





static int c5_iadc_get_supplier(struct qpnp_iadc_chip *iadc)
{
	struct device_node *node;
	struct platform_device *supplier;
	struct qpnp_vadc_chip *vadc;
	int rc = 0;

	node = of_parse_phandle(iadc->dev->of_node, "qcom,iadc-vadc", 0);
	if (!node)
		return -ENODEV;
	/* Synchronized IADC uses the legacy VADC register protocol only. */
	if (!of_device_is_compatible(node, "qcom,qpnp-vadc") ||
	    of_device_is_compatible(node, "qcom,qpnp-vadc-hc") ||
	    of_device_is_compatible(node, "qcom,qpnp-adc-hc-pm5")) {
		of_node_put(node);
		return -EINVAL;
	}
	/* of_find_device_by_node returns a referenced platform device. */
	supplier = of_find_device_by_node(node);
	of_node_put(node);
	if (!supplier)
		return -EPROBE_DEFER;
	if (&supplier->dev == iadc->dev) {
		put_device(&supplier->dev);
		return -EINVAL;
	}
	device_lock(&supplier->dev);
	if (!supplier->dev.driver || !dev_get_drvdata(&supplier->dev)) {
		rc = -EPROBE_DEFER;
	} else {
		vadc = qpnp_get_vadc(iadc->dev, "iadc");
		if (IS_ERR(vadc))
			rc = PTR_ERR(vadc);
		else if (vadc != dev_get_drvdata(&supplier->dev))
			rc = -EPROBE_DEFER;
		else
			iadc->vadc_dev = vadc;
	}
	device_unlock(&supplier->dev);
	if (rc) {
		put_device(&supplier->dev);
		return rc;
	}
	iadc->vadc_supplier = &supplier->dev;
	return 0;
}


static int qpnp_iadc_probe(struct platform_device *pdev)
{
	struct qpnp_iadc_chip *iadc;
	struct qpnp_adc_drv *adc;
	u32 base, trim, external;
	int count, rc, cleanup, i;

	count = c5_iadc_binding(pdev, &base, &trim);
	if (count < 0)
		return count;
	iadc = devm_kzalloc(&pdev->dev, sizeof(*iadc) +
			count * sizeof(struct sensor_device_attribute), GFP_KERNEL);
	if (!iadc)
		return -ENOMEM;
	adc = devm_kzalloc(&pdev->dev, sizeof(*adc), GFP_KERNEL);
	if (!adc)
		return -ENOMEM;
	iadc->dev = &pdev->dev;
	iadc->adc = adc;
	INIT_LIST_HEAD(&iadc->list);
	mutex_init(&iadc->op_lock);
	mutex_init(&adc->adc_lock);
	init_waitqueue_head(&iadc->api_wait);
	INIT_WORK(&iadc->trigger_completion_work, qpnp_iadc_trigger_completion);
	INIT_DELAYED_WORK(&iadc->iadc_work, qpnp_iadc_work);
	iadc->stopping = true;
	adc->regmap = dev_get_regmap(pdev->dev.parent, NULL);
	if (!adc->regmap)
		return -EINVAL;
	rc = c5_iadc_get_supplier(iadc);
	if (rc)
		return rc;
	/* The canonical helper maps other IRQ errors to ENXIO. Preserve a real
	 * supplier-defer result before calling it, without requiring a new IRQ.
	 */
	rc = platform_get_irq_byname(pdev, "eoc-int-en-set");
	if (rc == -EPROBE_DEFER)
		goto supplies;
	rc = qpnp_adc_get_devicetree_data(pdev, adc);
	if (rc)
		goto supplies;
	adc->offset = base;
	iadc->batt_id_trim_cnst_rds = trim;
	iadc->max_channels_available = count;
	rc = of_property_read_u32(pdev->dev.of_node, "qcom,rsense", &external);
	if (rc && of_find_property(pdev->dev.of_node, "qcom,rsense", NULL)) {
		rc = -EINVAL;
		goto supplies;
	}
	if (!rc) {
		if (external < 1000 || external > 2147483647U) {
			rc = -EINVAL;
			goto supplies;
		}
		iadc->external_rsense = true;
		iadc->rsense = external;
	}
	rc = of_property_read_u32(pdev->dev.of_node, "qcom,use-default-rds-trim", &external);
	if (rc && of_find_property(pdev->dev.of_node, "qcom,use-default-rds-trim", NULL)) {
		rc = -EINVAL;
		goto supplies;
	}
	if (!rc) {
		if (external > QPNP_IADC_RDS_DEFAULT_TYPEC) {
			rc = -EINVAL;
			goto supplies;
		}
		iadc->rds_trim_default_check = true;
		iadc->rds_trim_default_type = external;
	}
	iadc->iadc_poll_eoc = of_property_read_bool(pdev->dev.of_node, "qcom,iadc-poll-eoc");
	if (iadc->iadc_poll_eoc) {
		rc = device_init_wakeup(iadc->dev, true);
		if (rc)
			goto supplies;
		iadc->wakeup_owned = true;
	} else {
		rc = devm_request_irq(iadc->dev, adc->adc_irq_eoc, qpnp_iadc_isr,
				      IRQF_TRIGGER_RISING, "qpnp_iadc_interrupt", iadc);
		if (rc)
			goto supplies;
		iadc->irq_requested = true;
		rc = enable_irq_wake(adc->adc_irq_eoc);
		if (rc)
			goto irq;
		iadc->irq_wake_enabled = true;
	}
	rc = qpnp_iadc_version_check(iadc);
	if (rc)
		goto irq;
	rc = qpnp_iadc_comp_info(iadc);
	if (rc)
		goto irq;
	rc = qpnp_iadc_rds_trim_update_check(iadc);
	if (rc)
		goto irq;
	/* Initial calibration uses the private owned state, before discovery or
	 * sysfs/HWMON publication; failure is not a ready successful provider.
	 */
	mutex_lock(&iadc->op_lock);
	rc = c5_qpnp_iadc_calibrate_for_trim_locked(iadc, true);
	mutex_unlock(&iadc->op_lock);
	if (rc)
		goto irq;
	dev_set_drvdata(&pdev->dev, iadc);
	for (i = 0; i < count; i++) {
		memcpy(&iadc->sens_attr[i], &qpnp_adc_attr, sizeof(qpnp_adc_attr));
		iadc->sens_attr[i].dev_attr.attr.name = adc->adc_channels[i].name;
		iadc->sens_attr[i].index = adc->adc_channels[i].channel_num;
		sysfs_attr_init(&iadc->sens_attr[i].dev_attr.attr);
		rc = device_create_file(iadc->dev, &iadc->sens_attr[i].dev_attr);
		if (rc)
			goto attrs;
		iadc->attrs_created++;
	}
	iadc->iadc_hwmon = hwmon_device_register(iadc->dev);
	if (IS_ERR(iadc->iadc_hwmon)) {
		rc = PTR_ERR(iadc->iadc_hwmon);
		iadc->iadc_hwmon = NULL;
		goto attrs;
	}
	mutex_lock(&c5_iadc_registry_lock);
	iadc->ready = true;
	iadc->stopping = false;
	list_add(&iadc->list, &qpnp_iadc_device_list);
	mutex_unlock(&c5_iadc_registry_lock);
	c5_iadc_queue_auto(iadc);
	return 0;
attrs:
	c5_iadc_remove_attrs(iadc);
	dev_set_drvdata(&pdev->dev, NULL);
irq:
	/* No public operation was ever admitted on probe failure. */
	if (iadc->conversion_on)
		qpnp_iadc_enable(iadc, false);
	cleanup = c5_iadc_drain_irq(iadc);
	if (cleanup && !iadc->restore_error)
		iadc->restore_error = cleanup;
	if (iadc->restore_error)
		pr_err("IADC probe cleanup failed: %d\n", iadc->restore_error);
	if (iadc->wakeup_owned)
		device_init_wakeup(iadc->dev, false);
supplies:
	c5_iadc_release_supplies(iadc);
	put_device(iadc->vadc_supplier);
	iadc->vadc_supplier = NULL;
	return rc;
}

static int qpnp_iadc_remove(struct platform_device *pdev)
{
	struct qpnp_iadc_chip *iadc = dev_get_drvdata(&pdev->dev);
	int rc = 0, cleanup;

	if (!iadc)
		return 0;
	mutex_lock(&c5_iadc_registry_lock);
	iadc->stopping = true;
	iadc->ready = false;
	list_del_init(&iadc->list);
	mutex_unlock(&c5_iadc_registry_lock);
	/* No lock is held across an attribute/work/active-operation drain.
	 * EOC IRQ/completion stay functional until all conversions finish.
	 */
	c5_iadc_remove_attrs(iadc);
	cancel_delayed_work_sync(&iadc->iadc_work);
	wait_event(iadc->api_wait, !READ_ONCE(iadc->active_ops));
	if (iadc->conversion_on)
		rc = qpnp_iadc_enable(iadc, false);
	cleanup = c5_iadc_drain_irq(iadc);
	if (!rc)
		rc = cleanup;
	if (iadc->iadc_hwmon)
		hwmon_device_unregister(iadc->iadc_hwmon);
	if (iadc->wakeup_owned)
		device_init_wakeup(iadc->dev, false);
	c5_iadc_release_supplies(iadc);
	put_device(iadc->vadc_supplier);
	iadc->vadc_supplier = NULL;
	dev_set_drvdata(&pdev->dev, NULL);
	return rc;
}

static const struct of_device_id qpnp_iadc_match_table[] = {
	{	.compatible = "qcom,qpnp-iadc",
	},
	{}
};

static struct platform_driver qpnp_iadc_driver = {
	.driver		= {
		.name		= "qcom,qpnp-iadc",
		.of_match_table = qpnp_iadc_match_table,
	},
	.probe		= qpnp_iadc_probe,
	.remove		= qpnp_iadc_remove,
};

static int __init qpnp_iadc_init(void)
{
	return platform_driver_register(&qpnp_iadc_driver);
}
module_init(qpnp_iadc_init);

static void __exit qpnp_iadc_exit(void)
{
	platform_driver_unregister(&qpnp_iadc_driver);
}
module_exit(qpnp_iadc_exit);

MODULE_DESCRIPTION("QPNP PMIC current ADC driver");
MODULE_LICENSE("GPL v2");
