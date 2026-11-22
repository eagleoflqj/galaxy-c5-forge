/* ip_conntrack proc compat - based on ip_conntrack_standalone.c
 *
 * (C) 1999-2001 Paul `Rusty' Russell
 * (C) 2002-2006 Netfilter Core Team <coreteam@netfilter.org>
 * (C) 2006-2010 Patrick McHardy <kaber@trash.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/types.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/percpu.h>
#include <linux/security.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sysctl.h>
#include <linux/user_namespace.h>
#include <net/net_namespace.h>

#include <linux/netfilter.h>
#include <net/netfilter/nf_conntrack_core.h>
#include <net/netfilter/nf_conntrack_l3proto.h>
#include <net/netfilter/nf_conntrack_l4proto.h>
#include <net/netfilter/nf_conntrack_expect.h>
#include <net/netfilter/nf_conntrack_acct.h>
#include <linux/rculist_nulls.h>
#include <linux/export.h>

struct ct_iter_state {
	struct seq_net_private p;
	struct hlist_nulls_head *hash;
	unsigned int htable_size;
	unsigned int bucket;
};

static struct hlist_nulls_node *ct_get_first(struct seq_file *seq)
{
	struct ct_iter_state *st = seq->private;
	struct hlist_nulls_node *n;

	for (st->bucket = 0;
	     st->bucket < st->htable_size;
	     st->bucket++) {
		n = rcu_dereference(
			hlist_nulls_first_rcu(&st->hash[st->bucket]));
		if (!is_a_nulls(n))
			return n;
	}
	return NULL;
}

static struct hlist_nulls_node *ct_get_next(struct seq_file *seq,
				      struct hlist_nulls_node *head)
{
	struct ct_iter_state *st = seq->private;

	head = rcu_dereference(hlist_nulls_next_rcu(head));
	while (is_a_nulls(head)) {
		if (likely(get_nulls_value(head) == st->bucket)) {
			if (++st->bucket >= st->htable_size)
				return NULL;
		}
		head = rcu_dereference(
			hlist_nulls_first_rcu(&st->hash[st->bucket]));
	}
	return head;
}

static struct hlist_nulls_node *ct_get_idx(struct seq_file *seq, loff_t pos)
{
	struct hlist_nulls_node *head = ct_get_first(seq);

	if (head)
		while (pos && (head = ct_get_next(seq, head)))
			pos--;
	return pos ? NULL : head;
}

static void *ct_seq_start(struct seq_file *seq, loff_t *pos)
	__acquires(RCU)
{
	struct ct_iter_state *st = seq->private;

	rcu_read_lock();

	nf_conntrack_get_ht(&st->hash, &st->htable_size);
	return ct_get_idx(seq, *pos);
}

static void *ct_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	(*pos)++;
	return ct_get_next(s, v);
}

static void ct_seq_stop(struct seq_file *s, void *v)
	__releases(RCU)
{
	rcu_read_unlock();
}

#ifdef CONFIG_NF_CONNTRACK_SECMARK
static void ct_show_secctx(struct seq_file *s, const struct nf_conn *ct)
{
	int ret;
	u32 len;
	char *secctx;

	ret = security_secid_to_secctx(ct->secmark, &secctx, &len);
	if (ret)
		return;

	seq_printf(s, "secctx=%s ", secctx);

	security_release_secctx(secctx, len);
}
#else
static inline void ct_show_secctx(struct seq_file *s, const struct nf_conn *ct)
{
}
#endif

/* return 0 on success, 1 in case of error */
static int ct_seq_show(struct seq_file *s, void *v)
{
	struct nf_conntrack_tuple_hash *hash = v;
	struct nf_conn *ct = nf_ct_tuplehash_to_ctrack(hash);
	const struct nf_conntrack_l3proto *l3proto;
	const struct nf_conntrack_l4proto *l4proto;
	struct net *net = seq_file_net(s);
	int ret = 0;

	NF_CT_ASSERT(ct);
	if (unlikely(!atomic_inc_not_zero(&ct->ct_general.use)))
		return 0;

	if (nf_ct_should_gc(ct)) {
		nf_ct_kill(ct);
		goto release;
	}

	/* we only want to print DIR_ORIGINAL */
	if (NF_CT_DIRECTION(hash))
		goto release;

	if (nf_ct_l3num(ct) != AF_INET || !net_eq(nf_ct_net(ct), net))
		goto release;

	l3proto = __nf_ct_l3proto_find(nf_ct_l3num(ct));
	NF_CT_ASSERT(l3proto);
	l4proto = __nf_ct_l4proto_find(nf_ct_l3num(ct), nf_ct_protonum(ct));
	NF_CT_ASSERT(l4proto);

	ret = -ENOSPC;
	seq_printf(s, "%-8s %u %ld ",
		   l4proto->name, nf_ct_protonum(ct),
		   nf_ct_expires(ct)  / HZ);

	if (l4proto->print_conntrack)
		l4proto->print_conntrack(s, ct);

	print_tuple(s, &ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple,
		    l3proto, l4proto);


	if (seq_has_overflowed(s))
		goto release;

	if (seq_print_acct(s, ct, IP_CT_DIR_ORIGINAL))
		goto release;

	if (!(test_bit(IPS_SEEN_REPLY_BIT, &ct->status)))
		seq_printf(s, "[UNREPLIED] ");

	print_tuple(s, &ct->tuplehash[IP_CT_DIR_REPLY].tuple,
		    l3proto, l4proto);


	if (seq_print_acct(s, ct, IP_CT_DIR_REPLY))
		goto release;

	if (test_bit(IPS_ASSURED_BIT, &ct->status))
		seq_printf(s, "[ASSURED] ");

	if (seq_has_overflowed(s))
		goto release;

#if defined(CONFIG_NF_CONNTRACK_MARK)
	seq_printf(s, "mark=%u ", ct->mark);
#endif

	ct_show_secctx(s, ct);

	seq_printf(s, "use=%u\n", atomic_read(&ct->ct_general.use));

	if (seq_has_overflowed(s))
		goto release;

	ret = 0;
release:
	nf_ct_put(ct);
	return ret;
}

