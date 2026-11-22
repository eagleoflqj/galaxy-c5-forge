/* Copyright (c) 2010-2011, 2013-2014, The Linux Foundation.
 * All rights reserved.
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
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#include "rpm_log.h"

/* registers in MSM_RPM_LOG_PAGE_INDICES */
enum { MSM_RPM_LOG_TAIL, MSM_RPM_LOG_HEAD };

/* used to 4 byte align message lengths */
#define PADDED_LENGTH(x) (0xFFFFFFFC & ((x) + 3))

/* calculates the character string length of a message of byte length x */
#define PRINTED_LENGTH(x) ((x)*6 + 3)

/* number of ms to wait between checking for new messages in the RPM log */
#define RECHECK_TIME (50)

#define VERSION_8974 0x1000
#define RPM_ULOG_LENGTH_SHIFT 16
#define RPM_ULOG_LENGTH_MASK 0xFFFF0000

struct msm_rpm_log_device {
	struct msm_rpm_log_platform_data layout;
	struct dentry *file;
	bool stopping;
};

static bool rpm_log_stopping(const struct msm_rpm_log_platform_data *layout)
{
	const struct msm_rpm_log_device *log =
	    container_of(layout, struct msm_rpm_log_device, layout);

	return READ_ONCE(log->stopping);
}

struct msm_rpm_log_buffer {
	char *data;
	u32 len;
	u32 pos;
	struct mutex mutex;
	u32 max_len;
	u32 read_idx;
	struct msm_rpm_log_platform_data *pdata;
};

/******************************************************************************
 * Internal functions
 *****************************************************************************/

static inline u32
msm_rpm_log_read(const struct msm_rpm_log_platform_data *pdata, u32 page,
		 u32 reg)
{
	return readl_relaxed(pdata->reg_base + pdata->reg_offsets[page] +
			     reg * 4);
}

/*
 * msm_rpm_log_copy() - Copies messages from a volatile circular buffer in
 *			the RPM's shared memory into a private local buffer
 * msg_buffer:		pointer to local buffer (string)
 * buf_len:		length of local buffer in bytes
 * read_start_idx:	index into shared memory buffer
 *
 * Return value:	number of bytes written to the local buffer
 *
 * Copies messages stored in a circular buffer in the RPM Message Memory into
 * a specified local buffer.  The RPM processor is unaware of these reading
 * efforts, so care is taken to make sure that messages are valid both before
 * and after reading.  The RPM processor utilizes a ULog driver to write the
 * log.  The RPM processor maintains tail and head indices.  These correspond
 * to the next byte to write into, and the first valid byte, respectively.
 * Both indices increase monotonically (except for rollover).
 *
 * Messages take the form of [(u32)length] [(char)data0,1,...] in which the
 * length specifies the number of payload bytes.  Messages must be 4 byte
 * aligned, so padding is added at the end of a message as needed.
 *
 * Print format:
 * - 0xXX, 0xXX, 0xXX
 * - 0xXX
 * etc...
 */
