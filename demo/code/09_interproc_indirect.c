// 09_interproc_indirect.c -- Inter-procedural via function pointer
//
// fp is set to one of two functions depending on a branch. Both functions
// take a pointer arg and write x into it. Without inter-proc guards, the
// indirect call dispatches to BOTH callees, making q and t MayAlias.
// With inter-proc guards, the call edge for setQ is guarded by (argc>1) and
// for setT by !(argc>1), so q and t should be NoAlias.

#include <stdlib.h>

static int* g_x;

void writeX(int** target) {
    *target = g_x;
}

int main(int argc, char** argv) {
    g_x = (int*)malloc(sizeof(int));
    int* q = (int*)malloc(sizeof(int));
    int* t = (int*)malloc(sizeof(int));

    void (*fp)(int**);
    int** target;
    if (argc > 1) {
        target = &q;
    } else {
        target = &t;
    }
    fp = writeX;
    fp(target);                 // indirect call (function ptr) with branching arg
    return 0;
}
