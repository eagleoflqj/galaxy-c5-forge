/* Copyright (c) 2014-2017, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/rtmutex.h>
#include <linux/clk.h>
#include <linux/msm-bus.h>
#include "msm_bus_core.h"
#include "msm_bus_adhoc.h"
#include <trace/events/trace_msm_bus.h>

#define NUM_CL_HANDLES	50
#define NUM_LNODES	3
#define MAX_STR_CL	50

struct bus_search_type {
	struct list_head link;
	struct list_head node_list;
};

struct handle_type {
	int num_entries;
	struct msm_bus_client **cl_list;
};

static struct handle_type handle_list;
static LIST_HEAD(input_list);
static LIST_HEAD(apply_list);
static LIST_HEAD(commit_list);

DEFINE_RT_MUTEX(msm_bus_adhoc_lock);

/* Diagnostic-only successful-allocation provenance; caller holds arb lock. */
#if defined(CONFIG_QTI_MPM_C5LTE) && defined(CONFIG_DEBUG_FS) && !defined(MODULE)
struct c5_ebi_allocation_provenance {
	struct msm_bus_node_device_type *node;
	struct link_node *list;
	int capacity;
};
static struct c5_ebi_allocation_provenance c5_ebi_allocation;

static void c5_ebi_snapshot_note_alloc(struct msm_bus_node_device_type *node)
{
	if (!node->node_info || node->node_info->id != 512 ||
	    node->node_info->slv_rpm_id != 0 || node->num_lnodes <= 0 ||
	    IS_ERR_OR_NULL(node->lnode_list))
		return;
	c5_ebi_allocation.node = node;
	c5_ebi_allocation.list = node->lnode_list;
	c5_ebi_allocation.capacity = node->num_lnodes;
}
#define C5_EBI_NOTE_SUCCESSFUL_ALLOC(node) c5_ebi_snapshot_note_alloc(node)
#else
#define C5_EBI_NOTE_SUCCESSFUL_ALLOC(node) do { } while (0)
#endif

static bool chk_bl_list(struct list_head *black_list, unsigned int id)
{
	struct msm_bus_node_device_type *bus_node = NULL;

	list_for_each_entry(bus_node, black_list, link) {
		if (bus_node->node_info->id == id)
			return true;
	}
	return false;
}

static void copy_remaining_nodes(struct list_head *edge_list, struct list_head
	*traverse_list, struct list_head *route_list)
{
	struct bus_search_type *search_node;

	if (list_empty(edge_list) && list_empty(traverse_list))
		return;

	search_node = kzalloc(sizeof(struct bus_search_type), GFP_KERNEL);
	INIT_LIST_HEAD(&search_node->node_list);
	list_splice_init(edge_list, traverse_list);
	list_splice_init(traverse_list, &search_node->node_list);
	list_add_tail(&search_node->link, route_list);
}

/*
 * Duplicate instantiaion from msm_bus_arb.c. Todo there needs to be a
 * "util" file for these common func/macros.
 *
 */
uint64_t msm_bus_div64(uint64_t num, unsigned int base)
{
	uint64_t *n = &num;

	if ((num > 0) && (num < base))
		return 1;

	switch (base) {
	case 0:
		WARN(1, "AXI: Divide by 0 attempted\n");
	case 1: return num;
	case 2: return (num >> 1);
	case 4: return (num >> 2);
	case 8: return (num >> 3);
	case 16: return (num >> 4);
	case 32: return (num >> 5);
	}

	do_div(*n, base);
	return *n;
}

int msm_bus_device_match_adhoc(struct device *dev, void *id)
{
	int ret = 0;
	struct msm_bus_node_device_type *bnode = to_msm_bus_node(dev);

	if (bnode)
		ret = (bnode->node_info->id == *(unsigned int *)id);
	else
		ret = 0;

	return ret;
}

static int gen_lnode(struct device *dev,
			int next_hop, int prev_idx, const char *cl_name)
{
	struct link_node *lnode;
	struct msm_bus_node_device_type *cur_dev = NULL;
	int lnode_idx = -1;

	if (!dev)
		goto exit_gen_lnode;

	cur_dev = to_msm_bus_node(dev);
	if (!cur_dev) {
		MSM_BUS_ERR("%s: Null device ptr", __func__);
		goto exit_gen_lnode;
	}

	if (!cur_dev->num_lnodes) {
		cur_dev->lnode_list = devm_kzalloc(dev,
				sizeof(struct link_node) * NUM_LNODES,
								GFP_KERNEL);
		if (!cur_dev->lnode_list)
			goto exit_gen_lnode;

		lnode = cur_dev->lnode_list;
		cur_dev->num_lnodes = NUM_LNODES;
		C5_EBI_NOTE_SUCCESSFUL_ALLOC(cur_dev);
		lnode_idx = 0;
	} else {
		int i;

		for (i = 0; i < cur_dev->num_lnodes; i++) {
			if (!cur_dev->lnode_list[i].in_use)
				break;
		}

		if (i < cur_dev->num_lnodes) {
			lnode = &cur_dev->lnode_list[i];
			lnode_idx = i;
		} else {
			struct link_node *realloc_list;
			size_t cur_size = sizeof(struct link_node) *
					cur_dev->num_lnodes;

			cur_dev->num_lnodes += NUM_LNODES;
			realloc_list = msm_bus_realloc_devmem(
					dev,
					cur_dev->lnode_list,
					cur_size,
					sizeof(struct link_node) *
					cur_dev->num_lnodes, GFP_KERNEL);

			if (!realloc_list)
				goto exit_gen_lnode;

			cur_dev->lnode_list = realloc_list;
			C5_EBI_NOTE_SUCCESSFUL_ALLOC(cur_dev);
			lnode = &cur_dev->lnode_list[i];
			lnode_idx = i;
		}
	}

	lnode->in_use = 1;
	lnode->cl_name = cl_name;
	if (next_hop == cur_dev->node_info->id) {
		lnode->next = -1;
		lnode->next_dev = NULL;
	} else {
		lnode->next = prev_idx;
		lnode->next_dev = bus_find_device(&msm_bus_type, NULL,
					(void *) &next_hop,
					msm_bus_device_match_adhoc);
	}

	memset(lnode->lnode_ib, 0, sizeof(uint64_t) * NUM_CTX);
	memset(lnode->lnode_ab, 0, sizeof(uint64_t) * NUM_CTX);

exit_gen_lnode:
	return lnode_idx;
}

static int remove_lnode(struct msm_bus_node_device_type *cur_dev,
				int lnode_idx)
{
	int ret = 0;

	if (!cur_dev) {
		MSM_BUS_ERR("%s: Null device ptr", __func__);
		ret = -ENODEV;
		goto exit_remove_lnode;
	}

	if (lnode_idx != -1) {
		if (!cur_dev->num_lnodes ||
				(lnode_idx > (cur_dev->num_lnodes - 1))) {
			MSM_BUS_ERR("%s: Invalid Idx %d, num_lnodes %d",
				__func__, lnode_idx, cur_dev->num_lnodes);
			ret = -ENODEV;
			goto exit_remove_lnode;
		}

		cur_dev->lnode_list[lnode_idx].next = -1;
		cur_dev->lnode_list[lnode_idx].next_dev = NULL;
		cur_dev->lnode_list[lnode_idx].in_use = 0;
		cur_dev->lnode_list[lnode_idx].cl_name = NULL;
	}

exit_remove_lnode:
	return ret;
}