static u32 msm_rpm_log_copy(const struct msm_rpm_log_platform_data *pdata,
			    char *msg_buffer, u32 buf_len, u32 *read_idx)
{
	u32 head_idx, tail_idx;
	u32 pos = 0;
	u32 i = 0;
	u32 msg_len;
	u32 pos_start;
	char temp[4];

	tail_idx =
	    msm_rpm_log_read(pdata, MSM_RPM_LOG_PAGE_INDICES, MSM_RPM_LOG_TAIL);
	head_idx =
	    msm_rpm_log_read(pdata, MSM_RPM_LOG_PAGE_INDICES, MSM_RPM_LOG_HEAD);

	/* loop while the remote buffer has valid messages left to read */
	while (tail_idx - head_idx > 0 && tail_idx - *read_idx > 0) {
		if (rpm_log_stopping(pdata))
			break;
		head_idx = msm_rpm_log_read(pdata, MSM_RPM_LOG_PAGE_INDICES,
					    MSM_RPM_LOG_HEAD);
		tail_idx = msm_rpm_log_read(pdata, MSM_RPM_LOG_PAGE_INDICES,
					    MSM_RPM_LOG_TAIL);
		/* check if the message to be read is valid */
		if (tail_idx - *read_idx > tail_idx - head_idx) {
			*read_idx = head_idx;
			continue;
		}

		/*
		 * Ensure that all indices are 4 byte aligned.
		 * This conditions is required to interact with a ULog buffer
		 * properly.
		 */
		if (!IS_ALIGNED((tail_idx | head_idx | *read_idx), 4))
			break;

		msg_len =
		    msm_rpm_log_read(pdata, MSM_RPM_LOG_PAGE_BUFFER,
				     ((*read_idx) & pdata->log_len_mask) >> 2);

		/* Message length for 8974 is first 2 bytes.
		 * Exclude message length and format from message length.
		 */
		if (pdata->version == VERSION_8974) {
			msg_len = (msg_len & RPM_ULOG_LENGTH_MASK) >>
				  RPM_ULOG_LENGTH_SHIFT;
			if (msg_len < 4)
				break;
			msg_len -= 4;
		}

		/* handle messages that claim to be longer than the log */
		if (PADDED_LENGTH(msg_len) > tail_idx - *read_idx - 4)
			msg_len = tail_idx - *read_idx - 4;

		/* check that the local buffer has enough space for this msg */
		if (pos + PRINTED_LENGTH(msg_len) > buf_len)
			break;

		pos_start = pos;
		pos += scnprintf(msg_buffer + pos, buf_len - pos, "- ");

		/* copy message payload to local buffer */
		for (i = 0; i < msg_len; i++) {
			/* read from shared memory 4 bytes at a time */
			if (IS_ALIGNED(i, 4))
				*((u32 *)temp) = msm_rpm_log_read(
				    pdata, MSM_RPM_LOG_PAGE_BUFFER,
				    ((*read_idx + 4 + i) &
				     pdata->log_len_mask) >>
					2);

			pos += scnprintf(msg_buffer + pos, buf_len - pos,
					 "0x%02X, ", temp[i & 0x03]);
		}

		pos += scnprintf(msg_buffer + pos, buf_len - pos, "\n");

		head_idx = msm_rpm_log_read(pdata, MSM_RPM_LOG_PAGE_INDICES,
					    MSM_RPM_LOG_HEAD);
		tail_idx = msm_rpm_log_read(pdata, MSM_RPM_LOG_PAGE_INDICES,
					    MSM_RPM_LOG_TAIL);

		/* roll back if message that was read is not still valid */
		if (tail_idx - *read_idx > tail_idx - head_idx)
			pos = pos_start;

		*read_idx += PADDED_LENGTH(msg_len) + 4;
	}

	return pos;
}

/*
 * msm_rpm_log_file_read() - Reads in log buffer messages then outputs them to a
 *			     user buffer
 *
 * Return value:
 *	0:	 success
 *	-ENOMEM: no memory available
 *	-EINVAL: user buffer null or requested bytes 0
 *	-EFAULT: user buffer not writeable
 *	-EAGAIN: no bytes available at the moment
 */
static ssize_t msm_rpm_log_file_read(struct file *file, char __user *bufu,
				     size_t count, loff_t *ppos)
{
	u32 out_len, remaining;
	struct msm_rpm_log_platform_data *pdata;
	struct msm_rpm_log_buffer *buf;

	buf = file->private_data;

	if (!buf)
		return -ENOMEM;

	pdata = buf->pdata;

	if (!pdata)
		return -EINVAL;
	if (!buf->data)
		return -ENOMEM;
	if (!bufu || count == 0)
		return -EINVAL;
	if (!access_ok(VERIFY_WRITE, bufu, count))
		return -EFAULT;

	mutex_lock(&buf->mutex);
	if (rpm_log_stopping(pdata)) {
		mutex_unlock(&buf->mutex);
		return -ENODEV;
	}
	/* check for more messages if local buffer empty */
	if (buf->pos == buf->len) {
		buf->pos = 0;
		buf->len = msm_rpm_log_copy(pdata, buf->data, buf->max_len,
					    &(buf->read_idx));
	}

	if ((file->f_flags & O_NONBLOCK) && buf->len == 0) {
		mutex_unlock(&buf->mutex);
		return -EAGAIN;
	}

	/* loop until new messages arrive */
	while (buf->len == 0) {
		if (rpm_log_stopping(pdata)) {
			mutex_unlock(&buf->mutex);
			return -ENODEV;
		}
		cond_resched();
		if (msleep_interruptible(RECHECK_TIME))
			break;
		buf->len = msm_rpm_log_copy(pdata, buf->data, buf->max_len,
					    &(buf->read_idx));
	}

	out_len = ((buf->len - buf->pos) < count ? buf->len - buf->pos : count);

	remaining = __copy_to_user(bufu, &(buf->data[buf->pos]), out_len);
	buf->pos += out_len - remaining;
	mutex_unlock(&buf->mutex);

	return out_len - remaining;
}

