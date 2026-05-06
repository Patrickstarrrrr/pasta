// 13_two_path_merge.c -- Two paths converge to write the same destination
//
// Two assignments p = q under condA, p = t under !condA.
// q -> o1, t -> o2.
// Then *p = x writes x to either o1 or o2 depending on condA.
//
// Subsequent: y = *p reads back. y should point to whatever *p points to
// under each condition. Per-element guards on pts(p) should yield disjoint
// reads of o1's contents (under condA) and o2's contents (under !condA),
// so the values aliased through *p remain disjoint.

#include <stdlib.h>

int main(int argc, char** argv) {
    int* x = (int*)malloc(sizeof(int));
    int** o1 = (int**)malloc(sizeof(int*));
    int** o2 = (int**)malloc(sizeof(int*));
    int** q = o1;     // q -> *o1 (alias of o1)
    int** t = o2;     // t -> *o2 (alias of o2)
    int** p;

    if (argc > 1) {
        p = q;        // pts(p) gets pts(q) under condA
    } else {
        p = t;        // pts(p) gets pts(t) under !condA
    }
    int* y;
    y = (int*)*p;     // load: pts(y) = pts(*p) = (pts(o1)|condA) U (pts(o2)|!condA)

    // Are *o1 and *o2 aliased through y?
    // Vanilla: yes
    // CondAndersen per-element: should track that o1's content is reached
    // only under condA and o2's only under !condA -- but a single y can't
    // be NoAlias to both at once (it's the same y in both branches), so
    // this case may not produce a refinement on y itself.
    return 0;
}
