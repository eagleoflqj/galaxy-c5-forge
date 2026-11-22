/* SPDX-License-Identifier: GPL-2.0-only */
/* Private C5 v2 protocol prototype; not selected for an image. */
#ifndef C5_RPM_STATS_FORMAT_H
#define C5_RPM_STATS_FORMAT_H
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <asm/div64.h>

struct c5_rpm_v2 {
	u32 stat_type;
	u32 count;
	u64 last_entered_at;
	u64 last_exited_at;
	u64 accumulated;
	u32 client_votes;
	u32 reserved[3];
};

struct c5_rpm_master_v2 {
	u32 active_cores;
	u32 numshutdowns;
	u64 shutdown_req;
	u64 wakeup_ind;
	u64 bringup_req;
	u64 bringup_ack;
	u32 wakeup_reason;
	u32 last_sleep_transition_duration;
	u32 last_wake_transition_duration;
	u32 count;
	u64 last_entered_at;
	u64 last_exited_at;
	u64 accumulated_duration;
};

/* The DT size is the relocated mapping length, not a bound on the offset. */
static inline int c5_rpm_stats_extent(u64 base, u64 span, u32 offset,
				    u32 records, u64 *address)
{
	u64 limit = 0xffffffffULL;

	if (!address || (base & 3) || (offset & 3) || records != 2 ||
	    span < 2 * sizeof(struct c5_rpm_v2) || base > limit ||
	    offset > limit - base)
		return -EINVAL;
	base += offset;
	if (span - 1 > limit - base)
		return -ERANGE;
	*address = base;
	return 0;
}

static inline int c5_rpm_master_extent(u64 base, u64 span,
				     u32 stride, u32 masters)
{
	u64 needed;

	if ((base & 7) || !masters || masters > 32 ||
	    stride < sizeof(struct c5_rpm_master_v2) || (stride & 7) ||
	    base > 0xffffffffULL || !span || span - 1 > 0xffffffffULL - base)
		return -EINVAL;
	needed = (u64)(masters - 1) * stride + sizeof(struct c5_rpm_master_v2);
	return needed <= span ? 0 : -ERANGE;
}

/* Fail closed on truncation; never advance a pointer by snprintf's full size. */
#define C5_RPM_APPEND(fmt, ...) do { \
	int c5_used; \
	if (length >= capacity) \
		return -ENOSPC; \
	c5_used = snprintf(buf + length, capacity - length, fmt, ##__VA_ARGS__); \
	if (c5_used < 0 || (size_t)c5_used >= capacity - length) \
		return -ENOSPC; \
	length += c5_used; \
} while (0)

static inline int c5_rpm_format_stats(char *buf, size_t capacity,
				    const struct c5_rpm_v2 *data, u64 now)
{
	char name[5];
	u64 last, since, total;
	size_t length = 0;

	if (!buf || !capacity || !data)
		return -EINVAL;
	memcpy(name, &data->stat_type, sizeof(data->stat_type));
	name[4] = 0;
	/* Keep the legacy divide-before-multiply diagnostic units. */
	last = data->last_exited_at - data->last_entered_at;
	do_div(last, 19200000);
	last *= 1000;
	since = now - data->last_exited_at;
	do_div(since, 19200000);
	total = data->accumulated;
	do_div(total, 19200000);
	total *= 1000;
	C5_RPM_APPEND("RPM Mode:%s\n\t count:%d\ntime in last mode(msec):%llu\n"
		"time since last mode(sec):%llu\nactual last sleep(msec):%llu\n"
		"client votes: %#010x\n\n", name, (int)data->count,
		(unsigned long long)last, (unsigned long long)since,
		(unsigned long long)total, data->client_votes);
	return length;
}

static inline int c5_rpm_format_master(char *buf, size_t capacity,
				     const char *name,
				     const struct c5_rpm_master_v2 *r)
{
	size_t length = 0;
	unsigned int bit;

	if (!buf || !capacity || !name || !r)
		return -EINVAL;
	C5_RPM_APPEND("%s\n", name);
	C5_RPM_APPEND("\tshutdown_req:0x%llX\n", (unsigned long long)r->shutdown_req);
	C5_RPM_APPEND("\twakeup_ind:0x%llX\n", (unsigned long long)r->wakeup_ind);
	C5_RPM_APPEND("\tbringup_req:0x%llX\n", (unsigned long long)r->bringup_req);
	C5_RPM_APPEND("\tbringup_ack:0x%llX\n", (unsigned long long)r->bringup_ack);
	C5_RPM_APPEND("\tlast_sleep_transition_duration:0x%x\n", r->last_sleep_transition_duration);
	C5_RPM_APPEND("\tlast_wake_transition_duration:0x%x\n", r->last_wake_transition_duration);
	C5_RPM_APPEND("\tcount:%d\n", (int)r->count);
	C5_RPM_APPEND("\tlast_entered_at:0x%llX\n", (unsigned long long)r->last_entered_at);
	C5_RPM_APPEND("\tlast_exited_at:0x%llX\n", (unsigned long long)r->last_exited_at);
	C5_RPM_APPEND("\taccumulated_duration:0x%llX\n", (unsigned long long)r->accumulated_duration);
	C5_RPM_APPEND("\twakeup_reason:0x%x\n", r->wakeup_reason);
	C5_RPM_APPEND("\tnumshutdowns:0x%x\n", r->numshutdowns);
	C5_RPM_APPEND("\tactive_cores:0x%x\n", r->active_cores);
	/* The wire bitmap is u32, not a pointer to a native unsigned long. */
	for (bit = 0; bit < 32; bit++)
		if (r->active_cores & (1U << bit))
			C5_RPM_APPEND("\t\tcore%d\n", bit);
	return length;
}
#undef C5_RPM_APPEND
#endif
