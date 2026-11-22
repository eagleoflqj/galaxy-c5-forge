/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef C5_GNSS_IPC_HEADER_H
#define C5_GNSS_IPC_HEADER_H

/* These are classifications, not native return codes or receiver controls. */
enum c5_gnss_ipc_kind {
	C5_IPC_NONE, C5_IPC_TX, C5_IPC_RX_POSITION, C5_IPC_RX_SV,
	C5_IPC_RX_OTHER, C5_IPC_TX_ERROR, C5_IPC_BAD_HEADER
};

struct c5_gnss_ipc_metadata {
	unsigned int kind, message_id, flags, wire_bytes;
};

/* QMI LOC uses a seven-byte header. Never inspect its TLVs or body. */
static inline struct c5_gnss_ipc_metadata
c5_gnss_ipc_header(const unsigned char *header, unsigned int copied,
		   unsigned int wire_bytes, unsigned int direction,
		   unsigned int service)
{
	struct c5_gnss_ipc_metadata result = { 0, 0, 0, 0 };
	unsigned int declared;

	if (service != 0x10 || direction > 2)
		return result;
	result.wire_bytes = wire_bytes;
	if (direction == 2) {
		result.kind = C5_IPC_TX_ERROR;
		return result;
	}
	result.kind = C5_IPC_BAD_HEADER;
	if (!header || copied != 7 || wire_bytes < 7 || wire_bytes > 66000)
		return result;
	declared = header[5] | ((unsigned int)header[6] << 8);
	if (declared + 7 != wire_bytes ||
	    (header[0] != 0 && header[0] != 2 && header[0] != 4))
		return result;
	result.flags = header[0];
	result.message_id = header[3] | ((unsigned int)header[4] << 8);
	result.kind = direction == 0 ? C5_IPC_TX : C5_IPC_RX_OTHER;
	if (direction == 1 && result.flags == 4) {
		if (result.message_id == 0x24)
			result.kind = C5_IPC_RX_POSITION;
		else if (result.message_id == 0x25)
			result.kind = C5_IPC_RX_SV;
	}
	return result;
}
#endif
