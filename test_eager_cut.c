void test(int cond) {
    int x;
    int *p = &x;
    int *q;

    // cond=true 分支：q 被赋值为 p
    if (cond) {
        q = p;
    }

    // cond=false 分支：r 被赋值为 q
    // 此时 q 的 condPtsMap 中 x 的 guard = Atom(cond, true)
    // 而本分支的 edge guard = Atom(cond, false)
    // newCond = Atom(cond, true) ∧ Atom(cond, false) = False
    // eager SAT 应该 cut 掉这个 x
    int *r;
    if (!cond) {
        r = q;
    }
}

int main() {
    return 0;
}
