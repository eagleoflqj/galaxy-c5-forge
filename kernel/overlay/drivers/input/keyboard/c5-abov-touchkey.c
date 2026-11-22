// SPDX-License-Identifier: GPL-2.0-or-later
/* C5 FT1604 runtime port, based on Samsung's abov_touchkey_16xx.c
 * Copyright (C) 2013 Samsung Electronics Co.Ltd
 * Original author: Junkyeong Kim <jk0430.kim@samsung.com>
 * Runtime input path plus explicitly armed, manual C5-only firmware update.
 * No probe, resume, reset or factory automatic firmware write path.
 */
#include <crypto/hash.h>
#include <linux/err.h>
#include <linux/capability.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/fs.h>
#include <linux/jiffies.h>
#include <linux/fb.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_gpio.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pm_wakeup.h>
#include <linux/regulator/consumer.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#define C5_RECENT 254 /* Existing sec_touchkey.kl: APP_SWITCH */
#define C5_STATUS 0x00
#define C5_VERSION 0x01
#define C5_LED_ON 0x10
#define C5_LED_OFF 0x20
#define C5_MODEL 0x14
#define C5_FW_NAME "abov/abov_ft1604_c5.fw"
#define C5_FW_MODEL 0xa8
#define C5_FW_VERSION 0x05
#define C5_FW_SDCARD "/sdcard/Firmware/TOUCHKEY/abov_fw.bin"
#define C5_FW_HEADER 32
#define C5_FW_BLOCK 32
#define C5_FW_PAYLOAD 0x3000
#define C5_FW_SIZE (C5_FW_HEADER + C5_FW_PAYLOAD)
/* Fixed old delays total about 3.3 s; allow bounded busy waits up to 15 s. */
#define C5_FW_TIMEOUT_MS 15000
#define C5_FW_BUSY_POLLS 1000

extern struct class *sec_class;

struct c5_touchkey {
	struct i2c_client *client;
	struct input_dev *input;
	struct device *sec;
	struct regulator *vdd;
	struct pinctrl *pinctrl;
	struct pinctrl_state *active, *sleep;
	struct notifier_block fb;
	struct mutex state_lock, io_lock;
	int en_gpio;
	bool powered, enabled, opened, blanked, led;
	/* Only explicit sysfs update consumes arm; runtime paths never create a permit. */
	bool fw_armed, fw_uncertain, fw_stopping;
	int fw_state, fw_error, fw_restore_error;
	u8 fw_version, fw_model;
};

/* The accepted controller uses STOP-separated register-select and read. */
static int c5_read(struct c5_touchkey *tk, u8 reg, u8 *value)
{
	struct i2c_msg msg = { .addr = tk->client->addr, .len = 1, .buf = &reg };
	int ret;

	mutex_lock(&tk->io_lock);
	ret = i2c_transfer(tk->client->adapter, &msg, 1);
	if (ret == 1) {
		msg.flags = I2C_M_RD;
		msg.buf = value;
		ret = i2c_transfer(tk->client->adapter, &msg, 1);
	}
	mutex_unlock(&tk->io_lock);
	return ret == 1 ? 0 : (ret < 0 ? ret : -EIO);
}

static int c5_led_write(struct c5_touchkey *tk)
{
	u8 data[] = { C5_STATUS, tk->led ? C5_LED_ON : C5_LED_OFF };
	struct i2c_msg msg = { .addr = tk->client->addr, .len = sizeof(data), .buf = data };
	int ret;

	mutex_lock(&tk->io_lock);
	ret = i2c_transfer(tk->client->adapter, &msg, 1);
	mutex_unlock(&tk->io_lock);
	return ret == 1 ? 0 : (ret < 0 ? ret : -EIO);
}

/* Native dual-key encoding: zero=no event, even=press, odd=release. */
static void c5_report(struct input_dev *input, u8 status)
{
	u8 recent = status & 3;
	u8 back = (status >> 2) & 3;

	if (recent)
		input_report_key(input, C5_RECENT, !(recent & 1));
	if (back)
		input_report_key(input, KEY_BACK, !(back & 1));
	input_sync(input);
}

static void c5_release(struct c5_touchkey *tk)
{
	input_report_key(tk->input, C5_RECENT, 0);
	input_report_key(tk->input, KEY_BACK, 0);
	input_sync(tk->input);
}

static irqreturn_t c5_irq(int irq, void *data)
{
	struct c5_touchkey *tk = data;
	u8 status;
	int ret;

	if (!READ_ONCE(tk->enabled))
		return IRQ_HANDLED;
	ret = c5_read(tk, C5_STATUS, &status);
	if (ret) {
		dev_err_ratelimited(&tk->client->dev, "touchkey status read: %d\n", ret);
		c5_release(tk);
		return IRQ_HANDLED;
	}
	c5_report(tk->input, status);
	return IRQ_HANDLED;
}

