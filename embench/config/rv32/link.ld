/* Linker script for the rv32imac comparison images on Spike: everything in
   the RAM Spike provides at 0x80000000, a 1 MiB stack after .bss. */
OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
  RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 64M
}

SECTIONS
{
  .text :
  {
    KEEP(*(.text.init))
    *(.text .text.*)
  } > RAM

  .rodata ALIGN(8) :
  {
    *(.rodata .rodata.*)
    *(.srodata .srodata.*)
  } > RAM

  .tohost ALIGN(4096) :
  {
    KEEP(*(.tohost))
  } > RAM

  .data ALIGN(8) :
  {
    __data_start = .;
    *(.data .data.*)
    *(.sdata .sdata.*)
  } > RAM
  PROVIDE(__global_pointer$ = __data_start + 0x800);

  .bss ALIGN(8) :
  {
    __bss_start = .;
    *(.sbss .sbss.*)
    *(.bss .bss.*)
    *(COMMON)
    . = ALIGN(8);
    __bss_end = .;
  } > RAM

  . = ALIGN(16);
  __stack_bottom = .;
  . += 1M;
  __stack_top = .;

  /DISCARD/ : { *(.eh_frame) *(.comment) }
}
