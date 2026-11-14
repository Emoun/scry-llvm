/* Start-up code for the rv32imac comparison images on Spike.
 *
 * Spike starts at the ELF entry point with nothing set up.  _start sets the
 * global and stack pointers, clears .bss, calls __libc_init and main, and
 * reports main's return value to the host through the HTIF "tohost"
 * register (an exit command is (code << 1) | 1), which ends the simulation
 * with that exit code.  tohost/fromhost live in their own section so that
 * the linker script can place them where Spike finds them by symbol.
 */
        .section .text.init,"ax",@progbits
        .globl _start
        .type _start,@function
_start:
        .option push
        .option norelax
        la      gp, __global_pointer$
        .option pop
        la      sp, __stack_top
        la      t0, __bss_start
        la      t1, __bss_end
1:      bgeu    t0, t1, 2f
        sw      zero, 0(t0)
        addi    t0, t0, 4
        j       1b
2:      call    __libc_init
        li      a0, 0
        li      a1, 0
        call    main
        slli    a0, a0, 1
        ori     a0, a0, 1
        la      t0, tohost
        sw      a0, 0(t0)
        sw      zero, 4(t0)
3:      j       3b
        .size _start, .-_start

        .section .tohost,"aw",@progbits
        .align 6
        .globl tohost
tohost: .dword 0
        .align 6
        .globl fromhost
fromhost: .dword 0
