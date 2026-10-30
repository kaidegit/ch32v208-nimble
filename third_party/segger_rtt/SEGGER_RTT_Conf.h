/*********************************************************************
*                   (c) SEGGER Microcontroller GmbH                  *
*                        The Embedded Experts                        *
*                           www.segger.com                           *
**********************************************************************

---------------------------END-OF-HEADER------------------------------
Purpose : User configuration file for RTT, tailored for the
          CH32V208 (QingKe V4C, RV32IMAC) NimBLE bring-up.

          Included by SEGGER_RTT_ConfDefaults.h *before* the default
          values, so every define here takes precedence.
----------------------------------------------------------------------
*/

#ifndef SEGGER_RTT_CONF_H
#define SEGGER_RTT_CONF_H

/*********************************************************************
*
*       Defines, configurable
*
**********************************************************************
*/

//
// Actual RTT option names (SEGGER_RTT_BUFFER_SIZE_* were ignored). Keep
// the existing 1 KB / 16 B allocation within this board's 64 KB RAM.
//
#define BUFFER_SIZE_UP               (1024)
#define BUFFER_SIZE_DOWN             (16)

//
// Default operating mode: drop newest bytes when the up buffer is full
// instead of stalling the writer.  Safe from tasks and ISRs without a
// host attached.
//
#define SEGGER_RTT_MODE_DEFAULT       SEGGER_RTT_MODE_NO_BLOCK_SKIP

//
// Locking for multi-task / ISR use: save & restore the MIE bit of
// mstatus (global interrupt enable) on the RISC-V machine mode.
// NOTE: SEGGER_RTT_LOCK opens the enclosing '{' and declares the save
// variable; SEGGER_RTT_UNLOCK consumes it and closes the block (same
// split as SEGGER_RTT_ConfDefaults.h).
//
#if defined(__GNUC__) && (defined(__riscv) || defined(__riscv_xlen))
  #define SEGGER_RTT_LOCK()  {                                            \
      unsigned int _SEGGER_RTT__LockState;                                \
      __asm volatile ( "csrrci %0, mstatus, 8; fence.i"                    \
                       : "=r" ( _SEGGER_RTT__LockState ) : : "memory" );  \

  #define SEGGER_RTT_UNLOCK()                                             \
      __asm volatile ( "csrw mstatus, %0"                                \
                       : : "r" ( _SEGGER_RTT__LockState ) : "memory" );  \
  }
#endif

//
// printf formatting: SEGGER_RTT_printf.c in this tree is integer-only
// (%d/%u/%x/%s/%c); there is no float code path, which keeps the
// formatter free of any soft-float usage on rv32imac.
//
#define SEGGER_RTT_PRINTF_BUFFER_SIZE (64u)

#endif
/*************************** End of file ****************************/
