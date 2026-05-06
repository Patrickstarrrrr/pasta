// 03_independent_conditions.c — Two independent branch variables
//
// q is written under condA; t is written under condB. condA and condB are
// independent, so they CAN both be true simultaneously. Vanilla Andersen
// reports MayAlias(q, t); CondAndersen should ALSO report MayAlias because
// SAT(condA AND condB) = true. This case ensures CondAndersen does not
// over-prune when conditions are not mutually exclusive.

#include <stdlib.h>

int main(int argc, char** argv) {
    int* x = (int*)malloc(sizeof(int));
    int* q = (int*)malloc(sizeof(int));
    int* t = (int*)malloc(sizeof(int));
    int** pq = &q;
    int** pt = &t;

    if (argc > 1) {
        *pq = x;            // condA: argc > 1
    }
    if (argv != 0) {
        *pt = x;            // condB: argv != 0  (independent of condA)
    }
    return 0;
}