/* state_lock serializes power, native input open/close and FB transitions.
 * IRQ takes only io_lock: never hold io_lock while disabling/synchronizing IRQ.
 */
static void c5_power_off(void *data)
{
	struct c5_touchkey *tk = data;
	int ret;

	gpio_set_value_cansleep(tk->en_gpio, 0);
	if (tk->powered) {
		ret = regulator_disable(tk->vdd);
		if (ret)
			dev_err(&tk->client->dev, "touchkey power off: %d\n", ret);
		else
			tk->powered = false;
	}
}

static int c5_power_on(struct c5_touchkey *tk)
{
	int ret;

	ret = pinctrl_select_state(tk->pinctrl, tk->active);
	if (ret)
		return ret;
	if (!tk->powered) {
		ret = regulator_enable(tk->vdd);
		if (ret)
			return ret;
		tk->powered = true;
	}
	gpio_set_value_cansleep(tk->en_gpio, 1);
	msleep(150); /* Accepted ABOV_RESET_DELAY, no bootloader/update command. */
	return 0;
}

static int c5_apply(struct c5_touchkey *tk)
{
	bool want = tk->opened && !tk->blanked;
	int ret;

	if (want && tk->fw_uncertain)
		return -EIO;
	if (want == tk->enabled)
		return 0;
	if (want) {
		ret = c5_power_on(tk);
		if (ret)
			return ret;
		ret = c5_led_write(tk);
		if (ret) {
			c5_power_off(tk);
			return ret;
		}
		WRITE_ONCE(tk->enabled, true);
		enable_irq(tk->client->irq);
	} else {
		WRITE_ONCE(tk->enabled, false);
		disable_irq(tk->client->irq);
		c5_release(tk);
		c5_power_off(tk);
		ret = pinctrl_select_state(tk->pinctrl, tk->sleep);
		if (ret)
			return ret;
	}
	return 0;
}

static int c5_open(struct input_dev *input)
{
	struct c5_touchkey *tk = input_get_drvdata(input);
	int ret;

	mutex_lock(&tk->state_lock);
	tk->opened = true;
	ret = c5_apply(tk);
	if (ret)
		tk->opened = false;
	mutex_unlock(&tk->state_lock);
	return ret;
}

static void c5_close(struct input_dev *input)
{
	struct c5_touchkey *tk = input_get_drvdata(input);

	mutex_lock(&tk->state_lock);
	tk->opened = false;
	c5_apply(tk);
	mutex_unlock(&tk->state_lock);
}

static int c5_fb(struct notifier_block *nb, unsigned long event, void *data)
{
	struct c5_touchkey *tk = container_of(nb, struct c5_touchkey, fb);
	struct fb_event *ev = data;
	int ret;

	if (event != FB_EVENT_BLANK || !ev || !ev->data)
		return NOTIFY_DONE;
	mutex_lock(&tk->state_lock);
	tk->blanked = *(int *)ev->data != FB_BLANK_UNBLANK;
	ret = c5_apply(tk);
	mutex_unlock(&tk->state_lock);
	if (ret)
		dev_err(&tk->client->dev, "touchkey FB transition: %d\n", ret);
	return NOTIFY_OK;
}

static ssize_t brightness_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct c5_touchkey *tk = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%u\n", READ_ONCE(tk->led));
}

static ssize_t brightness_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct c5_touchkey *tk = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	ret = kstrtouint(buf, 10, &value);
	if (ret || value > 1)
		return -EINVAL;
	mutex_lock(&tk->state_lock);
	tk->led = value;
	ret = tk->enabled ? c5_led_write(tk) : 0;
	mutex_unlock(&tk->state_lock);
	return ret ? ret : count;
}
static DEVICE_ATTR(brightness, 0664, brightness_show, brightness_store);

#ifdef CONFIG_C5_CONTROLLER_FW_UPDATE
/* C5 FT1604 image format is fixed by the selected old image and driver.
 * Header byte 1 is module, byte 5 is version, bytes 8/9 are checksum BE.
 * The header is not programmed. The 384 payload blocks cover 0x1000..0x3fff.
 * Full-image SHA256 and exact C5 header fields are mandatory.
 * The additive precheck corroborates that single selected image, not a generic format.
 */
struct c5_fw_image {
	const u8 *payload;
	u8 model, version;
	u16 checksum;
};

/* Stack-owned capability: only the explicit, armed update entry constructs it.
 * It is passed to every loader transfer and invalidated before releasing io_lock.
 */
