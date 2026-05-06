// 12_perelem_critical.c -- A scenario specifically requiring per-element guards
//
// Two stores write the *same* destination pointer p, with different objects
// and disjoint conditions:
//   if (cond)  *p = x;     // pts(*p) gets x's target  under cond
//   else       *p = y;     // pts(*p) gets y's target  under !cond
//
// Then we ask: are x and y aliased through *p?
//
// With per-edge guards: derived edges (x -> *p) and (y -> *p) get disjoint
// guards on the *edges*, but pts(*p) is the same set, so x's target and
// y's target both live in pts(*p) -- and since they were originally x and y
// and not the targets, they end up aliased through *p's contents.
//
// With per-element guards on pts: the elem (*p, x_target) is guarded by
// cond, (*p, y_target) by !cond -- queries can prove disjoint.

#include <stdlib.h>

int main(int argc, char** argv) {
    int* x_target = (int*)malloc(sizeof(int));
    int* y_target = (int*)malloc(sizeof(int));
    int** x = &x_target;
    int** y = &y_target;
    int** p;

    int* dummy = (int*)malloc(sizeof(int));
    int** pp = &dummy;
    p = pp;                  // p -> dummy unconditionally

    if (argc > 1) {
        *p = *x;             // *pp gets x_target under cond
    } else {
        *p = *y;             // *pp gets y_target under !cond
    }

    // Now query: through *pp, is x_target aliased to y_target?
    // Vanilla: yes (both reach *pp)
    // CondAndersen: should still see them as MayAlias because
    //   pp -> dummy -> {x_target | cond, y_target | !cond}
    // and a single alias query goes through. The interesting part is that
    // the elements in pts(dummy) carry disjoint guards.
    return 0;
}
