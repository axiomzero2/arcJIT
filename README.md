# arcJIT

A 3-tier JIT compiler for the [Arc programming language](https://github.com/VxidDev/Arc), written in C++23.

## What this is

arcJIT is a just-in-time compiler that takes Arc's existing stack bytecode (the same bytecode produced by the upstream Arc compiler) and executes it through a layered pipeline:

```
Arc Chunk ──► Tier 0  (interpreter, profiling, ICs)
                │
                ▼
            Tier 1  (baseline SSA JIT, linear scan regalloc)
                │
                ▼
            Tier 2  (Sea of Nodes optimizing JIT, GVN + escape analysis + GCM)
```

The architecture is the same one used by V8 (Ignition → Sparkplug → TurboFan) and HotSpot (Interpreter → C1 → C2). Tier 0 is always available and gives zero startup latency. Tier 1 is fast to compile and gives a stable baseline while Tier 2 is being prepared. Tier 2 is the heavy optimizing compiler that produces peak throughput.

## Status

Pre-alpha. The full 3-tier pipeline is wired end-to-end — real Arc bytecode
runs through interpreter → Tier-1 baseline SSA JIT → Tier-2 Sea of Nodes
optimizing JIT, with background compilation via enkiTS and OSR support.

| Component                          | Status            |
| ---------------------------------- | ----------------- |
| Arc bytecode headers               | implemented       |
| Tier-0 interpreter (all opcodes)   | implemented       |
| Real heap objects (Number/String/List/Function/Instance/Class) | implemented |
| Real symbol table (hash map)       | implemented       |
| Inline caches + type feedback      | implemented       |
| Safepoint polling                  | implemented       |
| Tier-1 baseline SSA JIT            | implemented       |
| Chunk → Tier-1 lowering            | implemented       |
| Linear-scan register allocation    | implemented       |
| asmjit codegen (all Tier-1 ops)    | implemented       |
| Tier-2 Sea of Nodes JIT            | implemented       |
| Tier-1 → SoN lowering              | implemented       |
| SoN → Tier-1 lowering              | implemented       |
| GVN + ConstantFolding + DCE passes | implemented       |
| Pass manager (fixpoint iteration)  | implemented       |
| Tier ladder (hot-function detect)  | implemented       |
| Background compilation (enkiTS)    | implemented       |
| OSR (On-Stack Replacement)          | implemented       |
| Thread pools (enkiTS)              | wired             |
| End-to-end CLI                     | working           |

**186 tests passing.** Verified end-to-end:
- `arcjit-cli --tier 0 --bytecode "1+2+3"` → 6 (interpreter)
- `arcjit-cli --tier 1 --bytecode "1+2+3"` → 6 (Tier-1 JIT)
- `arcjit-cli --tier 2 --bytecode "1+2+3"` → 6 (Tier-2 SoN JIT, GVN+ConstFold fold to ConstInt(6))
- `arcjit-cli --tier 0 --bytecode "(10+20)*3"` → 90 (all three tiers agree)

### Optimization passes (Tier-2 pipeline)

| Pass | Transformations | Golden tests |
| --- | --- | --- |
| TypeNarrowing | Propagate TypeIds (Int, Float, Bool, Null, String) | 12 |
| GVN | Deduplicate identical computations | 12 |
| ConstantFolding | Fold ConstInt + ConstInt → ConstInt | 12 |
| AlgebraicSimplification | x+0→x, x*1→x, x*0→0, x-x→0, x/1→x, !!x→x | 12 |
| ComparisonFolding | x==x→true, x!=x→false, !(x<y)→x>=y | 12 |
| BranchFolding | if(true)→drop false branch, if(false)→drop true branch | 12 |
| StrengthReduction | x*2^k → x<<k (detects opportunities, future: emit shifts) | — |
| DCE | Remove dead pure nodes | 12 |

The pipeline runs all passes to a fixpoint (max 8 iterations). The graph
verifier runs after every pass in debug builds (Rule 42).

### Testing & debugging infrastructure (Rules 36-43)

| Component | Status |
| --- | --- |
| Textual IR dumper (`dump_graph_text`) | implemented |
| Graph verifier (runs after every pass in debug builds) | implemented |
| Golden test runner (`.in.ir` / `.out.ir` files, `--update-golden`) | implemented |
| 84 golden tests (12 each for 7 passes) | implemented |
| Replay serialization (bytecode + options → binary file) | implemented |
| Differential testing (interpreter ↔ Tier-1 ↔ Tier-2) | implemented |
| Pass instrumentation (PassEvent, timeline, env-var breakpoints) | implemented |
| Deopt validator (reconstruct state, compare against expected) | implemented |
| Rule 36 regression tests (5 tests for the Tier-2 branch bug) | implemented |

## Dependencies

- **C++23** compiler (gcc ≥ 14, clang ≥ 18, MSVC ≥ 19.40)
- **CMake** ≥ 3.28
- **asmjit** — x86-64 code generation (vendored via FetchContent)
- **enkiTS** — work-stealing task scheduler (vendored via FetchContent)

## Building

```bash
cmake --preset default
cmake --build --preset default
ctest --test-dir build
```

## Running

```bash
# Run a bytecode spec at a specific tier
./build/arcjit-cli --tier 0 --bytecode "1+2+3"      # interpreter
./build/arcjit-cli --tier 1 --bytecode "1+2+3"      # Tier-1 baseline SSA JIT
./build/arcjit-cli --tier 2 --bytecode "1+2+3"      # Tier-2 Sea of Nodes JIT

# Run a 10000-iteration benchmark at a tier
./build/arcjit-cli --bench 0
./build/arcjit-cli --bench 1
./build/arcjit-cli --bench 2

# Run synthetic demos
./build/arcjit-cli --demo tier1                      # 1+2+3 via asmjit-emitted x86-64
./build/arcjit-cli --demo tier2                      # SoN pipeline + Graphviz dump
```

## Design rules

The non-bypassable rules of the project are in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md). In summary:

1. Never optimize dynamic behavior without profile data.
2. Every speculative optimization must have a bailout path.
3. Every guard must have a deopt state.
4. Deoptimization must produce the same observable behavior as the interpreter.
5. Profiling must be conservative.
6. No `malloc` / `free` in the JIT hot path — use arena bump allocators.
7. No exceptions or RTTI in the JIT proper.
8. Every pass must be idempotent.
9. Mutator threads must never block on JIT or GC locks.
10. Compiler threads must never block on mutator state.

## License

See [LICENSE](LICENSE).
