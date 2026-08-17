// Loop example — exercises the interpreter's FOR_PREP / FOR_ITER opcodes
// and the JIT's loop unrolling / bounds-check elimination passes.

fn sum_to(n) {
    total = 0;
    for i in range(0, n) {
        total = total + i;
    }
    return total;
}

fn main() {
    return sum_to(100);
}
