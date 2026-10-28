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
// Up buffer: host reads our logs (4 KB so banner + bursts survive
// between RTT viewer polls in NO_BLOCK_SKIP mode).
// Down buffer: host -> target input, kept small.
//
#define SEGGER_RTT_BUFFER_SIZE_UP     (4096)    // Bytes
#define SEGGER_RTT_BUFFER_SIZE_DOWN   (64)      // Bytes

//
// Default operating mode: drop newest bytes when the up buffer is full
// instead of stalling the writer.  Safe from tasks and ISRs without a
// host attached.
//
#define SEGGER_RTT_MODE_DEFAULT       SEGGER_RTT_MODE_NO_BLOCK_SKIP

//
// Locking for multi-task / ISR use: save & restore the MIE bit of
// mstatus (global interrupt enable) on the RISC-V machine mode.
// GCC (used to compile the .c files, which is where these macros are
// expanded) supports the inline asm below.
//
#if defined(__GNUC__) && (defined(__riscv) || defined(__riscv_xlen))
  #define SEGGER_RTT_LOCK()  {                                             \
                                 unsigned int _SEGGER_RTT__LockState;      \
                                 __asm volatile ("csrr  %0, mstatus  \n\t" \
                                                 "csrci mstatus, 8   \n\t" \
                                                 "andi  %0, %0,  8   \n\t" \
                                                 : "=r" (_SEGGER_RTT__LockState) \
                                                 :                         \
                                                 :                         \
                                                 );                        \
    #define SEGGER_RTT_UNLOCK()    __asm volatile ("csrr  a1, mstatus  \n\t" \
                                                   "or    %0, %0, a1   \n\t" \
                                                   "csrs  mstatus, %0  \n\t" \
                                                   :                       \
                                                   : "r"  (_SEGGER_RTT__LockState) \
                                                   : "a1"                  \
                                                   );                      \
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
