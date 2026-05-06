// 21_guard_xor.c — k-limit stress test
//
// p and q are assigned in *opposite* branches of nested if/else.
// Under k=0 (unlimited):
//   p→obj = c1&c2,  q→obj = c1&~c2   →  conj = UNSAT  →  NoAlias
//   p→other = c1&~c2, q→other = c1&c2 →  conj = UNSAT  →  NoAlias
// Under k=1:
//   p→obj guard depth=2 > 1, collapse to TRUE
//   p→obj = TRUE, q→obj = c1&~c2     →  conj = SAT    →  MayAlias
//
// This should show k-limit directly affecting alias precision.

#include <stdlib.h>

int main(int argc, char** argv) {
    int obj;
    int other;
    int *p;
    int *q;

    if (argc > 1) {
        if (argc > 2) {
            p = &obj;
            q = &other;
        } else {
            p = &other;
            q = &obj;
        }
    }

    // Force alias checks via store
    *p = 1;
    *q = 2;

    return 0;
}
