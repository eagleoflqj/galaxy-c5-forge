/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Recovery-only v14 idmap handoff observation, NOT a boot fix.
 * Stages10..1f retain physical alias only before idmap removal.
 * Stages20..25 use a dedicated guarded one-page Normal-NC TTBR1 fixmap.
 * Only44 bytes of existing console RAM at0x40440000 are accessed.
 * Binary C5EF records carry tag/stage, its bitwise complement and sentinel,
 * twice. Publish only after exact immediate readback; no correction/repair.
 * Readback is not proof of reset retention or hardware progress.
 */
#ifndef _LINUX_C5_START_KERNEL_TRACE_H
#define _LINUX_C5_START_KERNEL_TRACE_H

#if 0 /* Obsolete early markers overwrite the previous native console header. */
#if !defined(CONFIG_ARM64) || !defined(CONFIG_ARM64_4K_PAGES) || CONFIG_ARM64_VA_BITS != 39 || CONFIG_PGTABLE_LEVELS != 3 || defined(CONFIG_RANDOMIZE_BASE) || defined(CONFIG_ARM64_SW_TTBR0_PAN)
#error "Unreviewed C5 early C integrity geometry"
#endif
#define _C5_STR1(x) #x
#define _C5_STR(x) _C5_STR1(x)
#define _C5_PAIR(a, b) \
	"str w10, [x9, #" _C5_STR(a) "]\n\t" \
	"str w10, [x9, #" _C5_STR(b) "]\n\t"
#define _C5_CHECK(a) \
	"ldr w11, [x9, #" _C5_STR(a) "]\n\t" \
	"cmp w11, w10\n\t" \
	"b.ne 1f\n\t"
#define _C5_CHECK_PAIR(a, b) _C5_CHECK(a) _C5_CHECK(b)
#define _C5_MAGIC \
	"movz w10, #0x3543\n\t" \
	"movk w10, #0x4645, lsl #16\n\t"
#define _C5_TAG \
	"movz w10, #%c0\n\t" \
	"movk w10, #0x4353, lsl #16\n\t"
#define _C5_COMPLEMENT \
	"movz w10, #%c1\n\t" \
	"movk w10, #0xbcac, lsl #16\n\t"
#define _C5_SENTINEL \
	"movz w10, #0x3cc3\n\t" \
	"movk w10, #0xa55a, lsl #16\n\t"
#define _c5_address_trace(stage, address) do { \
	(void)sizeof(char[((stage) >= 0x10 && (stage) <= 0x25) ? 1 : -1]); \
	asm volatile( \
		"movz x9, #%c2\n\t" \
		"movk x9, #%c3, lsl #16\n\t" \
		"movk x9, #%c4, lsl #32\n\t" \
		"movk x9, #%c5, lsl #48\n\t" \
		"str wzr, [x9]\n\t" \
		_C5_MAGIC _C5_PAIR(12, 28) \
		_C5_TAG _C5_PAIR(16, 32) \
		_C5_COMPLEMENT _C5_PAIR(20, 36) \
		_C5_SENTINEL _C5_PAIR(24, 40) \
		"movz w10, #32\n\t" \
		"str w10, [x9, #4]\n\t" \
		"str w10, [x9, #8]\n\t" \
		"dsb sy\n\t" \
		_C5_CHECK_PAIR(4, 8) \
		_C5_MAGIC _C5_CHECK_PAIR(12, 28) \
		_C5_TAG _C5_CHECK_PAIR(16, 32) \
		_C5_COMPLEMENT _C5_CHECK_PAIR(20, 36) \
		_C5_SENTINEL _C5_CHECK_PAIR(24, 40) \
		"movz w10, #0x4244\n\t" \
		"movk w10, #0x4347, lsl #16\n\t" \
		"str w10, [x9]\n\t" \
		"dsb sy\n\t" \
		"1:\n\t" \
		: : "i" (stage), "i" ((~(stage)) & 0xffff), \
		    "i" ((address) & 0xffffUL), "i" (((address) >> 16) & 0xffffUL), \
		    "i" (((address) >> 32) & 0xffffUL), "i" (((address) >> 48) & 0xffffUL) \
		: "x9", "x10", "x11", "cc", "memory"); \
} while (0)
#define c5_start_trace(stage) do { \
	(void)sizeof(char[((stage) >= 0x10 && (stage) <= 0x1f) ? 1 : -1]); \
	_c5_address_trace(stage, 0x40440000UL); \
} while (0)
#define c5_fixed_trace_if(ready, stage) do { \
	(void)sizeof(char[((stage) >= 0x20 && (stage) <= 0x25) ? 1 : -1]); \
	if (ready) _c5_address_trace(stage, __fix_to_virt(FIX_C5_TRACE)); \
} while (0)
#else
#define c5_start_trace(stage) do { } while (0)
#define c5_fixed_trace_if(ready, stage) do { } while (0)
#endif
#endif
