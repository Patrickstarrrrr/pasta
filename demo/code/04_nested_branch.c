// 04_nested_branch.c — Nested if/else
//
// Three writes guarded by nested conditions:
//   pts(a) gets x under (cond1 && cond2)
//   pts(b) gets x under (cond1 && !cond2)
//   pts(c) gets x under (!cond1)
// Each pair is mutually exclusive. CondAndersen should report NoAlias for all
// three pairs (a,b), (a,c), (b,c). This exercises 2-deep guard conjunctions.

#include <stdlib.h>

int main(int argc, char** argv) {
    int* x = (int*)malloc(sizeof(int));
    int* a = (int*)malloc(sizeof(int));
    int* b = (int*)malloc(sizeof(int));
    int* c = (int*)malloc(sizeof(int));
    int** p;

    if (argc > 1) {
        if (argc > 2) {
            p = &a;
        } else {
            p = &b;
        }
    } else {
        p = &c;
    }
    *p = x;
    return 0;
}
