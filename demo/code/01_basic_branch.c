// 01_basic_branch.c — Conditional Andersen motivating example
//
// q and t are both written by `*p = x` but only under mutually-exclusive
// conditions (argc>1 vs !(argc>1)). Vanilla Andersen reports MayAlias(q, t);
// CondAndersen should report NoAlias.

#include <stdlib.h>

int main(int argc, char** argv) {
    int* x = (int*)malloc(sizeof(int));   // o1
    int* q = (int*)malloc(sizeof(int));   // o2
    int* t = (int*)malloc(sizeof(int));   // o3
    int** p;

    if (argc > 1) {
        p = &q;
    } else {
        p = &t;
    }
    *p = x;            // writes x into either *q or *t (mutually exclusive)

    return 0;
}
