/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef C5_VIDC_LIFECYCLE_H
#define C5_VIDC_LIFECYCLE_H
struct vb2_queue;
void c5_vidc_stop_queue(struct vb2_queue *q, int (*legacy_stop)(struct vb2_queue *));
void c5_vidc_failed_start(struct vb2_queue *q);
#endif
