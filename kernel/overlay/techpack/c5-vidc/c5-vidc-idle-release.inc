/* SPDX-License-Identifier: GPL-2.0-only */
/* A sessionless, allocation-free probe has no firmware resources to release.
 * Never turn that no-op into a backwards state transition and global revoke.
 */
static bool c5_vidc_release_is_empty(struct msm_vidc_inst *inst)
{
	struct msm_vidc_list *lists[] = {
		&inst->registeredbufs, &inst->pendingq, &inst->scratchbufs,
		&inst->persistbufs, &inst->outputbufs, &inst->pending_getpropq,
	};
	unsigned int i;
	bool empty;
	int state;

	if (!inst->core)
		return false;
	lockdep_assert_held(&inst->core->ioctl_lock);
	state = READ_ONCE(inst->state);
	if (state != MSM_VIDC_CORE_UNINIT_DONE && state != MSM_VIDC_CORE_INIT &&
	    state != MSM_VIDC_CORE_INIT_DONE && state != MSM_VIDC_CORE_UNINIT)
		return false;
	if (READ_ONCE(inst->core->state) == VIDC_CORE_INVALID ||
	    READ_ONCE(inst->session) || inst->extradata_handle)
		return false;
	for (i = 0; i < MAX_PORT_NUM; i++) {
		struct vb2_queue *q = &inst->bufq[i].vb2_bufq;
		if (q->num_buffers || q->streaming || q->start_streaming_called)
			return false;
	}
	for (i = 0; i < ARRAY_SIZE(lists); i++) {
		mutex_lock(&lists[i]->lock);
		empty = list_empty(&lists[i]->list);
		mutex_unlock(&lists[i]->lock);
		if (!empty)
			return false;
	}
	pr_info("c5_vidc: empty sessionless release; state=%d\n", state);
	return true;
}
