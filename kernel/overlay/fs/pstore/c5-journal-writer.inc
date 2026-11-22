/* SPDX-License-Identifier: GPL-2.0-only */
/* Inside ram.c after ramoops_context, recovery diagnostic build only. */
#include <linux/c5-redundant-journal.h>
#ifdef CONFIG_PSTORE_FTRACE
#error "C5 journal requires native pstore ftrace writes disabled"
#endif
static volatile u32 *c5_journal_ring;
static u64 c5_journal_sequence;
static u32 c5_journal_generation;
static u32 c5_journal_busy;
static bool c5_journal_frozen;
static volatile u32 *c5_remote_tail;
static u32 c5_remote_busy;

/* Independent single-writer ownership of the previously unused last64 bytes.
 * It never takes the ordinary journal lock,so a stalled normal writer cannot
 * suppress the other cluster's observation. Removal must own both locks.
 */
volatile u32 *notrace c5_pstore_remote_claim(void)
{
	volatile u32 *tail;
	if (cmpxchg(&c5_remote_busy, 0U, 1U) != 0U)
		return NULL;
	tail = smp_load_acquire(&c5_remote_tail);
	if (!tail)
		smp_store_release(&c5_remote_busy, 0U);
	return tail;
}

void notrace c5_pstore_remote_release(void)
{
	smp_store_release(&c5_remote_busy, 0U);
}

static bool c5_journal_detach(void)
{
	if (cmpxchg(&c5_journal_busy, 0U, 1U) != 0U)
		return false;
	if (cmpxchg(&c5_remote_busy, 0U, 1U) != 0U) {
		smp_store_release(&c5_journal_busy, 0U);
		return false;
	}
	WRITE_ONCE(c5_journal_ring, NULL);
	WRITE_ONCE(c5_remote_tail, NULL);
	smp_store_release(&c5_remote_busy, 0U);
	smp_store_release(&c5_journal_busy, 0U);
	return true;
}

static void notrace c5_journal_record(u16 stage, u64 v0, u64 v1,
				     u64 v2, bool fatal)
{
	unsigned long flags;
	bool ok;
	if (!smp_load_acquire(&c5_journal_ring) || READ_ONCE(c5_journal_frozen))
		return;
	raw_local_irq_save(flags);
	if (cmpxchg(&c5_journal_busy, 0U, 1U) != 0U)
		goto out; /* Skip nested/concurrent writers; never wait for a CPU. */
	if (c5_journal_ring && !c5_journal_frozen) {
		ok = c5j_write(c5_journal_ring, c5_journal_sequence,
			c5_journal_generation, stage, raw_smp_processor_id(),
			v0, v1, v2, fatal);
		if (!ok || c5_journal_sequence == ~(u64)0)
			WRITE_ONCE(c5_journal_ring, NULL);
		else
			c5_journal_sequence++;
		if (ok && fatal)
			WRITE_ONCE(c5_journal_frozen, true);
	}
	smp_store_release(&c5_journal_busy, 0U);
out:
	raw_local_irq_restore(flags);
}

static void c5_journal_arm(struct ramoops_context *cxt)
{
	volatile u32 *ring;
	/* Same native WC mapping; no new fixmap, MMIO or general-RAM address. */
	if (cxt->phys_addr != 0x40400000ULL || cxt->size != 0x100000 ||
	    cxt->record_size != 0x40000 || cxt->console_size != 0x40000 ||
	    cxt->ftrace_size != 0x40000 || cxt->pmsg_size != 0x40000 ||
	    cxt->memtype || cxt->ecc_info.ecc_size || !cxt->fprz ||
	    cxt->fprz->paddr != 0x40480000ULL ||
	    cxt->fprz->size != 0x40000 || !cxt->fprz->vaddr)
		return;
	ring = (volatile u32 *)((char *)cxt->fprz->vaddr + C5J_OFFSET);
	if (!c5j_resume(ring, &c5_journal_sequence, &c5_journal_generation))
		return;
	smp_store_release(&c5_remote_tail, ring +
		(C5J_SLOTS * C5J_COPIES * C5J_WORDS));
	smp_store_release(&c5_journal_ring, ring);
	c5_journal_record(0x73e0, 0x2026101700000065ULL,
			  0x404bc000ULL, C5J_BYTES, false);
}
