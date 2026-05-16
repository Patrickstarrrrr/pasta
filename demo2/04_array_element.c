// 04_array_element.c
// An array of two pointers. Under mutually exclusive conditions,
// arr[0] or arr[1] receives 'shared'. Vanilla Andersen merges both.
// CondAndersen separates them by condition.

#include <stdlib.h>

int main(int argc, char** argv) {
    int* shared = (int*)malloc(sizeof(int));
    int* arr[2];

    if (argc > 1) {
        arr[0] = shared;
        arr[1] = (int*)malloc(sizeof(int));
    } else {
        arr[0] = (int*)malloc(sizeof(int));
        arr[1] = shared;
    }

    // -ander:      pts(arr[0])={shared, obj0}, pts(arr[1])={shared, obj1}
    //              alias(arr[0],arr[1])=MayAlias (shared)
    // -cond-ander: arr[0] gets shared under cond1, arr[1] under !cond1
    //              alias(arr[0],arr[1])=NoAlias
    return 0;
}
