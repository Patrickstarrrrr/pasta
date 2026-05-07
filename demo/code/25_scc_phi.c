// 25_scc_phi.c — Create a cycle via phi nodes in a loop
// Using local pointers in a non-deterministic loop creates phi nodes.
// The phi nodes form copy edges that create an SCC.
#include <stdlib.h>

extern void use_ptr(int *p);

void loop_phi(int flag, int n) {
    int *a = malloc(16);
    int *b = malloc(16);
    int *p = a;
    int *q = b;

    for (int i = 0; i < n; i++) {
        int *t = p;
        p = q;
        q = t;
    }

    use_ptr(p);
    use_ptr(q);
}

int main(int argc, char** argv) {
    loop_phi(argc > 1, argc);
    return 0;
}