static const struct seq_operations ct_seq_ops = {
	.start = ct_seq_start,
	.next  = ct_seq_next,
	.stop  = ct_seq_stop,
	.show  = ct_seq_show
};

static int ct_open(struct inode *inode, struct file *file)
{
	return seq_open_net(inode, file, &ct_seq_ops,
			sizeof(struct ct_iter_state));
}

static const struct file_operations ct_file_ops = {
	.owner   = THIS_MODULE,
	.open    = ct_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = seq_release_net,
};

struct ct_expect_iter_state {
	struct seq_net_private p;
	unsigned int bucket;
};

static struct hlist_node *ct_expect_get_first(struct seq_file *seq)
{
	struct ct_expect_iter_state *st = seq->private;
	struct hlist_node *n;

	for (st->bucket = 0; st->bucket < nf_ct_expect_hsize; st->bucket++) {
		n = rcu_dereference(hlist_first_rcu(&nf_ct_expect_hash[st->bucket]));
		if (n)
			return n;
	}
	return NULL;
}

static struct hlist_node *ct_expect_get_next(struct seq_file *seq,
					     struct hlist_node *head)
{
	struct ct_expect_iter_state *st = seq->private;

	head = rcu_dereference(hlist_next_rcu(head));
	while (head == NULL) {
		if (++st->bucket >= nf_ct_expect_hsize)
			return NULL;
		head = rcu_dereference(hlist_first_rcu(&nf_ct_expect_hash[st->bucket]));
	}
	return head;
}

static struct hlist_node *ct_expect_get_idx(struct seq_file *seq, loff_t pos)
{
	struct hlist_node *head = ct_expect_get_first(seq);

	if (head)
		while (pos && (head = ct_expect_get_next(seq, head)))
			pos--;
	return pos ? NULL : head;
}

static void *exp_seq_start(struct seq_file *seq, loff_t *pos)
	__acquires(RCU)
{
	rcu_read_lock();
	return ct_expect_get_idx(seq, *pos);
}

static void *exp_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	(*pos)++;
	return ct_expect_get_next(seq, v);
}

static void exp_seq_stop(struct seq_file *seq, void *v)
	__releases(RCU)
{
	rcu_read_unlock();
}

