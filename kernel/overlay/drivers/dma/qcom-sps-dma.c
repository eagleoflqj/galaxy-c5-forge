/*
 * Copyright (c) 2014-2015,2017, The Linux Foundation. All rights reserved.
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

/*
 * Qualcomm technologies inc, DMA API for BAM (Bus Access Manager).
 * This DMA driver uses sps-BAM API to access the HW, thus it is effectively a
 * DMA engine wrapper of the sps-BAM API.
 *
 * Client channel configuration example:
 * struct dma_slave_config config {
 *    .direction = DMA_MEM_TO_DEV;
 * };
 *
 * chan = dma_request_slave_channel(client_dev, "rx");
 * dmaengine_slave_config(chan, &config);
 */

#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_dma.h>
#include <linux/list.h>
#include <linux/msm-sps.h>
#include <linux/spinlock.h>
#include "dmaengine.h"

#define QBAM_OF_SLAVE_N_ARGS	(4)
#define QBAM_OF_MANAGE_LOCAL	"qcom,managed-locally"
#define QBAM_OF_SUM_THRESHOLD	"qcom,summing-threshold"
#define QBAM_MAX_DESCRIPTORS	(0x100)
#define QBAM_MAX_CHANNELS	(32)

/*
 * qbam_async_tx_descriptor - dma descriptor plus a list of xfer_bufs
 *
 * @sgl scatterlist of transfer buffers
 * @sg_len size of that list
 * @flags dma xfer flags
 */
struct qbam_transfer {
	phys_addr_t addr;
	u32 len;
};

struct qbam_async_tx_descriptor {
	struct dma_async_tx_descriptor	dma_desc;
	struct scatterlist		*sgl;
	unsigned int			sg_len;
	unsigned long			flags;
	struct qbam_transfer		transfers[QBAM_MAX_DESCRIPTORS];
	u32				residue;
	bool				prepared, submitted, issued;
	bool				issuing, eot_pending;
};

#define DMA_TO_QBAM_ASYNC_DESC(dma_async_desc) \
	container_of(dma_async_desc, struct qbam_async_tx_descriptor, dma_desc)

struct qbam_channel;
/*
 * qbam_device - top level device of current driver
 * @handle bam sps handle.
 * @regs bam register space virtual base address.
 * @mem_resource bam register space resource.
 * @deregister_required if bam is registered by this driver it need to be
 *   unregistered by this driver.
 * @manage is bame managed locally or remotely,
 * @summing_threshold event threshold.
 * @irq bam interrupt line.
 * @channels has the same channels as qbam_dev->dma_dev.channels but
 *   supports fast access by pipe index.
 */
struct qbam_device {
	struct dma_device		dma_dev;
	void __iomem			*regs;
	struct resource			*mem_resource;
	ulong				handle;
	bool				deregister_required;
	u32				summing_threshold;
	u32				manage;
	int				irq;
	struct qbam_channel		*channels[QBAM_MAX_CHANNELS];
};

/* qbam_pipe: aggregate of bam pipe related entries of qbam_channel */
struct qbam_pipe {
	u32				index;
	struct sps_pipe			*handle;
	struct sps_connect		cfg;
	u32				num_descriptors;
	u32				sps_connect_flags;
	u32				sps_register_event_flags;
	bool				connection_active;
	int				connection_error;
};

/*
 * qbam_channel - dma channel plus bam pipe info and current pending transfers
 *
 * @direction is a producer or consumer (MEM => DEV or DEV => MEM)
 * @pending_desc next set of transfer to process
 * @error last error that took place on the current pending_desc
 */
struct qbam_channel {
	struct qbam_pipe		bam_pipe;

	struct dma_chan			chan;
	enum dma_transfer_direction	direction;
	struct qbam_async_tx_descriptor	pending_desc;

	struct qbam_device		*qbam_dev;
	struct mutex			lock;
	spinlock_t			tx_lock;
	bool				tx_enabled;
	int				error;
};
#define DMA_TO_QBAM_CHAN(dma_chan) \
			container_of(dma_chan, struct qbam_channel, chan)
#define qbam_err(qbam_dev, fmt ...) dev_err(qbam_dev->dma_dev.dev, fmt)

static void qbam_tx_stop(struct qbam_channel *qbam_chan)
{
	unsigned long flags;

	spin_lock_irqsave(&qbam_chan->tx_lock, flags);
	qbam_chan->tx_enabled = false;
	spin_unlock_irqrestore(&qbam_chan->tx_lock, flags);
}