struct c5_fw_txn {
	struct c5_touchkey *tk;
	struct task_struct *owner;
	unsigned long deadline;
	bool active, enter_attempted;
};

enum { C5_FW_IDLE, C5_FW_RUNNING, C5_FW_PASS, C5_FW_FAIL };
enum c5_fw_source { C5_FW_BUILTIN, C5_FW_FIXED_SD };

/* Each request owns only one immutable local image. Never publish loader data
 * in controller state or substitute the builtin image for an I/i request.
 */
struct c5_fw_blob {
	const struct firmware *builtin;
	void *file_data;
	struct firmware image;
};

static int c5_fw_load(struct c5_touchkey *tk, enum c5_fw_source source,
		struct c5_fw_blob *blob)
{
	struct file *file;
	loff_t size = 0;
	int ret;

	if (source == C5_FW_BUILTIN) {
		ret = request_firmware_direct(&blob->builtin, C5_FW_NAME,
				&tk->client->dev);
		if (ret)
			return ret;
		blob->image.size = blob->builtin->size;
		blob->image.data = blob->builtin->data;
		return 0;
	}
	if (source != C5_FW_FIXED_SD)
		return -EINVAL;
	file = filp_open(C5_FW_SDCARD, O_RDONLY, 0);
	if (IS_ERR(file))
		return PTR_ERR(file);
	if (!S_ISREG(file_inode(file)->i_mode) ||
	    i_size_read(file_inode(file)) != C5_FW_SIZE) {
		ret = -EINVAL;
		goto close;
	}
	/* Selected 4.9 helper enforces regular-file, LSM and deny_write_access
	 * admission, bounds allocation, and frees/NULLs its buffer on failure.
	 */
	ret = kernel_read_file(file, &blob->file_data, &size, C5_FW_SIZE,
			READING_FIRMWARE);
	if (!ret && size != C5_FW_SIZE)
		ret = -EINVAL;
	if (!ret) {
		blob->image.size = size;
		blob->image.data = blob->file_data;
	}
close:
	filp_close(file, NULL);
	return ret;
}

static void c5_fw_put(struct c5_fw_blob *blob)
{
	release_firmware(blob->builtin);
	vfree(blob->file_data);
	blob->builtin = NULL;
	blob->file_data = NULL;
}

static int c5_fw_digest(const struct firmware *fw, u8 *digest)
{
	struct crypto_shash *tfm;
	struct shash_desc *desc;
	int ret;

	tfm = crypto_alloc_shash("sha256", 0, 0);
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
	kfree(desc);
	crypto_free_shash(tfm);
	return ret;
}

static int c5_fw_validate(const struct firmware *fw, struct c5_fw_image *image)
{
	static const u8 expected_digest[32] = {
		0xdc, 0x4c, 0x04, 0x63, 0xce, 0x1a, 0xc1, 0x4c,
		0x71, 0xb3, 0xfd, 0x3f, 0x91, 0xdf, 0x02, 0x69,
		0x62, 0x99, 0x32, 0xae, 0x02, 0xe2, 0x0c, 0xde,
		0x91, 0x8e, 0xb3, 0x63, 0xc6, 0x89, 0xc0, 0xcc,
	};
	u8 digest[32];
	unsigned int i;
	u16 checksum = 0;
	int ret;

	if (!fw || !fw->data || fw->size != C5_FW_SIZE)
		return -EINVAL;
	if (fw->data[1] != C5_FW_MODEL || fw->data[5] != C5_FW_VERSION ||
	    fw->data[8] != 0xa1 || fw->data[9] != 0x68)
		return -EINVAL;
	ret = c5_fw_digest(fw, digest);
	if (ret)
		return ret;
	if (memcmp(digest, expected_digest, sizeof(digest)))
		return -EBADMSG;
	for (i = C5_FW_HEADER; i < C5_FW_SIZE; i++)
		checksum += fw->data[i];
	if (checksum != ((u16)fw->data[8] << 8 | fw->data[9]))
		return -EBADMSG;
	image->payload = fw->data + C5_FW_HEADER;
	image->model = fw->data[1];
	image->version = fw->data[5];
	image->checksum = checksum;
	return 0;
}

static int c5_fw_permit(struct c5_fw_txn *txn)
{
	if (!txn || !txn->active || !txn->tk || txn->owner != current)
		return -EPERM;
	return time_after_eq(jiffies, txn->deadline) ? -ETIMEDOUT : 0;
}

static int c5_fw_send(struct c5_fw_txn *txn, const u8 *data, unsigned int len)
{
	int ret = c5_fw_permit(txn);

	if (ret)
		return ret;
	ret = i2c_master_send(txn->tk->client, data, len);
	if (ret != (int)len)
		return ret < 0 ? ret : -EIO;
	return c5_fw_permit(txn);
}

