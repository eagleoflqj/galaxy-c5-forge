// SPDX-License-Identifier: GPL-2.0-only
/* Cached C5 startup/IO metadata. No payload, MMIO read, query or control. */
#include <linux/c5_gnss_path.h>
#include <linux/cred.h>
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#define C5_GNSS_FIRST_RECORDS 64
#define C5_GNSS_LAST_RECORDS 16

struct c5_gnss_record {
	u64 sequence, monotonic_ns, arg0, arg1;
	s64 result;
	u32 stage;
};

struct c5_gnss_stat {
	u64 calls, errors, short_io, requested_bytes, completed_bytes;
};

struct c5_gnss_journal {
	u64 total;
	struct c5_gnss_stat stats[C5_GNSS_PATH_STAGE_LIMIT];
	struct c5_gnss_record first[C5_GNSS_FIRST_RECORDS];
	struct c5_gnss_record last[C5_GNSS_LAST_RECORDS];
};

static DEFINE_SPINLOCK(c5_gnss_lock);
static struct c5_gnss_journal c5_gnss_journal;

static bool c5_gnss_designated_board(void)
{
	const char *model;

	return of_root && !of_property_read_string(of_root, "model", &model) &&
		!strcmp(model, "Samsung C5LTE CHN OPEN PROJECT Rev09");
}

bool c5_gnss_path_target(void)
{
	return c5_gnss_designated_board();
}

void c5_gnss_path_note(unsigned int stage, u64 arg0, u64 arg1, s64 result)
{
	struct c5_gnss_record entry = { 0 };
	struct c5_gnss_stat *stat;
	unsigned long flags;
	bool block_io;

	if (!stage || stage >= C5_GNSS_PATH_STAGE_LIMIT ||
	    !c5_gnss_designated_board())
		return;
	entry.monotonic_ns = ktime_to_ns(ktime_get());
	entry.stage = stage;
	entry.arg0 = arg0;
	entry.arg1 = arg1;
	entry.result = result;
	block_io = stage == C5_GNSS_RMTS_BLOCK_READ ||
		stage == C5_GNSS_RMTS_BLOCK_WRITE;
	spin_lock_irqsave(&c5_gnss_lock, flags);
	entry.sequence = ++c5_gnss_journal.total;
	stat = &c5_gnss_journal.stats[stage];
	stat->calls++;
	if (result < 0)
		stat->errors++;
	if (block_io) {
		stat->requested_bytes += arg1;
		if (result >= 0) {
			stat->completed_bytes += result;
			if ((u64)result < arg1)
				stat->short_io++;
		}
	}
	if (entry.sequence <= C5_GNSS_FIRST_RECORDS)
		c5_gnss_journal.first[entry.sequence - 1] = entry;
	c5_gnss_journal.last[(entry.sequence - 1) % C5_GNSS_LAST_RECORDS] = entry;
	spin_unlock_irqrestore(&c5_gnss_lock, flags);
}

void c5_gnss_path_io(unsigned int stage, struct file *file, size_t count,
		     ssize_t result)
{
	char comm[TASK_COMM_LEN];
	unsigned int uid = __kuid_val(current_euid());
	dev_t dev;

	/* Native rmt_storage starts as root, then drops to its existing UID9999. */
	if ((uid != 0 && uid != 9999) || !file ||
	    !S_ISBLK(file_inode(file)->i_mode))
		return;
	get_task_comm(comm, current);
	if (strcmp(comm, "rmt_storage"))
		return;
	dev = file_inode(file)->i_rdev;
	/* No pathname, file offset, buffer pointer, block contents or NV value. */
	c5_gnss_path_note(stage, ((u64)MAJOR(dev) << 32) | MINOR(dev), count,
			  result);
}

static void c5_gnss_print_record(struct seq_file *s,
			       const struct c5_gnss_record *r)
{
	seq_printf(s, "record seq=%llu ns=%llu stage=%u arg0=%llu arg1=%llu result=%lld\n",
		   r->sequence, r->monotonic_ns, r->stage, r->arg0, r->arg1,
		   r->result);
}

static int c5_gnss_show(struct seq_file *s, void *unused)
{
	struct c5_gnss_journal *copy;
	unsigned long flags;
	u64 first, last, n;
	unsigned int i;

	copy = kmalloc(sizeof(*copy), GFP_KERNEL);
	if (!copy)
		return -ENOMEM;
	spin_lock_irqsave(&c5_gnss_lock, flags);
	*copy = c5_gnss_journal;
	spin_unlock_irqrestore(&c5_gnss_lock, flags);
	seq_printf(s, "format=1 total=%llu first_capacity=%u last_capacity=%u payload=none\n",
		   copy->total, C5_GNSS_FIRST_RECORDS, C5_GNSS_LAST_RECORDS);
	for (i = 1; i < C5_GNSS_PATH_STAGE_LIMIT; i++) {
		const struct c5_gnss_stat *v = &copy->stats[i];

		seq_printf(s, "stat stage=%u calls=%llu errors=%llu short=%llu requested=%llu completed=%llu\n",
			   i, v->calls, v->errors, v->short_io,
			   v->requested_bytes, v->completed_bytes);
	}
	first = min_t(u64, copy->total, C5_GNSS_FIRST_RECORDS);
	for (n = 0; n < first; n++)
		c5_gnss_print_record(s, &copy->first[n]);
	last = copy->total > C5_GNSS_LAST_RECORDS ?
		copy->total - C5_GNSS_LAST_RECORDS : 0;
	last = max_t(u64, last, first);
	for (n = last; n < copy->total; n++)
		c5_gnss_print_record(s,
			&copy->last[n % C5_GNSS_LAST_RECORDS]);
	kfree(copy);
	return 0;
}

static int c5_gnss_open(struct inode *inode, struct file *file)
{
	return single_open(file, c5_gnss_show, NULL);
}

static const struct file_operations c5_gnss_fops = {
	.owner = THIS_MODULE,
	.open = c5_gnss_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int __init c5_gnss_init(void)
{
	if (c5_gnss_designated_board())
		debugfs_create_file("c5_gnss_path", 0400, NULL, NULL,
				    &c5_gnss_fops);
	return 0;
}
late_initcall(c5_gnss_init);
MODULE_LICENSE("GPL v2");