/* Only a successful disconnect/configuration permits descriptor recycling. */
static void qbam_tx_reset(struct qbam_channel *qbam_chan, bool enabled)
{
	struct qbam_async_tx_descriptor *desc = &qbam_chan->pending_desc;
	unsigned long flags;

	spin_lock_irqsave(&qbam_chan->tx_lock, flags);
	desc->prepared = desc->submitted = desc->issued = false;
	desc->issuing = desc->eot_pending = false;
	desc->sgl = NULL;
	desc->sg_len = desc->residue = 0;
	desc->dma_desc.cookie = 0;
	qbam_chan->error = 0;
	qbam_chan->tx_enabled = enabled;
	spin_unlock_irqrestore(&qbam_chan->tx_lock, flags);
}

/*  qbam_disconnect_chan - disconnect a channel */
static int qbam_disconnect_chan(struct qbam_channel *qbam_chan)
{
	struct qbam_device  *qbam_dev    = qbam_chan->qbam_dev;
	struct sps_pipe     *pipe_handle = qbam_chan->bam_pipe.handle;
	struct sps_connect   pipe_config_no_irq = {.options = SPS_O_POLL};
	int ret;

	if (!qbam_chan->bam_pipe.connection_active)
		return 0;
	/*
	 * SW workaround:
	 * When disconnecting BAM pipe a spurious interrupt sometimes appears.
	 * To avoid that, we change the pipe setting from interrupt (default)
	 * to polling (SPS_O_POLL) before diconnecting the pipe.
	 */
	ret = sps_set_config(pipe_handle, &pipe_config_no_irq);
	if (ret)
		qbam_err(qbam_dev,
			"error:%d sps_set_config(pipe:%d) before disconnect\n",
			ret, qbam_chan->bam_pipe.index);

	ret = sps_disconnect(pipe_handle);
	if (ret)
		qbam_err(qbam_dev, "error:%d sps_disconnect(pipe:%d)\n",
			 ret, qbam_chan->bam_pipe.index);
	if (ret) {
		qbam_chan->bam_pipe.connection_error = ret;
	} else {
		qbam_chan->bam_pipe.connection_active = false;
		qbam_chan->bam_pipe.connection_error = 0;
	}

	return ret;
}

static void qbam_free_desc(struct qbam_channel *qbam_chan)
{
	struct sps_mem_buffer *desc = &qbam_chan->bam_pipe.cfg.desc;

	if (desc->base) {
		dma_free_coherent(qbam_chan->qbam_dev->dma_dev.dev,
				   desc->size, desc->base, desc->phys_base);
		memset(desc, 0, sizeof(*desc));
	}
}

/*  qbam_free_chan - disconnect channel and free its resources */
static void qbam_free_chan(struct dma_chan *chan)
{
	struct qbam_channel *qbam_chan = DMA_TO_QBAM_CHAN(chan);
	struct qbam_device  *qbam_dev  = qbam_chan->qbam_dev;

	if (!qbam_chan->bam_pipe.handle)
		return;

	mutex_lock(&qbam_chan->lock);
	qbam_tx_stop(qbam_chan);
	if (qbam_chan->bam_pipe.connection_error)
		goto out;
	if (qbam_disconnect_chan(qbam_chan)) {
		qbam_err(qbam_dev,
			"error free_chan() failed to disconnect(pipe:%d)\n",
			qbam_chan->bam_pipe.index);
		goto out;
	}
	qbam_free_desc(qbam_chan);
	qbam_tx_reset(qbam_chan, false);
out:
	mutex_unlock(&qbam_chan->lock);
}

static struct dma_chan *qbam_dma_xlate(struct of_phandle_args *dma_spec,
							struct of_dma *of)
{
	struct qbam_device  *qbam_dev  = of->of_dma_data;
	struct qbam_channel *qbam_chan;
	u32 channel_index;
	u32 num_descriptors;

	if (dma_spec->args_count != QBAM_OF_SLAVE_N_ARGS) {
		qbam_err(qbam_dev,
			"invalid number of dma arguments, expect:%d got:%d\n",
			QBAM_OF_SLAVE_N_ARGS, dma_spec->args_count);
		return NULL;
	};

	channel_index = dma_spec->args[0];

	if (channel_index >= QBAM_MAX_CHANNELS) {
		qbam_err(qbam_dev,
			"error: channel_index:%d out of bounds",
			channel_index);
		return NULL;
	}
	num_descriptors = dma_spec->args[1];
	if (!num_descriptors || (num_descriptors > QBAM_MAX_DESCRIPTORS)) {
		qbam_err(qbam_dev,
			"invalid number of descriptors, range[1..%d] got:%d\n",
			QBAM_MAX_DESCRIPTORS, num_descriptors);
		return NULL;
	}

	qbam_chan = qbam_dev->channels[channel_index];
	if (!qbam_chan)
		return NULL;

	/* A retained descriptor ring must keep its original configuration. */
	if (qbam_chan->bam_pipe.cfg.desc.base &&
	    (qbam_chan->bam_pipe.num_descriptors != num_descriptors ||
	     qbam_chan->bam_pipe.sps_connect_flags != dma_spec->args[2] ||
	     qbam_chan->bam_pipe.sps_register_event_flags != dma_spec->args[3]))
		return NULL;

