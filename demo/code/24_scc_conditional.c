// 24_scc_conditional.c — SCC with conditional copy edges
// This test creates a cycle between two pointer variables via direct
// copy edges (not load/store). The cycle forms an SCC in the
// constraint graph. Inside the cycle, one edge is conditional.
// With SCC skip (default), per-node conditional precision is preserved.
// With SCC merge (-cond-ander-merge-cond-scc), precision is lost.
#include <stdlib.h>

int* global_p;
int* global_q;

void force_copy_cycle(int flag) {
    int *a = malloc(16);
    int *b = malloc(16);

    // Create a cycle: p -> q -> p via global variables
    // These are direct pointer assignments (copy edges)
    if (flag) {
        global_p = global_q;  // conditional copy edge
    }
    global_q = global_p;      // unconditional copy edge
}

int main(int argc, char** argv) {
    global_p = malloc(16);
    global_q = malloc(16);
    force_copy_cycle(argc > 1);
    return 0;
}