static int prune_path(struct list_head *route_list, int dest, int src,
				struct list_head *black_list, int found,
				const char *cl_name)
{
	struct bus_search_type *search_node, *temp_search_node;
	struct msm_bus_node_device_type *bus_node;
	struct list_head *bl_list;
	struct list_head *temp_bl_list;
	int search_dev_id = dest;
	struct device *dest_dev = bus_find_device(&msm_bus_type, NULL,
					(void *) &dest,
					msm_bus_device_match_adhoc);
	int lnode_hop = -1;

	if (!found)
		goto reset_links;

	if (!dest_dev) {
		MSM_BUS_ERR("%s: Can't find dest dev %d", __func__, dest);
		goto exit_prune_path;
	}

	lnode_hop = gen_lnode(dest_dev, search_dev_id, lnode_hop, cl_name);

	list_for_each_entry_reverse(search_node, route_list, link) {
		list_for_each_entry(bus_node, &search_node->node_list, link) {
			unsigned int i;

			for (i = 0; i < bus_node->node_info->num_connections;
									i++) {
				if (bus_node->node_info->connections[i] ==
								search_dev_id) {
					dest_dev = bus_find_device(
						&msm_bus_type,
						NULL,
						(void *)
						&bus_node->node_info->
						id,
						msm_bus_device_match_adhoc);

					if (!dest_dev) {
						lnode_hop = -1;
						goto reset_links;
					}

					lnode_hop = gen_lnode(dest_dev,
							search_dev_id,
							lnode_hop, cl_name);
					search_dev_id =
						bus_node->node_info->id;
					break;
				}
			}
		}
	}
reset_links:
	list_for_each_entry_safe(search_node, temp_search_node, route_list,
									link) {
		list_for_each_entry(bus_node, &search_node->node_list,
								link)
			bus_node->node_info->is_traversed = false;

		list_del(&search_node->link);
		kfree(search_node);
	}

	list_for_each_safe(bl_list, temp_bl_list, black_list)
		list_del(bl_list);

exit_prune_path:
	return lnode_hop;
}

static void setup_bl_list(struct msm_bus_node_device_type *node,
				struct list_head *black_list)
{
	unsigned int i;

	for (i = 0; i < node->node_info->num_blist; i++) {
		struct msm_bus_node_device_type *bdev;

		bdev = to_msm_bus_node(node->node_info->black_connections[i]);
		list_add_tail(&bdev->link, black_list);
	}
}

static int getpath(struct device *src_dev, int dest, const char *cl_name)
{
	struct list_head traverse_list;
	struct list_head edge_list;
	struct list_head route_list;
	struct list_head black_list;
	struct msm_bus_node_device_type *src_node;
	struct bus_search_type *search_node;
	int found = 0;
	int depth_index = 0;
	int first_hop = -1;
	int src;

	INIT_LIST_HEAD(&traverse_list);
	INIT_LIST_HEAD(&edge_list);
	INIT_LIST_HEAD(&route_list);
	INIT_LIST_HEAD(&black_list);

	if (!src_dev) {
		MSM_BUS_ERR("%s: Cannot locate src dev ", __func__);
		goto exit_getpath;
	}

	src_node = to_msm_bus_node(src_dev);
	if (!src_node) {
		MSM_BUS_ERR("%s:Fatal, Source node not found", __func__);
		goto exit_getpath;
	}
	src = src_node->node_info->id;
	list_add_tail(&src_node->link, &traverse_list);

	while ((!found && !list_empty(&traverse_list))) {
		struct msm_bus_node_device_type *bus_node = NULL;
		/* Locate dest_id in the traverse list */
		list_for_each_entry(bus_node, &traverse_list, link) {
			if (bus_node->node_info->id == dest) {
				found = 1;
				break;
			}
		}

		if (!found) {
			unsigned int i;
			/* Setup the new edge list */
			list_for_each_entry(bus_node, &traverse_list, link) {
				/* Setup list of black-listed nodes */
				setup_bl_list(bus_node, &black_list);

				for (i = 0; i < bus_node->node_info->
						num_connections; i++) {
					bool skip;
					struct msm_bus_node_device_type
							*node_conn;
					node_conn =
					to_msm_bus_node(bus_node->node_info->
						dev_connections[i]);
					if (node_conn->node_info->
							is_traversed) {
						MSM_BUS_ERR("Circ Path %d\n",
						node_conn->node_info->id);
						goto reset_traversed;
					}
					skip = chk_bl_list(&black_list,
							bus_node->node_info->
							connections[i]);
					if (!skip) {
						list_add_tail(&node_conn->link,
							&edge_list);
						node_conn->node_info->
							is_traversed = true;
					}
				}
			}

			/* Keep tabs of the previous search list */
			search_node = kzalloc(sizeof(struct bus_search_type),
					 GFP_KERNEL);
			INIT_LIST_HEAD(&search_node->node_list);
			list_splice_init(&traverse_list,
					 &search_node->node_list);
			/* Add the previous search list to a route list */
			list_add_tail(&search_node->link, &route_list);
			/* Advancing the list depth */
			depth_index++;
			list_splice_init(&edge_list, &traverse_list);
		}
	}
reset_traversed:
	copy_remaining_nodes(&edge_list, &traverse_list, &route_list);
	first_hop = prune_path(&route_list, dest, src, &black_list, found,
								cl_name);

exit_getpath:
	return first_hop;
}

static uint64_t scheme1_agg_scheme(struct msm_bus_node_device_type *bus_dev,
			struct msm_bus_node_device_type *fab_dev, int ctx)
{
	uint64_t max_ib;
	uint64_t sum_ab;
	uint64_t bw_max_hz;
	uint32_t util_fact = 0;
	uint32_t vrail_comp = 0;
	struct node_util_levels_type *utils;
	int i;
	int num_util_levels;

	/*
	 *  Account for Util factor and vrail comp.
	 *  Util factor is picked according to the current sum(AB) for this
	 *  node and for this context.
	 *  Vrail comp is fixed for the entire performance range.
	 *  They default to 100 if absent.
	 *
	 *  The aggregated clock is computed as:
	 *  Freq_hz = max((sum(ab) * util_fact)/num_chan, max(ib)/vrail_comp)
	 *				/ bus-width
	 */
	if (bus_dev->node_info->agg_params.num_util_levels) {
		utils = bus_dev->node_info->agg_params.util_levels;
		num_util_levels =
			bus_dev->node_info->agg_params.num_util_levels;
	} else {
		utils = fab_dev->node_info->agg_params.util_levels;
		num_util_levels =
			fab_dev->node_info->agg_params.num_util_levels;
	}

	sum_ab = bus_dev->node_bw[ctx].sum_ab;
	max_ib = bus_dev->node_bw[ctx].max_ib;

	for (i = 0; i < num_util_levels; i++) {
		if (sum_ab < utils[i].threshold) {
			util_fact = utils[i].util_fact;
			break;
		}
	}
	if (i == num_util_levels)
		util_fact = utils[(num_util_levels - 1)].util_fact;

	vrail_comp = bus_dev->node_info->agg_params.vrail_comp ?
			bus_dev->node_info->agg_params.vrail_comp :
			fab_dev->node_info->agg_params.vrail_comp;

	bus_dev->node_bw[ctx].vrail_used = vrail_comp;
	bus_dev->node_bw[ctx].util_used = util_fact;

	if (util_fact && (util_fact != 100)) {
		sum_ab *= util_fact;
		sum_ab = msm_bus_div64(sum_ab, 100);
	}

	if (vrail_comp && (vrail_comp != 100)) {
		max_ib *= 100;
		max_ib = msm_bus_div64(max_ib, vrail_comp);
	}

	/* Account for multiple channels if any */
	if (bus_dev->node_info->agg_params.num_aggports > 1)
		sum_ab = msm_bus_div64(sum_ab,
				bus_dev->node_info->agg_params.num_aggports);

	if (!bus_dev->node_info->agg_params.buswidth) {
		MSM_BUS_WARN("No bus width found for %d. Using default\n",
					bus_dev->node_info->id);
		bus_dev->node_info->agg_params.buswidth = 8;
	}

	bw_max_hz = max(max_ib, sum_ab);
	bw_max_hz = msm_bus_div64(bw_max_hz,
				bus_dev->node_info->agg_params.buswidth);

	return bw_max_hz;
}

