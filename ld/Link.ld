/*
 * Linker script for CH32V208 (CH32V20x_D8W).
 *
 * Flash/RAM split configured as: 128KB code flash + 352KB data flash,
 * 64KB SRAM (the "FLASH-128K + RAM-64K" option in the WCH SDK).
 *
 * 0x0800_0000 .. 0x0807_0000  : application image (448K window; first 128K
 *                                is zero-wait-state code flash, the rest is
 *                                the re-mapped extended code region)
 * 0x0807_0000 .. 0x0807_8000  : reserved for NV data / bond storage (32K)
 * 0x2000_0000 .. 0x2001_0000  : 64KB SRAM
 *
 * Based on the WCH EVT BLE HAL Link.ld.
 */

ENTRY( _start )

__stack_size = 2048;

PROVIDE( _stack_size = __stack_size );

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 448K
    RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 64K
}

SECTIONS
{
    .init :
    {
        _sinit = .;
        . = ALIGN(4);
        KEEP(*(SORT_NONE(.init)))
        . = ALIGN(4);
        _einit = .;
    } >FLASH AT>FLASH

    .vector :
    {
        *(.vector);
        . = ALIGN(64);
        KEEP(*(SORT_NONE(.handle_reset)))
    } >FLASH AT>FLASH

    /* ISR-critical code: keeps LLE/BB/TIM3 interrupt paths in the
     * zero-wait-state first 128K and lets it be located early. */
    .highcode :
    {
        . = ALIGN(4);
        *(.highcode);
        *(.highcode.*);
        . = ALIGN(4);
    } >FLASH AT>FLASH

    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata*)
        *(.sdata2.*)
        *(.glue_7)
        *(.glue_7t)
        *(.gnu.linkonce.t.*)
        . = ALIGN(4);
    } >FLASH AT>FLASH

    .fini :
    {
        KEEP(*(SORT_NONE(.fini)))
        . = ALIGN(4);
    } >FLASH AT>FLASH

    PROVIDE( _etext = . );
    PROVIDE( _eitcm = . );

    .preinit_array  :
    {
        PROVIDE_HIDDEN (__preinit_array_start = .);
        KEEP (*(.preinit_array))
        PROVIDE_HIDDEN (__preinit_array_end = .);
    } >FLASH AT>FLASH

    .init_array     :
    {
        PROVIDE_HIDDEN (__init_array_start = .);
        KEEP (*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP (*(.init_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o ) .ctors))
        PROVIDE_HIDDEN (__init_array_end = .);
    } >FLASH AT>FLASH

    .fini_array     :
    {
        PROVIDE_HIDDEN (__fini_array_start = .);
        KEEP (*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP (*(.fini_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o ) .dtors))
        PROVIDE_HIDDEN (__fini_array_end = .);
    } >FLASH AT>FLASH

    .ctors          :
    {
        KEEP (*crtbegin.o(.ctors))
        KEEP (*crtbegin?.o(.ctors))
        KEEP (*(EXCLUDE_FILE (*crtend.o *crtend?.o ) .ctors))
        KEEP (*(SORT(.ctors.*)))
        KEEP (*(.ctors))
    } >FLASH AT>FLASH

    .dtors          :
    {
        KEEP (*crtbegin.o(.dtors))
        KEEP (*crtbegin?.o(.dtors))
        KEEP (*(EXCLUDE_FILE (*crtend.o *crtend?.o ) .dtors))
        KEEP (*(SORT(.dtors.*)))
        KEEP (*(.dtors))
    } >FLASH AT>FLASH

    .data : ALIGN(8)
    {
        *(.gnu.linkonce.r.*)
        *(.data .data.*)
        *(.gnu.linkonce.d.*)
        . = ALIGN(8);
        PROVIDE( __global_pointer$ = . + 0x800 );
        *(.sdata .sdata.*)
        *(.gnu.linkonce.s.*)
        . = ALIGN(8);
        *(.srodata.cst16)
        *(.srodata.cst8)
        *(.srodata.cst4)
        *(.srodata.cst2)
        *(.srodata .srodata.*)
        . = ALIGN(4);
        PROVIDE( _edata = .);
    } >RAM AT>FLASH

    /* Derive CRT copy boundaries from the actual output section.  A
     * preceding fixed-address section (e.g. .farcode) can move its LMA. */
    PROVIDE( _data_lma = LOADADDR(.data) );
    PROVIDE( _data_vma = ADDR(.data) );

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        PROVIDE( _sbss = .);
        *(.sbss*)
        *(.gnu.linkonce.sb.*)
        *(.bss*)
        *(.gnu.linkonce.b.*)
        *(COMMON*)
        . = ALIGN(4);
        PROVIDE( _ebss = .);
    } >RAM AT>FLASH

    /* heap for newlib _sbrk (used by NimBLE mem.c malloc path).
     * Fixed 15K cap: leave space for the additional LE CoC channel state.
     * MSYS/LL pools are static; newlib serves small host allocations.
     * an explicit size keeps remaining RAM visible in --print-memory-usage
     * instead of this NOLOAD region silently absorbing all slack. */
    .heap (NOLOAD) :
    {
        . = ALIGN(8);
        PROVIDE( _heap_start = . );
        . = _heap_start + 0x3c00;
        PROVIDE( _heap_end = . );
    } >RAM

    PROVIDE( _end = _heap_end );
    PROVIDE( end = . );

    .stack ORIGIN(RAM) + LENGTH(RAM) - __stack_size :
    {
        . = ALIGN(4);
        PROVIDE(_susrstack = . );
        . = . + __stack_size;
        PROVIDE( _eusrstack = .);
    } >RAM
}
