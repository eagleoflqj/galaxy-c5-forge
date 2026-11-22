/* SPDX-License-Identifier: GPL-2.0-only */
/* C5 manual-only update protocol, derived from Samsung's GPLv2 zt7538.c.
 * Native arm/update controls exist only under the separate WRITE_CONTROL gate.
 * All live persistent frontends use task-owned, one-purpose NVM transports.
 */
#if IS_ENABLED(CONFIG_C5_CONTROLLER_FW_UPDATE)
#include <crypto/hash.h>
#include <crypto/sha.h>
#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/vmalloc.h>

#define C5_ZT_FW_BYTES (44U * 1024U)
#define C5_ZT_DEADLINE_MS 30000

struct c5_zt_nvm_permit {
	struct zt7538_ts_info *info;
	struct task_struct *owner;
	unsigned long deadline;
	bool active;
	bool nvm_may_have_changed;
	enum c5_zt_nvm_purpose purpose;
	int restore_error; /* First failed safe-off attempt; primary error stays separate. */
};

static bool c5_zt_permit_valid(const struct c5_zt_nvm_permit *p)
{
	return p && p->active && p->info && p->owner == current &&
	p->purpose > C5_ZT_NVM_NONE && p->purpose <= C5_ZT_NVM_CLEAR_REFERENCE &&
		READ_ONCE(p->info->c5_io_owner) == current;
}

/* This is an operation capability, not permission for arbitrary registers. */
static bool c5_zt_purpose_allows(const struct c5_zt_nvm_permit *p, u16 reg)
{
	switch (reg) {
	case ZT7538_INIT_FLASH: case ZT7538_WRITE_FLASH: case ZT7538_READ_FLASH:
	case 0x01d3: case 0x01de: case 0x01df:
		return p->purpose == C5_ZT_NVM_FIRMWARE;
	case ZT7538_SAVE_CALIBRATION_CMD: case ZT7538_CALIBRATE_CMD:
		return p->purpose == C5_ZT_NVM_FIRMWARE ||
			p->purpose == C5_ZT_NVM_HW_CALIBRATE ||
			p->purpose == C5_ZT_NVM_REFERENCE_CALIBRATE;
	case ZT7538_SAVE_STATUS_CMD:
		return p->purpose == C5_ZT_NVM_SAVE_STATUS ||
			p->purpose == C5_ZT_NVM_CLEAR_REFERENCE;
	case 0xc003: case 0xc104:
		return p->purpose == C5_ZT_NVM_FIRMWARE ||
			p->purpose == C5_ZT_NVM_CLEAR_REFERENCE;
	default:
		return true; /* Only hard-coded operation helpers supply these registers. */
	}
}

/* Only exact safe protection-off packets may ignore an expired deadline. */
static int c5_zt_send_locked(struct c5_zt_nvm_permit *p, u16 reg,
		const u8 *data, u16 len, bool restore)
{
	u8 pkt[10];
	int ret;

	if (!c5_zt_permit_valid(p) || !c5_zt_purpose_allows(p, reg))
		return -EPERM;
	/* EEPROM status reads are needed by calibration; only clear-reference
	 * may write the EEPROM-info register. A zero-length packet selects a read. */
	if (reg == ZT7538_EEPROM_INFO_REG && len &&
		p->purpose != C5_ZT_NVM_CLEAR_REFERENCE)
		return -EPERM;
	if (len > 8 || (len && !data))
		return -EINVAL;
	if (restore) {
		if (len != 2 || data[0] || data[1] ||
			(reg != 0xc003 && reg != 0xc104))
			return -EPERM;
	} else if (time_after_eq(jiffies, p->deadline)) {
		return -ETIMEDOUT;
	}
	pkt[0] = reg & 0xff;
	pkt[1] = reg >> 8;
	if (len)
		memcpy(pkt + 2, data, len);
	if (reg == 0x01df || reg == ZT7538_WRITE_FLASH ||
		reg == ZT7538_SAVE_STATUS_CMD || reg == ZT7538_SAVE_CALIBRATION_CMD)
		p->nvm_may_have_changed = true; /* publication before ambiguous send */
	ret = i2c_master_send(p->info->client, pkt, len + 2);
	if (ret != len + 2)
		return ret < 0 ? ret : -EIO;
	usleep_range(DELAY_FOR_POST_TRANSCATION, DELAY_FOR_POST_TRANSCATION);
	return 0;
}

