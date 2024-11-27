/**
 * @file common_numa.c
 *
 * Copyright (C) IBM Corporation 2020
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <stdio.h>
#include <errno.h>
#include <numa.h>
#include <libconfig.h>
#include <stdlib.h>

#include "dr.h"
#include "ofdt.h"
#include "drmem.h"		/* for DYNAMIC_RECONFIG_MEM */
#include "common_numa.h"

struct ppcnuma_node *ppcnuma_fetch_node(struct ppcnuma_topology *numa, int nid)
{
	struct ppcnuma_node *node;

	if (nid > MAX_NUMNODES) {
		report_unknown_error(__FILE__, __LINE__);
		return NULL;
	}

	node = numa->nodes[nid];
	if (node)
		return node;

	node = zalloc(sizeof(struct ppcnuma_node));
	if (!node) {
		say(ERROR, "Can't allocate a new node\n");
		return NULL;
	}

	node->node_id = nid;

	if (!numa->node_count || nid < numa->node_min)
		numa->node_min = nid;
	if (nid > numa->node_max)
		numa->node_max = nid;

	numa->nodes[nid] = node;
	numa->node_count++;

	return node;
}

/*
 * Read the number of CPU for each node using the libnuma to get the details
 * from sysfs.
 */
static int read_numa_topology(struct ppcnuma_topology *numa)
{
	struct bitmask *cpus;
	struct ppcnuma_node *node;
	int rc, max_node, nid, i;

	if (numa_available() < 0)
		return -ENOENT;

	max_node = numa_max_node();
	if (max_node >= MAX_NUMNODES) {
		say(ERROR, "Too many nodes %d (max:%d)\n",
		    max_node, MAX_NUMNODES);
		return -EINVAL;
	}

	rc = 0;

	/* In case of allocation error, the libnuma is calling exit() */
	cpus = numa_allocate_cpumask();

	for (nid = 0; nid <= max_node; nid++) {

		if (!numa_bitmask_isbitset(numa_nodes_ptr, nid))
			continue;

		node = ppcnuma_fetch_node(numa, nid);
		if (!node) {
			rc = -ENOMEM;
			break;
		}

		rc = numa_node_to_cpus(nid, cpus);
		if (rc < 0)
			break;

		/* Count the CPUs in that node */
		for (i = 0; i < cpus->size; i++)
			if (numa_bitmask_isbitset(cpus, i))
				node->n_cpus++;

		numa->cpu_count += node->n_cpus;
	}

	numa_bitmask_free(cpus);

	if (rc) {
		ppcnuma_foreach_node(numa, nid, node)
			node->n_cpus = 0;
		numa->cpu_count = 0;
	}

	return rc;
}

static void
create_lmbs(unsigned int sort, struct ppcnuma_node *node, int count)
{
	struct lmb_list_head *lmb_list = NULL;
	struct dr_node *lmb = NULL;
	int rc = 0;
	static unsigned int drc_index = 0xdeadbeef;

	lmb_list = zalloc(sizeof(*lmb_list));
	if (lmb_list == NULL) {
		say(DEBUG, "Could not allocate LMB list head\n");
		return NULL;
	}

	lmb_list->sort = sort;

	/* Add count LMBs to lmb_list and node */
	for (int i = 0; i < count; i++) {
		lmb = lmb_list_add(drc_index++, lmb_list);
		if (lmb != NULL) {
			/* Add to node */
			lmb->lmb_numa_next = node->lmbs;
			node->lmbs = lmb;
			node->n_lmbs++;

			if (node->n_cpus)
				numa.lmb_count++;
			else
				numa.cpuless_lmb_count++;
		} else {
			/* FIXME */
		}
	}

	return;
}

static int
ppcnuma_get_config(struct ppcnuma_topology *numa, char *cfgfile)
{
	config_t             cfg;
	int                  count;
	int                  cpus;
	int                  mem;
	int                  nid;
	struct ppcnuma_node *node;
	config_setting_t *   nodes;


	config_init(&cfg);

	rc = config_read_file(&cfg, test_option);
	if (rc != CONFIG_TRUE) {
		(void) fprintf(stderr, "Error at line %d of file %s: %s\n",
			       config_error_line(&cfg),
			       config_error_file(&cfg), 
			       config_error_text(&cfg));
		config_destroy(&cfg);
		return -1;
	}

	nodes = config_lookup(&cfg, "nodes");
	if (nodes != NULL) {
		count = config_setting_length(nodes);
		for (int i = 0; i < count; i++) {

			cpus = -1;
			mem  = -1;
			nid  = -1;

			config_setting_lookup_int(node, "node", &nid);
			config_setting_lookup(node, "cpus", &cpus);
			config_setting_lookup(node, "mem", &mem);

			if ((node = ppc_fetch_node(numa, nid)) == NULL) {
				return -1;
			}

			node->n_cpus = cpus;
			create_lmbs(node, mem);
		}
	}

	numa_enabled = 1;

	return 0;
}

int ppcnuma_get_topology(struct ppcnuma_topology *numa)
{
	int rc;

	/* If testing, load topology from config file */
	if (test_option) {
		return ppcnuma_get_config(numa, test_option);
	}

	rc = numa_available();
	if (rc < 0)
		return rc;

	rc = get_min_common_depth();
	if (rc < 0)
		return rc;
	numa->min_common_depth = rc;

	rc = get_assoc_arrays(DYNAMIC_RECONFIG_MEM, &numa->aa,
			      numa->min_common_depth);
	if (rc)
		return rc;

	rc = read_numa_topology(numa);
	if (rc)
		return rc;

	if (!numa->node_count)
		return -1;

	return 0;
}
