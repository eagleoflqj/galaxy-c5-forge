// SPDX-License-Identifier: GPL-2.0-only
/* VB2's native ownership rules, with the frozen C5 firmware state machine. */
#include "msm_vidc_common.h"
#include "c5-vidc-native.h"
#include "c5-vidc-lifecycle.h"

static unsigned int c5_vidc_owned(struct vb2_queue *q)
{
	unsigned int i, count = 0;
	for (i = 0; i < q->num_buffers; i++)
		if (q->bufs[i] && q->bufs[i]->state == VB2_BUF_STATE_ACTIVE)
			count++;
	return count;
}

static void c5_vidc_return_buffer(struct vb2_buffer *vb,
	enum vb2_buffer_state state)
{
	unsigned int i;
	if (vb->state != VB2_BUF_STATE_ACTIVE)
		return;
	/* A failed START returns QUEUED, with the original payload intact.
	 * STREAMOFF returns ERROR; no stale payload is advertised as a frame.
	 */
	if (state == VB2_BUF_STATE_ERROR)
		for (i = 0; i < vb->num_planes; i++) {
			vb->planes[i].bytesused = 0;
			vb->planes[i].data_offset = 0;
			c5_vidc_planes(vb)[i].bytesused = 0;
			c5_vidc_planes(vb)[i].data_offset = 0;
		}
	vb2_buffer_done(vb, state);
}

static void c5_vidc_pending(struct msm_vidc_inst *inst, struct vb2_queue *q,
	enum vb2_buffer_state state)
{
	struct vb2_buf_entry *entry, *next;
	mutex_lock(&inst->pendingq.lock);
	list_for_each_entry_safe(entry, next, &inst->pendingq.list, list) {
		if (entry->vb->vb2_queue != q)
			continue;
		list_del(&entry->list);
		/* This list contains buffers not yet sent to firmware. */
		c5_vidc_return_buffer(entry->vb, state);
		kfree(entry);
	}
	mutex_unlock(&inst->pendingq.lock);
}

static void c5_vidc_finish_queue(struct vb2_queue *q,
	int (*legacy_stop)(struct vb2_queue *), enum vb2_buffer_state state)
{
	struct msm_vidc_inst *inst = q->drv_priv;
	struct buf_queue *port = inst ? msm_comm_get_vb2q(inst, q->type) : NULL;
	unsigned int i;
	int rc = 0;

	if (!inst || !inst->core || !port || &port->vb2_bufq != q) {
		vb2_queue_error(q);
		return;
	}
	lockdep_assert_held(&inst->core->ioctl_lock);
	lockdep_assert_held(&port->lock);
	c5_vidc_pending(inst, q, state);
	/* All userspace queue operations remain serialized by vdev->lock.
	 * Drop only the per-port lock while waiting for firmware, allowing its
	 * EBD/FBD callbacks to acquire that lock before the STOP acknowledgement.
	 */
	mutex_unlock(&port->lock);
	if (legacy_stop)
		rc = legacy_stop(q);
	mutex_lock(&port->lock);
	if (!rc && c5_vidc_owned(q)) {
		if (inst->state >= MSM_VIDC_START &&
		    inst->state < MSM_VIDC_RELEASE_RESOURCES_DONE) {
			mutex_unlock(&port->lock);
			rc = msm_comm_try_state(inst, MSM_VIDC_RELEASE_RESOURCES_DONE);
			mutex_lock(&port->lock);
		} else if (inst->state < MSM_VIDC_RELEASE_RESOURCES_DONE ||
			   inst->state == MSM_VIDC_CORE_INVALID) {
			rc = -EIO;
		}
	}
	if (rc) {
		pr_err("c5_vidc: queue quiesce failed %d; DMA context poisoned\n", rc);
		c5_vidc_native_poison(&inst->core->resources);
		vb2_queue_error(q);
	}
	/* Firmware has acknowledged quiescence, or the whole DMA context is
	 * revoked/quarantined and no backing pages can be freed/reused by us.
	 * The latter remains a hard failure, never a successful stop assertion.
	 */
	for (i = 0; i < q->num_buffers; i++)
		if (q->bufs[i])
			c5_vidc_return_buffer(q->bufs[i], state);
}

void c5_vidc_stop_queue(struct vb2_queue *q, int (*legacy_stop)(struct vb2_queue *))
{
	c5_vidc_finish_queue(q, legacy_stop, VB2_BUF_STATE_ERROR);
}

void c5_vidc_failed_start(struct vb2_queue *q)
{
	c5_vidc_finish_queue(q, NULL, VB2_BUF_STATE_QUEUED);
}
