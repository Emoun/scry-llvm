; RUN: llc -mtriple=scry32-unknown-none-elf -O2 < %s | FileCheck %s
; RUN: llc -mtriple=scry32-unknown-none-elf -O0 < %s | FileCheck %s --check-prefix=O0

; Jumps are issued as early as their condition allows and trigger after the
; last instruction of their block.  Backward jumps are taken on a non-zero
; condition, forward jumps on zero; a mismatch costs one eq.

declare void @f()
declare void @g()

; A forward conditional jump over the fall-through block.  The branch is
; taken on a non-zero condition towards a forward target, which needs the
; negated condition: the negating eq of the compare (icmp ne after
; canonicalisation) is folded away instead, so a single eq delivers straight
; to the near jmp; the block ends there (trigger 0).
; CHECK-LABEL: diamond:
; CHECK: eq =>0
; CHECK-NEXT: jmp {{[1-9][0-9]*}}, 0
; CHECK-NOT: jmp
; CHECK: call 0
define void @diamond(i32 %a, i32 %b) {
entry:
  %c = icmp eq i32 %a, %b
  br i1 %c, label %t, label %e
t:
  call void @f()
  br label %x
e:
  call void @g()
  br label %x
x:
  ret void
}

; A loop with eight volatile loads: the compare delivers straight to the
; near backward jump; the loop-carried values are delivered in flight.
; CHECK-LABEL: far:
; CHECK: lt =>0
; CHECK-NEXT: jmp -{{[0-9]+}}, {{[0-9]+}}
; CHECK-NOT: st
; CHECK: ret 0
define i32 @far(ptr %p, i32 %n) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %i1, %loop ]
  %s = phi i32 [ 0, %entry ], [ %s8, %loop ]
  %q1 = getelementptr i32, ptr %p, i32 1
  %v1 = load volatile i32, ptr %q1
  %s1 = add i32 %s, %v1
  %q2 = getelementptr i32, ptr %p, i32 2
  %v2 = load volatile i32, ptr %q2
  %s2 = add i32 %s1, %v2
  %q3 = getelementptr i32, ptr %p, i32 3
  %v3 = load volatile i32, ptr %q3
  %s3 = add i32 %s2, %v3
  %q4 = getelementptr i32, ptr %p, i32 4
  %v4 = load volatile i32, ptr %q4
  %s4 = add i32 %s3, %v4
  %q5 = getelementptr i32, ptr %p, i32 5
  %v5 = load volatile i32, ptr %q5
  %s5 = add i32 %s4, %v5
  %q6 = getelementptr i32, ptr %p, i32 6
  %v6 = load volatile i32, ptr %q6
  %s6 = add i32 %s5, %v6
  %q7 = getelementptr i32, ptr %p, i32 7
  %v7 = load volatile i32, ptr %q7
  %s7 = add i32 %s6, %v7
  %q8 = getelementptr i32, ptr %p, i32 8
  %v8 = load volatile i32, ptr %q8
  %s8 = add i32 %s7, %v8
  %i1 = add i32 %i, 1
  %c = icmp ult i32 %i1, %n
  br i1 %c, label %loop, label %done
done:
  ret i32 %s8
}

; Neither successor follows the block (block order is kept at -O0): an
; unconditional near jump is issued at the block start and the conditional
; one, sharing the trigger, overrides it when taken.  The conditional jump
; goes to the forward target whose polarity needs no negation.
; O0-LABEL: pair:
; O0: jmp {{[1-9][0-9]*}}, [[T:[1-9][0-9]*]]
; O0: lt =>0
; O0-NEXT: jmp {{[1-9][0-9]*}}, 0
; O0: ret 0
define void @pair(i32 %a, i32 %b) {
entry:
  %c = icmp ult i32 %a, %b
  br i1 %c, label %t, label %e
x:
  ret void
t:
  call void @f()
  br label %x
e:
  call void @g()
  br label %x
}
