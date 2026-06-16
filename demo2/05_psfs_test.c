#include <stdlib.h>

int a = 1, b = 2;

int main(int argc, char** argv) {
    int** pp = (int**)malloc(sizeof(int*));
    int* r;
    if (argc > 1) {
        *pp = &a;
        r = *pp;
    } else {
        *pp = &b;
        r = *pp;
    }
    return *r;
}