static uint64_t legacy_agg_scheme(struct msm_bus_node_device_type *bus_dev,
			struct msm_bus_node_device_type *fab_dev, int ctx)
{
	uint64_t max_ib;
	uint64_t sum_ab;
	uint64_t bw_max_hz;
	uint32_t util_fact = 0;
	uint32_t vrail_comp = 0;

	/*
	 *  Util_fact and vrail comp are obtained from fabric/Node's dts
	 *  properties and are fixed for the entire performance range.
	 *  They default to 100 if absent.
	 *
	 *  The clock frequency is computed as:
	 *  Freq_hz = max((sum(ab) * util_fact)/num_chan, max(ib)/vrail_comp)
	 *				/ bus-width
	 */
	util_fact = fab_dev->node_info->agg_params.util_levels[0].util_fact;
	vrail_comp = fab_dev->node_info->agg_params.vrail_comp;

	if (bus_dev->node_info->agg_params.num_util_levels)
		util_fact =
		bus_dev->node_info->agg_params.util_levels[0].util_fact ?
		bus_dev->node_info->agg_params.util_levels[0].util_fact :
		util_fact;

	vrail_comp = bus_dev->node_info->agg_params.vrail_comp ?
			bus_dev->node_info->agg_params.vrail_comp :
			vrail_comp;

	bus_dev->node_bw[ctx].vrail_used = vrail_comp;
	bus_dev->node_bw[ctx].util_used = util_fact;
	sum_ab = bus_dev->node_bw[ctx].sum_ab;
	max_ib = bus_dev->node_bw[ctx].max_ib;

	if (util_fact && (util_fact != 100)) {
		sum_ab *= util_fact;
		sum_ab = msm_bus_div64(sum_ab, 100);
	}

	if (vrail_comp && (vrail_comp != 100)) {
		max_ib *= 100;
		max_ib = msm_bus_div64(max_ib, vrail_comp);
	}

	/* Account for multiple channels if any */
	if (bus_dev->node_info->agg_params.num_aggports > 1)
		sum_ab = msm_bus_div64(sum_ab,
				bus_dev->node_info->agg_params.num_aggports);

	if (!bus_dev->node_info->agg_params.buswidth) {
		MSM_BUS_WARN("No bus width found for %d. Using default\n",
					bus_dev->node_info->id);
		bus_dev->node_info->agg_params.buswidth = 8;
	}

	bw_max_hz = max(max_ib, sum_ab);
	bw_max_hz = msm_bus_div64(bw_max_hz,
				bus_dev->node_info->agg_params.buswidth);

	return bw_max_hz;
}

static uint64_t aggregate_bus_req(struct msm_bus_node_device_type *bus_dev,
									int ctx)
{
	uint64_t bw_hz = 0;
	int i;
	struct msm_bus_node_device_type *fab_dev = NULL;
	uint32_t agg_scheme;
	uint64_t max_ib = 0;
	uint64_t sum_ab = 0;

	if (!bus_dev || !to_msm_bus_node(bus_dev->node_info->bus_device)) {
		MSM_BUS_ERR("Bus node pointer is Invalid");
		goto exit_agg_bus_req;
	}

	fab_dev = to_msm_bus_node(bus_dev->node_info->bus_device);
	for (i = 0; i < bus_dev->num_lnodes; i++) {
		max_ib = max(max_ib, bus_dev->lnode_list[i].lnode_ib[ctx]);
		sum_ab += bus_dev->lnode_list[i].lnode_ab[ctx];
	}

	bus_dev->node_bw[ctx].sum_ab = sum_ab;
	bus_dev->node_bw[ctx].max_ib = max_ib;

	if (bus_dev->node_info->agg_params.agg_scheme != AGG_SCHEME_NONE)
		agg_scheme = bus_dev->node_info->agg_params.agg_scheme;
	else
		agg_scheme = fab_dev->node_info->agg_params.agg_scheme;

	switch (agg_scheme) {
	case AGG_SCHEME_1:
		bw_hz = scheme1_agg_scheme(bus_dev, fab_dev, ctx);
		break;
	case AGG_SCHEME_LEG:
		bw_hz = legacy_agg_scheme(bus_dev, fab_dev, ctx);
		break;
	}

exit_agg_bus_req:
	return bw_hz;
}


static void del_inp_list(struct list_head *list)
{
	struct rule_update_path_info *rule_node;
	struct rule_update_path_info *rule_node_tmp;

	list_for_each_entry_safe(rule_node, rule_node_tmp, list, link) {
		list_del(&rule_node->link);
		rule_node->added = false;
	}
}

static void del_op_list(struct list_head *list)
{
	struct rule_apply_rcm_info *rule;
	struct rule_apply_rcm_info *rule_tmp;

	list_for_each_entry_safe(rule, rule_tmp, list, link)
		list_del(&rule->link);
}

static int msm_bus_apply_rules(struct list_head *list, bool after_clk_commit)
{
	struct rule_apply_rcm_info *rule;
	struct device *dev = NULL;
	struct msm_bus_node_device_type *dev_info = NULL;
	int ret = 0;

	list_for_each_entry(rule, list, link) {
		if (!rule)
			continue;

		if (rule && (rule->after_clk_commit != after_clk_commit))
			continue;

		dev = bus_find_device(&msm_bus_type, NULL,
				(void *) &rule->id,
				msm_bus_device_match_adhoc);

		if (!dev) {
			MSM_BUS_ERR("Can't find dev node for %d", rule->id);
			continue;
		}
		dev_info = to_msm_bus_node(dev);

		ret = msm_bus_enable_limiter(dev_info, rule->throttle,
							rule->lim_bw);
		if (ret)
			MSM_BUS_ERR("Failed to set limiter for %d", rule->id);
	}

	return ret;
}

static int commit_data(void)
{
	bool rules_registered = msm_rule_are_rules_registered();
	int ret;

	if (rules_registered) {
		msm_rules_update_path(&input_list, &apply_list);
		msm_bus_apply_rules(&apply_list, false);
	}

	ret = msm_bus_commit_data(&commit_list);

	if (rules_registered) {
		msm_bus_apply_rules(&apply_list, true);
		if (!ret)
			del_inp_list(&input_list);
		del_op_list(&apply_list);
	}
	if (!ret)
		INIT_LIST_HEAD(&input_list);
	INIT_LIST_HEAD(&apply_list);
	/* The fabric owns unlinking successful nodes; never drop failed votes. */
	return ret;
}

static void add_node_to_clist(struct msm_bus_node_device_type *node)
{
	struct msm_bus_node_device_type *node_parent =
			to_msm_bus_node(node->node_info->bus_device);

	if (!node->dirty) {
		list_add_tail(&node->link, &commit_list);
		node->dirty = true;
	}

	if (!node_parent->dirty) {
		list_add_tail(&node_parent->link, &commit_list);
		node_parent->dirty = true;
	}
}