static int c5_zt_send(struct c5_zt_nvm_permit *p, u16 reg,
		const u8 *data, u16 len, bool restore)
{
	int ret;
	if (!p || !p->info)
		return -EPERM;
	mutex_lock(&p->info->c5_io_lock);
	ret = c5_zt_send_locked(p, reg, data, len, restore);
	mutex_unlock(&p->info->c5_io_lock);
	return ret;
}

static int c5_zt_reg(struct c5_zt_nvm_permit *p, u16 reg, u16 value)
{
	u8 data[2] = { value & 0xff, value >> 8 };
	return c5_zt_send(p, reg, data, sizeof(data), false);
}

static int c5_zt_cmd(struct c5_zt_nvm_permit *p, u16 reg)
{
	return c5_zt_send(p, reg, NULL, 0, false);
}

static int c5_zt_read(struct c5_zt_nvm_permit *p, u16 reg, u8 *data, u16 len)
{
	int ret;
	if (!data || !len || len > 8)
		return -EINVAL;
	if (!p || !p->info)
		return -EPERM;
	mutex_lock(&p->info->c5_io_lock);
	ret = c5_zt_send_locked(p, reg, NULL, 0, false);
	if (ret)
		goto out;
	if (reg == ZT7538_READ_FLASH)
		zinitix_delay(1);
	else
		usleep_range(DELAY_FOR_TRANSCATION, DELAY_FOR_TRANSCATION);
	ret = i2c_master_recv(p->info->client, data, len);
	ret = ret == len ? 0 : (ret < 0 ? ret : -EIO);
out:
	mutex_unlock(&p->info->c5_io_lock);
	return ret;
}

static int c5_zt_read16(struct c5_zt_nvm_permit *p, u16 reg, u16 *value)
{
	u8 data[2];
	int ret = c5_zt_read(p, reg, data, sizeof(data));
	if (!ret)
		*value = data[0] | (data[1] << 8);
	return ret;
}

static int c5_zt_protect(struct c5_zt_nvm_permit *p)
{
	const u8 zero[2] = { 0, 0 };
	int vpp, wp;
	vpp = c5_zt_send(p, 0xc003, zero, sizeof(zero), true);
	wp = c5_zt_send(p, 0xc104, zero, sizeof(zero), true);
	if (p && !p->restore_error && (vpp || wp))
		p->restore_error = vpp ? vpp : wp;
	return vpp ? vpp : wp;
}

/* This is an identity allowlist for the supplied C5 image, not a signature.
 * New approved firmware requires an independently reviewed contract update.
 */
static const u8 c5_zt_production_sha256[SHA256_DIGEST_SIZE] = {
		0xa6,0x15,0x8c,0xae,0x3a,0xc9,0x91,0x2e,
		0x52,0x0f,0x1c,0x9a,0x68,0xbf,0xcb,0x5a,
		0x0d,0x42,0x65,0x87,0xff,0x1a,0xe9,0xb8,
		0xeb,0xd7,0x4e,0xdd,0xce,0xdc,0x52,0x7d
	};

static int c5_zt_validate_firmware(struct zt7538_ts_info *info,
		const struct firmware *fw, const u8 expected[SHA256_DIGEST_SIZE])
{
	struct crypto_shash *tfm;
	struct shash_desc *desc;
	u8 digest[SHA256_DIGEST_SIZE];
	int ret;

	if (!expected || !info || !info->pdata || !info->pdata->fw_name ||
		strcmp(info->pdata->fw_name, "zt7538_c5.fw") ||
		info->chip_code != ZT7538_IC_CHIP_CODE ||
		info->cap_info.ic_fw_size != C5_ZT_FW_BYTES || !fw ||
		!fw->data || fw->size != C5_ZT_FW_BYTES)
		return -EINVAL;
	if (fw->data[48] || fw->data[49] ||
		fw->data[52] != 5 || fw->data[53] ||
		fw->data[56] != 11 || fw->data[57] ||
		fw->data[60] != 13 || fw->data[61])
		return -EINVAL;
	tfm = crypto_alloc_shash("sha256-generic", 0, 0);
	if (IS_ERR(tfm))
		return PTR_ERR(tfm);
	desc = kmalloc(sizeof(*desc) + crypto_shash_descsize(tfm), GFP_KERNEL);
	if (!desc) {
		crypto_free_shash(tfm);
		return -ENOMEM;
	}
	desc->tfm = tfm;
	desc->flags = 0;
	ret = crypto_shash_digest(desc, fw->data, fw->size, digest);
	if (!ret && memcmp(digest, expected, sizeof(digest)))
		ret = -EBADMSG;
	kfree(desc);
	crypto_free_shash(tfm);
	return ret;
}

