/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef C5_UID_STAT_H
#define C5_UID_STAT_H
#include <linux/types.h>
#ifdef CONFIG_UID_STAT
int uid_stat_tcp_snd(uid_t uid, int size);
int uid_stat_tcp_rcv(uid_t uid, int size);
#else
#define uid_stat_tcp_snd(uid, size) do { } while (0)
#define uid_stat_tcp_rcv(uid, size) do { } while (0)
#endif
#endif