/*
 * msm_rpm_log_file_open() - Allows a new reader to open the RPM log virtual
 *			      file
 *
 * One local buffer is kmalloc'ed for each reader, so no resource sharing has
 * to take place (besides the read only access to the RPM log buffer).
 *
 * Return value:
 *	0:	 success
 *	-ENOMEM: no memory available
 */
static int msm_rpm_log_file_open(struct inode *inode, struct file *file)
{
	struct msm_rpm_log_buffer *buf;
	struct msm_rpm_log_platform_data *pdata;

	pdata = inode->i_private;
	if (pdata && rpm_log_stopping(pdata))
		return -ENODEV;
	if (!pdata)
		return -EINVAL;

	file->private_data =
	    kmalloc(sizeof(struct msm_rpm_log_buffer), GFP_KERNEL);
	if (!file->private_data) {
		pr_err("%s: ERROR kmalloc failed to allocate %zu bytes\n",
		       __func__, sizeof(struct msm_rpm_log_buffer));
		return -ENOMEM;
	}
	buf = file->private_data;

	buf->data = kmalloc(PRINTED_LENGTH(pdata->log_len), GFP_KERNEL);
	if (!buf->data) {
		kfree(file->private_data);
		file->private_data = NULL;
		pr_err("%s: ERROR kmalloc failed to allocate %d bytes\n",
		       __func__, PRINTED_LENGTH(pdata->log_len));
		return -ENOMEM;
	}

	buf->pdata = pdata;
	buf->len = 0;
	buf->pos = 0;
	mutex_init(&buf->mutex);
	buf->max_len = PRINTED_LENGTH(pdata->log_len);
	buf->read_idx =
	    msm_rpm_log_read(pdata, MSM_RPM_LOG_PAGE_INDICES, MSM_RPM_LOG_HEAD);
	return 0;
}

static int msm_rpm_log_file_close(struct inode *inode, struct file *file)
{
	kfree(((struct msm_rpm_log_buffer *)file->private_data)->data);
	kfree(file->private_data);
	return 0;
}

static const struct file_operations msm_rpm_log_file_fops = {
    .owner = THIS_MODULE,
    .open = msm_rpm_log_file_open,
    .read = msm_rpm_log_file_read,
    .release = msm_rpm_log_file_close,
};

/* Offsets are byte offsets in the single mapped firmware aperture. */
static bool rpm_log_range(u32 size, u32 offset, u32 bytes)
{
	return IS_ALIGNED(offset, sizeof(u32)) && offset <= size &&
	       bytes <= size - offset;
}

static int rpm_log_header(struct device_node *node,
			  const struct msm_rpm_log_platform_data *pdata,
			  const char *property, u32 *value)
{
	u32 offset;
	int ret = of_property_read_u32(node, property, &offset);

	if (ret)
		return ret;
	if (!rpm_log_range(pdata->phys_size, offset, sizeof(u32)))
		return -EINVAL;
	*value = readl_relaxed(pdata->reg_base + offset);
	return 0;
}

