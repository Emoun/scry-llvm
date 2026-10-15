; RUN: llc -mtriple=scry32-unknown-none-elf -O2 < %s | FileCheck %s

; CHECK-LABEL: loop:
; The loop-closing branch: a near backward jump issued right after the
; compare is stored, triggering after the block's last instruction.
; CHECK: lt =>0
; CHECK-NEXT: st [[[C:[0-9]+]]]
; CHECK-NEXT: ld u8 [[[C]]]
; CHECK-NEXT: jmp -{{[0-9]+}}, {{[1-9][0-9]*}}
; CHECK-NOT: jmp
; CHECK: ret 0
define i32 @loop(i32 %n) {
entry:
  br label %body
body:
  %i = phi i32 [ 0, %entry ], [ %inc, %body ]
  %acc = phi i32 [ 0, %entry ], [ %sum, %body ]
  %sum = add i32 %acc, %i
  %inc = add i32 %i, 1
  %c = icmp ult i32 %inc, %n
  br i1 %c, label %body, label %done
done:
  ret i32 %sum
}

; CHECK-LABEL: select:
; CHECK: pick =>0
define i32 @select(i1 %c, i32 %a, i32 %b) {
  %r = select i1 %c, i32 %a, i32 %b
  ret i32 %r
}

; CHECK-LABEL: call_many:
; Five arguments: four as trigger operands, one on the stack via the
; outgoing area reserved in the shared frame.
; CHECK: rsrv 16
; CHECK: st
; CHECK: call 0
; CHECK: free 16
declare i32 @callee(i32, i32, i32, i32, i32)
define i32 @call_many(i32 %a) {
  %r = call i32 @callee(i32 %a, i32 2, i32 3, i32 4, i32 5)
  ret i32 %r
}

; CHECK-LABEL: indirect:
; CHECK: ld u32 [{{[0-9]+}}]
; CHECK-NEXT: call 0
define void @indirect(ptr %f) {
  call void %f()
  ret void
}

; CHECK-LABEL: global_store:
; CHECK: const u32, %b3(g)
; CHECK: grow %b2(g)
; CHECK: grow %b1(g)
; CHECK: grow %b0(g)
; CHECK: st
@g = global i32 0
define void @global_store(i32 %v) {
  store i32 %v, ptr @g
  ret void
}
