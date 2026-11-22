/* SPDX-License-Identifier: GPL-2.0-only */
/* Diagnostic v33 first-oops-slot record, not crash recovery or authentication. */
#ifndef _C5_PSTORE_RECORD_H
#define _C5_PSTORE_RECORD_H
#define C5_PSTORE_MAGIC 0x33503543U /* C5P3 */
#define C5_PSTORE_SPAN_BYTES 100U
#define C5_PSTORE_PAYLOAD_BYTES 88U

#ifndef C5_PSTORE_LOAD
#define C5_PSTORE_LOAD(p) READ_ONCE(*(p))
#define C5_PSTORE_STORE(p, v) WRITE_ONCE(*(p), (v))
#define C5_PSTORE_BARRIER() dsb(sy)
#endif

static inline bool c5_pstore_record_write(volatile u32 *ring, u16 stage,
					u64 value0, u64 value1, u64 value2)
{
	u32 words[10];
	u32 checksum = 0xa55a3cc3U;
	unsigned int i;

	words[0] = C5_PSTORE_MAGIC;
	words[1] = 0x43530000U | stage;
	words[2] = ~words[1];
	words[4] = (u32)value0;
	words[5] = (u32)(value0 >> 32);
	words[6] = (u32)value1;
	words[7] = (u32)(value1 >> 32);
	words[8] = (u32)value2;
	words[9] = (u32)(value2 >> 32);
	for (i = 0; i < 10; i++)
		if (i != 3)
			checksum ^= words[i];
	words[3] = checksum;

	C5_PSTORE_STORE(ring, 0); /* Invalidate before any payload mutation. */
	/* Valid legacy/native uncompressed ramoops header: ====0.0\n.
	 * Zero is framing only, never represented as a measured timestamp. */
	C5_PSTORE_STORE(ring + 3, 0x3d3d3d3dU);
	C5_PSTORE_STORE(ring + 4, 0x0a302e30U);
	for (i = 0; i < 10; i++) {
		C5_PSTORE_STORE(ring + 5 + i, words[i]);
		C5_PSTORE_STORE(ring + 15 + i, words[i]);
	}
	C5_PSTORE_STORE(ring + 1, C5_PSTORE_PAYLOAD_BYTES);
	C5_PSTORE_STORE(ring + 2, C5_PSTORE_PAYLOAD_BYTES);
	C5_PSTORE_BARRIER();
	if (C5_PSTORE_LOAD(ring + 1) != C5_PSTORE_PAYLOAD_BYTES ||
	    C5_PSTORE_LOAD(ring + 2) != C5_PSTORE_PAYLOAD_BYTES ||
	    C5_PSTORE_LOAD(ring + 3) != 0x3d3d3d3dU ||
	    C5_PSTORE_LOAD(ring + 4) != 0x0a302e30U)
		return false;
	for (i = 0; i < 10; i++)
		if (C5_PSTORE_LOAD(ring + 5 + i) != words[i] ||
		    C5_PSTORE_LOAD(ring + 15 + i) != words[i])
			return false;
	C5_PSTORE_STORE(ring, 0x43474244U); /* Native persistent RAM signature. */
	C5_PSTORE_BARRIER();
	return true;
}
#endif