	/* The DMA core owns exclusivity, module refs and resource allocation. */
	if (!dma_get_slave_channel(&qbam_chan->chan))
		return NULL;

	qbam_chan->bam_pipe.num_descriptors          = num_descriptors;
	qbam_chan->bam_pipe.sps_connect_flags        = dma_spec->args[2];
	qbam_chan->bam_pipe.sps_register_event_flags = dma_spec->args[3];

	return &qbam_chan->chan;
}

static enum dma_status qbam_tx_status(struct dma_chan *chan,
			dma_cookie_t cookie, struct dma_tx_state *state)
{
	struct qbam_channel *qbam_chan = DMA_TO_QBAM_CHAN(chan);
	struct qbam_async_tx_descriptor *desc = &qbam_chan->pending_desc;
	enum dma_status ret;
	unsigned long flags;

	spin_lock_irqsave(&qbam_chan->tx_lock, flags);
	ret = dma_cookie_status(chan, cookie, state);
	if (cookie < DMA_MIN_COOKIE)
		ret = DMA_ERROR;
	else if (ret != DMA_COMPLETE &&
		 (qbam_chan->error || qbam_chan->bam_pipe.connection_error))
		ret = DMA_ERROR;
	if (ret != DMA_COMPLETE && cookie == desc->dma_desc.cookie)
		dma_set_residue(state, desc->residue);
	spin_unlock_irqrestore(&qbam_chan->tx_lock, flags);
	return ret;
}

/*
 * qbam_init_bam_handle - find or create bam handle.
 *
 * BAM device needs to be registered for each BLSP once and only once. if it
 * was registered, then we find the handle to the registered bam and return
 * it, otherwise we register it here.
 * The module which registered BAM is responsible for deregistering it.
 */
static int qbam_init_bam_handle(struct qbam_device *qbam_dev)
{
	int ret = 0;
	struct sps_bam_props bam_props = {0};

	/*
	 * Check if BAM is already registred with SPS on the current
	 * BLSP. If it isn't then go ahead and register it.
	 */
	ret = sps_phy2h(qbam_dev->mem_resource->start, &qbam_dev->handle);
	if (qbam_dev->handle)
		return 0;

	if (!qbam_dev->regs)
		qbam_dev->regs = ioremap(qbam_dev->mem_resource->start,
				       resource_size(qbam_dev->mem_resource));
	if (!qbam_dev->regs) {
		qbam_err(qbam_dev, "error ioremap(phy:0x%lx len:0x%lx)\n",
			 (ulong) qbam_dev->mem_resource->start,
			 (ulong) resource_size(qbam_dev->mem_resource));
		return -ENOMEM;
	};

	bam_props.phys_addr		= qbam_dev->mem_resource->start;
	bam_props.virt_addr		= qbam_dev->regs;
	bam_props.summing_threshold	= qbam_dev->summing_threshold;
	bam_props.manage		= qbam_dev->manage;
	bam_props.irq			= qbam_dev->irq;

	ret = sps_register_bam_device(&bam_props, &qbam_dev->handle);
	if (ret)
		qbam_err(qbam_dev, "error:%d sps_register_bam_device\n"
			 "(phy:0x%lx virt:0x%lx irq:%d)\n",
			 ret, (ulong) bam_props.phys_addr,
			 (ulong) bam_props.virt_addr, qbam_dev->irq);
	else
		qbam_dev->deregister_required = true;

	return ret;
}


static int qbam_alloc_chan(struct dma_chan *chan)
{
	struct qbam_channel *qbam_chan = DMA_TO_QBAM_CHAN(chan);

	if (qbam_chan->bam_pipe.connection_error)
		return qbam_chan->bam_pipe.connection_error;

	if (!qbam_chan->bam_pipe.handle) {
		qbam_chan->bam_pipe.handle = sps_alloc_endpoint();
		if (!qbam_chan->bam_pipe.handle)
			return -ENOMEM;
	}
	return 0;
}

static void qbam_tx_complete(struct qbam_channel *qbam_chan)
{
	struct qbam_async_tx_descriptor *desc = &qbam_chan->pending_desc;
	dma_async_tx_callback callback = NULL;
	void *param = NULL;
	unsigned long flags;

	spin_lock_irqsave(&qbam_chan->tx_lock, flags);
	if (qbam_chan->tx_enabled && !qbam_chan->error &&
	    desc->eot_pending && !desc->issuing && desc->submitted && desc->issued &&
	    desc->dma_desc.cookie >= DMA_MIN_COOKIE) {
		dma_cookie_complete(&desc->dma_desc);
		desc->prepared = desc->submitted = desc->issued = false;
		desc->eot_pending = false;
		desc->sgl = NULL;
		desc->sg_len = desc->residue = 0;
		callback = desc->dma_desc.callback;
		param = desc->dma_desc.callback_param;
	}
	spin_unlock_irqrestore(&qbam_chan->tx_lock, flags);
	if (callback)
		callback(param);
}