static int exp_seq_show(struct seq_file *s, void *v)
{
	struct nf_conntrack_expect *exp;
	const struct hlist_node *n = v;

	exp = hlist_entry(n, struct nf_conntrack_expect, hnode);

	if (exp->tuple.src.l3num != AF_INET ||
	    !net_eq(nf_ct_net(exp->master), seq_file_net(s)))
		return 0;

	if (exp->timeout.function)
		seq_printf(s, "%ld ", timer_pending(&exp->timeout)
			   ? (long)(exp->timeout.expires - jiffies)/HZ : 0);
	else
		seq_printf(s, "- ");

	seq_printf(s, "proto=%u ", exp->tuple.dst.protonum);

	print_tuple(s, &exp->tuple,
		    __nf_ct_l3proto_find(exp->tuple.src.l3num),
		    __nf_ct_l4proto_find(exp->tuple.src.l3num,
					 exp->tuple.dst.protonum));
	seq_putc(s, '\n');
	return seq_has_overflowed(s) ? -ENOSPC : 0;
}

static const struct seq_operations exp_seq_ops = {
	.start = exp_seq_start,
	.next = exp_seq_next,
	.stop = exp_seq_stop,
	.show = exp_seq_show
};

static int exp_open(struct inode *inode, struct file *file)
{
	return seq_open_net(inode, file, &exp_seq_ops,
			    sizeof(struct ct_expect_iter_state));
}

static const struct file_operations ip_exp_file_ops = {
	.owner   = THIS_MODULE,
	.open    = exp_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = seq_release_net,
};

static void *ct_cpu_seq_start(struct seq_file *seq, loff_t *pos)
{
	struct net *net = seq_file_net(seq);
	int cpu;

	if (*pos == 0)
		return SEQ_START_TOKEN;

	for (cpu = *pos-1; cpu < nr_cpu_ids; ++cpu) {
		if (!cpu_possible(cpu))
			continue;
		*pos = cpu+1;
		return per_cpu_ptr(net->ct.stat, cpu);
	}

	return NULL;
}

static void *ct_cpu_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	struct net *net = seq_file_net(seq);
	int cpu;

	for (cpu = *pos; cpu < nr_cpu_ids; ++cpu) {
		if (!cpu_possible(cpu))
			continue;
		*pos = cpu+1;
		return per_cpu_ptr(net->ct.stat, cpu);
	}

	return NULL;
}

static void ct_cpu_seq_stop(struct seq_file *seq, void *v)
{
}

static int ct_cpu_seq_show(struct seq_file *seq, void *v)
{
	struct net *net = seq_file_net(seq);
	unsigned int nr_conntracks = atomic_read(&net->ct.count);
	const struct ip_conntrack_stat *st = v;

	if (v == SEQ_START_TOKEN) {
		seq_printf(seq, "entries  searched found new invalid ignore delete delete_list insert insert_failed drop early_drop icmp_error  expect_new expect_create expect_delete search_restart\n");
		return 0;
	}

	seq_printf(seq, "%08x  %08x %08x %08x %08x %08x %08x %08x "
			"%08x %08x %08x %08x %08x  %08x %08x %08x %08x\n",
		   nr_conntracks,
		   st->searched,
		   (u32)(st->found + st->lookup_found),
		   st->new,
		   st->invalid,
		   st->ignore,
		   st->delete,
		   st->delete_list,
		   (u32)(st->insert + st->confirm_insert),
		   st->insert_failed,
		   st->drop,
		   st->early_drop,
		   st->error,

		   st->expect_new,
		   st->expect_create,
		   st->expect_delete,
		   st->search_restart
		);
	return 0;
}

static const struct seq_operations ct_cpu_seq_ops = {
	.start  = ct_cpu_seq_start,
	.next   = ct_cpu_seq_next,
	.stop   = ct_cpu_seq_stop,
	.show   = ct_cpu_seq_show,
};

static int ct_cpu_seq_open(struct inode *inode, struct file *file)
{
	return seq_open_net(inode, file, &ct_cpu_seq_ops,
			    sizeof(struct seq_net_private));
}

static const struct file_operations ct_cpu_seq_fops = {
	.owner   = THIS_MODULE,
	.open    = ct_cpu_seq_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = seq_release_net,
};

int nf_conntrack_ipv4_compat_init_net(struct net *net);
void nf_conntrack_ipv4_compat_fini_net(struct net *net);

#ifdef CONFIG_SYSCTL
static int compat_log_min;
static int compat_log_max = 255;

static int compat_buckets(struct ctl_table *table, int write,
			  void __user *buffer, size_t *lenp, loff_t *ppos)
{
	struct ctl_table snapshot = *table;
	unsigned int size = READ_ONCE(nf_conntrack_htable_size);

	if (write)
		return -EACCES;
	snapshot.data = &size;
	return proc_dointvec(&snapshot, 0, buffer, lenp, ppos);
}