static int update_path(struct device *src_dev, int dest, uint64_t act_req_ib,
			uint64_t act_req_bw, uint64_t slp_req_ib,
			uint64_t slp_req_bw, uint64_t cur_ib, uint64_t cur_bw,
			int src_idx, int ctx)
{
	struct device *next_dev = NULL;
	struct link_node *lnode = NULL;
	struct msm_bus_node_device_type *dev_info = NULL;
	int curr_idx;
	int ret = 0;
	struct rule_update_path_info *rule_node;
	bool rules_registered = msm_rule_are_rules_registered();

	if (IS_ERR_OR_NULL(src_dev)) {
		MSM_BUS_ERR("%s: No source device", __func__);
		ret = -ENODEV;
		goto exit_update_path;
	}

	next_dev = src_dev;

	if (src_idx < 0) {
		MSM_BUS_ERR("%s: Invalid lnode idx %d", __func__, src_idx);
		ret = -ENXIO;
		goto exit_update_path;
	}
	curr_idx = src_idx;

	while (next_dev) {
		int i;

		dev_info = to_msm_bus_node(next_dev);

		if (curr_idx >= dev_info->num_lnodes) {
			MSM_BUS_ERR("%s: Invalid lnode Idx %d num lnodes %d",
			 __func__, curr_idx, dev_info->num_lnodes);
			ret = -ENXIO;
			goto exit_update_path;
		}

		lnode = &dev_info->lnode_list[curr_idx];
		if (!lnode) {
			MSM_BUS_ERR("%s: Invalid lnode ptr lnode %d",
				 __func__, curr_idx);
			ret = -ENXIO;
			goto exit_update_path;
		}
		lnode->lnode_ib[ACTIVE_CTX] = act_req_ib;
		lnode->lnode_ab[ACTIVE_CTX] = act_req_bw;
		lnode->lnode_ib[DUAL_CTX] = slp_req_ib;
		lnode->lnode_ab[DUAL_CTX] = slp_req_bw;

		for (i = 0; i < NUM_CTX; i++)
			dev_info->node_bw[i].cur_clk_hz =
					aggregate_bus_req(dev_info, i);

		add_node_to_clist(dev_info);

		if (rules_registered) {
			rule_node = &dev_info->node_info->rule;
			rule_node->id = dev_info->node_info->id;
			rule_node->ib = dev_info->node_bw[ACTIVE_CTX].max_ib;
			rule_node->ab = dev_info->node_bw[ACTIVE_CTX].sum_ab;
			rule_node->clk =
				dev_info->node_bw[ACTIVE_CTX].cur_clk_hz;
			if (!rule_node->added) {
				list_add_tail(&rule_node->link, &input_list);
				rule_node->added = true;
			}
		}

		next_dev = lnode->next_dev;
		curr_idx = lnode->next;
	}

exit_update_path:
	return ret;
}

static int remove_path(struct device *src_dev, int dst, uint64_t cur_ib,
			uint64_t cur_ab, int src_idx, int active_only)
{
	struct device *next_dev = NULL;
	struct link_node *lnode = NULL;
	struct msm_bus_node_device_type *dev_info = NULL;
	int ret = 0;
	int cur_idx = src_idx;
	int next_idx;

	/* Update the current path to zero out all request from
	 * this cient on all paths
	 */
	if (!src_dev) {
		MSM_BUS_ERR("%s: Can't find source device", __func__);
		ret = -ENODEV;
		goto exit_remove_path;
	}

	ret = update_path(src_dev, dst, 0, 0, 0, 0, cur_ib, cur_ab, src_idx,
							active_only);
	if (ret) {
		MSM_BUS_ERR("%s: Error zeroing out path ctx %d",
					__func__, ACTIVE_CTX);
		goto exit_remove_path;
	}

	next_dev = src_dev;

	while (next_dev) {
		dev_info = to_msm_bus_node(next_dev);
		lnode = &dev_info->lnode_list[cur_idx];
		next_idx = lnode->next;
		next_dev = lnode->next_dev;
		remove_lnode(dev_info, cur_idx);
		cur_idx = next_idx;
	}

exit_remove_path:
	return ret;
}

static void getpath_debug(int src, int curr, int active_only)
{
	struct device *dev_node;
	struct device *dev_it;
	unsigned int hop = 1;
	int idx;
	struct msm_bus_node_device_type *devinfo;
	int i;

	dev_node = bus_find_device(&msm_bus_type, NULL,
				(void *) &src,
				msm_bus_device_match_adhoc);

	if (!dev_node) {
		MSM_BUS_ERR("SRC NOT FOUND %d", src);
		return;
	}

	idx = curr;
	devinfo = to_msm_bus_node(dev_node);
	dev_it = dev_node;

	MSM_BUS_ERR("Route list Src %d", src);
	while (dev_it) {
		struct msm_bus_node_device_type *busdev =
			to_msm_bus_node(devinfo->node_info->bus_device);

		MSM_BUS_ERR("Hop[%d] at Device %d ctx %d", hop,
					devinfo->node_info->id, active_only);

		for (i = 0; i < NUM_CTX; i++) {
			MSM_BUS_ERR("dev info sel ib %llu",
						devinfo->node_bw[i].cur_clk_hz);
			MSM_BUS_ERR("dev info sel ab %llu",
						devinfo->node_bw[i].sum_ab);
		}

		dev_it = devinfo->lnode_list[idx].next_dev;
		idx = devinfo->lnode_list[idx].next;
		if (dev_it)
			devinfo = to_msm_bus_node(dev_it);

		MSM_BUS_ERR("Bus Device %d", busdev->node_info->id);
		MSM_BUS_ERR("Bus Clock %llu", busdev->clk[active_only].rate);

		if (idx < 0)
			break;
		hop++;
	}
}

static void unregister_client_adhoc(uint32_t cl)
{
	int i;
	struct msm_bus_scale_pdata *pdata;
	int lnode, src, curr, dest;
	uint64_t  cur_clk, cur_bw;
	struct msm_bus_client *client;
	struct device *src_dev;

	rt_mutex_lock(&msm_bus_adhoc_lock);
	if (!cl) {
		MSM_BUS_ERR("%s: Null cl handle passed unregister\n",
				__func__);
		goto exit_unregister_client;
	}
	client = handle_list.cl_list[cl];
	pdata = client->pdata;
	if (!pdata) {
		MSM_BUS_ERR("%s: Null pdata passed to unregister\n",
				__func__);
		goto exit_unregister_client;
	}

	curr = client->curr;
	if ((curr < 0) || (curr >= pdata->num_usecases)) {
		MSM_BUS_ERR("Invalid index Defaulting curr to 0");
		curr = 0;
	}

	MSM_BUS_DBG("%s: Unregistering client %p", __func__, client);

	for (i = 0; i < pdata->usecase->num_paths; i++) {
		src = client->pdata->usecase[curr].vectors[i].src;
		dest = client->pdata->usecase[curr].vectors[i].dst;

		lnode = client->src_pnode[i];
		src_dev = client->src_devs[i];
		cur_clk = client->pdata->usecase[curr].vectors[i].ib;
		cur_bw = client->pdata->usecase[curr].vectors[i].ab;
		remove_path(src_dev, dest, cur_clk, cur_bw, lnode,
						pdata->active_only);
	}
	commit_data();
	msm_bus_dbg_client_data(client->pdata, MSM_BUS_DBG_UNREGISTER, cl);
	kfree(client->src_pnode);
	kfree(client->src_devs);
	kfree(client);
	handle_list.cl_list[cl] = NULL;
exit_unregister_client:
	rt_mutex_unlock(&msm_bus_adhoc_lock);
}

static int alloc_handle_lst(int size)
{
	int ret = 0;
	struct msm_bus_client **t_cl_list;

	if (!handle_list.num_entries) {
		t_cl_list = kzalloc(sizeof(struct msm_bus_client *)
			* NUM_CL_HANDLES, GFP_KERNEL);
		if (ZERO_OR_NULL_PTR(t_cl_list)) {
			ret = -ENOMEM;
			MSM_BUS_ERR("%s: Failed to allocate handles list",
								__func__);
			goto exit_alloc_handle_lst;
		}
		handle_list.cl_list = t_cl_list;
		handle_list.num_entries += NUM_CL_HANDLES;
	} else {
		t_cl_list = krealloc(handle_list.cl_list,
				sizeof(struct msm_bus_client *) *
				(handle_list.num_entries + NUM_CL_HANDLES),
				GFP_KERNEL);
		if (ZERO_OR_NULL_PTR(t_cl_list)) {
			ret = -ENOMEM;
			MSM_BUS_ERR("%s: Failed to allocate handles list",
								__func__);
			goto exit_alloc_handle_lst;
		}

		handle_list.cl_list = t_cl_list;
		memset(&handle_list.cl_list[handle_list.num_entries], 0,
			NUM_CL_HANDLES * sizeof(struct msm_bus_client *));
		handle_list.num_entries += NUM_CL_HANDLES;
	}
exit_alloc_handle_lst:
	return ret;
}