static int c5_zt_program(struct c5_zt_nvm_permit *p,
		const u8 *firmware, u32 size, u8 *verify)
{
	u32 off;
	u16 chip;
	int ret, cleanup;
	bool protection_touched = false;
	if (!c5_zt_permit_valid(p))
		return -EPERM;
	if (!firmware || !verify || size != C5_ZT_FW_BYTES)
		return -EINVAL;
	if (!zt7538_power_control(p->info, POWER_OFF) ||
		!zt7538_power_control(p->info, POWER_ON))
		return -EIO;
	zinitix_delay(10);
	ret = c5_zt_reg(p, 0xc000, 1);
	if (ret)
		return ret;
	usleep_range(10, 10);
	ret = c5_zt_read16(p, 0xcc00, &chip);
	if (ret || chip != ZT7538_IC_CHIP_CODE)
		return ret ? ret : -ENODEV;
	ret = c5_zt_cmd(p, 0xc004);
	if (ret)
		return ret;
	usleep_range(10, 10);
	ret = c5_zt_reg(p, 0xc002, 1);
	if (ret)
		return ret;
	zinitix_delay(5);
	protection_touched = true; /* even an error reply may follow a partial send */
	ret = c5_zt_reg(p, 0xc003, 1);
	if (ret)
		goto out;
	ret = c5_zt_reg(p, 0xc104, 1);
	if (ret)
		goto out;
	ret = c5_zt_cmd(p, ZT7538_INIT_FLASH);
	if (ret)
		goto out;
	ret = c5_zt_cmd(p, 0x01df);
	if (ret)
		goto out;
	zinitix_delay(100);
	ret = c5_zt_reg(p, 0x01de, 1);
	if (ret)
		goto out;
	zinitix_delay(1);
	ret = c5_zt_reg(p, 0x01d3, 8);
	if (ret)
		goto out;
	for (off = 0; off < size; off += 8) {
		ret = c5_zt_send(p, ZT7538_WRITE_FLASH, firmware + off, 8, false);
		if (ret)
			goto out;
		usleep_range(100, 100);
		if ((off + 8) % 64 == 0)
			zinitix_delay(8);
	}
	ret = c5_zt_protect(p);
	if (ret)
		goto out;
	protection_touched = false;
	ret = c5_zt_cmd(p, ZT7538_INIT_FLASH);
	if (ret)
		goto out;
	for (off = 0; off < size; off += 8) {
		ret = c5_zt_read(p, ZT7538_READ_FLASH, verify + off, 8);
		if (ret)
			goto out;
	}
	if (memcmp(firmware, verify, size))
		ret = -EBADMSG;
out:
	if (protection_touched) {
		cleanup = c5_zt_protect(p);
		if (!ret)
			ret = cleanup;
	}
	return ret;
}

static int c5_zt_calibrate(struct c5_zt_nvm_permit *p)
{
	u16 eeprom = 1;
	int poll, ret;
	ret = c5_zt_reg(p, ZT7538_TOUCH_MODE, 7);
	if (ret)
		return ret;
	zinitix_delay(10);
	ret = c5_zt_cmd(p, ZT7538_CLEAR_INT_STATUS_CMD);
	if (ret)
		return ret;
	zinitix_delay(10);
	ret = c5_zt_cmd(p, ZT7538_CLEAR_INT_STATUS_CMD);
	if (ret)
		return ret;
	zinitix_delay(50);
	ret = c5_zt_cmd(p, ZT7538_CLEAR_INT_STATUS_CMD);
	if (ret)
		return ret;
	zinitix_delay(10);
	ret = c5_zt_cmd(p, ZT7538_CALIBRATE_CMD);
	if (ret)
		return ret;
	ret = c5_zt_cmd(p, ZT7538_CLEAR_INT_STATUS_CMD);
	if (ret)
		return ret;
	zinitix_delay(10);
	ret = c5_zt_cmd(p, ZT7538_CLEAR_INT_STATUS_CMD);
	if (ret)
		return ret;
	for (poll = 0; poll < 11; poll++) {
		zinitix_delay(500);
		ret = c5_zt_cmd(p, ZT7538_CLEAR_INT_STATUS_CMD);
		if (ret)
			return ret;
		ret = c5_zt_read16(p, ZT7538_EEPROM_INFO_REG, &eeprom);
		if (ret)
			return ret;
		if (!(eeprom & 1))
			break;
		if (poll == 4) {
			ret = c5_zt_cmd(p, ZT7538_CALIBRATE_CMD);
			if (ret)
				return ret;
			zinitix_delay(10);
			ret = c5_zt_cmd(p, ZT7538_CLEAR_INT_STATUS_CMD);
			if (ret)
				return ret;
		}
	}
	if (eeprom & 1)
		return -ETIMEDOUT; /* no SAVE and no success after timeout */
	ret = c5_zt_reg(p, ZT7538_TOUCH_MODE, TOUCH_POINT_MODE);
	if (ret)
		return ret;
	if (p->info->cap_info.ic_int_mask) {
		ret = c5_zt_reg(p, ZT7538_INT_ENABLE_FLAG, p->info->cap_info.ic_int_mask);
		if (ret)
			return ret;
	}
	usleep_range(100, 100);
	ret = c5_zt_cmd(p, ZT7538_SAVE_CALIBRATION_CMD);
	if (ret)
		return ret;
	zinitix_delay(1000);
	return 0;
}