static void qbam_eot_callback(struct sps_event_notify *notify)
{
	struct qbam_channel *qbam_chan = notify->user;
	unsigned long flags;

	if (!qbam_chan || (notify->event_id != SPS_EVENT_EOT &&
			  notify->event_id != SPS_EVENT_DESC_DONE) ||
	    notify->data.transfer.user != &qbam_chan->pending_desc)
		return;
	spin_lock_irqsave(&qbam_chan->tx_lock, flags);
	if (qbam_chan->pending_desc.submitted && qbam_chan->pending_desc.issued)
		qbam_chan->pending_desc.eot_pending = true;
	spin_unlock_irqrestore(&qbam_chan->tx_lock, flags);
	qbam_tx_complete(qbam_chan);
}

static void qbam_error_callback(struct sps_event_notify *notify)
{
	struct qbam_channel *qbam_chan = notify->user;
	unsigned long flags;

	if (!qbam_chan)
		return;
	spin_lock_irqsave(&qbam_chan->tx_lock, flags);
	if (!qbam_chan->error)
		qbam_chan->error = -EIO;
	spin_unlock_irqrestore(&qbam_chan->tx_lock, flags);
	qbam_err(qbam_chan->qbam_dev, "error: qbam_error_callback(pipe:%d)\n",
		 qbam_chan->bam_pipe.index);
}

static int qbam_connect_chan(struct qbam_channel *qbam_chan)
{
	int ret = 0;
	int disconnect_ret;
	struct qbam_device       *qbam_dev = qbam_chan->qbam_dev;
	struct sps_register_event bam_eot_event = {
		.mode		= SPS_TRIGGER_CALLBACK,
		.options	= qbam_chan->bam_pipe.sps_register_event_flags,
		.callback	= qbam_eot_callback,
		.user		= qbam_chan,
		};
	struct sps_register_event bam_error_event = {
		.mode		= SPS_TRIGGER_CALLBACK,
		.options	= SPS_O_ERROR,
		.callback	= qbam_error_callback,
		.user		= qbam_chan,
		};

	if (qbam_chan->bam_pipe.connection_error)
		return qbam_chan->bam_pipe.connection_error;
	if (qbam_chan->bam_pipe.connection_active)
		return -EBUSY;
	/* A failed SPS connect may itself have ignored a failed rollback. */
	qbam_chan->bam_pipe.connection_active = true;
	ret = sps_connect(qbam_chan->bam_pipe.handle, &qbam_chan->bam_pipe.cfg);
	if (ret) {
		qbam_chan->bam_pipe.connection_error = ret;
		qbam_err(qbam_dev, "error:%d sps_connect(pipe:%d)\n", ret,
			 qbam_chan->bam_pipe.index);
		return ret;
	}

	ret = sps_register_event(qbam_chan->bam_pipe.handle, &bam_eot_event);
	if (ret) {
		qbam_err(qbam_dev, "error:%d sps_register_event(eot@pipe:%d)\n",
			 ret, qbam_chan->bam_pipe.index);
		goto need_disconnect;
	}

	ret = sps_register_event(qbam_chan->bam_pipe.handle, &bam_error_event);
	if (ret) {
		qbam_err(qbam_dev, "error:%d sps_register_event(err@pipe:%d)\n",
			 ret, qbam_chan->bam_pipe.index);
		goto need_disconnect;
	}

	return 0;

need_disconnect:
	disconnect_ret = qbam_disconnect_chan(qbam_chan);
	if (disconnect_ret)
		qbam_err(qbam_dev, "error:%d sps_disconnect(pipe:%d)\n", disconnect_ret,
			 qbam_chan->bam_pipe.index);
	return ret;
}

/*
 * qbam_slave_cfg - configure and connect a BAM pipe
 *
 * @cfg only cares about cfg->direction
 */
static int qbam_slave_cfg(struct dma_chan *chan,
						struct dma_slave_config *cfg)
{
	int ret = 0;
	struct qbam_channel *qbam_chan = DMA_TO_QBAM_CHAN(chan);
	struct qbam_device *qbam_dev = qbam_chan->qbam_dev;
	struct sps_connect *pipe_cfg = &qbam_chan->bam_pipe.cfg;

	if (cfg->direction != DMA_MEM_TO_DEV && cfg->direction != DMA_DEV_TO_MEM)
		return -EINVAL;
	mutex_lock(&qbam_chan->lock);
	if (qbam_chan->bam_pipe.connection_error) {
		ret = qbam_chan->bam_pipe.connection_error;
		goto out;
	}
	if (qbam_chan->bam_pipe.connection_active) {
		ret = -EBUSY;
		goto out;
	}
	if (!qbam_dev->handle) {
		ret = qbam_init_bam_handle(qbam_dev);
		if (ret)
			goto out;
	}

