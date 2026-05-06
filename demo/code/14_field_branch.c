// 14_field_branch.c -- Conditional store into different struct fields
//
// A struct with two pointer fields. Conditional branch decides which field
// receives x. Vanilla Andersen: pts(s.f1) and pts(s.f2) both contain x's
// target, so they MayAlias (because *both fields could point to x).
// CondAndersen with per-element GEP guards: f1 receives x only under cond,
// f2 only under !cond, so they should be NoAlias.

#include <stdlib.h>

struct S {
    int* f1;
    int* f2;
};

int main(int argc, char** argv) {
    int* x = (int*)malloc(sizeof(int));
    struct S* s = (struct S*)malloc(sizeof(struct S));

    if (argc > 1) {
        s->f1 = x;       // GEP to f1, then store under cond
    } else {
        s->f2 = x;       // GEP to f2, then store under !cond
    }

    int* p1 = s->f1;     // load f1
    int* p2 = s->f2;     // load f2
    // Vanilla: pts(p1) = pts(p2) = {x_target}, MayAlias
    // CondAndersen per-element: pts(p1) under cond, pts(p2) under !cond
    return 0;
}