static int c5_zt_versions_allowed(u16 major, u16 minor, u16 reg)
{
	if (major > 0xff || minor > 0xff || reg == 0xffff)
		return -ERANGE;
	if (major > 5 || (major == 5 && (minor > 11 || (minor == 11 && reg > 13))))
		return -EPERM;
	return 0;
}

static int c5_zt_resident_version_ok(struct c5_zt_nvm_permit *p)
{
	u16 major, minor, reg;
	int ret;
	ret = c5_zt_read16(p, ZT7538_FIRMWARE_VERSION, &major);
	if (!ret)
		ret = c5_zt_read16(p, ZT7538_MINOR_FW_VERSION, &minor);
	if (!ret)
		ret = c5_zt_read16(p, ZT7538_DATA_VERSION_REG, &reg);
	if (ret)
		return ret;
	return c5_zt_versions_allowed(major, minor, reg);
}

#if ESD_TIMER_INTERVAL
/* Timer callback never takes info->lock. No spin lock spans a sync wait. */
static void c5_zt_stop_esd(struct zt7538_ts_info *info)
{
	unsigned long flags;
	spin_lock_irqsave(&info->lock, flags);
	info->p_esd_timeout_tmr = NULL;
	spin_unlock_irqrestore(&info->lock, flags);
	/* The callback clears p_esd_timeout_tmr before queueing work. NULL
	 * therefore cannot prove it returned; always drain the stable timer.
	 */
	del_timer_sync(&info->esd_timeout_tmr);
}
#endif

/* Owned immutable input: never publish bytes in info->fw_data. */
struct c5_zt_payload {
	const struct firmware *firmware;
	void *file_data;
	struct firmware view;
};

static void c5_zt_payload_release(struct c5_zt_payload *payload)
{
	release_firmware(payload->firmware);
	vfree(payload->file_data);
	memset(payload, 0, sizeof(*payload));
}

static int c5_zt_payload_load(struct zt7538_ts_info *info,
		enum c5_zt_fw_source source, struct c5_zt_payload *payload)
{
	loff_t size = 0;
	int ret;
	memset(payload, 0, sizeof(*payload));
	switch (source) {
	case C5_ZT_FW_BUILTIN:
		ret = request_firmware(&payload->firmware,
			"tsp_zinitix/zt7538_c5.fw", &info->client->dev);
		break;
	case C5_ZT_FW_FFU:
		ret = request_firmware(&payload->firmware,
			ZINITIX_DEFAULT_FFU_FW, &info->client->dev);
		break;
	case C5_ZT_FW_FIXED_SD:
		ret = kernel_read_file_from_path(ZINITIX_DEFAULT_UMS_FW,
			&payload->file_data, &size, C5_ZT_FW_BYTES, READING_FIRMWARE);
		if (!ret && size != C5_ZT_FW_BYTES)
			ret = -EINVAL;
		if (!ret) {
			payload->view.data = payload->file_data;
			payload->view.size = size;
		}
		break;
	default:
		return -EINVAL;
	}
	if (!ret && payload->firmware)
		payload->view = *payload->firmware;
	return ret;
}

