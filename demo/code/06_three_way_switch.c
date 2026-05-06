// 06_three_way_switch.c — Three-way branch via switch
//
// Three mutually-exclusive cases, each writing x to a different target.
// CondAndersen should report NoAlias for any pair of (a, b, c) because the
// switch successors are pairwise disjoint.

#include <stdlib.h>

int main(int argc, char** argv) {
    int* x = (int*)malloc(sizeof(int));
    int* a = (int*)malloc(sizeof(int));
    int* b = (int*)malloc(sizeof(int));
    int* c = (int*)malloc(sizeof(int));
    int** p;

    switch (argc) {
        case 1:  p = &a; break;
        case 2:  p = &b; break;
        default: p = &c; break;
    }
    *p = x;
    return 0;
}
