/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef C5_ACTIVITY_STATS_H
#define C5_ACTIVITY_STATS_H
#ifdef CONFIG_NET_ACTIVITY_STATS
void activity_stats_update(void);
#else
static inline void activity_stats_update(void) { }
#endif
#endif
