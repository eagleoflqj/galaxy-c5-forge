/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef C5_GNSS_REGISTRATION_H
#define C5_GNSS_REGISTRATION_H

/* Only Register Events 0x21: no position/NMEA/NV body is examined. */
struct c5_reg_message {
	unsigned int kind, tag, status, error;
	unsigned long long mask;
};

static inline struct c5_reg_message
c5_reg_parse(const unsigned char *p, unsigned int size, unsigned int direction)
{
	struct c5_reg_message m = { 0, 0, 0, 0, 0 };
	unsigned int i;

	if (!p || direction > 1 || (size != 18 && size != 14))
		return m;
	if (p[3] != 0x21 || p[4] || p[6])
		return m;
	if (direction == 0 && size == 18 && !p[0] && p[5] == 11 &&
	    p[7] == 1 && p[8] == 8 && !p[9]) {
		m.kind = 1;
		for (i = 0; i < 8; i++)
			m.mask |= (unsigned long long)p[10 + i] << (8 * i);
	} else if (direction == 1 && size == 14 && p[0] == 2 &&
		   p[5] == 7 && p[7] == 2 && p[8] == 4 && !p[9]) {
		m.kind = 2;
		m.status = p[10] | ((unsigned int)p[11] << 8);
		m.error = p[12] | ((unsigned int)p[13] << 8);
	}
	if (m.kind)
		m.tag = p[1] | ((unsigned int)p[2] << 8);
	return m;
}

#define C5_REG_SLOTS 8
struct c5_reg_slot {
	unsigned int valid, node, client, server, tag, ack, status, error;
	unsigned long long mask, request_seq;
};
struct c5_reg_cache {
	unsigned long long requests, responses, unmatched, evicted;
	unsigned int next;
	struct c5_reg_slot slots[C5_REG_SLOTS];
};

/* Caller serializes updates and snapshots; no kernel/transport call occurs. */
static inline void c5_reg_note(struct c5_reg_cache *c,
		unsigned int node, unsigned int client, unsigned int server,
		const struct c5_reg_message *m)
{
	struct c5_reg_slot *s = 0;
	unsigned int i, empty = C5_REG_SLOTS;
	if (!c || !m || (m->kind != 1 && m->kind != 2))
		return;
	for (i = 0; i < C5_REG_SLOTS; i++) {
		if (!c->slots[i].valid) {
			if (empty == C5_REG_SLOTS)
				empty = i;
			continue;
		}
		if (c->slots[i].node == node && c->slots[i].client == client &&
		    c->slots[i].server == server) {
			s = &c->slots[i];
			break;
		}
	}
	if (m->kind == 2) {
		c->responses++;
		if (!s || s->tag != m->tag || s->ack) {
			c->unmatched++;
			return;
		}
		s->ack = 1;
		s->status = m->status;
		s->error = m->error;
		return;
	}
	c->requests++;
	if (!s) {
		if (empty != C5_REG_SLOTS)
			s = &c->slots[empty];
		else {
			s = &c->slots[c->next];
			c->next = (c->next + 1) % C5_REG_SLOTS;
			c->evicted++;
		}
	}
	*s = (struct c5_reg_slot){ 0 };
	s->valid = 1;
	s->node = node;
	s->client = client;
	s->server = server;
	s->tag = m->tag;
	s->mask = m->mask;
	s->request_seq = c->requests;
}
#endif
