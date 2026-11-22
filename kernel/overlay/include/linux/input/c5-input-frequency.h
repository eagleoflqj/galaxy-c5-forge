/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _C5_INPUT_FREQUENCY_H_
#define _C5_INPUT_FREQUENCY_H_
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/cpufreq_limit.h>

#define C5_INPUT_GENRES 9

struct c5_input_frequency {
	struct mutex lock;
	unsigned int votes[C5_INPUT_GENRES];
	/* Last acknowledged request in the exclusive DVFS_TOUCH_ID slot, not Hz. */
	unsigned int applied;
	int last_error;
};

static int c5_input_frequency_vote(struct c5_input_frequency *state,
				 unsigned int genre, unsigned int floor)
{
	unsigned int old, maximum = 0, i;
	int ret = 0;

	if (genre >= C5_INPUT_GENRES || floor == UINT_MAX)
		return -EINVAL;
	mutex_lock(&state->lock);
	old = state->votes[genre];
	state->votes[genre] = floor;
	for (i = 0; i < C5_INPUT_GENRES; i++)
		maximum = max(maximum, state->votes[i]);
	if (maximum != state->applied) {
		ret = set_freq_limit(DVFS_TOUCH_ID, maximum ? maximum : UINT_MAX);
		if (ret)
			state->votes[genre] = old;
		else
			state->applied = maximum;
	}
	state->last_error = ret;
	mutex_unlock(&state->lock);
	return ret;
}
#endif