static uint32_t gen_handle(struct msm_bus_client *client)
{
	uint32_t handle = 0;
	int i;
	int ret = 0;

	for (i = 0; i < handle_list.num_entries; i++) {
		if (i && !handle_list.cl_list[i]) {
			handle = i;
			break;
		}
	}

	if (!handle) {
		ret = alloc_handle_lst(NUM_CL_HANDLES);

		if (ret) {
			MSM_BUS_ERR("%s: Failed to allocate handle list",
							__func__);
			goto exit_gen_handle;
		}
		handle = i + 1;
	}
	handle_list.cl_list[handle] = client;
exit_gen_handle:
	return handle;
}

static uint32_t register_client_adhoc(struct msm_bus_scale_pdata *pdata)
{
	int src, dest;
	int i;
	struct msm_bus_client *client = NULL;
	int *lnode;
	struct device *dev;
	uint32_t handle = 0;

	rt_mutex_lock(&msm_bus_adhoc_lock);
	client = kzalloc(sizeof(struct msm_bus_client), GFP_KERNEL);
	if (!client) {
		MSM_BUS_ERR("%s: Error allocating client data", __func__);
		goto exit_register_client;
	}
	client->pdata = pdata;

	lnode = kcalloc(pdata->usecase->num_paths, sizeof(int), GFP_KERNEL);
	if (ZERO_OR_NULL_PTR(lnode)) {
		MSM_BUS_ERR("%s: Error allocating pathnode ptr!", __func__);
		goto exit_lnode_malloc_fail;
	}
	client->src_pnode = lnode;

	client->src_devs = kzalloc(pdata->usecase->num_paths *
					sizeof(struct device *), GFP_KERNEL);
	if (IS_ERR_OR_NULL(client->src_devs)) {
		MSM_BUS_ERR("%s: Error allocating pathnode ptr!", __func__);
		goto exit_src_dev_malloc_fail;
	}
	client->curr = -1;

	for (i = 0; i < pdata->usecase->num_paths; i++) {
		src = pdata->usecase->vectors[i].src;
		dest = pdata->usecase->vectors[i].dst;

		if ((src < 0) || (dest < 0)) {
			MSM_BUS_ERR("%s:Invalid src/dst.src %d dest %d",
				__func__, src, dest);
			goto exit_invalid_data;
		}
		dev = bus_find_device(&msm_bus_type, NULL,
				(void *) &src,
				msm_bus_device_match_adhoc);
		if (IS_ERR_OR_NULL(dev)) {
			MSM_BUS_ERR("%s:Failed to find path.src %d dest %d",
				__func__, src, dest);
			goto exit_invalid_data;
		}
		client->src_devs[i] = dev;

		lnode[i] = getpath(dev, dest, client->pdata->name);
		if (lnode[i] < 0) {
			MSM_BUS_ERR("%s:Failed to find path.src %d dest %d",
				__func__, src, dest);
			goto exit_invalid_data;
		}
	}

	handle = gen_handle(client);
	msm_bus_dbg_client_data(client->pdata, MSM_BUS_DBG_REGISTER,
					handle);
	MSM_BUS_DBG("%s:Client handle %d %s", __func__, handle,
						client->pdata->name);
	rt_mutex_unlock(&msm_bus_adhoc_lock);
	return handle;
exit_invalid_data:
	kfree(client->src_devs);
exit_src_dev_malloc_fail:
	kfree(lnode);
exit_lnode_malloc_fail:
	kfree(client);
exit_register_client:
	rt_mutex_unlock(&msm_bus_adhoc_lock);
	return handle;
}

/* Validate the whole registered route before changing any vote or dirty list. */
static int validate_path_step(struct device *dev, int idx, int dest,
			     struct device **next, int *next_idx)
{
	struct msm_bus_node_device_type *node;
	struct link_node *link;

	if (IS_ERR_OR_NULL(dev))
		return -ENODEV;
	node = to_msm_bus_node(dev);
	if (IS_ERR_OR_NULL(node->node_info) ||
	    IS_ERR_OR_NULL(node->node_info->bus_device))
		return -ENODEV;
	if (idx < 0 || idx >= node->num_lnodes ||
	    IS_ERR_OR_NULL(node->lnode_list))
		return -ENXIO;
	link = &node->lnode_list[idx];
	if (!link->in_use)
		return -ENXIO;
	if (dest < 0 || (!link->next_dev &&
			 node->node_info->id != (unsigned int)dest))
		return -ENXIO;
	*next = link->next_dev;
	*next_idx = link->next;
	return 0;
}

static int validate_client_path(struct device *dev, int idx, int dest)
{
	struct device *slow = dev;
	int slow_idx = idx;
	int ret, step;

	if (IS_ERR_OR_NULL(dev))
		return -ENODEV;
	/* Floyd's cycle check needs no guessed topology size or allocation. */
	while (dev) {
		for (step = 0; step < 2 && dev; step++) {
			ret = validate_path_step(dev, idx, dest, &dev, &idx);
			if (ret)
				return ret;
		}
		if (!dev)
			break;
		ret = validate_path_step(slow, slow_idx, dest, &slow,
					 &slow_idx);
		if (ret)
			return ret;
		if (dev == slow && idx == slow_idx)
			return -ELOOP;
	}
	return 0;
}

static int validate_client_paths(struct msm_bus_client *client,
				 unsigned int idx)
{
	struct msm_bus_scale_pdata *pdata;
	struct msm_bus_paths *base, *target, *current_case;
	int i, ret;

	if (IS_ERR_OR_NULL(client) || IS_ERR_OR_NULL(client->pdata))
		return -ENXIO;
	pdata = client->pdata;
	if (pdata->num_usecases <= 0 ||
	    idx >= (unsigned int)pdata->num_usecases ||
	    IS_ERR_OR_NULL(pdata->usecase) ||
	    IS_ERR_OR_NULL(client->src_pnode) ||
	    IS_ERR_OR_NULL(client->src_devs) || client->curr < -1 ||
	    client->curr >= pdata->num_usecases)
		return -ENXIO;
	base = &pdata->usecase[0];
	target = &pdata->usecase[idx];
	current_case = client->curr < 0 ? base : &pdata->usecase[client->curr];
	if (base->num_paths <= 0 || target->num_paths != base->num_paths ||
	    current_case->num_paths != base->num_paths ||
	    IS_ERR_OR_NULL(base->vectors) ||
	    IS_ERR_OR_NULL(target->vectors) || IS_ERR_OR_NULL(current_case->vectors))
		return -ENXIO;
	for (i = 0; i < base->num_paths; i++) {
		if (target->vectors[i].src != base->vectors[i].src ||
		    target->vectors[i].dst != base->vectors[i].dst ||
		    current_case->vectors[i].src != base->vectors[i].src ||
		    current_case->vectors[i].dst != base->vectors[i].dst ||
		    IS_ERR_OR_NULL(client->src_devs[i]))
			return -ENXIO;
		ret = validate_client_path(client->src_devs[i],
				 client->src_pnode[i], base->vectors[i].dst);
		if (ret)
			return ret;
		if (base->vectors[i].src < 0 ||
		    to_msm_bus_node(client->src_devs[i])->node_info->id !=
				(unsigned int)base->vectors[i].src)
			return -ENXIO;
	}
	return 0;
}

