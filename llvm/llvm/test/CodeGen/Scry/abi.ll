; RUN: llc -mtriple=scry32-unknown-none-elf -O2 < %s | FileCheck %s
; RUN: llc -mtriple=scry32-unknown-none-elf -O0 < %s | FileCheck %s

; Integers narrower than 32 bits are passed and returned as 32-bit values.

declare zeroext i8 @get8()
declare void @take8(i8 signext)
declare void @llvm.memset.p0.i32(ptr, i8, i32, i1)

; CHECK-LABEL: call_ret8:
; The 32-bit result arriving after the call is captured by an echo at the
; trigger slot and truncated before it is used as a byte.
; CHECK: call 0
; CHECK-NEXT: echo =>0
; CHECK-NEXT: cast u8, =>{{[0-9]+}}
define i8 @call_ret8() {
  %r = call zeroext i8 @get8()
  %s = add i8 %r, 1
  ret i8 %s
}

; CHECK-LABEL: call_arg8:
; A signext byte argument is sign extended to 32 bits before the call and
; aimed past the callee address at the call's trigger slot.
; CHECK: cast i8, =>0
; CHECK-NEXT: cast u32, =>4
; CHECK-NEXT: const i32, %pcrel_b2(take8-6)
; CHECK-NEXT: grow %pcrel_b1(take8-4)
; CHECK-NEXT: grow %pcrel_b0(take8-2)
; CHECK-NEXT: call 0
define void @call_arg8(i8 %a) {
  call void @take8(i8 signext %a)
  ret void
}

; CHECK-LABEL: ret16:
; Narrow return values are widened (here: zero extended from the incoming
; 32-bit operand with a mask) and aimed past the return trigger.
; CHECK: const u32, 255
; CHECK-NEXT: grow 255
; CHECK-NEXT: and =>1
; CHECK-NEXT: ret 0
define zeroext i16 @ret16(i16 %a) {
  ret i16 %a
}

; CHECK-LABEL: memset_i8:
; The memset libcall's i8 value is passed as a 32-bit operand; the two
; constant arguments are built in reverse and carried by one echo.
; CHECK: const u32, 100
; CHECK-NEXT: const u32, 7
; CHECK-NEXT: echo =>4
; CHECK-NEXT: const i32, %pcrel_b2(memset-6)
define void @memset_i8(ptr %p) {
  call void @llvm.memset.p0.i32(ptr %p, i8 7, i32 100, i1 false)
  ret void
}
