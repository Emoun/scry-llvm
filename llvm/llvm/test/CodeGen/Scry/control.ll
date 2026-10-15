; RUN: llc -mtriple=scry32-unknown-none-elf -O2 < %s | FileCheck %s

; CHECK-LABEL: loop:
; The loop-carried values arrive in flight at the landing pads of the body
; (one echo per position; nothing is loaded or stored), the compare delivers
; straight to a near backward jump, and the values for the next iteration
; (or the exit block) are aimed past the block end.
; CHECK: %body
; CHECK: landing pad 1
; CHECK: lt =>0
; CHECK-NEXT: jmp -{{[0-9]+}}, {{[0-9]+}}
; CHECK-NOT: jmp
; CHECK-NOT: ld
; CHECK-NOT: st
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
; CHECK: pick =>1
; CHECK-NEXT: ret 0
define i32 @select(i1 %c, i32 %a, i32 %b) {
  %r = select i1 %c, i32 %a, i32 %b
  ret i32 %r
}

; CHECK-LABEL: call_many:
; Five arguments: four as trigger operands (the three constants built in
; reverse and carried by one echo), one on the stack via the outgoing area
; reserved in the shared frame.
; CHECK: rsrv 16
; CHECK: const u32, 5
; CHECK-NEXT: st
; CHECK-NEXT: const u32, 4
; CHECK-NEXT: const u32, 3
; CHECK-NEXT: const u32, 2
; CHECK-NEXT: echo =>4
; CHECK: call 0
; CHECK: free 16
declare i32 @callee(i32, i32, i32, i32, i32)
define i32 @call_many(i32 %a) {
  %r = call i32 @callee(i32 %a, i32 2, i32 3, i32 4, i32 5)
  ret i32 %r
}

; CHECK-LABEL: indirect:
; The function pointer argument is delivered to the call itself.
; CHECK: echo =>0
; CHECK-NEXT: call 0
define void @indirect(ptr %f) {
  call void %f()
  ret void
}

; CHECK-LABEL: global_store:
; The value is aimed past the address chain at the store: [value, address];
; the address is PC-relative to the store.
; CHECK: echo =>3
; CHECK-NEXT: const i32, %pcrel_b2(g-6)
; CHECK-NEXT: grow %pcrel_b1(g-4)
; CHECK-NEXT: grow %pcrel_b0(g-2)
; CHECK-NEXT: st
@g = global i32 0
define void @global_store(i32 %v) {
  store i32 %v, ptr @g
  ret void
}

; CHECK-LABEL: indexed_load:
; An element of a global array is loaded with the [base, index] form: the
; base is the PC-relative array address, the index is scaled by the load.
; CHECK-NOT: sh
; CHECK: const i32, %pcrel_b2(tab-6)
; CHECK-NEXT: grow %pcrel_b1(tab-4)
; CHECK-NEXT: grow %pcrel_b0(tab-2)
; CHECK-NEXT: ld u32, =>{{[0-9]+}}
; CHECK-NEXT: ret 0
@tab = global [16 x i32] zeroinitializer
define i32 @indexed_load(i32 %i) {
  %p = getelementptr inbounds nuw [16 x i32], ptr @tab, i32 0, i32 %i
  %v = load i32, ptr %p
  ret i32 %v
}

; CHECK-LABEL: field_store:
; A store at a constant offset that is a multiple of the access size uses
; the same form with a constant index ([value, base, index]).
; CHECK: echo =>{{[0-9]+}}
; CHECK-NEXT: const u32, 2
; CHECK-NEXT: const i32, %pcrel_b2(tab-6)
; CHECK-NEXT: grow %pcrel_b1(tab-4)
; CHECK-NEXT: grow %pcrel_b0(tab-2)
; CHECK-NEXT: st
define void @field_store(i32 %v) {
  store i32 %v, ptr getelementptr inbounds nuw (i8, ptr @tab, i32 8)
  ret void
}