/* Different legacy frontends have different volatile restoration contracts. */
static int c5_zt_standalone(struct c5_zt_nvm_permit *permit, u16 old_mode,
		int *restore_error)
{
	int ret = 0, cleanup = 0, r, i;
	bool periodic_changed = false, protection_attempted = false;
	switch (permit->purpose) {
	case C5_ZT_NVM_HW_CALIBRATE:
		zinitix_delay(100); /* Independent ioctl admission delay in the old provider. */
		ret = c5_zt_calibrate(permit);
		/* The independent ioctl restores its prior mode and resets. */
		r = c5_zt_reg(permit, ZT7538_TOUCH_MODE, old_mode);
		if (r && !cleanup) cleanup = r;
		r = c5_zt_cmd(permit, ZT7538_SWRESET_CMD);
		if (r && !cleanup) cleanup = r;
		break;
	case C5_ZT_NVM_SAVE_STATUS:
		ret = c5_zt_cmd(permit, ZT7538_SAVE_STATUS_CMD);
		if (!ret) zinitix_delay(1000);
		break;
	case C5_ZT_NVM_REFERENCE_CALIBRATE:
	case C5_ZT_NVM_CLEAR_REFERENCE:
#if ESD_TIMER_INTERVAL
		periodic_changed = true;
		ret = c5_zt_reg(permit, ZT7538_PERIODICAL_INTERRUPT_INTERVAL, 0);
		if (ret) break;
#endif
		if (permit->purpose == C5_ZT_NVM_REFERENCE_CALIBRATE) {
			ret = c5_zt_calibrate(permit);
			for (i = 0; i < 5; i++) {
				r = c5_zt_cmd(permit, ZT7538_CLEAR_INT_STATUS_CMD);
				if (r && !cleanup) cleanup = r;
				usleep_range(10, 10);
			}
		} else {
			ret = c5_zt_reg(permit, ZT7538_EEPROM_INFO_REG, 0xffff);
			if (ret) break;
			protection_attempted = true; /* A short enable may have taken effect. */
			ret = c5_zt_reg(permit, 0xc003, 1);
			if (ret) break;
			ret = c5_zt_reg(permit, 0xc104, 1);
			if (ret) break;
			usleep_range(100, 100);
			ret = c5_zt_cmd(permit, ZT7538_SAVE_STATUS_CMD);
			if (!ret) zinitix_delay(500);
		}
		break;
	default:
		return -EINVAL;
	}
	if (protection_attempted) {
		r = c5_zt_protect(permit); /* Both exact safe-off writes, even after expiry. */
		if (r && !cleanup) cleanup = r;
	}
#if ESD_TIMER_INTERVAL
	if (periodic_changed) {
		r = c5_zt_reg(permit, ZT7538_PERIODICAL_INTERRUPT_INTERVAL,
			SCAN_RATE_HZ * ESD_TIMER_INTERVAL);
		if (r && !cleanup) cleanup = r;
	}
#endif
	*restore_error = cleanup;
	if (!ret && cleanup) ret = cleanup;
	if (!ret && time_after_eq(jiffies, permit->deadline)) ret = -ETIMEDOUT;
	return ret;
}

/* One shared admission/resource transaction for every live persistent frontend.
 * No physical I/O mutex is held across power/init/work callbacks.
 */
