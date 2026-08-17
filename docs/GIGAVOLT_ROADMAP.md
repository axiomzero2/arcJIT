# Gigavolt Roadmap — The Final Form

> This document is the complete roadmap for arcJIT's "absurd but serious" tier.
> It defines the advanced optimizations, runtime machinery, Arc-specific
> optimizations, and the implementation waves that take arcJIT from "very good
> JIT" to "absurdly serious JIT."

## Named machinery

Each piece of runtime machinery has an electrical-themed name:

| Name | Component | Purpose |
|------|-----------|---------|
| **Spark** | Tier 0 | Register interpreter |
| **Jolt** | Tier 1 | Baseline SSA JIT |
| **Surge** | Tier 2 | Optimizing Sea of Nodes JIT |
| **Gigavolt** | Surge pipeline | The named optimization pipeline (14 passes) |
| **Watchdog** | Assumption registry | Track speculative assumptions + invalidation |
| **Trip** | Code invalidation engine | Safely uninstall/patch compiled code |
| **Meter** | Profile confidence engine | Track sample count, stability, deopt history |
| **Governor** | Speculation policy engine | Decide what speculation to use |
| **Regulator** | Cost model engine | Track compile time, code size, register pressure |
| **Fuse** | Compile budget engine | Enforce limits on compile time/memory/code size |
| **GroundFault** | Deopt analytics | Record and analyze every deopt event |
| **Contacts** | IC state machine | Uninitialized → Monomorphic → Polymorphic → Megamorphic |
| **Wiring** | Shape registry | Track shape IDs, field offsets, transitions |
| **Sequence** | Collection versioning | Version counters for lists/iterators |
| **LiveWire** | OSR machinery | Entry points, state mapping, loop backedge triggers |
| **Breaker** | Stack/safepoint maps | GC roots, deopt sites, register maps |
| **Relay** | Runtime stub library | Reusable stubs for IC miss, deopt, allocation, etc. |
| **Probe** | Target feature detection | Runtime CPU detection (SSE, AVX, BMI, etc.) |
| **Capacitor** | Code cache manager | Manage code memory, eviction, aging |
| **Oscilloscope** | Replay | Record and replay compile jobs |
| **Stepdown** | Reduction | Auto-reduce failing test cases |
| **Lightning Rod** | Chaos testing | Random deopts, OSR transitions, code invalidation |
| **Storm** | Fuzzing infrastructure | Coverage-guided fuzzing of bytecode/profiles/graphs |
| **GroundTruth** | Differential oracle | Interpreter == baseline == optimized |
| **Dyno** | Performance CI | Benchmark tracking, regression detection |

## Advanced optimizations (30 items)

### Speculation & specialization
1. Context-sensitive compilation / function cloning
2. Argument, receiver, and return-type specialization
3. Flow-sensitive type narrowing
4. Value speculation
5. Guard strengthening / guard weakening

### Loop optimization
6. Scalar evolution (SCEV)
7. Loop versioning
8. Loop cloning / loop splitting
9. Loop rotation and loop inversion

### Code layout
10. Hot/cold splitting
11. Function splitting
12. Tail duplication and block cloning
13. Branchless code / select formation (cmov)

### Representation
14. Integer width narrowing (Int64 → Int32)
15. Division and modulo strength reduction (magic constants)

### Intrinsics
16. Math intrinsics (abs, min, max, sqrt, floor, etc.)
17. Memory intrinsics (memcpy, memset, REP MOVSB)
18. List / array builtins (len, get, set, append)
19. Packed / homogeneous collection specialization
20. Iterator allocation elimination

### Error handling
21. Lazy error object allocation
22. Exception profile optimization

### FFI
23. FFI signature specialization
24. FFI purity / read-only / noescape annotations

### Memory optimization
25. Dead store elimination
26. Store sinking / store hoisting
27. Memory partial redundancy elimination (PRE)
28. Load introduction (speculative prefetch)
29. Alias versioning

### Refcount
30. Ownership / borrow analysis for refcounts

## Arc-specific optimizations (10 items)

1. `FOR item IN list` fast path (lower to indexed loop)
2. List element type specialization (packed Int/Float arrays)
3. Dynamic field representation specialization (unboxed fields)
4. Arena allocation site specialization (inline bump allocation)
5. Arena scope inference (bulk-free temporary arenas)
6. FFI symbol specialization (direct call to known address)
7. Position-aware error optimization (defer error object materialization)
8. `TRY...CATCH` cold path outlining
9. FFI transition optimization
10. Standard library intrinsics

## New rules (44-50)

### Rule 44 — No assumption without invalidation
Every speculative assumption must have a registry entry, an invalidation path,
dependent code tracking, and a fallback tier.

### Rule 45 — No specialization without fallback
Every specialized clone must have a generic fallback, a deopt path, a profile
downgrade path, and a budget limit.

### Rule 46 — No profile data without confidence
Profile data must include sample count, stability, age, decay, variance, and
deopt correlation. Low-confidence data must not trigger aggressive speculation.

### Rule 47 — No aggressive pass without cost model
Inlining, cloning, unrolling, vectorization, block duplication, loop versioning,
and PEA materialization must all use a cost model.

### Rule 48 — No FFI optimization without ABI proof
FFI optimizations must prove calling convention correctness, stack alignment,
register clobbering, errno handling, thread state transition, memory ownership,
and exception safety.

### Rule 49 — No vectorization without dependence proof
Vectorization must prove no aliasing (or versioned check), bounds safety,
alignment, correct remainder handling, correct scalar fallback, and correct
exception behavior.

### Rule 50 — No persistent state without versioning
Profile caches, code caches, serialized snapshots, AOT artifacts, and compiled
metadata must all be versioned.

## Implementation waves

### Wave 1: Core advanced machinery
1. Watchdog (assumption registry)
2. Trip (code invalidation)
3. Meter (profile confidence)
4. GroundFault (deopt analytics)
5. Governor (speculation policy)

### Wave 2: Dynamic language speed
6. Flow-sensitive type narrowing
7. Context-sensitive function cloning
8. Shape field representation specialization
9. List/iterator fast paths
10. Sequence (collection versioning)

### Wave 3: Loop and memory power
11. SCEV (scalar evolution)
12. Loop versioning
13. Loop cloning/splitting
14. Memory PRE
15. Dead store elimination

### Wave 4: Code layout and backend polish
16. Hot/cold splitting
17. Function splitting
18. Tail duplication
19. Branchless select formation
20. Probe (target feature detection)

### Wave 5: FFI and error specialization
21. FFI signature specialization
22. FFI purity annotations
23. Lazy error allocation
24. Exception profile optimization
25. TRY...CATCH cold path outlining

## Deferred

- Trace JIT (method JIT is stable first)
- E-Graphs / equality saturation
- Polyhedral loop optimization
- ML-guided heuristics
- Persistent code cache
- Out-of-process compiler
- Full debugger support

## The real Gigavolt

The speed comes from this combination:

```
Great IR
+ aggressive speculation
+ strong profile feedback
+ safe deopt
+ robust invalidation
+ careful cost models
+ relentless testing
```

That is the real Gigavolt.
