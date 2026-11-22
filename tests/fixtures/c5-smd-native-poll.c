/* SPDX-License-Identifier: GPL-2.0-only
 * Poll callback derived verbatim from Linux Foundation msm_smd_pkt.c,
 * Copyright (c) 2008-2018, The Linux Foundation. All rights reserved.
 * HOST stubs exercise readiness masks only; no device or QMI payload.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <stddef.h>
#include <poll.h>
struct smd_pkt_dev { void *ch; int poll_mode, ch_read_wait_queue, ch_lock;
    int has_reset, i, sigs_updated; };
struct file { void *private_data; };
typedef int poll_table;
static int available;
static int smd_read_avail(void *ch) { assert(ch); return available; }
static void poll_wait(struct file *f, int *q, poll_table *p) { (void)f; (void)q; (void)p; }
static void mutex_lock(int *m) { (void)m; }
static void mutex_unlock(int *m) { (void)m; }
#define pr_err(...) ((void)0)
#define pr_err_ratelimited(...) ((void)0)
#define D_POLL(...) ((void)0)
static unsigned int smd_pkt_poll(struct file *file, poll_table *wait)
{
	struct smd_pkt_dev *smd_pkt_devp;
	unsigned int mask = 0;

	smd_pkt_devp = file->private_data;
	if (!smd_pkt_devp) {
		pr_err_ratelimited("%s on a NULL device\n", __func__);
		return POLLERR;
	}

	smd_pkt_devp->poll_mode = 1;
	poll_wait(file, &smd_pkt_devp->ch_read_wait_queue, wait);
	mutex_lock(&smd_pkt_devp->ch_lock);
	if (smd_pkt_devp->has_reset || !smd_pkt_devp->ch) {
		mutex_unlock(&smd_pkt_devp->ch_lock);
		return POLLERR;
	}

	if (smd_read_avail(smd_pkt_devp->ch)) {
		mask |= POLLIN | POLLRDNORM;
		D_POLL("%s sets POLLIN for smd_pkt_dev id: %d\n",
			__func__, smd_pkt_devp->i);
	}

	if (smd_pkt_devp->sigs_updated) {
		mask |= POLLPRI;
		D_POLL("%s sets POLLPRI for smd_pkt_dev id: %d\n",
			__func__, smd_pkt_devp->i);
	}
	mutex_unlock(&smd_pkt_devp->ch_lock);

	return mask;
}

int main(void) {
 struct smd_pkt_dev dev = {.ch=(void*)1}; struct file f={&dev};
 assert(POLLIN==1 && POLLPRI==2 && POLLERR==8);
 /* Actual native status-only wake: old daemon reads with no packet. */
 available=0; dev.sigs_updated=1;
 unsigned mask=smd_pkt_poll(&f,NULL); assert(mask==POLLPRI);
 assert((mask&11)!=0 && (mask&9)==0 && dev.poll_mode==1);
 /* Data is still delivered with and without a simultaneous status update. */
 for (int sig=0;sig<2;sig++) {
  dev.sigs_updated=sig; available=1; mask=smd_pkt_poll(&f,NULL);
  assert((mask&11)&POLLIN); assert((mask&9)&POLLIN);
 }
 /* Idle does not become readable. Reset and closed-channel errors survive. */
 available=0; dev.sigs_updated=0; assert(smd_pkt_poll(&f,NULL)==0);
 dev.has_reset=1; mask=smd_pkt_poll(&f,NULL);
 assert(mask==POLLERR && (mask&11)==POLLERR && (mask&9)==POLLERR);
 dev.has_reset=0; dev.ch=NULL; assert(smd_pkt_poll(&f,NULL)==POLLERR);
 f.private_data=NULL; assert(smd_pkt_poll(&f,NULL)==POLLERR);
 return 0;
}