static int c5_zt_manual_operation_policy(struct zt7538_ts_info *info,
		enum c5_zt_nvm_purpose purpose, enum c5_zt_fw_source source,
		const u8 expected[SHA256_DIGEST_SIZE])
{
	struct c5_zt_payload payload = { 0 };
	struct wakeup_source *wake = NULL;
	struct c5_zt_nvm_permit permit = { 0 };
	u8 *verify = NULL;
	u16 checksum, version, minor, reg, old_mode;
	int ret, restore_error = 0;
	bool admitted = false, mutating = false;
	enum c5_zt_nvm_purpose armed;
#if ESD_TIMER_INTERVAL
	bool esd_was_running;
#endif
	if (!info || !info->input_dev) return -EINVAL;
	mutex_lock(&info->input_dev->mutex);
	mutex_lock(&info->c5_fw_lock);
	armed = info->c5_nvm_purpose;
	if (!READ_ONCE(info->c5_fw_armed)) { ret = -EPERM; goto out_unlock; }
	WRITE_ONCE(info->c5_fw_armed, false);
	info->c5_nvm_purpose = C5_ZT_NVM_NONE;
	if (!capable(CAP_SYS_RAWIO) || armed != purpose ||
		purpose <= C5_ZT_NVM_NONE || purpose > C5_ZT_NVM_CLEAR_REFERENCE) {
		ret = -EPERM; goto out_unlock;
	}
	ret = c5_zt_versions_allowed(info->cap_info.fw_version,
		info->cap_info.fw_minor_version, info->cap_info.reg_data_version);
	if (ret) goto out_unlock;
	if (!info->c5_fw_ready || info->c5_fw_removing || info->c5_fw_failed ||
		info->c5_fw_suspended || info->fw_data || !info->device_enabled) {
		ret = -EBUSY; goto out_unlock;
	}
	if (info->chip_code != 0xe538 || info->cap_info.ic_fw_size != C5_ZT_FW_BYTES) {
		ret = -ENODEV; goto out_unlock;
	}
	info->c5_fw_restore_error = 0;
	wake = wakeup_source_register("c5_zinitix_update");
	if (!wake) { ret = -ENOMEM; goto out_unlock; }
	__pm_stay_awake(wake);
	if (purpose == C5_ZT_NVM_FIRMWARE) {
		ret = c5_zt_payload_load(info, source, &payload);
		if (ret) goto out_firmware;
		ret = c5_zt_validate_firmware(info, &payload.view, expected);
		if (ret) goto out_firmware;
		verify = kmalloc(C5_ZT_FW_BYTES, GFP_KERNEL);
		if (!verify) { ret = -ENOMEM; goto out_firmware; }
	}
	mutex_lock(&info->c5_io_lock);
	if (info->c5_io_owner) {
		mutex_unlock(&info->c5_io_lock); ret = -EBUSY; goto out_buffer;
	}
	WRITE_ONCE(info->c5_io_owner, current);
	WRITE_ONCE(info->c5_fw_busy, true);
	mutex_unlock(&info->c5_io_lock);
	cancel_delayed_work_sync(&info->reset_work);
#if ESD_TIMER_INTERVAL
	esd_was_running = READ_ONCE(info->p_esd_timeout_tmr) != NULL;
	c5_zt_stop_esd(info);
	cancel_work_sync(&info->tmr_work);
#endif
	disable_irq(info->irq);
	down(&info->work_lock);
	if (info->work_state != NOTHING || !info->device_enabled || info->is_lpm_suspend) {
		ret = -EBUSY; goto out_admission;
	}
#ifdef CONFIG_SECURE_TOUCH
	if (atomic_read(&info->secure_enabled) == SECURE_TOUCH_ENABLED) {
		ret = -EBUSY; goto out_admission;
	}
#endif
	old_mode = info->touch_mode;
	info->work_state = purpose == C5_ZT_NVM_FIRMWARE ? UPGRADE :
		(purpose == C5_ZT_NVM_HW_CALIBRATE ? HW_CALIBRAION : SET_MODE);
	admitted = true;
	clear_report_data(info);
	permit.info = info; permit.owner = current; permit.active = true;
	permit.purpose = purpose;
	permit.deadline = jiffies + msecs_to_jiffies(C5_ZT_DEADLINE_MS);
	ret = c5_zt_resident_version_ok(&permit);
	if (!ret) {
		mutating = true;
		if (purpose == C5_ZT_NVM_FIRMWARE) {
			ret = c5_zt_program(&permit, payload.view.data, payload.view.size, verify);
			if (!ret && (!zt7538_power_control(info, POWER_OFF) ||
				!zt7538_power_control(info, POWER_ON_SEQUENCE))) ret = -EIO;
			if (!ret) ret = c5_zt_read16(&permit, ZT7538_CHECKSUM_RESULT, &checksum);
			if (!ret && checksum != 0x55aa) ret = -EBADMSG;
			if (!ret) ret = c5_zt_calibrate(&permit);
			if (!ret) ret = c5_zt_read16(&permit, ZT7538_CHECKSUM_RESULT, &checksum);
			if (!ret) ret = c5_zt_read16(&permit, ZT7538_FIRMWARE_VERSION, &version);
			if (!ret) ret = c5_zt_read16(&permit, ZT7538_MINOR_FW_VERSION, &minor);
			if (!ret) ret = c5_zt_read16(&permit, ZT7538_DATA_VERSION_REG, &reg);
			if (!ret && (checksum != 0x55aa || version != 5 || minor != 11 || reg != 13)) ret = -EBADMSG;
			if (!ret && !init_touch(info, true)) ret = -EIO;
		} else {
			ret = c5_zt_standalone(&permit, old_mode, &restore_error);
		}
	}
	if (!restore_error)
		restore_error = permit.restore_error;
	permit.active = false;
	if (ret && mutating) {
		if (!zt7538_power_control(info, POWER_OFF) && !restore_error)
			restore_error = -EIO;
		info->device_enabled = 0; info->c5_fw_failed = true;
		info->c5_fw_uncertain = permit.nvm_may_have_changed;
		info->c5_fw_holds_irq = true;
	}
	info->work_state = NOTHING;
out_admission:
	up(&info->work_lock);
	mutex_lock(&info->c5_io_lock);
	WRITE_ONCE(info->c5_io_owner, NULL); WRITE_ONCE(info->c5_fw_busy, false);
	mutex_unlock(&info->c5_io_lock);
	if (!mutating || !ret) {
		enable_irq(info->irq);
#if ESD_TIMER_INTERVAL
		if (admitted || esd_was_running) esd_timer_start(CHECK_ESD_TIMER, info);
#endif
	}
out_buffer:
	kfree(verify);
out_firmware:
	c5_zt_payload_release(&payload);
	if (wake) { __pm_relax(wake); wakeup_source_unregister(wake); }
out_unlock:
	info->c5_fw_last_error = ret;
	info->c5_fw_restore_error = restore_error;
	mutex_unlock(&info->c5_fw_lock);
	mutex_unlock(&info->input_dev->mutex);
	return ret;
}

