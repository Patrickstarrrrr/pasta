// 05_cascade_via_struct.c
// Three-way cascade on struct fields. Under three mutually-exclusive
// conditions, one field receives 'shared' while the others get fresh objects.
// Vanilla Andersen: all three fields may alias because shared is in all pts.
// CondAndersen: each gets shared under a disjoint condition.

#include <stdlib.h>

struct S {
    int* f1;
    int* f2;
    int* f3;
};

int main(int argc, char** argv) {
    int* shared = (int*)malloc(sizeof(int));
    struct S s;

    if (argc > 1) {
        s.f1 = shared;
        s.f2 = (int*)malloc(sizeof(int));
        s.f3 = (int*)malloc(sizeof(int));
    } else if (argc > 0) {
        s.f1 = (int*)malloc(sizeof(int));
        s.f2 = shared;
        s.f3 = (int*)malloc(sizeof(int));
    } else {
        s.f1 = (int*)malloc(sizeof(int));
        s.f2 = (int*)malloc(sizeof(int));
        s.f3 = shared;
    }

    // -ander:      all three fields contain 'shared' + fresh object
    //              alias(f1,f2)=MayAlias, alias(f1,f3)=MayAlias, alias(f2,f3)=MayAlias
    // -cond-ander: each field gets shared under a different, mutually exclusive guard
    //              all three pairs become NoAlias
    return 0;
}
