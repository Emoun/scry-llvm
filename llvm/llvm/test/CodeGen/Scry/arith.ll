; RUN: llc -mtriple=scry32-unknown-none-elf -O2 < %s | FileCheck %s
; RUN: llc -mtriple=scry32-unknown-none-elf -O0 < %s | FileCheck %s
; RUN: llc -mtriple=scry32-unknown-none-elf -O2 -filetype=obj < %s -o %t.o
; RUN: llvm-readelf -h %t.o | FileCheck %s --check-prefix=ELF

; ELF: Machine: {{.*}}264

; CHECK-LABEL: add32:
; The first argument is spread to its slot, loaded back, added and returned
; through the return trigger.
; CHECK: echo =>{{[0-9]+}}, =>{{[0-9]+}}
; CHECK: st [{{[0-9]+}}]
; CHECK: add Low =>0
; CHECK: ret 0
define i32 @add32(i32 %a, i32 %b) {
  %r = add i32 %a, %b
  ret i32 %r
}

; CHECK-LABEL: sdiv32:
; Signed division is a libcall (Scry division is Euclidean).
; CHECK: const u32, %b3(__divsi3)
; CHECK: grow %b2(__divsi3)
; CHECK: grow %b1(__divsi3)
; CHECK: grow %b0(__divsi3)
; CHECK: call 0
define i32 @sdiv32(i32 %a, i32 %b) {
  %r = sdiv i32 %a, %b
  ret i32 %r
}

; CHECK-LABEL: udiv64:
; CHECK: div Low =>0
define i64 @udiv64(i64 %a, i64 %b) {
  %r = udiv i64 %a, %b
  ret i64 %r
}

; CHECK-LABEL: urem8:
; CHECK: div High =>0
define i8 @urem8(i8 %a, i8 %b) {
  %r = urem i8 %a, %b
  ret i8 %r
}

; CHECK-LABEL: ashr32:
; Arithmetic shifts re-tag the value as signed and negate the amount.
; CHECK: cast i32, =>{{[0-9]+}}
; CHECK: const i32, 0
; CHECK: sub Low =>0
; CHECK: sh Low =>0
define i32 @ashr32(i32 %a, i32 %n) {
  %r = ashr i32 %a, %n
  ret i32 %r
}

; CHECK-LABEL: slt32:
; CHECK: cast i32, =>{{[0-9]+}}
; CHECK: cast i32, =>0
; CHECK: lt =>0
define i1 @slt32(i32 %a, i32 %b) {
  %r = icmp slt i32 %a, %b
  ret i1 %r
}

; CHECK-LABEL: sext8:
; CHECK: cast i8, =>0
; CHECK: cast u32, =>0
define i32 @sext8(i8 %a) {
  %r = sext i8 %a to i32
  ret i32 %r
}

; CHECK-LABEL: const64:
; 1234567890000 = 0x11F71FB0450: most significant byte first, then grow.
; CHECK: const u64, 1
; CHECK: grow 31
; CHECK: grow 113
; CHECK: grow 251
; CHECK: grow 4
; CHECK: grow 80
define i64 @const64() {
  ret i64 1234567890000
}
