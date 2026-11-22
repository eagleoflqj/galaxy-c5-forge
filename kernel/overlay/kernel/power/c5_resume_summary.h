/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_C5_RESUME_SUMMARY_H
#define _LINUX_C5_RESUME_SUMMARY_H
#include <linux/types.h>
/* The caller owns a NUL-terminated record for the duration of this call. */
bool c5_resume_summary_append_record(const char *record, bool first_gpio);
void c5_resume_summary_arm_gpio(void);
void c5_resume_summary_disarm_gpio(void);
#endif
