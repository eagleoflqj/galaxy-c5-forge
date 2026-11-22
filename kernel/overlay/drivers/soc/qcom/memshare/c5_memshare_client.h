/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef C5_MEMSHARE_CLIENT_H
#define C5_MEMSHARE_CLIENT_H

/* The C5 DT uses CMA for every child; legacy dynamic clients use the parent. */
static inline struct device *c5_memshare_select_device(unsigned int client_id,
		unsigned int count, struct memshare_child *const *children,
		struct device *parent)
{
	unsigned int i;

	if (!children || count > MAX_CLIENTS)
		return 0;
	for (i = 0; i < count; i++)
		if (children[i] && (unsigned int)children[i]->client_id == client_id)
			return children[i]->dev;
	return parent;
}

#endif