	if (qbam_chan->bam_pipe.cfg.desc.base) {
		if (qbam_chan->direction != cfg->direction) {
			ret = -EINVAL;
			goto out;
		}
		goto cfg_done;
	}

	ret = sps_get_config(qbam_chan->bam_pipe.handle,
						&qbam_chan->bam_pipe.cfg);
	if (ret) {
		qbam_err(qbam_dev, "error:%d sps_get_config(0x%p)\n",
			 ret, qbam_chan->bam_pipe.handle);
		goto out;
	}

	qbam_chan->direction = cfg->direction;
	if (cfg->direction == DMA_MEM_TO_DEV) {
		pipe_cfg->source          = SPS_DEV_HANDLE_MEM;
		pipe_cfg->destination     = qbam_dev->handle;
		pipe_cfg->mode            = SPS_MODE_DEST;
		pipe_cfg->src_pipe_index  = 0;
		pipe_cfg->dest_pipe_index = qbam_chan->bam_pipe.index;
	} else {
		pipe_cfg->source          = qbam_dev->handle;
		pipe_cfg->destination     = SPS_DEV_HANDLE_MEM;
		pipe_cfg->mode            = SPS_MODE_SRC;
		pipe_cfg->src_pipe_index  = qbam_chan->bam_pipe.index;
		pipe_cfg->dest_pipe_index = 0;
	}
	pipe_cfg->options   =  qbam_chan->bam_pipe.sps_connect_flags;
	pipe_cfg->desc.size = (qbam_chan->bam_pipe.num_descriptors + 1) *
						 sizeof(struct sps_iovec);
	/* Explicit ownership must survive a failed disconnect during unbind. */
	pipe_cfg->desc.base = dma_alloc_coherent(qbam_dev->dma_dev.dev,
						  pipe_cfg->desc.size,
						  &pipe_cfg->desc.phys_base,
						  GFP_KERNEL);
	if (!pipe_cfg->desc.base) {
		qbam_err(qbam_dev,
			"error dma_alloc_coherent(desc-sz:%llu * n-descs:%d)\n",
			(u64) sizeof(struct sps_iovec),
			qbam_chan->bam_pipe.num_descriptors);
		ret = -ENOMEM;
		goto out;
	}
cfg_done:
	ret = qbam_connect_chan(qbam_chan);
	if (ret && !qbam_chan->bam_pipe.connection_active)
		qbam_free_desc(qbam_chan);
	if (!ret)
		qbam_tx_reset(qbam_chan, true);
out:
	mutex_unlock(&qbam_chan->lock);
	return ret;
}

static int qbam_flush_chan(struct dma_chan *chan)
{
	struct qbam_channel *qbam_chan = DMA_TO_QBAM_CHAN(chan);
	int ret;

	mutex_lock(&qbam_chan->lock);
	qbam_tx_stop(qbam_chan);
	if (qbam_chan->bam_pipe.connection_error) {
		ret = qbam_chan->bam_pipe.connection_error;
		goto out;
	}
	if (!qbam_chan->bam_pipe.cfg.desc.base) {
		qbam_tx_reset(qbam_chan, false);
		ret = 0;
		goto out;
	}
	ret = qbam_disconnect_chan(qbam_chan);

	if (ret) {
		qbam_err(qbam_chan->qbam_dev,
			 "error: disconnect flush(pipe:%d\n)",
			 qbam_chan->bam_pipe.index);
		goto out;
	}
	qbam_tx_reset(qbam_chan, false);
	ret = qbam_connect_chan(qbam_chan);
	if (ret)
		qbam_err(qbam_chan->qbam_dev,
			 "error: reconnect flush(pipe:%d\n)",
			 qbam_chan->bam_pipe.index);
	else
		qbam_tx_reset(qbam_chan, true);
out:
	mutex_unlock(&qbam_chan->lock);
	return ret;
}

/* qbam_tx_submit - sets the descriptor as the next one to be executed */
static dma_cookie_t qbam_tx_submit(struct dma_async_tx_descriptor *dma_desc)
{
	struct qbam_channel *qbam_chan = DMA_TO_QBAM_CHAN(dma_desc->chan);
	struct qbam_async_tx_descriptor *desc = &qbam_chan->pending_desc;
	dma_cookie_t ret;
	unsigned long flags;

	spin_lock_irqsave(&qbam_chan->tx_lock, flags);
	ret = qbam_chan->error ? qbam_chan->error :
		qbam_chan->bam_pipe.connection_error;
	if (!ret && !qbam_chan->tx_enabled)
		ret = -ENOTCONN;
	if (!ret && (!desc->prepared || desc->submitted))
		ret = -EBUSY;
	if (!ret) {
		ret = dma_cookie_assign(dma_desc);
		desc->submitted = true;
	}
	spin_unlock_irqrestore(&qbam_chan->tx_lock, flags);
	return ret;
}

