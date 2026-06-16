#include <stdlib.h>

int a, b;
int* g;

void set(int* p) {
    g = p;
}

int main(int argc, char** argv) {
    set(&a);
    int* x = g;
    set(&b);
    int* y = g;
    return *x + *y;
}
