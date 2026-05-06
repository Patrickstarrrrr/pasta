// 10_interproc_return.c -- Inter-procedural via return value
//
// `pick` returns either q or t depending on its argument. The caller writes
// x through the returned pointer. CondAndersen sees that q is returned only
// under (cond > 0) and t only under !(cond > 0), so the pair (q, t) should
// be NoAlias.

#include <stdlib.h>

static int* g_q;
static int* g_t;

int* pick(int cond) {
    if (cond > 0) {
        return g_q;
    } else {
        return g_t;
    }
}

int main(int argc, char** argv) {
    int* x = (int*)malloc(sizeof(int));
    g_q = (int*)malloc(sizeof(int));
    g_t = (int*)malloc(sizeof(int));

    int* p = pick(argc);
    *p = 42;                    // write 42 (not pointer, but ensures p is used)
    int** pp;
    if (argc > 1) {
        pp = &g_q;
    } else {
        pp = &g_t;
    }
    *pp = x;
    return 0;
}