static int c5_zinitix_manual_update_policy(struct zt7538_ts_info *info,
		const u8 expected[SHA256_DIGEST_SIZE])
{
	return c5_zt_manual_operation_policy(info, C5_ZT_NVM_FIRMWARE,
		C5_ZT_FW_BUILTIN, expected);
}

static int __used c5_zinitix_manual_update(struct zt7538_ts_info *info)
{
	return c5_zinitix_manual_update_policy(info, c5_zt_production_sha256);
}

static int c5_zt_execute_frontend(struct zt7538_ts_info *info,
		enum c5_zt_nvm_purpose purpose, enum c5_zt_fw_source source)
{
#if IS_ENABLED(CONFIG_C5_CONTROLLER_FW_WRITE_CONTROL)
	return c5_zt_manual_operation_policy(info, purpose, source, c5_zt_production_sha256);
#else
	return -EPERM;
#endif
}

#if IS_ENABLED(CONFIG_C5_CONTROLLER_FW_WRITE_CONTROL)
static int c5_zt_arm(struct zt7538_ts_info *info, enum c5_zt_nvm_purpose purpose)
{
	int ret = 0;
	mutex_lock(&info->c5_fw_lock);
	WRITE_ONCE(info->c5_fw_armed, false); info->c5_nvm_purpose = C5_ZT_NVM_NONE;
	if (!capable(CAP_SYS_RAWIO)) ret = -EPERM;
	else if (purpose < C5_ZT_NVM_NONE || purpose > C5_ZT_NVM_CLEAR_REFERENCE) ret = -EINVAL;
	else if (purpose != C5_ZT_NVM_NONE && (!info->c5_fw_ready || info->c5_fw_removing ||
		info->c5_fw_failed || info->c5_fw_suspended || info->c5_fw_busy || !info->device_enabled)) ret = -EBUSY;
	else { info->c5_nvm_purpose = purpose; WRITE_ONCE(info->c5_fw_armed, purpose != C5_ZT_NVM_NONE); }
	mutex_unlock(&info->c5_fw_lock);
	return ret;
}

