// 02_no_condition.c — Baseline (no branch)
//
// `*p = x` and `*r = x` are unconditional, so q and r really do both observe
// x. Both vanilla Andersen and CondAndersen should report MayAlias(q, r).
// This case ensures CondAndersen does not over-prune.

#include <stdlib.h>

int main() {
    int* x = (int*)malloc(sizeof(int));
    int* q = (int*)malloc(sizeof(int));
    int* r = (int*)malloc(sizeof(int));
    int** p1 = &q;
    int** p2 = &r;
    *p1 = x;
    *p2 = x;
    return 0;
}
