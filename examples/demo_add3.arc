// Examples for arcJIT. These are placeholder .arc files that demonstrate
// the kind of input the JIT will eventually consume. Real execution of these
// files requires the full Arc compiler front-end to be linked in (planned for
// a later milestone). For now, the CLI demos (`arcjit-cli --demo tier1` and
// `--demo tier2`) are the runnable targets.

fn add3(a, b, c) {
    return a + b + c;
}

fn main() {
    return add3(1, 2, 3);
}
