/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _LINUX_C5_GNSS_PATH_H
#define _LINUX_C5_GNSS_PATH_H

#include <linux/types.h>

struct file;
enum c5_gnss_path_stage {
	C5_GNSS_PIL_PROXY_VOTE = 1,
	C5_GNSS_PIL_PROXY_UNVOTE,
	C5_GNSS_PIL_IMAGE_INIT,
	C5_GNSS_PIL_MEMORY_SETUP,
	C5_GNSS_PIL_AUTH_RESET,
	C5_GNSS_RFSA_ALLOCATED,
	C5_GNSS_RFSA_PERMISSION,
	C5_GNSS_RFSA_MMAP,
	C5_GNSS_RFSA_REQUEST,
	C5_GNSS_RFSA_RESPONSE,
	C5_GNSS_RMTS_BLOCK_READ,
	C5_GNSS_RMTS_BLOCK_WRITE,
	C5_GNSS_PIL_SEGMENT_LOAD,
	C5_GNSS_RFSA_SCM_CAPABILITY,
	C5_GNSS_RFSA_DMA_STATE,
	C5_GNSS_RFSA_CPU_PHYS,
	C5_GNSS_RFSA_UIO_ATTR,
	C5_GNSS_RFSA_SCM_CALL_ABI,
	C5_GNSS_IPC_TX,
	C5_GNSS_IPC_RX_POSITION,
	C5_GNSS_IPC_RX_SV,
	C5_GNSS_IPC_RX_OTHER,
	C5_GNSS_IPC_TX_ERROR,
	C5_GNSS_IPC_BAD_HEADER,
	C5_GNSS_PATH_STAGE_LIMIT
};

/* Built-in only: the VFS cannot depend on a loadable UIO module. */
#ifdef CONFIG_UIO_MSM_SHAREDMEM
bool c5_gnss_path_target(void);
void c5_gnss_path_scm_capabilities(void);
void c5_gnss_path_note(unsigned int stage, u64 arg0, u64 arg1, s64 result);
void c5_gnss_path_io(unsigned int stage, struct file *file, size_t count,
		     ssize_t result);
#else
static inline bool c5_gnss_path_target(void) { return false; }
static inline void c5_gnss_path_scm_capabilities(void) { }
static inline void c5_gnss_path_note(unsigned int stage, u64 arg0, u64 arg1,
				     s64 result) { }
static inline void c5_gnss_path_io(unsigned int stage, struct file *file,
				   size_t count, ssize_t result) { }
#endif
#endif