static const struct ctl_table compat_core_table[] = {
	{ .procname = "ip_conntrack_max", .data = &nf_conntrack_max,
	  .maxlen = sizeof(int), .mode = 0644, .proc_handler = proc_dointvec },
	{ .procname = "ip_conntrack_count", .maxlen = sizeof(int),
	  .mode = 0444, .proc_handler = proc_dointvec },
	{ .procname = "ip_conntrack_buckets", .maxlen = sizeof(int),
	  .mode = 0444, .proc_handler = compat_buckets },
	{ .procname = "ip_conntrack_checksum", .maxlen = sizeof(int),
	  .mode = 0644, .proc_handler = proc_dointvec },
	{ .procname = "ip_conntrack_log_invalid", .maxlen = sizeof(int),
	  .mode = 0644, .proc_handler = proc_dointvec_minmax,
	  .extra1 = &compat_log_min, .extra2 = &compat_log_max },
	{ }
};

static int compat_core_init(struct net *net)
{
	struct ctl_table *table;

	table = kmemdup(compat_core_table, sizeof(compat_core_table), GFP_KERNEL);
	if (!table)
		return -ENOMEM;
	table[1].data = &net->ct.count;
	table[3].data = &net->ct.sysctl_checksum;
	table[4].data = &net->ct.sysctl_log_invalid;
	if (!net_eq(&init_net, net))
		table[0].mode = 0444;
	/* Match canonical conntrack visibility in unprivileged user namespaces. */
	if (net->user_ns != &init_user_ns)
		table[0].procname = NULL;
	net->ct.compat_sysctl_header =
		register_net_sysctl(net, "net/ipv4/netfilter", table);
	if (!net->ct.compat_sysctl_header) {
		kfree(table);
		return -ENOMEM;
	}
	net->ct.compat_sysctl_table = table;
	return 0;
}
#endif

void nf_conntrack_ipv4_compat_fini_net(struct net *net)
{
	if (net->ct.compat_proc_mask & 4)
		remove_proc_entry("ip_conntrack", net->proc_net_stat);
	if (net->ct.compat_proc_mask & 2)
		remove_proc_entry("ip_conntrack_expect", net->proc_net);
	if (net->ct.compat_proc_mask & 1)
		remove_proc_entry("ip_conntrack", net->proc_net);
	net->ct.compat_proc_mask = 0;
#ifdef CONFIG_SYSCTL
	if (net->ct.compat_sysctl_header) {
		unregister_net_sysctl_table(net->ct.compat_sysctl_header);
		net->ct.compat_sysctl_header = NULL;
		kfree(net->ct.compat_sysctl_table);
		net->ct.compat_sysctl_table = NULL;
	}
#endif
}

int nf_conntrack_ipv4_compat_init_net(struct net *net)
{
	struct proc_dir_entry *pde;
	kuid_t uid = make_kuid(net->user_ns, 0);
	kgid_t gid = make_kgid(net->user_ns, 0);

	if (net->ct.compat_proc_mask)
		return -EBUSY;
#ifdef CONFIG_SYSCTL
	if (net->ct.compat_sysctl_header || net->ct.compat_sysctl_table)
		return -EBUSY;
	if (compat_core_init(net))
		return -ENOMEM;
#endif
	pde = proc_create("ip_conntrack", 0440, net->proc_net, &ct_file_ops);
	if (!pde)
		goto fail;
	net->ct.compat_proc_mask |= 1;
	if (uid_valid(uid) && gid_valid(gid))
		proc_set_user(pde, uid, gid);
	pde = proc_create("ip_conntrack_expect", 0440, net->proc_net,
			  &ip_exp_file_ops);
	if (!pde)
		goto fail;
	net->ct.compat_proc_mask |= 2;
	if (uid_valid(uid) && gid_valid(gid))
		proc_set_user(pde, uid, gid);
	pde = proc_create("ip_conntrack", 0444, net->proc_net_stat,
			  &ct_cpu_seq_fops);
	if (!pde)
		goto fail;
	net->ct.compat_proc_mask |= 4;
	return 0;
fail:
	nf_conntrack_ipv4_compat_fini_net(net);
	return -ENOMEM;
}