static int c5_fw_recv(struct c5_fw_txn *txn, u8 *data, unsigned int len)
{
	int ret = c5_fw_permit(txn);

	if (ret)
		return ret;
	ret = i2c_master_recv(txn->tk->client, data, len);
	if (ret != (int)len)
		return ret < 0 ? ret : -EIO;
	return c5_fw_permit(txn);
}

static int c5_fw_delay(struct c5_fw_txn *txn, unsigned int ms)
{
	int ret = c5_fw_permit(txn);

	if (ret)
		return ret;
	msleep(ms);
	return c5_fw_permit(txn);
}

static int c5_fw_busy(struct c5_fw_txn *txn)
{
	unsigned int i;
	u8 value;
	int ret;

	for (i = 0; i < C5_FW_BUSY_POLLS; i++) {
		ret = c5_fw_recv(txn, &value, 1);
		if (ret)
			return ret;
		if (!value)
			return 0;
		ret = c5_fw_delay(txn, 1);
		if (ret)
			return ret;
	}
	return -ETIMEDOUT;
}

static int c5_fw_program(struct c5_fw_txn *txn, const struct c5_fw_image *image)
{
	const u8 enter[] = { 0xac, 0x5b, 0x2d };
	const u8 checksum_cmd[] = { 0xac, 0x9e, 0x10, 0x00, 0x3f, 0xff };
	u8 packet[36] = { 0xac, 0x7a }, checksum[5], reg = 0;
	unsigned int offset, address;
	int ret;

	ret = c5_fw_permit(txn);
	if (ret)
		return ret;
	gpio_set_value_cansleep(txn->tk->en_gpio, 0);
	ret = c5_fw_delay(txn, 30);
	if (ret)
		return ret;
	gpio_set_value_cansleep(txn->tk->en_gpio, 1);
	ret = c5_fw_delay(txn, 45);
	if (ret)
		return ret;
	/* Entry may erase: even a failed/short entry transfer makes state uncertain.
	 * The old driver defines no separate erase or bootloader identity command.
	 */
	txn->enter_attempted = true;
	ret = c5_fw_send(txn, enter, sizeof(enter));
	if (ret)
		return ret;
	ret = c5_fw_delay(txn, 1100);
	if (ret)
		return ret;
	for (offset = 0; offset < C5_FW_PAYLOAD; offset += C5_FW_BLOCK) {
		address = 0x1000 + offset;
		packet[2] = address >> 8;
		packet[3] = address;
		memcpy(packet + 4, image->payload + offset, C5_FW_BLOCK);
		ret = c5_fw_send(txn, packet, sizeof(packet));
		if (ret)
			return ret;
		ret = c5_fw_delay(txn, 2);
		if (ret)
			return ret;
		ret = c5_fw_busy(txn);
		if (ret)
			return ret;
		ret = c5_fw_delay(txn, 3);
		if (ret)
			return ret;
		ret = c5_fw_busy(txn);
		if (ret)
			return ret;
	}
	ret = c5_fw_send(txn, checksum_cmd, sizeof(checksum_cmd));
	if (ret)
		return ret;
	ret = c5_fw_delay(txn, 5);
	if (ret)
		return ret;
	ret = c5_fw_busy(txn);
	if (ret)
		return ret;
	/* Preserve the selected STOP-separated register-select/read convention. */
	ret = c5_fw_send(txn, &reg, 1);
	if (ret)
		return ret;
	ret = c5_fw_recv(txn, checksum, sizeof(checksum));
	if (ret)
		return ret;
	return (((u16)checksum[3] << 8) | checksum[4]) == image->checksum ? 0 : -EBADMSG;
}

/* state_lock is held. No runtime entry calls this function. */
static int __maybe_unused c5_fw_update_locked(struct c5_touchkey *tk,
		enum c5_fw_source source)
{
	struct c5_fw_blob blob = { 0 };
	struct wakeup_source *wake = NULL;
	struct c5_fw_image image;
	struct c5_fw_txn txn = { .tk = tk, .owner = current };
	const char *name;
	bool was_enabled = tk->enabled, was_powered = tk->powered;
	u8 model, version;
	int ret, restore = 0;

