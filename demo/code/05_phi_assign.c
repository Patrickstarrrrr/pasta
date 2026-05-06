// 05_phi_assign.c — Direct conditional pointer assignment
//
// Variant of 01 without an outer load/store of p.  At -O1 this typically
// becomes a phi node merging &q and &t into a single SSA value, exercising
// the Phi handling in CondAndersen::buildInitialEdgeGuards. Expected:
// MayAlias(q, t) under vanilla Andersen, NoAlias under CondAndersen.

#include <stdlib.h>

int main(int argc, char** argv) {
    int* x = (int*)malloc(sizeof(int));
    int* q = (int*)malloc(sizeof(int));
    int* t = (int*)malloc(sizeof(int));
    int** p = (argc > 1) ? &q : &t;
    *p = x;
    return 0;
}