/*
 * qbam_prep_slave_sg - creates qbam_xfer_buf from a list of sg
 *
 * @chan: dma channel
 * @sgl: scatter gather list
 * @sg_len: length of sg
 * @direction: DMA transfer direction
 * @flags: DMA flags
 * @context: transfer context (unused)
 * @return the newly created descriptor or negative ERR_PTR() on error
 */
static struct dma_async_tx_descriptor *qbam_prep_slave_sg(struct dma_chan *chan,
	struct scatterlist *sgl, unsigned int sg_len,
	enum dma_transfer_direction direction, unsigned long flags,
	void *context)
{
	struct qbam_channel *qbam_chan = DMA_TO_QBAM_CHAN(chan);
	struct qbam_async_tx_descriptor *desc = &qbam_chan->pending_desc;
	struct scatterlist *sg;
	unsigned long irq_flags;
	u64 total = 0;
	int i, error;

	if (qbam_chan->direction != direction || !sgl || !sg_len ||
	    sg_len > qbam_chan->bam_pipe.num_descriptors ||
	    sg_len > QBAM_MAX_DESCRIPTORS ||
	    !(((flags & SPS_IOVEC_FLAG_EOT) &&
	       (qbam_chan->bam_pipe.sps_register_event_flags & SPS_O_EOT)) ||
	      ((flags & SPS_IOVEC_FLAG_INT) &&
	       (qbam_chan->bam_pipe.sps_register_event_flags & SPS_O_DESC_DONE))))
		return ERR_PTR(-EINVAL);
	for_each_sg(sgl, sg, sg_len, i) {
		if (!sg_dma_len(sg) || sg_dma_len(sg) > 0xffff)
			return ERR_PTR(-EINVAL);
		total += sg_dma_len(sg);
	}
	if (total > U32_MAX)
		return ERR_PTR(-EOVERFLOW);
	spin_lock_irqsave(&qbam_chan->tx_lock, irq_flags);
	error = qbam_chan->error ? qbam_chan->error :
		qbam_chan->bam_pipe.connection_error;
	if (!error && !qbam_chan->tx_enabled)
		error = -ENOTCONN;
	if (!error && desc->prepared)
		error = -EBUSY;
	if (error) {
		spin_unlock_irqrestore(&qbam_chan->tx_lock, irq_flags);
		return ERR_PTR(error);
	}
	for_each_sg(sgl, sg, sg_len, i) {
		desc->transfers[i].addr = sg_dma_address(sg);
		desc->transfers[i].len = sg_dma_len(sg);
	}
	desc->dma_desc.chan = &qbam_chan->chan;
	desc->dma_desc.tx_submit = qbam_tx_submit;
	desc->dma_desc.cookie = 0;
	desc->dma_desc.callback = NULL;
	desc->dma_desc.callback_param = NULL;
	desc->sgl = sgl;
	desc->sg_len = sg_len;
	desc->flags = flags;
	desc->residue = total;
	desc->prepared = true;
	desc->submitted = desc->issued = false;
	desc->issuing = desc->eot_pending = false;
	spin_unlock_irqrestore(&qbam_chan->tx_lock, irq_flags);
	return &desc->dma_desc;
}

/*
 * qbam_issue_pending - queue pending descriptor to BAM
 *
 * Iterate over the transfers of the pending descriptor and push them to bam
 */
static void qbam_issue_pending(struct dma_chan *chan)
{
	struct qbam_channel *qbam_chan = DMA_TO_QBAM_CHAN(chan);
	struct qbam_async_tx_descriptor *desc = &qbam_chan->pending_desc;
	unsigned long flags;
	dma_cookie_t cookie;
	unsigned int count;
	unsigned int i;
	int ret;

	mutex_lock(&qbam_chan->lock);
	spin_lock_irqsave(&qbam_chan->tx_lock, flags);
	if (!qbam_chan->tx_enabled || qbam_chan->error ||
	    qbam_chan->bam_pipe.connection_error || !desc->submitted ||
	    desc->issued) {
		spin_unlock_irqrestore(&qbam_chan->tx_lock, flags);
		goto out;
	}
	desc->issued = true;
	desc->issuing = true;
	cookie = desc->dma_desc.cookie;
	count = desc->sg_len;
	spin_unlock_irqrestore(&qbam_chan->tx_lock, flags);

	for (i = 0; i < count; i++) {
		bool last = (i == count - 1);
		phys_addr_t addr = desc->transfers[i].addr;
		u32 len = desc->transfers[i].len;
		u32 transfer_flags = last ? desc->flags : 0;

		/* SPS can synchronously deliver callbacks; never hold tx_lock. */
		ret = sps_transfer_one(qbam_chan->bam_pipe.handle,
				      addr, len, last ? desc : NULL, transfer_flags);
		if (ret) {
			spin_lock_irqsave(&qbam_chan->tx_lock, flags);
			if (desc->dma_desc.cookie == cookie && !qbam_chan->error)
				qbam_chan->error = ret < 0 ? ret : -EIO;
			spin_unlock_irqrestore(&qbam_chan->tx_lock, flags);
			qbam_err(qbam_chan->qbam_dev,
				 "error:%d sps_transfer_one(pipe:%d)\n",
				 ret, qbam_chan->bam_pipe.index);
			break;
		}
		/* Final EOT may be latched; completion waits for API return. */
		if (last)
			break;
		spin_lock_irqsave(&qbam_chan->tx_lock, flags);
		ret = qbam_chan->error;
		spin_unlock_irqrestore(&qbam_chan->tx_lock, flags);
		if (ret)
			break;
	}
	spin_lock_irqsave(&qbam_chan->tx_lock, flags);
	if (desc->dma_desc.cookie == cookie)
		desc->issuing = false;
	spin_unlock_irqrestore(&qbam_chan->tx_lock, flags);
	qbam_tx_complete(qbam_chan);
out:
	mutex_unlock(&qbam_chan->lock);
}