static ssize_t c5_fw_arm_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct zt7538_ts_info *info = dev_get_drvdata(dev); ssize_t ret;
	mutex_lock(&info->c5_fw_lock); ret = scnprintf(buf, PAGE_SIZE, "%u\n", info->c5_fw_armed); mutex_unlock(&info->c5_fw_lock); return ret;
}
static ssize_t c5_fw_arm_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct zt7538_ts_info *info = dev_get_drvdata(dev); int ret;
	ret = c5_zt_arm(info, sysfs_streq(buf, "0") ? C5_ZT_NVM_NONE :
		(sysfs_streq(buf, "1") ? C5_ZT_NVM_FIRMWARE : -1));
	return ret ? ret : count;
}
static ssize_t c5_nvm_arm_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct zt7538_ts_info *info = dev_get_drvdata(dev); ssize_t ret;
	mutex_lock(&info->c5_fw_lock); ret = scnprintf(buf, PAGE_SIZE, "%u\n", info->c5_nvm_purpose); mutex_unlock(&info->c5_fw_lock); return ret;
}
static ssize_t c5_nvm_arm_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct zt7538_ts_info *info = dev_get_drvdata(dev); int purpose = -1, ret;
	if (sysfs_streq(buf, "0")) purpose = C5_ZT_NVM_NONE;
	else if (sysfs_streq(buf, "firmware")) purpose = C5_ZT_NVM_FIRMWARE;
	else if (sysfs_streq(buf, "hw-calibrate")) purpose = C5_ZT_NVM_HW_CALIBRATE;
	else if (sysfs_streq(buf, "save-status")) purpose = C5_ZT_NVM_SAVE_STATUS;
	else if (sysfs_streq(buf, "ref-calibrate")) purpose = C5_ZT_NVM_REFERENCE_CALIBRATE;
	else if (sysfs_streq(buf, "clear-reference")) purpose = C5_ZT_NVM_CLEAR_REFERENCE;
	ret = c5_zt_arm(info, purpose); return ret ? ret : count;
}
static ssize_t c5_fw_update_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct zt7538_ts_info *info = dev_get_drvdata(dev); int ret;
	if (!sysfs_streq(buf, "1")) {
		c5_zt_arm(info, -1); return -EINVAL;
	}
	ret = c5_zinitix_manual_update(info); return ret ? ret : count;
}
#endif

static ssize_t c5_fw_status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct zt7538_ts_info *info = dev_get_drvdata(dev); ssize_t ret;
	mutex_lock(&info->c5_fw_lock);
	ret = scnprintf(buf, PAGE_SIZE,
		"chip=%04x fw=%u minor=%u reg=%u armed=%u busy=%u ready=%u quarantined=%u uncertain=%u last_errno=%d purpose=%u restore_errno=%d\n",
		READ_ONCE(info->chip_code), READ_ONCE(info->cap_info.fw_version),
		READ_ONCE(info->cap_info.fw_minor_version), READ_ONCE(info->cap_info.reg_data_version),
		info->c5_fw_armed, info->c5_fw_busy, info->c5_fw_ready, info->c5_fw_failed,
		info->c5_fw_uncertain, info->c5_fw_last_error, info->c5_nvm_purpose, info->c5_fw_restore_error);
	mutex_unlock(&info->c5_fw_lock); return ret;
}
#if IS_ENABLED(CONFIG_C5_CONTROLLER_FW_WRITE_CONTROL)
static DEVICE_ATTR(c5_fw_arm, 0600, c5_fw_arm_show, c5_fw_arm_store);
static DEVICE_ATTR(c5_nvm_arm, 0600, c5_nvm_arm_show, c5_nvm_arm_store);
static DEVICE_ATTR(c5_fw_update, 0200, NULL, c5_fw_update_store);
#endif
static DEVICE_ATTR(c5_fw_status, 0400, c5_fw_status_show, NULL);
static struct attribute *c5_zt_fw_attrs[] = {
#if IS_ENABLED(CONFIG_C5_CONTROLLER_FW_WRITE_CONTROL)
	&dev_attr_c5_fw_arm.attr, &dev_attr_c5_nvm_arm.attr, &dev_attr_c5_fw_update.attr,
#endif
	&dev_attr_c5_fw_status.attr, NULL
};
static const struct attribute_group c5_zt_fw_group = { .attrs = c5_zt_fw_attrs };
static void c5_zt_controls_publish(struct zt7538_ts_info *info)
{
	mutex_lock(&info->c5_fw_lock); info->c5_fw_ready = true; mutex_unlock(&info->c5_fw_lock);
	if (!sysfs_create_group(&info->client->dev.kobj, &c5_zt_fw_group)) info->c5_fw_controls_registered = true;
	else dev_warn(&info->client->dev, "C5 firmware status/control group unavailable\n");
}
static void c5_zt_controls_remove(struct zt7538_ts_info *info)
{
	mutex_lock(&info->c5_fw_lock);
	info->c5_fw_ready = false; info->c5_fw_removing = true;
	WRITE_ONCE(info->c5_fw_armed, false); info->c5_nvm_purpose = C5_ZT_NVM_NONE;
	mutex_unlock(&info->c5_fw_lock);
	if (info->c5_fw_controls_registered) {
		sysfs_remove_group(&info->client->dev.kobj, &c5_zt_fw_group);
		info->c5_fw_controls_registered = false;
	}
}
#endif /* CONFIG_C5_CONTROLLER_FW_UPDATE */
