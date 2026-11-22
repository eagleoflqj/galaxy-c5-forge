#ifndef _NF_CONNTRACK_COMMON_H
#define _NF_CONNTRACK_COMMON_H

#include <uapi/linux/netfilter/nf_conntrack_common.h>

struct ip_conntrack_stat {
	unsigned int found;
	unsigned int invalid;
	unsigned int ignore;
	unsigned int insert;
	unsigned int insert_failed;
	unsigned int drop;
	unsigned int early_drop;
	unsigned int error;
	unsigned int expect_new;
	unsigned int expect_create;
	unsigned int expect_delete;
	unsigned int search_restart;
#ifdef CONFIG_NF_CONNTRACK_PROC_COMPAT
	/* Historical events, appended so native field offsets stay unchanged. */
	unsigned int searched;
	unsigned int new;
	unsigned int delete;
	unsigned int delete_list;
	/* Legacy-only events omitted from native canonical counters. */
	unsigned int lookup_found;
	unsigned int confirm_insert;
#endif
};

/* call to create an explicit dependency on nf_conntrack. */
void need_conntrack(void);

#endif /* _NF_CONNTRACK_COMMON_H */