	if (tk->fw_stopping) {
		tk->fw_armed = false;
		return -ESHUTDOWN;
	}
	if (!tk->fw_armed)
		return -EPERM;
	tk->fw_armed = false;
	tk->fw_restore_error = 0;
	WRITE_ONCE(tk->fw_state, C5_FW_RUNNING);
	ret = of_property_read_string(tk->client->dev.of_node, "abov,firmware_name", &name);
	if (ret || strcmp(name, C5_FW_NAME)) {
		ret = -ENODEV;
		goto out;
	}
	wake = wakeup_source_register("c5_abov_update");
	if (!wake) {
		ret = -ENOMEM;
		goto out;
	}
	__pm_stay_awake(wake);
	ret = c5_fw_load(tk, source, &blob);
	if (ret)
		goto out;
	ret = c5_fw_validate(&blob.image, &image);
	if (ret)
		goto out;
	if (was_enabled) {
		WRITE_ONCE(tk->enabled, false);
		disable_irq(tk->client->irq);
		c5_release(tk);
	}
	ret = c5_power_on(tk);
	if (ret)
		goto restore;
	ret = c5_read(tk, C5_MODEL, &model);
	if (ret)
		goto restore;
	ret = c5_read(tk, C5_VERSION, &version);
	if (ret)
		goto restore;
	if (model != image.model || !version || version == 0xff) {
		ret = -ENODEV;
		goto restore;
	}
	/* Explicit S requests only this exact known image, including same-version
	 * reflash. Never downgrade an unreviewed newer resident version.
	 */
	if (version > image.version) {
		ret = -EPERM;
		goto restore;
	}
	mutex_lock(&tk->io_lock);
	txn.deadline = jiffies + msecs_to_jiffies(C5_FW_TIMEOUT_MS);
	txn.active = true;
	ret = c5_fw_program(&txn, &image);
	txn.active = false;
	/* Reset is also attempted on partial entry/program failure, without retrying NVM. */
	gpio_set_value_cansleep(tk->en_gpio, 0);
	msleep(30);
	gpio_set_value_cansleep(tk->en_gpio, 1);
	msleep(150);
	mutex_unlock(&tk->io_lock);
	if (!ret) {
		ret = c5_read(tk, C5_MODEL, &model);
		if (!ret)
			ret = c5_read(tk, C5_VERSION, &version);
		if (!ret && (model != image.model || version != image.version))
			ret = -EBADMSG;
	}
	if (!ret) {
		tk->fw_uncertain = false;
		tk->fw_model = model;
		tk->fw_version = version;
	} else if (txn.enter_attempted) {
		tk->fw_uncertain = true;
	}
restore:
	if (tk->fw_uncertain) {
		WRITE_ONCE(tk->enabled, false);
		c5_power_off(tk);
		restore = pinctrl_select_state(tk->pinctrl, tk->sleep);
	} else if (was_enabled) {
		restore = c5_apply(tk);
	} else if (!was_powered) {
		c5_power_off(tk);
		restore = pinctrl_select_state(tk->pinctrl, tk->sleep);
	}
	if (!was_powered && tk->powered && !restore)
		restore = -EIO;
	tk->fw_restore_error = restore;
	if (!ret && restore)
		ret = restore;
	if (!ret && was_enabled && !tk->enabled)
		ret = -EIO;
out:
	tk->fw_armed = false;
	tk->fw_error = ret;
	WRITE_ONCE(tk->fw_state, ret ? C5_FW_FAIL : C5_FW_PASS);
	c5_fw_put(&blob);
	if (wake) {
		__pm_relax(wake);
		wakeup_source_unregister(wake);
	}
	return ret;
}

#ifdef CONFIG_C5_CONTROLLER_FW_WRITE_CONTROL
static ssize_t touchkey_firm_update_arm_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct c5_touchkey *tk = dev_get_drvdata(dev);
	bool armed;

	mutex_lock(&tk->state_lock);
	armed = tk->fw_armed;
	mutex_unlock(&tk->state_lock);
	return scnprintf(buf, PAGE_SIZE, "%u\n", armed);
}

static ssize_t touchkey_firm_update_arm_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct c5_touchkey *tk = dev_get_drvdata(dev);
	unsigned int value;
	int ret;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	ret = kstrtouint(buf, 10, &value);
	mutex_lock(&tk->state_lock);
	if (tk->fw_stopping) {
		tk->fw_armed = false;
		mutex_unlock(&tk->state_lock);
		return -ESHUTDOWN;
	}
	tk->fw_armed = !ret && value == 1;
	mutex_unlock(&tk->state_lock);
	return ret || value > 1 ? -EINVAL : count;
}

