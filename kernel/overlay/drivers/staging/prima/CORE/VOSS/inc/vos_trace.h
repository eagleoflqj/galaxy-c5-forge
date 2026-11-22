/*
 * Copyright (c) 2011-2015 The Linux Foundation. All rights reserved.
 *
 * Previously licensed under the ISC license by Qualcomm Atheros, Inc.
 *
 *
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all
 * copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
 * WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
 * AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * This file was originally distributed by Qualcomm Atheros, Inc.
 * under proprietary terms before Copyright ownership was assigned
 * to the Linux Foundation.
 */

#if !defined( __VOS_TRACE_H )
#define __VOS_TRACE_H

/**=========================================================================
  
  \file  vos_trace.h
  
  \brief virtual Operating System Servies (vOS)
               
   Trace, logging, and debugging definitions and APIs
  
  ========================================================================*/

/* $Header$ */

/*--------------------------------------------------------------------------
  Include Files
  ------------------------------------------------------------------------*/
#include  <vos_types.h>  // For VOS_MODULE_ID...
#include  <stdarg.h>       // For va_list...
#include  <vos_status.h>
#include  <i_vos_types.h>

/*-------------------------------------------------------------------------- 
  Type declarations 
  ------------------------------------------------------------------------*/

typedef enum 
{
   // NONE means NO traces will be logged.  This value is in place for the 
   // vos_trace_setlevel() to allow the user to turn off all traces.
   VOS_TRACE_LEVEL_NONE = 0,
   
   // the following trace levels are the ones that 'callers' of VOS_TRACE()
   // can specify in for the VOS_TRACE_LEVEL parameter.  Traces are classified
   // by severity (FATAL being more serious than INFO for example).   
   VOS_TRACE_LEVEL_FATAL,
   VOS_TRACE_LEVEL_ERROR, 
   VOS_TRACE_LEVEL_WARN,  
   VOS_TRACE_LEVEL_INFO,
   VOS_TRACE_LEVEL_INFO_HIGH,
   VOS_TRACE_LEVEL_INFO_MED,
   VOS_TRACE_LEVEL_INFO_LOW,
   VOS_TRACE_LEVEL_DEBUG,

   // ALL means all trace levels will be active.  This value is in place for the 
   // vos_trace_setlevel() to allow the user to turn ON all traces.
   VOS_TRACE_LEVEL_ALL, 

   
   // not a real level.  Used to identify the maximum number of 
   // VOS_TRACE_LEVELs defined.
   VOS_TRACE_LEVEL_MAX   
    
} VOS_TRACE_LEVEL;

/* Log types. These types are defined in mailbox*/
typedef enum
{
    WLAN_MGMT_FRAME_LOGS    = 0,
    WLAN_FW_LOGS            = 1,
    WLAN_FW_MEMORY_DUMP     = 2
}FrameLoggingType;

/*-------------------------------------------------------------------------- 
  Preprocessor definitions and constants
  ------------------------------------------------------------------------*/
#define ASSERT_BUFFER_SIZE ( 512 )

// below definition is obsolete and is no longer being used in BMP and WM
// TODO: remove this once this is not used on Android
#define VOS_ENABLE_TRACING 
#define MAX_VOS_TRACE_RECORDS 4000
#define INVALID_VOS_TRACE_ADDR 0xffffffff
#define DEFAULT_VOS_TRACE_DUMP_COUNT 0

#include  <i_vos_trace.h>   

#ifdef TRACE_RECORD