static int qbam_deregister_bam_dev(struct qbam_device *qbam_dev)
{
	int ret;

	if (!qbam_dev->handle)
		return 0;

	ret = sps_deregister_bam_device(qbam_dev->handle);
	if (ret)
		qbam_err(qbam_dev,
			"error:%d sps_deregister_bam_device(hndl:0x%lx) failed",
			ret, qbam_dev->handle);
	else {
		qbam_dev->handle = 0;
		qbam_dev->deregister_required = false;
	}
	return ret;
}

static int qbam_pipes_free(struct qbam_device *qbam_dev)
{
	struct qbam_channel *qbam_chan_cur, *qbam_chan_next;
	int ret, error = 0;

	list_for_each_entry_safe(qbam_chan_cur, qbam_chan_next,
			&qbam_dev->dma_dev.channels, chan.device_node) {
		mutex_lock(&qbam_chan_cur->lock);
		qbam_tx_stop(qbam_chan_cur);
		ret = qbam_chan_cur->bam_pipe.connection_error;
		if (!ret)
			ret = qbam_disconnect_chan(qbam_chan_cur);
		if (!ret && qbam_chan_cur->bam_pipe.handle) {
			ret = sps_free_endpoint(qbam_chan_cur->bam_pipe.handle);
			if (!ret)
				qbam_chan_cur->bam_pipe.handle = NULL;
		}
		if (ret) {
			qbam_chan_cur->bam_pipe.connection_error = ret;
			if (!error)
				error = ret;
			mutex_unlock(&qbam_chan_cur->lock);
			continue;
		}
		qbam_free_desc(qbam_chan_cur);
		mutex_unlock(&qbam_chan_cur->lock);
		list_del(&qbam_chan_cur->chan.device_node);
		qbam_dev->channels[qbam_chan_cur->bam_pipe.index] = NULL;
		kfree(qbam_chan_cur);
	}
	return error;
}

static void qbam_device_free(struct qbam_device *qbam_dev)
{
	struct device *dev = qbam_dev->dma_dev.dev;

	if (qbam_dev->regs)
		iounmap(qbam_dev->regs);
	if (qbam_dev->mem_resource)
		release_mem_region(qbam_dev->mem_resource->start,
				   resource_size(qbam_dev->mem_resource));
	kfree(qbam_dev);
	put_device(dev);
	module_put(THIS_MODULE);
}

