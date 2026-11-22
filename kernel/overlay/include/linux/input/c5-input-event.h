/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _C5_INPUT_EVENT_H_
#define _C5_INPUT_EVENT_H_
#include <linux/input.h>
#include <linux/errno.h>

enum c5_input_genre {
	C5_TOUCH, C5_MULTITOUCH, C5_KEY, C5_TOUCHKEY, C5_KEYBOARD,
	C5_MOUSE, C5_WHEEL, C5_PEN, C5_HOVER, C5_GENRE_COUNT,
};

/* Owned by exactly one input_dev, under that device's event_lock.  Keep the
 * press-time classification until release; MSC_SCAN need not repeat then.
 * No input values, touch coordinates or tracking IDs leave the input core. */
struct c5_input_event_state {
	u8 key_genre[KEY_CNT];
	u32 count[C5_GENRE_COUNT];
	bool scan;
	bool has_pen;
	bool has_mt;
};

static unsigned int c5_input_event_key(struct c5_input_event_state *state,
				      unsigned int code, int value)
{
	unsigned int genre, saved;
	bool scanned = state->scan;

	state->scan = false;
	if (code >= KEY_CNT || (value != 0 && value != 1))
		return 0;
	saved = state->key_genre[code];
	if (!value) {
		if (saved) {
			genre = saved - 1;
			if (state->count[genre])
				state->count[genre]--;
			state->key_genre[code] = 0;
		}
		return 0;
	}
	if (saved)
		return 0;
	switch (code) {
	case KEY_BACK:
	case KEY_HOMEPAGE:
	case 254: /* Samsung KEY_RECENT; native APPSELECT is a distinct code. */
	case KEY_APPSELECT:
		genre = C5_TOUCHKEY;
		break;
	case KEY_VOLUMEUP:
	case KEY_VOLUMEDOWN:
	case KEY_POWER:
		genre = C5_KEY;
		break;
	case BTN_TOOL_PEN:
		genre = C5_HOVER;
		break;
	case BTN_TOUCH:
		if (state->has_mt && !state->has_pen)
			return 0;
		genre = state->has_pen ? C5_PEN : C5_TOUCH;
		break;
	case BTN_LEFT:
	case BTN_RIGHT:
	case BTN_MIDDLE:
		genre = C5_MOUSE;
		break;
	default:
		if (!scanned)
			return 0;
		genre = C5_KEYBOARD;
		break;
	}
	state->key_genre[code] = genre + 1;
	state->count[genre]++;
	return 0;
}

static void c5_input_event_mt(struct c5_input_event_state *state,
			      unsigned int contacts)
{
	if (!state->has_mt || state->has_pen)
		return;
	state->count[C5_TOUCH] = !!contacts;
	state->count[C5_MULTITOUCH] = contacts ? contacts - 1 : 0;
}

static unsigned int c5_input_event_value(struct c5_input_event_state *state,
					unsigned int type, unsigned int code,
					int value)
{
	if (type == EV_KEY)
		return c5_input_event_key(state, code, value);
	if (type == EV_MSC && code == MSC_SCAN)
		state->scan = true;
	if (type == EV_SYN && code == SYN_REPORT)
		state->scan = false;
	/* A wheel detent has no matching release; it starts a finite head pulse. */
	if (type == EV_REL && code == REL_WHEEL && value)
		return 1U << C5_WHEEL;
	return 0;
}

/* Protected by the controller's IRQ-safe event lock.  Publishing and taking
 * a snapshot share this lock, including the worker's final disarm decision.
 * A coalesced press/release retains the press bit and final active count. */
struct c5_input_pending {
	u64 count[C5_GENRE_COUNT];
	unsigned int activity;
	bool dirty;
	bool armed;
};

struct c5_input_snapshot {
	u64 count[C5_GENRE_COUNT];
	unsigned int activity;
};

static int c5_input_pending_publish(struct c5_input_pending *pending,
				   const u32 before[C5_GENRE_COUNT],
				   const u32 after[C5_GENRE_COUNT],
				   unsigned int pulses)
{
	u64 totals[C5_GENRE_COUNT];
	unsigned int activity = pulses;
	bool changed = !!pulses;
	unsigned int i;

	if (pulses >> C5_GENRE_COUNT)
		return -EINVAL;
	for (i = 0; i < C5_GENRE_COUNT; i++) {
		u64 remaining;

		if (pending->count[i] < before[i])
			return -ERANGE;
		remaining = pending->count[i] - before[i];
		if (~(u64)0 - remaining < after[i])
			return -ERANGE;
		totals[i] = remaining + after[i];
		changed |= before[i] != after[i];
		if (after[i] > before[i])
			activity |= 1U << i;
	}
	if (!changed)
		return 0;
	for (i = 0; i < C5_GENRE_COUNT; i++)
		pending->count[i] = totals[i];
	pending->activity |= activity;
	pending->dirty = true;
	if (!pending->armed) {
		pending->armed = true;
		return 1;
	}
	return 0;
}

static bool c5_input_pending_take(struct c5_input_pending *pending,
				  struct c5_input_snapshot *snapshot)
{
	unsigned int i;

	if (!pending->dirty) {
		pending->armed = false;
		return false;
	}
	for (i = 0; i < C5_GENRE_COUNT; i++)
		snapshot->count[i] = pending->count[i];
	snapshot->activity = pending->activity;
	pending->activity = 0;
	pending->dirty = false;
	return true;
}
#endif