static int rpm_log_dt_layout(struct platform_device *pdev,
			     struct msm_rpm_log_platform_data *pdata)
{
	struct device_node *node = pdev->dev.of_node;
	struct resource *res, *offset;
	void __iomem *shift_reg;
	u32 shift = 0, rpm_base, buffer;
	u64 address;
	int ret;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res || !resource_size(res) || resource_size(res) > U32_MAX)
		return -EINVAL;
	offset = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (offset) {
		if (resource_size(offset) < sizeof(u32))
			return -EINVAL;
		shift_reg = ioremap_nocache(offset->start, sizeof(u32));
		if (!shift_reg)
			return -ENOMEM;
		shift = readl_relaxed(shift_reg);
		iounmap(shift_reg);
	}
	if (shift > (resource_size_t)-1 - res->end)
		return -EINVAL;
	pdata->phys_addr_base = res->start + shift;
	pdata->phys_size = resource_size(res);
	pdata->reg_base = devm_ioremap_nocache(
	    &pdev->dev, pdata->phys_addr_base, pdata->phys_size);
	if (!pdata->reg_base)
		return -ENOMEM;
	ret =
	    rpm_log_header(node, pdata, "qcom,offset-version", &pdata->version);
	if (ret)
		return ret;
	if (pdata->version != VERSION_8974)
		return -EINVAL;
	ret = of_property_read_u32(node, "qcom,rpm-addr-phys", &rpm_base);
	if (ret)
		return ret;
	ret = rpm_log_header(node, pdata, "qcom,offset-page-buffer-addr",
			     &buffer);
	if (ret)
		return ret;
	address = (u64)rpm_base + buffer;
	if (address < pdata->phys_addr_base ||
	    address - pdata->phys_addr_base > pdata->phys_size)
		return -EINVAL;
	pdata->reg_offsets[MSM_RPM_LOG_PAGE_BUFFER] =
	    address - pdata->phys_addr_base;
	ret =
	    rpm_log_header(node, pdata, "qcom,offset-log-len", &pdata->log_len);
	if (ret)
		return ret;
	ret = rpm_log_header(node, pdata, "qcom,offset-log-len-mask",
			     &pdata->log_len_mask);
	if (ret)
		return ret;
	return of_property_read_u32(
	    node, "qcom,offset-page-indices",
	    &pdata->reg_offsets[MSM_RPM_LOG_PAGE_INDICES]);
}

static int msm_rpm_log_probe(struct platform_device *pdev)
{
	struct msm_rpm_log_device *log;
	struct msm_rpm_log_platform_data *pdata;
	int ret;

	log = devm_kzalloc(&pdev->dev, sizeof(*log), GFP_KERNEL);
	if (!log)
		return -ENOMEM;
	pdata = &log->layout;
	if (pdev->dev.of_node) {
		ret = rpm_log_dt_layout(pdev, pdata);
		if (ret)
			return ret;
	} else {
		if (!dev_get_platdata(&pdev->dev))
			return -EINVAL;
		memcpy(pdata, dev_get_platdata(&pdev->dev), sizeof(*pdata));
		pdata->reg_base = devm_ioremap(
		    &pdev->dev, pdata->phys_addr_base, pdata->phys_size);
		if (!pdata->reg_base)
			return -ENOMEM;
	}
	if (pdata->log_len < sizeof(u32) || !is_power_of_2(pdata->log_len) ||
	    pdata->log_len_mask != pdata->log_len - 1 ||
	    pdata->log_len > (U32_MAX - 3) / 6 ||
	    !rpm_log_range(pdata->phys_size,
			   pdata->reg_offsets[MSM_RPM_LOG_PAGE_BUFFER],
			   pdata->log_len) ||
	    !rpm_log_range(pdata->phys_size,
			   pdata->reg_offsets[MSM_RPM_LOG_PAGE_INDICES],
			   2 * sizeof(u32)))
		return -EINVAL;
	log->file = debugfs_create_file("rpm_log", S_IRUGO, NULL, pdata,
					&msm_rpm_log_file_fops);
	if (IS_ERR_OR_NULL(log->file))
		return log->file ? PTR_ERR(log->file) : -ENOMEM;
	platform_set_drvdata(pdev, log);
	return 0;
}

static int msm_rpm_log_remove(struct platform_device *pdev)
{
	struct msm_rpm_log_device *log = platform_get_drvdata(pdev);

	/* Full debugfs file operations drain in-flight opens and reads here. */
	WRITE_ONCE(log->stopping, true);
	debugfs_remove(log->file);
	platform_set_drvdata(pdev, NULL);
	return 0;
}

static struct of_device_id rpm_log_table[] = {
    {.compatible = "qcom,rpm-log"},
    {},
};

static struct platform_driver msm_rpm_log_driver = {
    .probe = msm_rpm_log_probe,
    .remove = msm_rpm_log_remove,
    .driver =
	{
	    .name = "msm_rpm_log",
	    .owner = THIS_MODULE,
	    .of_match_table = rpm_log_table,
	},
};

static int __init msm_rpm_log_init(void)
{
	return platform_driver_register(&msm_rpm_log_driver);
}

static void __exit msm_rpm_log_exit(void)
{
	platform_driver_unregister(&msm_rpm_log_driver);
}

module_init(msm_rpm_log_init);
module_exit(msm_rpm_log_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("MSM RPM Log driver");
MODULE_VERSION("1.0");
MODULE_ALIAS("platform:msm_rpm_log");
