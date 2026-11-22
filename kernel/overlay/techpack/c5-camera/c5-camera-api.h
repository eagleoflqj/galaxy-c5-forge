/* C5-local API bridge. No global kernel or HAL UAPI layout replacement. */
#ifndef C5_CAMERA_API_H
#define C5_CAMERA_API_H
#include <linux/slab.h>
#include <linux/completion.h>
#include <linux/regulator/consumer.h>
#include <linux/media-bus-format.h>
#include <media/media-entity.h>
#include <media/v4l2-common.h>
#include <media/videobuf2-v4l2.h>
static inline int c5_camera_entity_init(struct media_entity *entity,
		unsigned int pads, struct media_pad *pad, unsigned int extra)
{
	if (extra)
		return -EINVAL;
	return media_entity_pads_init(entity, pads, pad);
}
#endif