#define CASE_RETURN_STRING( str )           \
    case ( ( str ) ): return( (tANI_U8*)(#str) );

#define MTRACE(p) p
#define NO_SESSION 0xFF

#else
#define MTRACE(p) {  }
#define CASE_RETURN_STRING( str ) { }

#endif

/*--------------------------------------------------------------------------
  Structure definition
  ------------------------------------------------------------------------*/
typedef struct  svosTraceRecord
{
    v_U32_t time;
    v_U8_t module;
    v_U8_t code;
    v_U8_t session;
    v_U32_t data;
}tvosTraceRecord, *tpvosTraceRecord;

typedef struct svosTraceData
{
    // MTRACE logs are stored in ring buffer where head represents the position
    // of first record, tail represents the position of last record added till
    // now and num is the count of total record added.
    v_U32_t head;
    v_U32_t tail;
    v_U32_t num;
    v_U16_t numSinceLastDump;

    //Config for controlling the trace
    v_U8_t enable;
    v_U16_t dumpCount; //will dump after number of records reach this number.

}tvosTraceData;


/*------------------------------------------------------------------------- 
  Function declarations and documenation
  ------------------------------------------------------------------------*/


/*----------------------------------------------------------------------------
  
  \brief vos_trace_setLevel() - Set the trace level for a particular module
  
  This is an external API that allows trace levels to be set for each module.
  
  \param level - trace level.   A member of the VOS_TRACE_LEVEL 
         enumeration indicating the severity of the condition causing the
         trace message to be issued.   More severe conditions are more 
         likely to be logged.
         
  \return  nothing
    
  \sa
  --------------------------------------------------------------------------*/
void vos_trace_setLevel( VOS_MODULE_ID module, VOS_TRACE_LEVEL level );

/**----------------------------------------------------------------------------
  
  \brief vos_trace_getLevel() - Get the trace level
  
  This is an external API that returns a boolean value to signify if a 
  particular trace level is set for the specified module.
  
  \param level - trace level.   A member of the VOS_TRACE_LEVEL enumeration 
                 indicating the severity of the condition causing the trace 
                 message to be issued.
         
                 Note that individual trace levels are the only valid values
                 for this API.  VOS_TRACE_LEVEL_NONE and VOS_TRACE_LEVEL_ALL
                 are not valid input and will return FALSE

  \return  VOS_FALSE - the specified trace level for the specified module is OFF 
    
           VOS_TRUE - the specified trace level for the specified module is ON
    
  \sa vos_trace_setLevel()
  --------------------------------------------------------------------------*/
v_BOOL_t vos_trace_getLevel( VOS_MODULE_ID module, VOS_TRACE_LEVEL level );

typedef void (*tpvosTraceCb) (void *pMac, tpvosTraceRecord, v_U16_t);
typedef void (*tp_vos_state_info_cb) (void);

void vos_trace(v_U8_t module, v_U8_t code, v_U8_t session, v_U32_t data);
void vosTraceRegister(VOS_MODULE_ID, tpvosTraceCb);
void vos_register_debug_callback(VOS_MODULE_ID moduleID,
                                      tp_vos_state_info_cb vosStateInfoCb);
VOS_STATUS vos_trace_spin_lock_init(void);
void vosTraceInit(void);
void vos_register_debugcb_init(void);
void vosTraceEnable(v_U32_t, v_U8_t enable);
void vosTraceDumpAll(void*, v_U8_t, v_U8_t, v_U32_t, v_U32_t);
void vos_state_info_dump_all(void);

/* C5_PNO_DIAG_BEGIN: fixed payload-free ordinary-context instrumentation. */
enum vos_c5_pno_event {
    C5PNO_INIT_CALL,
    C5PNO_HDD_START,
    C5PNO_HDD_PRE_EXIT,
    C5PNO_HDD_PRE_EXIT_RET0,
    C5PNO_HDD_SUBMIT,
    C5PNO_HDD_SME_FAIL,
    C5PNO_HDD_WAIT_TIMEOUT,
    C5PNO_HDD_WAIT_DONE,
    C5PNO_HDD_STOP,
    C5PNO_HDD_STOP_EARLY,
    C5PNO_HDD_STOP_SUBMIT,
    C5PNO_HDD_STOP_TIMEOUT,
    C5PNO_HDD_STOP_DONE,
    C5PNO_PMC_ENTER,
    C5PNO_PMC_FAIL,
    C5PNO_PMC_QUEUED,
    C5PNO_SME_DISPATCH,
    C5PNO_SME_ALLOC_EXIT,
    C5PNO_SME_POST,
    C5PNO_SME_POST_OK,
    C5PNO_SME_POST_NONOK,
    C5PNO_WDA_ENTER,
    C5PNO_WDA_ALLOC_EXIT,
    C5PNO_WDA_WDI_IMMEDIATE_FAIL,
    C5PNO_WDA_REQ_STATUS_FAIL,
    C5PNO_WDI_PROCESS,
    C5PNO_WDI_ACTIVE_REJECT,
    C5PNO_PACK_V0,
    C5PNO_PACK_V1,
    C5PNO_WDI_PACK_FAIL,
    C5PNO_WDI_SEND_CALL,
    C5PNO_WDI_SEND_OK,
    C5PNO_WDI_SEND_NONOK,
    C5PNO_FW_RSP_OK,
    C5PNO_FW_RSP_NONOK,
    C5PNO_WDA_RSP_START_OK,
    C5PNO_WDA_RSP_START_NONOK,
    C5PNO_WDA_RSP_STOP_OK,
    C5PNO_WDA_RSP_STOP_NONOK,
    C5PNO_WDI_MATCH,
    C5PNO_WDA_MATCH_ENTRY,
    C5PNO_WDA_MATCH_POST_OK,
    C5PNO_WDA_MATCH_POST_NONOK,
    C5PNO_SME_MATCH_ENTRY,
    C5PNO_SME_MATCH_CALLBACK,
    C5PNO_HDD_MATCH,
    C5PNO_HDD_MATCH_DEFER,
    C5PNO_HDD_MATCH_DROP,
    C5PNO_HDD_BSS_FAIL,
    C5PNO_HDD_CFG_NOTIFY,
    C5PNO_HDD_RESUME,
    C5PNO_HDD_RESUME_BSS_FAIL,
    C5PNO_HDD_RESUME_NOTIFY,
    C5PNO_WIRE_LEGACY,
    C5PNO_WIRE_MODERN,
    C5PNO_WIRE_INVALID,
    C5PNO_WIRE_BAD_SIZE,
    C5PNO_WIRE_BAD_FRAME,
    C5PNO_WIRE_BAD_FREQ,
    C5PNO_WIRE_AMBIGUOUS,
    C5PNO_EVENT_COUNT
};
#if defined(CONFIG_QTI_MPM_C5LTE) && defined(FEATURE_WLAN_SCAN_PNO) && !defined(MODULE)
#define VOS_C5_PNO_UMAC_HEADER_SIZE 52U
void vos_c5_pno_diag_note(enum vos_c5_pno_event event);
v_U64_t vos_c5_pno_diag_request(int start);
void vos_c5_pno_diag_shape(v_U64_t request, v_U32_t mode, v_U32_t timers,
                         v_U32_t interval, v_U32_t repeat, v_U32_t networks);
void vos_c5_pno_diag_config(v_U32_t support, v_U32_t ssids, v_U32_t matches);
#else
static inline void vos_c5_pno_diag_note(enum vos_c5_pno_event event) { }
static inline v_U64_t vos_c5_pno_diag_request(int start) { return 0; }
static inline void vos_c5_pno_diag_shape(v_U64_t request, v_U32_t mode,
    v_U32_t timers, v_U32_t interval, v_U32_t repeat, v_U32_t networks) { }
static inline void vos_c5_pno_diag_config(v_U32_t support, v_U32_t ssids,
                                        v_U32_t matches) { }
#endif
/* C5_PNO_DIAG_END */
#endif
