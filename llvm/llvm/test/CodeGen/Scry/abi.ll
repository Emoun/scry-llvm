; RUN: llc -mtriple=scry32-unknown-none-elf -O2 < %s | FileCheck %s
; RUN: llc -mtriple=scry32-unknown-none-elf -O0 < %s | FileCheck %s

; Integers narrower than 32 bits are passed and returned as 32-bit values.

declare zeroext i8 @get8()
declare void @take8(i8 signext)
declare void @llvm.memset.p0.i32(ptr, i8, i32, i1)

; CHECK-LABEL: call_ret8:
; The 32-bit result arriving after the call is stored in a 32-bit slot and
; truncated before it is used as a byte.
; CHECK: call 0
; CHECK-NEXT: st [{{[0-9]+}}]
; CHECK: cast u8, =>0
define i8 @call_ret8() {
  %r = call zeroext i8 @get8()
  %s = add i8 %r, 1
  ret i8 %s
}

; CHECK-LABEL: call_arg8:
; A signext byte argument is sign extended to 32 bits before the call.
; CHECK: cast i8, =>0
; CHECK: cast u32, =>0
; CHECK: call 0
define void @call_arg8(i8 %a) {
  call void @take8(i8 signext %a)
  ret void
}

; CHECK-LABEL: ret16:
; Narrow return values are widened (here: zero extended from the incoming
; 32-bit operand with a mask) before the return trigger.
; CHECK: const u32, 255
; CHECK: grow 255
; CHECK: and =>0
; CHECK: ret 0
define zeroext i16 @ret16(i16 %a) {
  ret i16 %a
}

; CHECK-LABEL: memset_i8:
; The memset libcall's i8 value is passed as a 32-bit operand.
; CHECK: const u32, 7
; CHECK: const u32, %b3(memset)
define void @memset_i8(ptr %p) {
  call void @llvm.memset.p0.i32(ptr %p, i8 7, i32 100, i1 false)
  ret void
}
