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

Pre-alpha. This is the initial scaffold plus the core design — see [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the full design rules.

| Component                          | Status            |
| ---------------------------------- | ----------------- |
| Arc bytecode headers               | implemented       |
| Tier-0 interpreter (all opcodes)   | implemented       |
| Inline caches + type feedback      | implemented       |
| Safepoint polling                  | implemented       |
| Tier-1 baseline SSA JIT            | scaffold + demo   |
| Tier-2 Sea of Nodes JIT            | scaffold + demo   |
| Pass manager                       | implemented       |
| Thread pools (enkiTS)              | wired             |
| asmjit codegen                     | wired             |
| End-to-end demo                    | working           |

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
./build/arcjit-cli --tier 0 examples/fib.arc        # interpreter only
./build/arcjit-cli --tier 1 examples/fib.arc        # + baseline JIT
./build/arcjit-cli --tier 2 examples/fib.arc        # + Sea of Nodes JIT
./build/arcjit-cli --dump-ir examples/fib.arc      # print IR graph
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