static int update_client_paths(struct msm_bus_client *client, bool log_trns,
							unsigned int idx)
{
	int lnode, src, dest, cur_idx;
	uint64_t req_clk, req_bw, curr_clk, curr_bw, slp_clk, slp_bw;
	int i, ret = 0;
	struct msm_bus_scale_pdata *pdata;
	struct device *src_dev;

	if (!client) {
		MSM_BUS_ERR("Client handle  Null");
		ret = -ENXIO;
		goto exit_update_client_paths;
	}

	pdata = client->pdata;
	if (!pdata) {
		MSM_BUS_ERR("Client pdata Null");
		ret = -ENXIO;
		goto exit_update_client_paths;
	}

	ret = validate_client_paths(client, idx);
	if (ret)
		goto exit_update_client_paths;
	cur_idx = client->curr;
	for (i = 0; i < pdata->usecase->num_paths; i++) {
		src = pdata->usecase[idx].vectors[i].src;
		dest = pdata->usecase[idx].vectors[i].dst;

		lnode = client->src_pnode[i];
		src_dev = client->src_devs[i];
		req_clk = client->pdata->usecase[idx].vectors[i].ib;
		req_bw = client->pdata->usecase[idx].vectors[i].ab;
		if (cur_idx < 0) {
			curr_clk = 0;
			curr_bw = 0;
		} else {
			curr_clk =
				client->pdata->usecase[cur_idx].vectors[i].ib;
			curr_bw = client->pdata->usecase[cur_idx].vectors[i].ab;
			MSM_BUS_DBG("%s:ab: %llu ib: %llu\n", __func__,
					curr_bw, curr_clk);
		}

		if (pdata->active_only) {
			slp_clk = 0;
			slp_bw = 0;
		} else {
			slp_clk = req_clk;
			slp_bw = req_bw;
		}

		ret = update_path(src_dev, dest, req_clk, req_bw, slp_clk,
			slp_bw, curr_clk, curr_bw, lnode, pdata->active_only);

		if (ret) {
			MSM_BUS_ERR("%s: Update path failed! %d ctx %d\n",
					__func__, ret, pdata->active_only);
			goto exit_update_client_paths;
		}

		if (log_trns)
			getpath_debug(src, lnode, pdata->active_only);
	}
	client->commit_pending = true;
	ret = commit_data();
	if (!ret) {
		client->curr = idx;
		client->commit_pending = false;
	}
exit_update_client_paths:
	return ret;
}

static int update_context(uint32_t cl, bool active_only,
					unsigned int ctx_idx)
{
	int ret = 0;
	struct msm_bus_scale_pdata *pdata;
	struct msm_bus_client *client;

	rt_mutex_lock(&msm_bus_adhoc_lock);
	if (!cl || !handle_list.cl_list || handle_list.num_entries <= 0 ||
	    cl >= (unsigned int)handle_list.num_entries) {
		MSM_BUS_ERR("%s: Invalid client handle %d", __func__, cl);
		ret = -ENXIO;
		goto exit_update_context;
	}

	client = handle_list.cl_list[cl];
	if (!client) {
		ret = -ENXIO;
		goto exit_update_context;
	}

	pdata = client->pdata;
	if (!pdata) {
		ret = -ENXIO;
		goto exit_update_context;
	}
	if (pdata->active_only == active_only) {
		MSM_BUS_ERR("No change in context(%d==%d), skip\n",
					pdata->active_only, active_only);
		ret = -ENXIO;
		goto exit_update_context;
	}

	if (pdata->num_usecases <= 0 ||
	    ctx_idx >= (unsigned int)pdata->num_usecases) {
		MSM_BUS_ERR("Client %u passed invalid index: %d\n",
			cl, ctx_idx);
		ret = -ENXIO;
		goto exit_update_context;
	}

	ret = validate_client_paths(client, ctx_idx);
	if (ret)
		goto exit_update_context;
	pdata->active_only = active_only;

	msm_bus_dbg_client_data(client->pdata, ctx_idx, cl);
	ret = update_client_paths(client, false, ctx_idx);
	if (ret) {
		pdata->active_only = !active_only;
		pr_err("%s: Err updating path\n", __func__);
		goto exit_update_context;
	}

	trace_bus_update_request_end(pdata->name);

exit_update_context:
	rt_mutex_unlock(&msm_bus_adhoc_lock);
	return ret;
}

static int update_request_adhoc(uint32_t cl, unsigned int index)
{
	int ret = 0;
	struct msm_bus_scale_pdata *pdata;
	struct msm_bus_client *client;
	const char *test_cl = "Null";
	bool log_transaction = false;

	rt_mutex_lock(&msm_bus_adhoc_lock);

	if (!cl || !handle_list.cl_list || handle_list.num_entries <= 0 ||
	    cl >= (unsigned int)handle_list.num_entries) {
		MSM_BUS_ERR("%s: Invalid client handle %d", __func__, cl);
		ret = -ENXIO;
		goto exit_update_request;
	}

	client = handle_list.cl_list[cl];
	if (!client) {
		MSM_BUS_ERR("%s: Invalid client pointer ", __func__);
		ret = -ENXIO;
		goto exit_update_request;
	}

	pdata = client->pdata;
	if (!pdata) {
		MSM_BUS_ERR("%s: Client data Null.[client didn't register]",
				__func__);
		ret = -ENXIO;
		goto exit_update_request;
	}

	if (pdata->num_usecases <= 0 ||
	    index >= (unsigned int)pdata->num_usecases) {
		MSM_BUS_ERR("Client %u passed invalid index: %d\n",
			cl, index);
		ret = -ENXIO;
		goto exit_update_request;
	}

	ret = validate_client_paths(client, index);
	if (ret)
		goto exit_update_request;
	if (client->curr == (int)index && !client->commit_pending &&
	    list_empty(&commit_list)) {
		MSM_BUS_DBG("%s: Not updating client request idx %d unchanged",
				__func__, index);
		goto exit_update_request;
	}

	if (!strcmp(test_cl, pdata->name))
		log_transaction = true;

	MSM_BUS_DBG("%s: cl: %u index: %d curr: %d num_paths: %d\n", __func__,
		cl, index, client->curr, client->pdata->usecase->num_paths);
	msm_bus_dbg_client_data(client->pdata, index, cl);
	ret = update_client_paths(client, log_transaction, index);
	if (ret) {
		pr_err("%s: Err updating path\n", __func__);
		goto exit_update_request;
	}

	trace_bus_update_request_end(pdata->name);

exit_update_request:
	rt_mutex_unlock(&msm_bus_adhoc_lock);
	return ret;
}

static void free_cl_mem(struct msm_bus_client_handle *cl)
{
	if (cl) {
		kfree(cl->name);
		kfree(cl);
		cl = NULL;
	}
}

static int update_bw_adhoc(struct msm_bus_client_handle *cl, u64 ab, u64 ib)
{
	int ret = 0;
	char *test_cl = "test-client";
	bool log_transaction = false;
	u64 slp_ib, slp_ab;

	rt_mutex_lock(&msm_bus_adhoc_lock);

	if (!cl) {
		MSM_BUS_ERR("%s: Invalid client handle %p", __func__, cl);
		ret = -ENXIO;
		goto exit_update_request;
	}

	if (!strcmp(test_cl, cl->name))
		log_transaction = true;

	msm_bus_dbg_rec_transaction(cl, ab, ib);

	if ((cl->cur_act_ib == ib) && (cl->cur_act_ab == ab) &&
	    !cl->commit_pending && list_empty(&commit_list)) {
		MSM_BUS_DBG("%s:no change in request", cl->name);
		goto exit_update_request;
	}

	if (cl->active_only) {
		slp_ib = 0;
		slp_ab = 0;
	} else {
		slp_ib = ib;
		slp_ab = ab;
	}

	ret = update_path(cl->mas_dev, cl->slv, ib, ab, slp_ib, slp_ab,
		cl->cur_act_ib, cl->cur_act_ab, cl->first_hop, cl->active_only);

	if (ret) {
		MSM_BUS_ERR("%s: Update path failed! %d active_only %d\n",
				__func__, ret, cl->active_only);
		goto exit_update_request;
	}

	cl->commit_pending = true;
	ret = commit_data();
	if (ret)
		goto exit_update_request;
	cl->commit_pending = false;
	cl->cur_act_ib = ib;
	cl->cur_act_ab = ab;
	cl->cur_dual_ib = slp_ib;
	cl->cur_dual_ab = slp_ab;

	if (log_transaction)
		getpath_debug(cl->mas, cl->first_hop, cl->active_only);
	trace_bus_update_request_end(cl->name);
exit_update_request:
	rt_mutex_unlock(&msm_bus_adhoc_lock);

	return ret;
}

