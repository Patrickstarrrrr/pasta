// 15_leak_fp_balanced.c -- A potential false-positive leak scenario
//
// p is allocated under cond, freed under the SAME cond. Vanilla SABER
// (path-condition-aware via SaberCondAllocator on value flow) should
// already correctly handle this. We test it to confirm CondAndersen
// doesn't BREAK existing correctness.

#include <stdlib.h>

int main(int argc, char** argv) {
    int* p = NULL;
    if (argc > 1) {
        p = (int*)malloc(sizeof(int));   // alloc under (argc>1)
    }
    // ... use p ...
    if (argc > 1) {
        free(p);                         // free under same condition
    }
    return 0;
}
