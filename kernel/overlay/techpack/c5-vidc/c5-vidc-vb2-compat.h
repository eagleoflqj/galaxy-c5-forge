/* SPDX-License-Identifier: GPL-2.0-only */
/* C5-local native VB2 storage bridge; never replace a global media header. */
#ifndef C5_VIDC_VB2_COMPAT_H
#define C5_VIDC_VB2_COMPAT_H
#ifndef C5_VIDC_HOST_TEST
#include <linux/slab.h>
#include <media/videobuf2-v4l2.h>
#endif

struct c5_vidc_buffer {
	struct vb2_v4l2_buffer native;
	/* Original HAL uses reserved[] for fd/offset and output crop metadata. */
	struct v4l2_plane legacy_planes[VIDEO_MAX_PLANES];
};

static inline struct c5_vidc_buffer *c5_vidc_buffer(struct vb2_buffer *vb)
{
	return container_of(to_vb2_v4l2_buffer(vb), struct c5_vidc_buffer,
			native);
}

static inline struct v4l2_plane *c5_vidc_planes(struct vb2_buffer *vb)
{
	return c5_vidc_buffer(vb)->legacy_planes;
}

/* Caller holds the original per-port lock around native QBUF/DQBUF. */
static inline int c5_vidc_qbuf(struct vb2_queue *q, struct v4l2_buffer *b)
{
	struct vb2_buffer *vb;
	struct v4l2_plane saved[VIDEO_MAX_PLANES];
	int rc;

	if (!q || !b || b->type != q->type ||
	    b->memory != V4L2_MEMORY_USERPTR || b->index >= q->num_buffers ||
	    !b->m.planes || !b->length || b->length > VIDEO_MAX_PLANES)
		return -EINVAL;
	vb = q->bufs[b->index];
	if (!vb || !vb->num_planes || vb->num_planes > VIDEO_MAX_PLANES ||
	    b->length != vb->num_planes ||
	    (vb->state != VB2_BUF_STATE_DEQUEUED &&
	     vb->state != VB2_BUF_STATE_PREPARED))
		return -EINVAL;
	memcpy(saved, c5_vidc_planes(vb), sizeof(saved));
	memset(c5_vidc_planes(vb), 0, sizeof(saved));
	memcpy(c5_vidc_planes(vb), b->m.planes,
		vb->num_planes * sizeof(*b->m.planes));
	rc = vb2_qbuf(q, b);
	if (rc && (vb->state == VB2_BUF_STATE_DEQUEUED ||
		   vb->state == VB2_BUF_STATE_PREPARED))
		memcpy(c5_vidc_planes(vb), saved, sizeof(saved));
	return rc;
}

static inline void c5_vidc_buffer_done(struct vb2_buffer *vb,
		enum vb2_buffer_state state)
{
	unsigned int i;
	bool invalid = !vb->num_planes || vb->num_planes > VIDEO_MAX_PLANES;

	for (i = 0; !invalid && i < vb->num_planes; i++) {
		struct v4l2_plane *source = &c5_vidc_planes(vb)[i];
		if (source->length != vb->planes[i].length ||
		    source->bytesused > source->length ||
		    source->data_offset > source->length)
			invalid = true;
	}
	for (i = 0; i < vb->num_planes && i < VIDEO_MAX_PLANES; i++) {
		vb->planes[i].bytesused = invalid ? 0 :
			c5_vidc_planes(vb)[i].bytesused;
		vb->planes[i].data_offset = invalid ? 0 :
			c5_vidc_planes(vb)[i].data_offset;
		/* Firmware can return an extradata address; preserve original ABI. */
		if (!invalid)
			vb->planes[i].m.userptr = c5_vidc_planes(vb)[i].m.userptr;
	}
	if (invalid)
		state = VB2_BUF_STATE_ERROR;
	vb2_buffer_done(vb, state);
}

static inline int c5_vidc_dqbuf(struct vb2_queue *q, struct v4l2_buffer *b,
		bool nonblocking)
{
	struct vb2_buffer *vb;
	unsigned int i;
	int rc;

	if (!q || !b)
		return -EINVAL;
	rc = vb2_dqbuf(q, b, nonblocking);
	if (rc)
		return rc;
	if (!b->m.planes || b->index >= q->num_buffers ||
	    !b->length || b->length > VIDEO_MAX_PLANES)
		return -EINVAL;
	vb = q->bufs[b->index];
	if (!vb || b->length != vb->num_planes)
		return -EINVAL;
	for (i = 0; i < b->length; i++)
		memcpy(b->m.planes[i].reserved, c5_vidc_planes(vb)[i].reserved,
			sizeof(b->m.planes[i].reserved));
	return 0;
}

/* Native queue_setup's second pass must validate existing allocations. */
static inline int c5_vidc_queue_sizes(unsigned int allocated_planes,
		unsigned int *planes, unsigned int sizes[],
		unsigned int required_planes, const unsigned int required_sizes[])
{
	unsigned int i;

	if (!planes || !sizes || !required_sizes || !required_planes ||
	    required_planes > VIDEO_MAX_PLANES ||
	    (allocated_planes && allocated_planes != required_planes))
		return -EINVAL;
	for (i = 0; i < required_planes; i++)
		if (!required_sizes[i] ||
		    (allocated_planes && sizes[i] < required_sizes[i]))
			return -EINVAL;
	if (!allocated_planes) {
		memcpy(sizes, required_sizes, required_planes * sizeof(*sizes));
		*planes = required_planes;
	}
	return 0;
}
#endif