static int update_bw_context(struct msm_bus_client_handle *cl, u64 act_ab,
				u64 act_ib, u64 slp_ib, u64 slp_ab)
{
	int ret = 0;
	bool old_active_only;

	rt_mutex_lock(&msm_bus_adhoc_lock);
	if (!cl) {
		MSM_BUS_ERR("Invalid client handle %p", cl);
		ret = -ENXIO;
		goto exit_change_context;
	}

	if ((cl->cur_act_ib == act_ib) &&
		(cl->cur_act_ab == act_ab) &&
		(cl->cur_dual_ib == slp_ib) &&
		(cl->cur_dual_ab == slp_ab) && !cl->commit_pending &&
		list_empty(&commit_list)) {
		MSM_BUS_ERR("No change in vote");
		goto exit_change_context;
	}

	old_active_only = cl->active_only;
	cl->active_only = !slp_ab && !slp_ib;
	msm_bus_dbg_rec_transaction(cl, cl->cur_act_ab, cl->cur_dual_ib);
	ret = update_path(cl->mas_dev, cl->slv, act_ib, act_ab, slp_ib,
				slp_ab, cl->cur_act_ab, cl->cur_act_ab,
				cl->first_hop, cl->active_only);
	if (ret) {
		MSM_BUS_ERR("%s: Update path failed! %d active_only %d\n",
				__func__, ret, cl->active_only);
		cl->active_only = old_active_only;
		goto exit_change_context;
	}
	cl->commit_pending = true;
	ret = commit_data();
	if (ret) {
		cl->active_only = old_active_only;
		goto exit_change_context;
	}
	cl->commit_pending = false;
	cl->cur_act_ib = act_ib;
	cl->cur_act_ab = act_ab;
	cl->cur_dual_ib = slp_ib;
	cl->cur_dual_ab = slp_ab;
	trace_bus_update_request_end(cl->name);
exit_change_context:
	rt_mutex_unlock(&msm_bus_adhoc_lock);
	return ret;
}

static void unregister_adhoc(struct msm_bus_client_handle *cl)
{
	rt_mutex_lock(&msm_bus_adhoc_lock);
	if (!cl) {
		MSM_BUS_ERR("%s: Null cl handle passed unregister\n",
				__func__);
		goto exit_unregister_client;
	}

	MSM_BUS_DBG("%s: Unregistering client %p", __func__, cl);

	remove_path(cl->mas_dev, cl->slv, cl->cur_act_ib, cl->cur_act_ab,
				cl->first_hop, cl->active_only);
	commit_data();
	msm_bus_dbg_remove_client(cl);
	kfree(cl->name);
	kfree(cl);
exit_unregister_client:
	rt_mutex_unlock(&msm_bus_adhoc_lock);
}

static struct msm_bus_client_handle*
register_adhoc(uint32_t mas, uint32_t slv, char *name, bool active_only)
{
	struct msm_bus_client_handle *client = NULL;
	int len = 0;

	rt_mutex_lock(&msm_bus_adhoc_lock);

	if (!(mas && slv && name)) {
		pr_err("%s: Error: src dst name num_paths are required",
								 __func__);
		goto exit_register;
	}

	client = kzalloc(sizeof(struct msm_bus_client_handle), GFP_KERNEL);
	if (!client) {
		MSM_BUS_ERR("%s: Error allocating client data", __func__);
		goto exit_register;
	}

	len = strnlen(name, MAX_STR_CL);
	client->name = kzalloc((len + 1), GFP_KERNEL);
	if (!client->name) {
		MSM_BUS_ERR("%s: Error allocating client name buf", __func__);
		free_cl_mem(client);
		goto exit_register;
	}
	strlcpy(client->name, name, MAX_STR_CL);
	client->active_only = active_only;

	client->mas = mas;
	client->slv = slv;

	client->mas_dev = bus_find_device(&msm_bus_type, NULL,
					(void *) &mas,
					msm_bus_device_match_adhoc);
	if (IS_ERR_OR_NULL(client->mas_dev)) {
		MSM_BUS_ERR("%s:Failed to find path.src %d dest %d",
			__func__, client->mas, client->slv);
		free_cl_mem(client);
		goto exit_register;
	}

	client->first_hop = getpath(client->mas_dev, client->slv, client->name);
	if (client->first_hop < 0) {
		MSM_BUS_ERR("%s:Failed to find path.src %d dest %d",
			__func__, client->mas, client->slv);
		free_cl_mem(client);
		goto exit_register;
	}

	MSM_BUS_DBG("%s:Client handle %p %s", __func__, client,
						client->name);
	msm_bus_dbg_add_client(client);
exit_register:
	rt_mutex_unlock(&msm_bus_adhoc_lock);
	return client;
}
/**
 *  msm_bus_arb_setops_adhoc() : Setup the bus arbitration ops
 *  @ arb_ops: pointer to the arb ops.
 */
void msm_bus_arb_setops_adhoc(struct msm_bus_arb_ops *arb_ops)
{
	arb_ops->register_client = register_client_adhoc;
	arb_ops->update_request = update_request_adhoc;
	arb_ops->unregister_client = unregister_client_adhoc;
	arb_ops->update_context = update_context;

	arb_ops->register_cl = register_adhoc;
	arb_ops->unregister = unregister_adhoc;
	arb_ops->update_bw = update_bw_adhoc;
	arb_ops->update_bw_context = update_bw_context;
}

/* UNSELECTED a3 C5 diagnostic draft: per-open immutable software-vote copy only. */
#if defined(CONFIG_QTI_MPM_C5LTE) && defined(CONFIG_DEBUG_FS) && !defined(MODULE)
#include <linux/debugfs.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>

#define C5_EBI_SNAPSHOT_MAX_SLOTS 512

enum c5_ebi_snapshot_status {
	C5_EBI_OK = 0,
	C5_EBI_PROFILE = 1,
	C5_EBI_PROVIDER = 2,
	C5_EBI_NODE = 3,
	C5_EBI_LIMIT = 4,
	C5_EBI_LIST = 5,
	C5_EBI_OVERFLOW = 6,
	C5_EBI_PROVENANCE = 7,
};

struct c5_ebi_snapshot_row {
	u64 dual_ab;
	u32 in_use, name_code;
};

struct c5_ebi_snapshot {
	u32 status, slots, rows, dirty;
	u64 total_all, total_in_use, cached, last_sum;
	struct c5_ebi_snapshot_row row[C5_EBI_SNAPSHOT_MAX_SLOTS];
};

/* Generated only from the actual selected packed DT's bus-name constants. */
static const char * const c5_ebi_snapshot_names[] = {
	"other",
	"blsp1_uart0",
	"devfreq_spdm",
	"grp3d",
	"ipa",
	"mdss_dsi",
	"mdss_mdp",
	"msm-rng-noc",
	"pil-venus",
	"qcedev-noc",
	"qcrypto-noc",
	"qseecom-noc",
	"sdcc_ice_noc",
	"sdhc1",
	"sdhc2",
	"usb2",
	"vdec-core0-ddr",
	"vdec-core1-ddr",
	"venc-ddr",
	"venus-arm9-ddr",
	"gpu-devbw", /* Fixed output alias; never accepted as a raw client name. */

};

static u32 c5_ebi_snapshot_name_code(const char *name)
{
	u32 i;

	if (!name || strnlen(name, MAX_STR_CL + 1) > MAX_STR_CL)
		return 0;
	if (!strcmp(name, "soc:qcom,gpubw"))
		return ARRAY_SIZE(c5_ebi_snapshot_names) - 1;
	/* Preserve old names; exclude the output-only GPU alias. */
	for (i = 1; i < ARRAY_SIZE(c5_ebi_snapshot_names) - 1; i++)
		if (!strcmp(name, c5_ebi_snapshot_names[i]))
			return i;
	return 0;
}

