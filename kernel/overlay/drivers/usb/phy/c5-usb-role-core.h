/* SPDX-License-Identifier: GPL-2.0-only */
/* Literal transition helpers: external I/O is supplied by typed callbacks. */
enum c5_usb_role {
	C5_USB_NONE,
	C5_USB_DEVICE,
	C5_USB_HOST,
};

static int c5_usb_role_from_muic(unsigned long action, muic_attached_dev_t attached)
{
	if (action == MUIC_NOTIFY_CMD_DETACH)
		return C5_USB_NONE;
	if (action != MUIC_NOTIFY_CMD_ATTACH)
		return -EINVAL;
	switch (attached) {
	case ATTACHED_DEV_OTG_MUIC:
		return C5_USB_HOST;
	case ATTACHED_DEV_USB_MUIC:
	case ATTACHED_DEV_CDP_MUIC:
	case ATTACHED_DEV_UNOFFICIAL_ID_USB_MUIC:
	case ATTACHED_DEV_UNOFFICIAL_ID_CDP_MUIC:
	case ATTACHED_DEV_JIG_USB_OFF_MUIC:
	case ATTACHED_DEV_JIG_USB_ON_MUIC:
		return C5_USB_DEVICE;
	default:
		return C5_USB_NONE;
	}
}

struct c5_usb_role_io {
	void *data;
	int (*get)(void *data, bool host);
	int (*set)(void *data, bool host, bool on);
};

/* Clear the former role before publishing the new one. */
static int c5_usb_publish_role(const struct c5_usb_role_io *io, int role)
{
	int host, device, ret;

	if (role < C5_USB_NONE || role > C5_USB_HOST)
		return -EINVAL;
	host = io->get(io->data, true);
	if (host < 0)
		return host;
	device = io->get(io->data, false);
	if (device < 0)
		return device;
	if (host > 1 || device > 1 || (host && device))
		return -EINVAL;
	if (host && role != C5_USB_HOST) {
		ret = io->set(io->data, true, false);
		if (ret < 0)
			return ret;
	}
	if (device && role != C5_USB_DEVICE) {
		ret = io->set(io->data, false, false);
		if (ret < 0)
			return ret;
	}
	if (role == C5_USB_HOST && !host)
		return io->set(io->data, true, true);
	if (role == C5_USB_DEVICE && !device)
		return io->set(io->data, false, true);
	return 0;
}

struct c5_usb_source {
	bool owned;
	bool failed;
	int cleanup_error;
};

struct c5_usb_source_io {
	void *data;
	int (*get)(void *data);
	int (*set)(void *data, bool on);
};

/* Caller serializes this state; never adopt or disable a foreign source. */
static int c5_usb_source_apply(struct c5_usb_source *source,
			       const struct c5_usb_source_io *io, bool on)
{
	int state, ret, primary;

	if (on && source->failed)
		return -EIO;
	state = io->get(io->data);
	if (state < 0)
		return state;
	if (state > 1)
		return -EINVAL;
	if (!source->owned && state)
		return -EBUSY;
	if (!on && !source->owned)
		return 0;
	if (on && source->owned) {
		if (state)
			return 0;
		source->failed = true;
		return -EIO;
	}
	ret = io->set(io->data, on);
	if (ret < 0) {
		source->failed = true;
		return ret;
	}
	source->owned = on;
	ret = io->get(io->data);
	if (ret == on)
		return 0;
	primary = ret < 0 ? ret : -EIO;
	source->failed = true;
	if (on) {
		/* One cleanup only after an acknowledged owned enable. */
		source->cleanup_error = io->set(io->data, false);
		if (!source->cleanup_error)
			source->owned = false;
	}
	return primary;
}
