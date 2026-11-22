/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_C5_MPM_REPLAY_H
#define _LINUX_C5_MPM_REPLAY_H

#include <linux/errno.h>

#if defined(CONFIG_QTI_MPM_C5LTE) && defined(CONFIG_HARDIRQS_SW_RESEND)
/* Built-in C5 MPM caller only; no module or userspace injection interface. */
int c5_irq_replay_pending(unsigned int irq);
#else
static inline int c5_irq_replay_pending(unsigned int irq)
{
	(void)irq;
	return -EOPNOTSUPP;
}
#endif
#endif
