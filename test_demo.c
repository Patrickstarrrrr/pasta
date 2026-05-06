// Demo: showcase precision difference between Andersen and Conditional Andersen
// Matches the slides example exactly

void *o1, *o2, *o3;
void **x, **q, **t;
void **p1, **p2, **p;

void demo() {
    x = (void**)&o1;
    q = (void**)&o2;
    t = (void**)&o3;
    
    int cond = 1;
    
    // Branch-sensitive assignments
    if (cond) {
        p1 = q;
    } else {
        // p1 remains uninitialized on this path
    }
    
    if (!cond) {
        p2 = t;
    } else {
        // p2 remains uninitialized on this path
    }
    
    // Phi via select: p = phi(p1, p2)
    p = cond ? p1 : p2;
    
    // Store: *p = x
    *p = (void*)x;
}

int main() {
    demo();
    return 0;
}
