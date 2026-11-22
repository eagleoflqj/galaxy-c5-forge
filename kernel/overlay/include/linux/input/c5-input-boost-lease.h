/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _C5_INPUT_BOOST_LEASE_H_
#define _C5_INPUT_BOOST_LEASE_H_
#include <linux/types.h>
/* Private input-client cleanup is idempotent and never releases a stale token.
 * Other native clients retain the public single-state API and its results. */
int c5_input_boost_lease(bool requested, u64 *token);
#endif
