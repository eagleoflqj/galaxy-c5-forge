/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _C5_INPUT_PHASE_H_
#define _C5_INPUT_PHASE_H_
#include <linux/input/c5-input-config.h>

/* All calls, timer actions and provider operations execute on the one ordered
 * input workqueue with the genre mutex held.  IRQ paths only publish counts.
 * Callback signatures keep the real delayed-work implementation separate
 * from the literal-C timing model. */
struct c5_input_phase {
	struct c5_input_param param[2];
	unsigned int index;
	unsigned int count;
	bool press;
	bool change_on_release;
	void *owner;
	bool (*pending)(void *owner, unsigned int slot);
	void (*cancel)(void *owner, unsigned int slot);
	void (*schedule)(void *owner, unsigned int slot, unsigned int ms);
	void (*apply)(void *owner, const struct c5_input_param *param);
	void (*remove)(void *owner);
};

static void c5_input_phase_event(struct c5_input_phase *p, bool on)
{
	unsigned int previous;

	if (!p->press) {
		if (!on)
			return;
		p->index = 0;
		p->cancel(p->owner, 0);
		p->cancel(p->owner, 1);
		p->apply(p->owner, &p->param[0]);
		p->schedule(p->owner, 0, p->param[0].time);
		p->index = 1;
		p->press = true;
		return;
	}
	/* Only states0/1 can be pressed.  Never index param[2] while idle. */
	if (p->index > 1)
		return;
	previous = p->index ? p->index - 1 : 0;
	if (!on && !p->count) {
		if (p->pending(p->owner, previous) || !p->param[previous].time) {
			if (p->change_on_release || !p->param[previous].time) {
				p->cancel(p->owner, previous);
				p->apply(p->owner, &p->param[p->index]);
			}
			p->schedule(p->owner, p->index, p->param[p->index].time);
			if (!p->pending(p->owner, p->index) && p->param[p->index].time)
				p->schedule(p->owner, previous, p->param[previous].time);
		} else if (p->param[p->index].time) {
			p->schedule(p->owner, p->index, p->param[p->index].time);
		} else {
			p->schedule(p->owner, previous, p->param[previous].time);
		}
		p->index++;
		p->press = false;
	} else if (on && p->pending(p->owner, p->index)) {
		p->cancel(p->owner, p->index);
		p->schedule(p->owner, previous, p->param[previous].time);
	}
}

static void c5_input_phase_timeout(struct c5_input_phase *p)
{
	unsigned int restored = p->index;

	if (p->index > 2)
		return;
	if (p->index == 2 && p->pending(p->owner, 0))
		return;
	if (p->index == 2 && p->pending(p->owner, 1))
		p->index = 1;
	if (p->index < 2) {
		unsigned int previous = p->index ? p->index - 1 : 0;

		if (p->param[previous].time) {
			p->apply(p->owner, &p->param[p->index]);
			if (p->change_on_release) {
				p->schedule(p->owner, p->index, p->param[p->index].time);
				p->index++;
				p->press = false;
			}
		}
		if (restored == 2)
			p->index = restored;
	} else {
		p->remove(p->owner);
		p->index = 0;
		p->press = false;
	}
}
#endif
