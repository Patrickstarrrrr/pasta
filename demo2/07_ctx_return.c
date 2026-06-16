int a, b;

int* id(int* p) {
    return p;
}

int main(int argc, char** argv) {
    int* x = id(&a);
    int* y = id(&b);
    return *x + *y;
}
