// Test case for Conditional Andersen (Phase 1)
// Based on the slides example

void test() {
    int o1, o2, o3;
    void **x = (void**)&o1;
    void **q = (void**)&o2;
    void **t = (void**)&o3;
    void **p1, **p2, **p;
    
    int cond = 1;
    if (cond) {
        p1 = q;
    } else {
        p2 = t;
    }
    
    // Simulate phi: p = phi(p1, p2)
    if (cond) {
        p = p1;
    } else {
        p = p2;
    }
    
    *p = (void*)x;  // store: *p = x
}

int main() {
    test();
    return 0;
}
