// 20_ultra_deep_nesting.c — 10-level nested if/else
//
// Creates deep guard chains: p=&ai has guard depth = i+1.
// k-limit should truncate deep guards, reducing precision.

#include <stdlib.h>

int main(int argc, char** argv) {
    int* x = (int*)malloc(sizeof(int));
    int* a1 = (int*)malloc(sizeof(int));
    int* a2 = (int*)malloc(sizeof(int));
    int* a3 = (int*)malloc(sizeof(int));
    int* a4 = (int*)malloc(sizeof(int));
    int* a5 = (int*)malloc(sizeof(int));
    int* a6 = (int*)malloc(sizeof(int));
    int* a7 = (int*)malloc(sizeof(int));
    int* a8 = (int*)malloc(sizeof(int));
    int* a9 = (int*)malloc(sizeof(int));
    int* a10 = (int*)malloc(sizeof(int));
    int** p;

    if (argc > 1) {
        if (argc > 2) {
            if (argc > 3) {
                if (argc > 4) {
                    if (argc > 5) {
                        if (argc > 6) {
                            if (argc > 7) {
                                if (argc > 8) {
                                    if (argc > 9) {
                                        if (argc > 10) {
                                            p = &a10;
                                        } else {
                                            p = &a9;
                                        }
                                    } else {
                                        p = &a8;
                                    }
                                } else {
                                    p = &a7;
                                }
                            } else {
                                p = &a6;
                            }
                        } else {
                            p = &a5;
                        }
                    } else {
                        p = &a4;
                    }
                } else {
                    p = &a3;
                }
            } else {
                p = &a2;
            }
        } else {
            p = &a1;
        }
    } else {
        p = &x;
    }

    *p = x;
    return 0;
}
