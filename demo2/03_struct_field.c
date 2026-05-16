// 03_struct_field.c
// A struct has two pointer fields f1 and f2. Under mutually exclusive
// conditions, one field receives 'shared' while the other receives a fresh
// object. Vanilla Andersen collapses both fields to contain 'shared'.
// CondAndersen keeps them separated by branch condition.

#include <stdlib.h>

struct S {
    int* f1;
    int* f2;
};

int main(int argc, char** argv) {
    int* shared = (int*)malloc(sizeof(int));
    struct S* s = (struct S*)malloc(sizeof(struct S));

    if (argc > 1) {
        s->f1 = shared;
        s->f2 = (int*)malloc(sizeof(int));
    } else {
        s->f1 = (int*)malloc(sizeof(int));
        s->f2 = shared;
    }

    int* p1 = s->f1;
    int* p2 = s->f2;

    // -ander:      pts(p1)={shared, obj1}, pts(p2)={shared, obj2}
    //              alias(p1,p2)=MayAlias (shared)
    // -cond-ander: p1 gets shared under cond1, p2 gets shared under !cond1
    //              alias(p1,p2)=NoAlias
    return 0;
}