static int qbam_probe(struct platform_device *pdev)
{
	struct qbam_device *qbam_dev;
	struct resource *mem_resource;
	int ret, i;
	bool managed_locally;
	struct device_node *of_node = pdev->dev.of_node;

	qbam_dev = kzalloc(sizeof(*qbam_dev), GFP_KERNEL);
	if (!qbam_dev)
		return -ENOMEM;

	qbam_dev->dma_dev.dev = get_device(&pdev->dev);
	/* Keep callback code resident even if cleanup later cannot complete. */
	__module_get(THIS_MODULE);
	platform_set_drvdata(pdev, qbam_dev);
	INIT_LIST_HEAD(&qbam_dev->dma_dev.channels);

	mem_resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!mem_resource) {
		qbam_err(qbam_dev, "missing 'reg' DT entry");
		ret = -ENODEV;
		goto err_free_device;
	}
	/* Held on uncertain cleanup: prevents a second owner of this aperture. */
	qbam_dev->mem_resource = request_mem_region(mem_resource->start,
			resource_size(mem_resource), dev_name(&pdev->dev));
	if (!qbam_dev->mem_resource) {
		ret = -EBUSY;
		goto err_free_device;
	}

	qbam_dev->irq = platform_get_irq(pdev, 0);
	if (qbam_dev->irq <= 0) {
		qbam_err(qbam_dev, "missing DT IRQ resource entry");
		ret = qbam_dev->irq ? qbam_dev->irq : -EINVAL;
		goto err_free_device;
	}

	ret = of_property_read_u32(of_node, QBAM_OF_SUM_THRESHOLD,
				   &qbam_dev->summing_threshold);
	if (ret) {
		qbam_err(qbam_dev, "missing '%s' DT entry",
			 QBAM_OF_SUM_THRESHOLD);
		goto err_free_device;
	}

	/* read from DT and set sps_bam_props.manage */
	managed_locally = of_property_read_bool(of_node, QBAM_OF_MANAGE_LOCAL);
	qbam_dev->manage = managed_locally ? SPS_BAM_MGR_LOCAL :
					     SPS_BAM_MGR_DEVICE_REMOTE;

	/* Init channels */
	INIT_LIST_HEAD(&qbam_dev->dma_dev.channels);
	/* Register stable software slots before DMA core registration. */
	for (i = 0; i < QBAM_MAX_CHANNELS; i++) {
		struct qbam_channel *channel;

		channel = kzalloc(sizeof(*channel), GFP_KERNEL);
		if (!channel) {
			ret = -ENOMEM;
			goto err_free_channels;
		}
		channel->chan.device = &qbam_dev->dma_dev;
		channel->qbam_dev = qbam_dev;
		channel->bam_pipe.index = i;
		dma_cookie_init(&channel->chan);
		mutex_init(&channel->lock);
		spin_lock_init(&channel->tx_lock);
		list_add_tail(&channel->chan.device_node,
			      &qbam_dev->dma_dev.channels);
		qbam_dev->channels[i] = channel;
	}

	/* Set capabilities */
	dma_cap_zero(qbam_dev->dma_dev.cap_mask);
	dma_cap_set(DMA_SLAVE,		qbam_dev->dma_dev.cap_mask);
	dma_cap_set(DMA_PRIVATE,	qbam_dev->dma_dev.cap_mask);

	/* Initialize dmaengine callback apis */
	qbam_dev->dma_dev.device_alloc_chan_resources	= qbam_alloc_chan;
	qbam_dev->dma_dev.device_free_chan_resources	= qbam_free_chan;
	qbam_dev->dma_dev.device_prep_slave_sg		= qbam_prep_slave_sg;
	qbam_dev->dma_dev.device_terminate_all		= qbam_flush_chan;
	qbam_dev->dma_dev.device_config			= qbam_slave_cfg;
	qbam_dev->dma_dev.device_issue_pending		= qbam_issue_pending;
	qbam_dev->dma_dev.device_tx_status		= qbam_tx_status;

	/* Publish to the OF framework only after successful DMA registration. */
	ret = dma_async_device_register(&qbam_dev->dma_dev);
	if (ret)
		goto err_free_channels;

	ret = of_dma_controller_register(of_node, qbam_dma_xlate, qbam_dev);
	if (ret) {
		qbam_err(qbam_dev, "error:%d of_dma_controller_register()\n",
			 ret);
		goto err_unregister_dma;
	}
	return 0;

err_unregister_dma:
	dma_async_device_unregister(&qbam_dev->dma_dev);
err_free_channels:
	qbam_pipes_free(qbam_dev);
err_free_device:
	platform_set_drvdata(pdev, NULL);
	qbam_device_free(qbam_dev);
	return ret;
}

static int qbam_remove(struct platform_device *pdev)
{
	struct qbam_device *qbam_dev = platform_get_drvdata(pdev);
	int ret;

	of_dma_controller_free(pdev->dev.of_node);
	dma_async_device_unregister(&qbam_dev->dma_dev);

	/* free BAM pipes resources */
	ret = qbam_pipes_free(qbam_dev);
	if (ret)
		goto retain_owner;

	if (qbam_dev->deregister_required) {
		ret = qbam_deregister_bam_dev(qbam_dev);
		if (ret)
			goto retain_owner;
	}

	platform_set_drvdata(pdev, NULL);
	qbam_device_free(qbam_dev);
	return 0;

retain_owner:
	/* Driver core ignores remove errors and releases all devres anyway. */
	qbam_err(qbam_dev,
		 "cleanup failed:%d; retaining device/MMIO/DMA owners until reboot\n",
		 ret);
	return ret;
}

static const struct of_device_id qbam_of_match[] = {
	{ .compatible = "qcom,sps-dma" },
	{}
};
MODULE_DEVICE_TABLE(of, qbam_of_match);

static struct platform_driver qbam_driver = {
	.probe = qbam_probe,
	.remove = qbam_remove,
	.driver = {
		.name = "qcom-sps-dma",
		.owner = THIS_MODULE,
		.of_match_table = qbam_of_match,
	},
};

module_platform_driver(qbam_driver);

MODULE_DESCRIPTION("DMA-API driver to qcom BAM");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:qcom-sps-dma");
