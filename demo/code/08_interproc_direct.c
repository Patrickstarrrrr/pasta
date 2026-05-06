// 08_interproc_direct.c -- Inter-procedural direct call with branching caller
//
// `set_target` is called twice from different branches of main, with
// different actual arguments (q vs t). Each call copies the actual argument
// to the formal parameter `dst` inside set_target, then writes x through it.
//
// Vanilla Andersen: pts(dst) = {q, t}, then *dst = x makes both q and t
// MayAlias (because they "could both" receive x).
// CondAndersen: each call edge carries a guard for its callsite branch;
// q-write is under (argc>1), t-write under !(argc>1), so q and t should be
// NoAlias.
//
// NOTE: at -O0 LLVM emits both args as memory loads through alloca; the
// CallPE inter-proc Copy edges live in main's BBs (callsites), so SVF's
// stmtPathGuard already attaches the right caller-branch guard.

#include <stdlib.h>

void set_target(int** dst, int* val) {
    *dst = val;                  // store guarded by *no* condition inside callee
}

int main(int argc, char** argv) {
    int* x = (int*)malloc(sizeof(int));
    int* q = (int*)malloc(sizeof(int));
    int* t = (int*)malloc(sizeof(int));

    if (argc > 1) {
        set_target(&q, x);       // call under (argc > 1)
    } else {
        set_target(&t, x);       // call under !(argc > 1)
    }
    return 0;
}
