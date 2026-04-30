// Test case for Conditional Andersen (Phase 1)
// Global variables to keep names in SVFIR

int o1, o2, o3;
void **x, **q, **t;
void **p1, **p2, **p;

void test() {
    x = (void**)&o1;
    q = (void**)&o2;
    t = (void**)&o3;
    
    int cond = 1;
    if (cond) {
        p1 = q;
    } else {
        p2 = t;
    }
    
    // phi via select
    p = cond ? p1 : p2;
    
    *p = (void*)x;  // store
}

int main() {
    test();
    return 0;
}
