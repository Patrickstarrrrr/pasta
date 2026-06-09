; ModuleID = 'test_eager_cut'
target datalayout = "e-m:o-i64:64-i128:128-n32:64-S128"
target triple = "arm64-apple-macosx"

define void @test(i32 noundef %cond) {
entry:
  %x = alloca i32, align 4
  %p = alloca ptr, align 8
  %q = alloca ptr, align 8
  %r = alloca ptr, align 8

  store ptr %x, ptr %p, align 8

  %c = icmp ne i32 %cond, 0
  br i1 %c, label %true_br, label %merge1

true_br:
  %p1 = load ptr, ptr %p, align 8
  store ptr %p1, ptr %q, align 8
  br label %merge1

merge1:
  br i1 %c, label %merge2, label %false_br

false_br:
  %q1 = load ptr, ptr %q, align 8
  store ptr %q1, ptr %r, align 8
  br label %merge2

merge2:
  ret void
}

define i32 @main() {
entry:
  ret i32 0
}
