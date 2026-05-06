// 11_layered_store.c -- Two layers of conditional store
//
// p is conditionally pointed to either q or t. Then *p = x writes x into
// the conditionally selected target. Crucially, q and t are themselves
// conditional aliases of o1 and o2.
//
// q ----(condA)---> o1
// t ----(!condA)--> o2
// p = (condA ? q : t)
// *p = x  =>  o1 receives x under condA, o2 receives x under !condA
//
// Then we ask: is o1 aliased to o2? They both received x via *p, but under
// disjoint conditions. CondAndersen should report NoAlias.

#include <stdlib.h>

int main(int argc, char** argv) {
    int* x = (int*)malloc(sizeof(int));
    int** o1 = (int**)malloc(sizeof(int*));
    int** o2 = (int**)malloc(sizeof(int*));
    int** q = (int**)malloc(sizeof(int*));
    int** t = (int**)malloc(sizeof(int*));
    int*** p;

    if (argc > 1) {
        *q = (int*)o1;       // q -> o1 under condA
        p = q;
    } else {
        *t = (int*)o2;       // t -> o2 under !condA
        p = t;
    }
    **p = x;                  // writes x into either o1 or o2
    return 0;
}
