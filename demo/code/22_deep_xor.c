// 22_deep_xor.c — 5-level nested guard xor
//
// p and q are ONLY assigned inside the deepest nested if/else.
// No unconditional assignments, no OR-merge absorption.
// Guard depths: 5 (And chain of 5 conditions).
// k=1..4: guards collapsed → MayAlias
// k=0 or k>=5: guards preserved → NoAlias

#include <stdlib.h>

int main(int argc, char** argv) {
    int obj;
    int other;
    int *p;
    int *q;

    if (argc > 1) {
        if (argc > 2) {
            if (argc > 3) {
                if (argc > 4) {
                    if (argc > 5) {
                        p = &obj;
                        q = &other;
                    } else {
                        p = &other;
                        q = &obj;
                    }
                }
            }
        }
    }

    *p = 1;
    *q = 2;

    return 0;
}
