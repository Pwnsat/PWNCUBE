OUTPUT_ARCH( "riscv" )
ENTRY(_start)

/*
 * MCU IRAM origin. rtthread.bin is linked for 0x40000 (the RV1106 HPMCU IRAM),
 * which is where the rv1106_hpmcu_wrap trampoline loads/jumps it. Do NOT relink
 * to a DDR address and release the core there — executing from DDR hangs the SPL
 * and bricks the boot (recover via the SPI-NAND CLK-short maskrom).
 */
MEMORY {
    RAM (rwx) : ORIGIN = 0x40000, LENGTH = 0x3c000
}

STACK_SIZE = (1024 * 2);

CL_SIZE = 0;

/*
 * Shared DDR region for the A7<->RISC-V RPMsg vrings (CubeSat Paso 0).
 * This is in DDR (the MCU can address the full memory map), NOT in the MCU
 * SRAM window above. The base/size MUST match the Linux device-tree
 * reserved-memory / rpmsg "reg" node. See docs/migration/60-*.
 */
__linux_share_rpmsg_start__ = 0x0FF00000;
__linux_share_rpmsg_end__   = 0x0FF00000 + 0x00100000;   /* 1 MB */

SECTIONS {
    .text : {
        PROVIDE(__TEXT_START__ = .);
        *start_rv1106_mcu.o(.text .text.*)
        *(.text .text.*)
        *(sc_test_section)

        /* section information for finsh shell */
        . = ALIGN(4);
        __fsymtab_start = .;
        KEEP(*(FSymTab))
        __fsymtab_end = .;
        . = ALIGN(4);
        __vsymtab_start = .;
        KEEP(*(VSymTab))
        __vsymtab_end = .;
        . = ALIGN(4);

        . = ALIGN(4);
        __rt_init_start = .;
        KEEP(*(SORT(.rti_fn*)))
        __rt_init_end = .;
        . = ALIGN(4);

        /* section information for modules */
        . = ALIGN(4);
        __rtmsymtab_start = .;
        KEEP(*(RTMSymTab))
        __rtmsymtab_end = .;

        . = ALIGN(CL_SIZE);
        PROVIDE(__TEXT_END__ = .);
    } > RAM

    /* data segment */
    .data : {
        PROVIDE(__DATA_START__ = .);
        *(.data .data.*)
        . = ALIGN(CL_SIZE);
        PROVIDE(__DATA_END__ = .);
    } > RAM

    .sdata : {
        _gp = . + 0x800;
        *(.srodata.cst16) *(.srodata.cst8) *(.srodata.cst4) *(.srodata.cst2) *(.srodata*)
        *(.sdata .sdata.* .gnu.linkonce.s.*)
        . = ALIGN(CL_SIZE);
    } > RAM

    /* thread-local data segment */
    .tdata : {
        PROVIDE(_tls_data = .);
        PROVIDE(_tdata_begin = .);
        *(.tdata .tdata.*)
        PROVIDE(_tdata_end = .);
        . = ALIGN(CL_SIZE);
    } > RAM

    .tbss : {
        PROVIDE(__BSS_START__ = .);
        *(.tbss .tbss.*)
        . = ALIGN(CL_SIZE);
        PROVIDE(_tbss_end = .);
    } > RAM

    /* bss segment */
    .sbss : {
        *(.sbss .sbss.* .gnu.linkonce.sb.*)
        *(.scommon)
    } > RAM

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(CL_SIZE);
        PROVIDE(__BSS_END__ = .);
    } > RAM

    .stack (NOLOAD): {
        FILL(0);
        PROVIDE(__STACK_START__ = .);
        . += STACK_SIZE;
        PROVIDE(__C_STACK_TOP__ = .);
        PROVIDE(_sp = .);
        PROVIDE(__STACK_END__ = .);
    } > RAM

    .save_data (NOLOAD):
    {
        . = . + 8;
        . = ALIGN(8);
        SAVE_DATA_LENGTH = _tdata_end  - __TEXT_END__;
        PROVIDE ( __SAVE_DATA_START__ = .);
        . = . + SAVE_DATA_LENGTH;
        . = ALIGN(8);
        PROVIDE ( __SAVE_DATA_END__ = .);
    } > RAM

   /* End of uninitalized data segement */
    ._user_heap (NOLOAD):
    {
        . = ALIGN(8);
        PROVIDE ( end = . );
        PROVIDE ( _end = . );
        PROVIDE ( __HEAP_START__ = .);
        HEAP_SIZE  = ORIGIN(RAM) + LENGTH(RAM) - __HEAP_START__;
        . = . + HEAP_SIZE;
        . = ALIGN(8);
        PROVIDE ( __HEAP_END__ = .);
        PROVIDE ( _heap_end = .);
    } > RAM
}
