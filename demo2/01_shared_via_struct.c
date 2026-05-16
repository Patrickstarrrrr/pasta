// 01_shared_via_struct.c
// Two struct fields f1/f2. Under mutually exclusive conditions,
// one field receives 'shared' while the other gets a fresh object.
// Vanilla Andersen: both fields contain 'shared', so alias(f1,f2)=MayAlias.
// CondAndersen: f1 gets shared under cond1, f2 gets shared under !cond1.

#include <stdlib.h>

struct S {
    int* f1;
    int* f2;
};

int main(int argc, char** argv) {
    int* shared = (int*)malloc(sizeof(int));
    struct S s;

    if (argc > 1) {
        s.f1 = shared;
        s.f2 = (int*)malloc(sizeof(int));
    } else {
        s.f1 = (int*)malloc(sizeof(int));
        s.f2 = shared;
    }

    // -ander:      pts(s.f1)={shared, obj1}, pts(s.f2)={shared, obj2}
    //              alias(s.f1, s.f2)=MayAlias (shared)
    // -cond-ander: f1 gets shared under cond1, f2 under !cond1
    //              alias(s.f1, s.f2)=NoAlias
    return 0;
}