static bool c5_ebi_snapshot_profile(void)
{
	struct device_node *root, *psci;
	const char *model;
	bool match = false;

	if (!of_machine_is_compatible("qcom,msm8952-mtp"))
		return false;
	root = of_find_node_by_path("/");
	if (root) {
		match = !of_property_read_string(root, "model", &model) &&
			!strcmp(model, "Samsung C5LTE CHN OPEN PROJECT Rev09");
		of_node_put(root);
	}
	if (!match)
		return false;
	psci = of_find_node_by_path("/psci");
	if (!psci)
		return false;
	match = of_device_is_compatible(psci, "arm,psci-1.0") &&
		of_property_read_bool(psci, "samsung,c5-legacy-psci-state");
	of_node_put(psci);
	return match;
}

/* Exact OF identity match; never reinterpret unrelated bus devices. */
static int c5_ebi_snapshot_node_match(struct device *dev, void *data)
{
	return dev->of_node == data;
}

/* Caller holds msm_bus_adhoc_lock. No allocation, formatting, trace or I/O. */
static void c5_ebi_snapshot_copy_locked(struct c5_ebi_snapshot *s,
			struct msm_bus_node_device_type *node)
{
	u64 all = 0, in_use = 0;
	int i, count = node->num_lnodes;

	s->status = C5_EBI_LIMIT;
	if (count < 0 || count > C5_EBI_SNAPSHOT_MAX_SLOTS)
		return;
	s->status = C5_EBI_LIST;
	if (count && IS_ERR_OR_NULL(node->lnode_list))
		return;
	s->status = C5_EBI_PROVENANCE;
	if (c5_ebi_allocation.node != node ||
	    c5_ebi_allocation.list != node->lnode_list ||
	    c5_ebi_allocation.capacity != count)
		return;
	for (i = 0; i < count; i++) {
		const struct link_node *link = &node->lnode_list[i];
		struct c5_ebi_snapshot_row *row = &s->row[i];
		u64 ab = link->lnode_ab[DUAL_CTX];

		s->status = C5_EBI_OVERFLOW;
		if (~0ULL - all < ab || (link->in_use && ~0ULL - in_use < ab))
			return;
		all += ab;
		if (link->in_use)
			in_use += ab;
		row->dual_ab = ab;
		row->in_use = !!link->in_use;
		row->name_code = link->in_use ?
			c5_ebi_snapshot_name_code(link->cl_name) : 0;
		if (row->in_use || ab)
			s->rows++;
	}
	s->slots = count;
	s->total_all = all;
	s->total_in_use = in_use;
	s->cached = node->node_bw[DUAL_CTX].sum_ab;
	s->last_sum = node->node_bw[DUAL_CTX].last_sum_ab;
	s->dirty = !!node->dirty;
	s->status = C5_EBI_OK;
}

/*
 * Device core holds this same platform-device lock throughout probe/remove.
 * Take it before the existing arbitration lock: a device ref alone cannot
 * protect the native remove callback's explicit kfree(bus_node).
 */
static void c5_ebi_snapshot_take(struct c5_ebi_snapshot *s)
{
	struct device_node *bus_np = NULL, *ebi_np = NULL;
	struct platform_device *pdev = NULL;
	struct device *bus_dev = NULL;
	struct msm_bus_node_device_type *node;

	s->status = C5_EBI_PROFILE;
	if (!c5_ebi_snapshot_profile())
		return;
	s->status = C5_EBI_PROVIDER;
	bus_np = of_find_node_by_path("/soc/ad-hoc-bus");
	ebi_np = of_find_node_by_path("/soc/ad-hoc-bus/slv-ebi");
	if (!bus_np || !ebi_np || ebi_np->parent != bus_np ||
	    !of_device_is_available(bus_np) || !of_device_is_available(ebi_np) ||
	    !of_device_is_compatible(bus_np, "qcom,msm-bus-device"))
		goto out_nodes;
	pdev = of_find_device_by_node(bus_np);
	if (!pdev)
		goto out_nodes;
	device_lock(&pdev->dev);
	if (!pdev->dev.driver ||
	    strcmp(pdev->dev.driver->name, "msm_bus_device"))
		goto out_device_unlock;
	rt_mutex_lock(&msm_bus_adhoc_lock);
	s->status = C5_EBI_NODE;
	bus_dev = bus_find_device(&msm_bus_type, NULL, ebi_np,
				 c5_ebi_snapshot_node_match);
	if (!bus_dev)
		goto out_arb_unlock;
	node = to_msm_bus_node(bus_dev);
	if (!node->node_info || node->node_info->id != 512 ||
	    node->node_info->slv_rpm_id != 0 ||
	    node->node_info->agg_params.buswidth != 16 ||
	    !node->node_info->name ||
	    strcmp(node->node_info->name, "slv-ebi"))
		goto out_arb_unlock;
	c5_ebi_snapshot_copy_locked(s, node);
out_arb_unlock:
	rt_mutex_unlock(&msm_bus_adhoc_lock);
	if (bus_dev)
		put_device(bus_dev);
out_device_unlock:
	device_unlock(&pdev->dev);
	put_device(&pdev->dev);
out_nodes:
	of_node_put(ebi_np);
	of_node_put(bus_np);
}

static int c5_ebi_snapshot_show(struct seq_file *m, void *unused)
{
	const struct c5_ebi_snapshot *s = m->private;
	u32 i;

	seq_puts(m, "C5_EBI_DUAL_AB_V1\n");
	if (s->status != C5_EBI_OK) {
		seq_printf(m, "AVAILABLE 0 STATUS %u\nEND\n", s->status);
		return 0;
	}
	seq_printf(m, "AVAILABLE 1 STATUS 0 SLOTS %u ROWS %u TOTAL_ALL %llu TOTAL_IN_USE %llu CACHED %llu LAST_SUM %llu DIRTY %u CACHE_EQUAL %u ACTIVE_ONLY_KNOWN 0\n",
		s->slots, s->rows, s->total_all, s->total_in_use, s->cached,
		s->last_sum, s->dirty,
		!!(s->total_all == s->cached));
	for (i = 0; i < s->slots; i++) {
		const struct c5_ebi_snapshot_row *row = &s->row[i];

		if (!row->in_use && !row->dual_ab)
			continue;
		seq_printf(m, "ROW %u IN_USE %u NAME %s NAME_KNOWN %u DUAL_AB %llu\n",
			i, row->in_use, c5_ebi_snapshot_names[row->name_code],
			!!row->name_code, row->dual_ab);
	}
	seq_puts(m, "END\n");
	return 0;
}

static int c5_ebi_snapshot_open(struct inode *inode, struct file *file)
{
	struct c5_ebi_snapshot *s;
	int ret;

	/* Per-open allocation and all formatting occur outside provider locks. */
	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	c5_ebi_snapshot_take(s);
	ret = single_open(file, c5_ebi_snapshot_show, s);
	if (ret)
		kfree(s);
	return ret;
}

static int c5_ebi_snapshot_release(struct inode *inode, struct file *file)
{
	struct seq_file *m = file->private_data;

	kfree(m->private);
	return single_release(inode, file);
}

static const struct file_operations c5_ebi_snapshot_fops = {
	.owner = THIS_MODULE,
	.open = c5_ebi_snapshot_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = c5_ebi_snapshot_release,
};

static int __init c5_ebi_snapshot_init(void)
{
	struct dentry *entry;

	if (!c5_ebi_snapshot_profile())
		return 0;
	entry = debugfs_create_file("c5_ebi_dual_ab", 0400, NULL, NULL,
				    &c5_ebi_snapshot_fops);
	return IS_ERR_OR_NULL(entry) ? -ENOMEM : 0;
}
late_initcall(c5_ebi_snapshot_init);
#endif
