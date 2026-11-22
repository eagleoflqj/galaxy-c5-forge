/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_C5_CPU_PCNOC_H
#define _LINUX_C5_CPU_PCNOC_H

/* 0=pending,1=held,negative=unavailable/error. Read-only RAM status. */
int c5_cpu_pcnoc_status(void);

#endif