static ssize_t touchkey_firm_update_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct c5_touchkey *tk = dev_get_drvdata(dev);
	bool armed, transaction = false;
	int ret;

	mutex_lock(&tk->state_lock);
	armed = tk->fw_armed;
	if (tk->fw_stopping) {
		tk->fw_armed = false;
		ret = -ESHUTDOWN;
	} else if (!capable(CAP_SYS_RAWIO) || !armed) {
		tk->fw_armed = false;
		ret = -EPERM;
	} else if (sysfs_streq(buf, "s") || sysfs_streq(buf, "S")) {
		transaction = true;
		ret = c5_fw_update_locked(tk, C5_FW_BUILTIN);
	} else if (sysfs_streq(buf, "i") || sysfs_streq(buf, "I")) {
		transaction = true;
		ret = c5_fw_update_locked(tk, C5_FW_FIXED_SD);
	} else {
		tk->fw_armed = false;
		ret = -EINVAL;
	}
	/* Rejected explicit update attempts are failures too; never leave a
	 * previous PASS as their apparent legacy result. No hardware was touched.
	 */
	if (ret && !transaction) {
		tk->fw_error = ret;
		tk->fw_restore_error = 0;
		WRITE_ONCE(tk->fw_state, C5_FW_FAIL);
	}
	mutex_unlock(&tk->state_lock);
	return ret ? ret : count;
}

#endif /* future, separately selected write control */

static ssize_t c5_fw_status_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct c5_touchkey *tk = dev_get_drvdata(dev);
	static const char * const state[] = { "IDLE", "Downloading", "PASS", "FAIL" };
	ssize_t ret;

	mutex_lock(&tk->state_lock);
	ret = scnprintf(buf, PAGE_SIZE, "%s errno=%d restore_errno=%d uncertain=%u enabled=%u powered=%u\n",
		state[tk->fw_state], tk->fw_error, tk->fw_restore_error,
		tk->fw_uncertain, tk->enabled, tk->powered);
	mutex_unlock(&tk->state_lock);
	return ret;
}

/* Compatibility state is a view of the real transaction, never a second
 * success producer. Old zero-initialized state printed PASS before any update;
 * preserve that idle meaning while c5_fw_status explicitly retains IDLE.
 */
static ssize_t touchkey_firm_update_status_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct c5_touchkey *tk = dev_get_drvdata(dev);

	switch (READ_ONCE(tk->fw_state)) {
	case C5_FW_IDLE:
	case C5_FW_PASS:
		return scnprintf(buf, PAGE_SIZE, "PASS\n");
	case C5_FW_RUNNING:
		return scnprintf(buf, PAGE_SIZE, "Downloading\n");
	case C5_FW_FAIL:
		return scnprintf(buf, PAGE_SIZE, "Fail\n");
	default:
		return -EIO;
	}
}

static ssize_t touchkey_firm_version_phone_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "0x%02x\n", C5_FW_VERSION);
}

/* Caller holds state_lock and io_lock. Preserve STOP-separated native reads. */
static int c5_panel_read_locked(struct c5_touchkey *tk, u8 reg, u8 *data,
		unsigned int len)
{
	struct i2c_msg msg = { .addr = tk->client->addr, .len = 1, .buf = &reg };
	int ret;

	ret = i2c_transfer(tk->client->adapter, &msg, 1);
	if (ret == 1) {
		msg.flags = I2C_M_RD;
		msg.buf = data;
		msg.len = len;
		ret = i2c_transfer(tk->client->adapter, &msg, 1);
	}
	return ret == 1 ? 0 : (ret < 0 ? ret : -EIO);
}

/* This fixed checksum query neither enters the loader nor programs flash.
 * It is intentionally separate from permit-only NVM transfers.
 */
static int c5_panel_checksum_locked(struct c5_touchkey *tk)
{
	const u8 command[] = { 0xac, 0x9e, 0x10, 0x00, 0x3f, 0xff };
	u8 busy, checksum[5];
	unsigned int i;
	int ret;

	ret = i2c_master_send(tk->client, command, sizeof(command));
	if (ret != (int)sizeof(command))
		return ret < 0 ? ret : -EIO;
	msleep(5);
	for (i = 0; i < C5_FW_BUSY_POLLS; i++) {
		ret = i2c_master_recv(tk->client, &busy, 1);
		if (ret != 1)
			return ret < 0 ? ret : -EIO;
		if (!busy)
			return c5_panel_read_locked(tk, C5_STATUS, checksum,
					sizeof(checksum));
		msleep(1);
	}
	return -ETIMEDOUT;
}

