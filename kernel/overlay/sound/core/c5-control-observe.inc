/* SPDX-License-Identifier: GPL-2.0-only */
/* Bounded C5 diagnostic metadata only; all operations retain native behavior. */
static atomic_t c5_ctl_observe_count = ATOMIC_INIT(0);

static void c5_ctl_observe(const char *op, unsigned int command, long result)
{
	const char *leader = current->group_leader->comm;
	const char *role;

	if (!strcmp(leader, "audio@2.0-servi"))
		role = "HAL";
	else if (!strcmp(leader, "audioserver"))
		role = "server";
	else
		return;
	if (atomic_inc_return(&c5_ctl_observe_count) <= 128)
		pr_info("C5 audio ctl role=%s tgid=%d op=%s cmd=%#x rc=%ld\n",
			role, task_tgid_nr(current), op, command, result);
}

static int c5_ctl_observed_open(struct inode *inode, struct file *file)
{
	int result;

	c5_ctl_observe("open-enter", iminor(inode), 0);
	result = snd_ctl_open(inode, file);
	c5_ctl_observe("open-return", iminor(inode), result);
	return result;
}

static long c5_ctl_observed_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg)
{
	long result;

	c5_ctl_observe("ioctl-enter", cmd, 0);
	result = snd_ctl_ioctl(file, cmd, arg);
	c5_ctl_observe("ioctl-return", cmd, result);
	return result;
}

#ifdef CONFIG_COMPAT
static long c5_ctl_observed_compat(struct file *file, unsigned int cmd,
				  unsigned long arg)
{
	long result;

	c5_ctl_observe("compat-enter", cmd, 0);
	result = snd_ctl_ioctl_compat(file, cmd, arg);
	c5_ctl_observe("compat-return", cmd, result);
	return result;
}
#else
#define c5_ctl_observed_compat NULL
#endif
