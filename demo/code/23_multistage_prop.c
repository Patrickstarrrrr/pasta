// 23_multistage_prop.c — Multi-stage propagation for deep guard testing
//
// Stage 1: p = &obj under c1  (p→obj = c1, depth=1)
// Stage 2: q = p under c2     (q→obj = c2 & c1, depth=2)
// Stage 3: r = q under c3     (r→obj = c3 & c2 & c1, depth=3)
// Stage 4: s = r under c4     (s→obj = c4 & c3 & c2 & c1, depth=4)
//
// Then store through s triggers derived copy guard:
//   derived_guard = storeG & s→obj = And(c5, And(c4, And(c3, And(c2, c1))))
//
// With true tree-height depth (max+1):
//   depth of s→obj = 4, depth of derived guard = 5
//
// k=1..4: derived guard truncated → more MayAlias
// k>=5:   derived guard preserved → fewer MayAlias

#include <stdlib.h>

void sink(int*);

int main(int argc, char** argv) {
    int obj;
    int other;
    int *p;
    int *q;
    int *r;
    int *s;
    int *t;

    if (argc > 1) { p = &obj; }
    if (argc > 2) { q = p; }
    if (argc > 3) { r = q; }
    if (argc > 4) { s = r; }
    if (argc > 5) { t = s; }

    // Force alias checks
    *t = 1;
    *s = 2;

    return 0;
}
