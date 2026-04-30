// Test SCC handling in Conditional Andersen

void *o1, *o2;
void **a, **b, **p;

void test_scc() {
    a = (void**)&o1;
    b = (void**)&o2;
    
    // Create a cycle: a -> b -> a
    a = b;
    b = a;
    
    int cond = 1;
    if (cond) {
        p = a;
    } else {
        p = b;
    }
    
    *p = (void*)&o1;
}

int main() {
    test_scc();
    return 0;
}
