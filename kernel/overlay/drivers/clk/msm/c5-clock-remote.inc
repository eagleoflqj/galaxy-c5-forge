/* SPDX-License-Identifier: GPL-2.0-only */
/* Included after RCGR macros. Diagnostic only; no native clock action changes.
 * A single asynchronous IPI targets the other physical cluster in the checked
 * C5 map:physical cluster0 or0x100. Select by runtime map,not logical CPU
 * numbering or the producer's cluster. Never wait for the target to
 * complete. Readiness waiting is bounded1000 one-us iterations,then native
 * execution continues even if no observer ran. Read CMD/CFG once before
 * readiness;then at most4194304 RAM-only heartbeat iterations and20 commits.
 * No timer read or delay call is made inside the callback heartbeat loop:
 * udelay itself depends on CNTVCT advancing and is not an independent liveness
 * bound. cpu_relax is an instruction hint,not a calibrated time interval.
 * Frozen initial MMIO samples are never presented as current register values.
 */
#ifdef CONFIG_C5_RECOVERY_ONLY
#include <linux/cpu.h>
#include <linux/smp.h>
#include <asm/smp_plat.h>
#include <asm/processor.h>
#include <linux/c5-clock-remote.h>
static struct {
	struct mux_div_clk *md;
	u32 once, ready, phase, producer, target, maps, precommand_once;
} c5_remote_clock;
static struct call_single_data c5_remote_csd;

static bool c5_remote_hwid_valid(u64 hwid)
{
	return !(hwid & ~0x103ULL);
}

static void notrace c5_rcg_remote_callback(void *unused)
{
	volatile u32 *tail = c5_pstore_remote_claim();
	struct mux_div_clk *md = c5_remote_clock.md;
	u32 observer = raw_smp_processor_id();
	u32 phase = 1U, cmd = 0U, cfg = 0U, flags = 0U;
	u32 old_phase = 0U;
	u32 iteration, sequence = 0U;
	(void)unused;
	if (!tail)
		goto unavailable;
	if (observer >= 8U || observer != c5_remote_clock.target ||
	    !c5_remote_hwid_valid(cpu_logical_map(observer)) ||
	    !c5_remote_hwid_valid(cpu_logical_map(c5_remote_clock.producer)) ||
	    (u32)cpu_logical_map(observer) != (c5_remote_clock.maps >> 16) ||
	    (u32)cpu_logical_map(c5_remote_clock.producer) !=
		(c5_remote_clock.maps & 0xffffU))
		goto out;
	cmd = readl_relaxed(RCGR_CMD_REG(md));
	cfg = readl_relaxed(RCGR_SRC_REG(md));
	if (cmd & CMD_RCGR_CONFIG_UPDATE_BIT)
		flags |= 0x100U;
	for (iteration = 0; iteration < C5R_LOOPS; iteration++) {
		phase = READ_ONCE(c5_remote_clock.phase);
		if ((phase != old_phase || iteration == 1U || iteration == 4096U ||
		     iteration == 65536U || iteration == 1048576U ||
		     iteration == 4194303U) &&
		    sequence < C5R_MAX_WRITES - 1U) {
			if (!c5r_write(tail, ++sequence, phase, observer,
				c5_remote_clock.producer, cmd, cfg, iteration,
				flags, c5_remote_clock.maps))
				goto out;
			old_phase = phase;
		}
		smp_store_release(&c5_remote_clock.ready, 1U);
		if (phase >= 13U)
			break;
		cpu_relax();
	}
	flags |= phase >= 13U ? 1U : 2U;
	(void)c5r_write(tail, ++sequence, phase, observer,
		c5_remote_clock.producer, cmd, cfg, iteration, flags,
		c5_remote_clock.maps);
out:
	c5_pstore_remote_release();
unavailable:
	smp_store_release(&c5_remote_clock.ready, 2U);
	WRITE_ONCE(c5_remote_clock.once, 2U);
}

static void c5_rcg_remote_start(struct mux_div_clk *md, u32 src_sel)
{
	u32 producer = raw_smp_processor_id();
	unsigned int cpu, target = 8U, count;
	int rc;
	if (!c5_boot_trace_rate_active() || producer >= 8U || src_sel != 5U ||
	    !c5_remote_hwid_valid(cpu_logical_map(producer)) ||
	    !md->c.dbg_name || strcmp(md->c.dbg_name, "a53ssmux_bc"))
		return;
	if (cmpxchg(&c5_remote_clock.once, 0U, 1U) != 0U)
		return;
	for_each_online_cpu(cpu)
		if (cpu < 8U && cpu != producer &&
		    c5_remote_hwid_valid(cpu_logical_map(cpu)) &&
		    ((cpu_logical_map(cpu) ^ cpu_logical_map(producer)) & 0x100ULL)) {
			target = cpu;
			break;
		}
	if (target == 8U)
		goto unavailable;
	c5_remote_clock.md = md;
	c5_remote_clock.producer = producer;
	c5_remote_clock.target = target;
	c5_remote_clock.maps = (u32)cpu_logical_map(producer) |
		((u32)cpu_logical_map(target) << 16);
	WRITE_ONCE(c5_remote_clock.phase, 1U);
	/* The preallocated async API explicitly supports IRQ-off callers. Its
	 * storage is static and never reused:once changes0->1->2 only.
	 */
	c5_remote_csd.func = c5_rcg_remote_callback;
	c5_remote_csd.info = NULL;
	rc = smp_call_function_single_async(target, &c5_remote_csd);
	if (rc)
		goto unavailable;
	for (count = 0; count < 1000U; count++) {
		if (smp_load_acquire(&c5_remote_clock.ready))
			break;
		udelay(1);
	}
	return;
unavailable:
	WRITE_ONCE(c5_remote_clock.once, 2U);
}

static void c5_rcg_remote_phase(struct mux_div_clk *md, u32 phase)
{
	if (READ_ONCE(c5_remote_clock.once) == 1U &&
	    c5_remote_clock.md == md &&
	    READ_ONCE(c5_remote_clock.phase) < phase)
		WRITE_ONCE(c5_remote_clock.phase, phase);
}

/* Controlled diagnostic comparison, not a clock fix: select one transaction
 * whose pre-CMD/post-CMD/poll-loop synchronous observers are omitted.
 * Selection was already limited to the first active exact BC/src5 request
 * with a valid runtime physical map and an online opposite-cluster target.
 * RAM-only phases bracket the existing atomic claim; no MMIO, wait or
 * synchronous packet write is added here. Publication changes timing.
 * Claim once even if the queued observer has already finished or failed.
 */
static bool c5_rcg_precommand_control(struct mux_div_clk *md)
{
	bool selected;
	if (!c5_boot_trace_rate_active() || md != c5_remote_clock.md ||
	    raw_smp_processor_id() != c5_remote_clock.producer)
		return false;
	c5_rcg_remote_phase(md, 4U);
	selected = cmpxchg(&c5_remote_clock.precommand_once, 0U, 1U) == 0U;
	c5_rcg_remote_phase(md, 5U);
	return selected;
}
#else
static inline void c5_rcg_remote_start(struct mux_div_clk *md, u32 src) { }
static inline void c5_rcg_remote_phase(struct mux_div_clk *md, u32 phase) { }
static inline bool c5_rcg_precommand_control(struct mux_div_clk *md) { return false; }
#endif