static ssize_t touchkey_firm_version_panel_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct c5_touchkey *tk = dev_get_drvdata(dev);
	struct wakeup_source *wake = NULL;
	u8 version, model;
	int ret;

	/* Never wait for a flash transaction then hide that this read raced it. */
	if (!mutex_trylock(&tk->state_lock))
		return -EBUSY;
	if (tk->fw_stopping) {
		ret = -ESHUTDOWN;
		goto out;
	}
	if (tk->fw_state == C5_FW_RUNNING) {
		ret = -EBUSY;
		goto out;
	}
	if (!tk->powered || tk->fw_uncertain) {
		ret = -EHOSTDOWN;
		goto out;
	}
	wake = wakeup_source_register("c5_abov_metadata");
	if (!wake) {
		ret = -ENOMEM;
		goto out;
	}
	__pm_stay_awake(wake);
	mutex_lock(&tk->io_lock);
	ret = c5_panel_read_locked(tk, C5_VERSION, &version, 1);
	if (!ret)
		ret = c5_panel_read_locked(tk, C5_MODEL, &model, 1);
	if (!ret && (model != C5_FW_MODEL || !version || version == 0xff))
		ret = -ENODEV;
	if (!ret)
		ret = c5_panel_checksum_locked(tk);
	mutex_unlock(&tk->io_lock);
	if (!ret) {
		tk->fw_version = version;
		tk->fw_model = model;
		ret = scnprintf(buf, PAGE_SIZE, "0x%02x\n", version);
	}
out:
	if (wake) {
		__pm_relax(wake);
		wakeup_source_unregister(wake);
	}
	mutex_unlock(&tk->state_lock);
	return ret;
}

#ifdef CONFIG_C5_CONTROLLER_FW_WRITE_CONTROL
static DEVICE_ATTR(touchkey_firm_update_arm, 0600,
		touchkey_firm_update_arm_show, touchkey_firm_update_arm_store);
static DEVICE_ATTR(touchkey_firm_update, 0200, NULL, touchkey_firm_update_store);
#endif
static DEVICE_ATTR(touchkey_firm_update_status, 0444,
		touchkey_firm_update_status_show, NULL);
static DEVICE_ATTR(c5_fw_status, 0444, c5_fw_status_show, NULL);
static DEVICE_ATTR(touchkey_firm_version_phone, 0444,
		touchkey_firm_version_phone_show, NULL);
static DEVICE_ATTR(touchkey_firm_version_panel, 0444,
		touchkey_firm_version_panel_show, NULL);
static struct attribute *c5_fw_attrs[] = {
#ifdef CONFIG_C5_CONTROLLER_FW_WRITE_CONTROL
	&dev_attr_touchkey_firm_update_arm.attr,
	&dev_attr_touchkey_firm_update.attr,
#endif
	&dev_attr_touchkey_firm_update_status.attr,
	&dev_attr_c5_fw_status.attr,
	&dev_attr_touchkey_firm_version_phone.attr,
	&dev_attr_touchkey_firm_version_panel.attr,
	NULL,
};
static const struct attribute_group c5_fw_group = { .attrs = c5_fw_attrs };
#endif /* firmware implementation; no automatic caller */

static int c5_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct c5_touchkey *tk;
	int irq_gpio, ret;
	u8 version;

	if (!dev->of_node || client->addr != 0x20)
		return -ENODEV;
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;
	if (!sec_class)
		return -EPROBE_DEFER;
	tk = devm_kzalloc(dev, sizeof(*tk), GFP_KERNEL);
	if (!tk)
		return -ENOMEM;
	tk->client = client;
	mutex_init(&tk->state_lock);
	mutex_init(&tk->io_lock);
	i2c_set_clientdata(client, tk);
	tk->pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(tk->pinctrl))
		return PTR_ERR(tk->pinctrl);
	tk->active = pinctrl_lookup_state(tk->pinctrl, "touchkey_active");
	tk->sleep = pinctrl_lookup_state(tk->pinctrl, "touchkey_suspend");
	if (IS_ERR(tk->active))
		return PTR_ERR(tk->active);
	if (IS_ERR(tk->sleep))
		return PTR_ERR(tk->sleep);
	tk->en_gpio = of_get_named_gpio(dev->of_node, "abov,tkey_en-gpio", 0);
	irq_gpio = of_get_named_gpio(dev->of_node, "abov,irq-gpio", 0);
	if (tk->en_gpio < 0)
		return tk->en_gpio;
	if (irq_gpio < 0)
		return irq_gpio;
	ret = devm_gpio_request_one(dev, tk->en_gpio, GPIOF_OUT_INIT_LOW, "c5-tkey-en");
	if (ret)
		return ret;
	ret = devm_gpio_request_one(dev, irq_gpio, GPIOF_IN, "c5-tkey-irq");
	if (ret)
		return ret;
	if (client->irq <= 0 || gpio_to_irq(irq_gpio) != client->irq)
		return -EINVAL;
	tk->vdd = devm_regulator_get(dev, "vddo");
	if (IS_ERR(tk->vdd))
		return PTR_ERR(tk->vdd);
	ret = regulator_set_voltage(tk->vdd, 1800000, 1800000);
	if (ret)
		return ret;
	ret = devm_add_action_or_reset(dev, c5_power_off, tk);
	if (ret)
		return ret;
	ret = c5_power_on(tk);
	if (ret)
		return ret;
	ret = c5_read(tk, C5_VERSION, &version);
	if (ret)
		return ret;
	ret = devm_request_threaded_irq(dev, client->irq, NULL, c5_irq,
				      IRQF_TRIGGER_LOW | IRQF_ONESHOT, "c5-abov-touchkey", tk);
	if (ret)
		return ret;
	disable_irq(client->irq);
	/* Allocate after IRQ devres: managed input close precedes IRQ/power teardown. */
	tk->input = devm_input_allocate_device(dev);
	if (!tk->input)
		return -ENOMEM;
	tk->input->name = "sec_touchkey";
	tk->input->id.bustype = BUS_I2C;
	tk->input->open = c5_open;
	tk->input->close = c5_close;
	input_set_drvdata(tk->input, tk);
	input_set_capability(tk->input, EV_KEY, C5_RECENT);
	input_set_capability(tk->input, EV_KEY, KEY_BACK);
	ret = input_register_device(tk->input);
	if (ret)
		return ret;
	tk->sec = device_create(sec_class, NULL, 0, tk, "sec_touchkey");
	if (IS_ERR(tk->sec))
		return PTR_ERR(tk->sec);
	ret = device_create_file(tk->sec, &dev_attr_brightness);
	if (ret)
		goto err_device;
	ret = sysfs_create_link(&tk->sec->kobj, &tk->input->dev.kobj, "input");
	if (ret)
		goto err_file;
