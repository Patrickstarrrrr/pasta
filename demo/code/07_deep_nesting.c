// 07_deep_nesting.c — 4-level nested if/else
//
// Exercises K-Path-Sensitive abstraction. There are 4 mutually-exclusive
// targets (a, b, c, d) guarded by nested conditions of depth 1-4.
//
// Expected alias refinements at different k values:
//   k=0  → 0 refinements (all guards collapsed to TRUE)
//   k=1  → some refinements (only depth-1 guards survive)
//   k=2  → more refinements
//   k=3  → more refinements
//   k>=4 → all 6 pairs refined to NoAlias

#include <stdlib.h>

int main(int argc, char** argv) {
    int* x = (int*)malloc(sizeof(int));
    int* a = (int*)malloc(sizeof(int));
    int* b = (int*)malloc(sizeof(int));
    int* c = (int*)malloc(sizeof(int));
    int* d = (int*)malloc(sizeof(int));
    int** p;

    if (argc > 1) {                    // cond1
        if (argc > 2) {                // cond1 && cond2
            if (argc > 3) {            // cond1 && cond2 && cond3
                p = &a;                // depth 3
            } else {
                p = &b;                // depth 3
            }
        } else {
            p = &c;                    // depth 2
        }
    } else {
        p = &d;                        // depth 1
    }
    *p = x;
    return 0;
}
