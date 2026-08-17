// Call example — exercises the interpreter's CALL opcode and the JIT's
// call inlining pass.

fn fib(n) {
    if n < 2 {
        return n;
    }
    return fib(n - 1) + fib(n - 2);
}

fn main() {
    return fib(20);
}