#ifdef CONFIG_C5_CONTROLLER_FW_UPDATE
	ret = sysfs_create_group(&tk->sec->kobj, &c5_fw_group);
	if (ret)
		goto err_link;
#endif
	tk->fw_version = version;
	tk->fb.notifier_call = c5_fb;
	ret = fb_register_client(&tk->fb);
	if (ret)
		goto err_fw_group;
	dev_info(dev, "C5 touchkey runtime ready, firmware=%02x (unchanged)\n", version);
	return 0;
err_fw_group:
#ifdef CONFIG_C5_CONTROLLER_FW_UPDATE
	sysfs_remove_group(&tk->sec->kobj, &c5_fw_group);
#endif
#ifdef CONFIG_C5_CONTROLLER_FW_UPDATE
err_link:
#endif
	sysfs_remove_link(&tk->sec->kobj, "input");
err_file:
	device_remove_file(tk->sec, &dev_attr_brightness);
err_device:
	device_unregister(tk->sec);
	return ret;
}

static int c5_remove(struct i2c_client *client)
{
	struct c5_touchkey *tk = i2c_get_clientdata(client);

	mutex_lock(&tk->state_lock);
	tk->fw_stopping = true;
	tk->fw_armed = false;
	mutex_unlock(&tk->state_lock);
#ifdef CONFIG_C5_CONTROLLER_FW_UPDATE
	sysfs_remove_group(&tk->sec->kobj, &c5_fw_group);
#endif
	fb_unregister_client(&tk->fb);
	sysfs_remove_link(&tk->sec->kobj, "input");
	device_remove_file(tk->sec, &dev_attr_brightness);
	device_unregister(tk->sec);
	c5_close(tk->input);
	return 0;
}

static void c5_shutdown(struct i2c_client *client)
{
	struct c5_touchkey *tk = i2c_get_clientdata(client);

	mutex_lock(&tk->state_lock);
	tk->fw_stopping = true;
	tk->fw_armed = false;
	tk->opened = false;
	c5_apply(tk);
	c5_power_off(tk);
	mutex_unlock(&tk->state_lock);
}

static const struct of_device_id c5_of_match[] = {
	{ .compatible = "abov,mc96ft16xx" }, { }
};
MODULE_DEVICE_TABLE(of, c5_of_match);
static const struct i2c_device_id c5_ids[] = {
	{ "mc96ft16xx", 0 }, { }
};
MODULE_DEVICE_TABLE(i2c, c5_ids);
static struct i2c_driver c5_driver = {
	.driver = { .name = "c5-abov-touchkey", .of_match_table = c5_of_match },
	.probe = c5_probe, .remove = c5_remove, .shutdown = c5_shutdown, .id_table = c5_ids,
};
module_i2c_driver(c5_driver);
MODULE_DESCRIPTION("Samsung C5 ABOV FT1604 runtime keys and armed manual firmware update");
MODULE_LICENSE("GPL");
