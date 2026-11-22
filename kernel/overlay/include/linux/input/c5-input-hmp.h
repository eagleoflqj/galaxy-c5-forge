/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _C5_INPUT_HMP_H_
#define _C5_INPUT_HMP_H_
#include <linux/mutex.h>
#include <linux/input/c5-input-frequency.h>
#include <linux/input/c5-input-boost-lease.h>

struct c5_input_hmp {
	struct mutex lock;
	bool wanted[C5_INPUT_GENRES];
	u64 token;
	int last_error;
};

static int c5_input_hmp_vote(struct c5_input_hmp *state,
			     unsigned int genre, unsigned int requested)
{
	bool any = false;
	unsigned int i;
	int ret = 0;

	if (genre >= C5_INPUT_GENRES || requested > 1)
		return -EINVAL;
	mutex_lock(&state->lock);
	/* These are desired inputs, not a claim that a rejected lease applied. */
	state->wanted[genre] = requested;
	for (i = 0; i < C5_INPUT_GENRES; i++)
		any |= state->wanted[i];
	if (any || state->token)
		ret = c5_input_boost_lease(any, &state->token);
	state->last_error = ret;
	mutex_unlock(&state->lock);
	return ret;
}
#endif
