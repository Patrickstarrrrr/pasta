// 16_leak_alias_fp.c -- A false-positive leak driven by alias-confusion
//
// Two pointer slots q and t. Under cond, q gets fresh malloc; t doesn't.
// Under !cond, t gets fresh malloc; q doesn't. Then we free *p where p
// points to whichever was allocated.
//
// Vanilla Andersen: pts(p) = {q, t}, so SABER may think the alloc could
// flow into either slot regardless of cond. Combined with the conditional
// free, vanilla path-cond on the *value flow* might still get this right;
// CondAndersen on the *pts side* additionally narrows the per-element
// guards on pts(p) so the slicing graph has fewer infeasible edges.

#include <stdlib.h>

int main(int argc, char** argv) {
    int* q = NULL;
    int* t = NULL;
    int** p;
    if (argc > 1) {
        q = (int*)malloc(sizeof(int));
        p = &q;
    } else {
        t = (int*)malloc(sizeof(int));
        p = &t;
    }
    free(*p);                           // frees whichever was allocated
    return 0;
}
