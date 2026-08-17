# arcJIT — Design Rules and Architecture

> The non-bypassable rules of the project. If you break these, the JIT will be slow, incorrect, or unmaintainable.
> All implementation must follow these rules before any other consideration. Style, features, and shortcuts come second.

This document is the source of truth for the arcJIT project. It captures:

1. The 3-tier execution pipeline and how each tier fits Arc's existing bytecode.
2. The non-bypassable performance and correctness laws.
3. The Sea-of-Nodes IR design.
4. The full pass list per tier.
5. The threading / memory / safepoint model.
6. C++23 usage rules.

---

## 0. Arc compatibility baseline

arcJIT does not reinvent Arc. It consumes Arc's **real** bytecode as defined by the upstream Arc project (`include/compiler.h`, `include/vm.h`, `include/value.h`, `include/object.h`, `docs/bytecode.md`).

### 0.1 Value model (must match Arc exactly)

```c
typedef enum {
  VAL_UNDEF,   // raises NameError on read
  VAL_NULL,
  VAL_INT,     // unboxed int64_t
  VAL_FLOAT,   // unboxed double
  VAL_OBJ,     // heap Object*
} ValueType;

typedef struct {
  ValueType type;
  union {
    int64_t  i;
    double   f;
    Object*  obj;
  } as;
} Value;
```

- `Int` and `Float` are **unboxed**. The interpreter fast path never allocates for these.
- Only `VAL_OBJ` heap-allocates. Object kinds: `OBJ_NUMBER_INT`, `OBJ_NUMBER_FLOAT`, `OBJ_STRING`, `OBJ_LIST`, `OBJ_FUNCTION`, `OBJ_NATIVE_FUNCTION`, `OBJ_FUNCTION_CALL`, `OBJ_MODULE`, `OBJ_ERROR`, `OBJ_RETURN`, `OBJ_FILE`, `OBJ_BREAK`, `OBJ_CONTINUE`, `OBJ_CLASS`, `OBJ_INSTANCE`, `OBJ_NULL`.

### 0.2 Bytecode (Tier-0 input)

The 35 opcodes from `compiler.h`:

| Group              | Opcodes                                                                                |
| ------------------ | ------------------------------------------------------------------------------------- |
| Data movement      | `LOAD_CONST`, `LOAD_VAR`, `LOAD_LOCAL`, `STORE_VAR`, `STORE_LOCAL`, `POP`             |
| Arithmetic         | `ADD`, `SUB`, `MUL`, `DIV`, `POW`, `NEG`                                              |
| Comparison/logic   | `EQ`, `NE`, `LT`, `GT`, `LTE`, `GTE`, `AND`, `OR`, `NOT`                             |
| Control flow       | `JUMP`, `JUMP_IF_FALSE`, `FOR_PREP`, `FOR_ITER`, `BREAK`, `CONTINUE`, `RETURN`, `HALT` |
| Functions          | `CALL`                                                                                |
| Collections        | `BUILD_LIST`, `INDEX_GET`, `INDEX_SET`, `STORE_INDEX`                                |
| OOP                | `PROPERTY_ACCESS`, `PROPERTY_SET`                                                     |
| Exceptions         | `TRY_PUSH`, `TRY_POP`                                                                 |
| Modules           | `IMPORT`                                                                              |

Operand encoding (big-endian, matches Arc's `_read_short` / `_read_const_idx`):

- Constant index: 3 bytes (24-bit).
- Jump offset: 2 bytes signed.
- Slot / count / arg count: 1 byte.

### 0.3 Chunk layout

```c
typedef struct Chunk {
  uint8_t*   code;          // bytecode stream
  size_t     count;
  size_t     capacity;
  Object**   constants;     // constant pool (functions, strings, numbers)
  size_t     constCount;
  PosEntry*  positions;     // source map for error reporting
  char*      filename;
  char*      sourcetext;
  int        maxLocals;
} Chunk;
```

arcJIT reads `Chunk` directly. It does **not** re-parse Arc source.

### 0.4 Object graph (heap state the JIT must respect)

- `String` — owned `char*` buffer, refcounted.
- `List` — `Object**` array, refcounted, growable.
- `Function` — captures `Chunk*`, `params`, `maxLocals`, function name.
- `NativeFunction` — `Object* (*NativeFunc)(Object**, size_t)`.
- `Class` — has its own `Chunk*` (class body bytecode) + `maxLocals`.
- `Instance` — `Class*` + `SymbolTable* fields`.
- `Module` — wraps a parsed AST + lexer + parser state.

The JIT must preserve the **copy-on-load semantics** Arc uses: `LOAD_CONST` / `LOAD_VAR` / `LOAD_LOCAL` perform a `copyValue()` if the value is a non-static heap object. Any tier that elides this copy must prove the value is immutable (static, function, or scalar).

---

## 1. The 3-Tier execution pipeline

The industry-standard layered approach (V8: Ignition → Sparkplug → Maglev → TurboFan; HotSpot: Interpreter → C1 → C2). Skipping directly from interpreter to Sea-of-Nodes creates a "compilation cliff" — the SoN compiler is heavy; if a function gets hot, the app will stutter while the SoN graph is built, optimized, and register-allocated.

```
Arc Chunk ──► Tier 0 (Interpreter)
                │  + inline caches (ICs)
                │  + type feedback vectors (TFVs)
                │  + invocation / branch counters
                │  + safepoint polling
                ▼
            Tier 1 (Baseline SSA JIT)
                │  fast Linear-Scan regalloc
                │  monomorphic IC stubs
                │  profiling traps
                ▼
            Tier 2 (Sea of Nodes JIT)
                   GVN, escape analysis,
                   global code motion, OSR
```

### 1.1 Tier 0 — Register-style interpreter over Arc's stack bytecode

- **Input**: Arc `Chunk` (stack bytecode).
- **Goal**: zero compilation latency, fast startup, profile collection.
- **Mechanics**:
  - Executes the bytecode while updating **Inline Caches** and **Type Feedback Vectors**.
  - Contains the **Safepoint Polling** logic — every loop back-edge and function return checks an atomic flag for GC/JIT yield.
  - Maintains invocation and branch-taken counters.
  - Fast paths must match Arc's `vmRun()` fast paths exactly: `INT+INT` arithmetic inlined, `JUMP_IF_FALSE` on `VAL_INT` inlined.

### 1.2 Tier 1 — Baseline SSA JIT

- **Input**: hot `Chunk` + Tier-0 profiles.
- **IR**: standard CFG in SSA form.
- **Goal**: eliminate interpreter dispatch overhead in milliseconds. Provide a stable baseline execution speed while Tier 2 compiles.
- **Mechanics**:
  - **No heavy optimizations** — no loop unrolling, no vectorization, no escape analysis.
  - **Fast register allocation** — Linear Scan.
  - **Monomorphic IC stubs** — for `PROPERTY_ACCESS`, `INDEX_GET`, `CALL`: emit a stub that checks the observed shape and fast-paths the common case; falls back to a C++ runtime call to update the IC on miss.
  - **Profiling traps** — inject lightweight counters to gather exact branch-taken frequencies for Tier 2.

### 1.3 Tier 2 — Sea of Nodes optimizing JIT

- **Input**: Tier-1 SSA graph + exact profiles gathered during Tier-1 execution.
- **IR**: Sea of Nodes — combined data-dependency graph and control-dependence graph.
- **Goal**: maximum throughput on hot paths.
- **Mechanics**:
  - **GVN** — deduplicate identical computations across the entire function, regardless of basic-block boundaries.
  - **Escape analysis & scalar replacement** — break apart Arc objects that don't escape the function; allocate their fields directly in CPU registers.
  - **Global code motion** — schedule instructions early (hide latency) or late (reduce register pressure) across basic-block boundaries.
  - **On-Stack Replacement (OSR)** — when Tier 2 finishes, map the current CPU registers to the Tier-2 machine code and overwrite the instruction pointer while the function is still running.

---

## 2. Non-bypassable performance & correctness laws

These are the law. Break them and the JIT will be slow, incorrect, or unmaintainable.

### A. Runtime / Speculation laws

#### A.1 Never optimize dynamic behavior without profile data

Arc is dynamic: field access, method dispatch, types, shapes, FFI calls, exceptions. You cannot statically know enough.

- Tier 1 must collect profiles.
- Tier 2 must consume profiles.
- No speculative optimization is allowed without either:
  - **proof**, or
  - **profile + guard + deopt path**.

If you optimize blindly, you will generate slow generic code or incorrect code.

#### A.2 Every speculative optimization must have a bailout path

If you assume:
- this value is `Int`
- this object has Shape A
- this array index is in bounds
- this branch is never taken
- this FFI pointer is valid
- this type is monomorphic

…then you must have:

```text
guard -> success path
      -> deopt/failure path
```

No guard, no speculation.

#### A.3 Every guard must have a deopt state

A guard without a reconstructible runtime state is useless.

For every guard that can fail, the JIT must know:
- interpreter/baseline register state
- stack state
- locals state
- which bytecode offset to resume at
- pending exception state (try-stack position)

This is captured in a **FrameState** node attached to every potentially-failing node.

#### A.4 Deoptimization must produce the same observable behavior as the interpreter

This is non-negotiable. If the JIT optimizes `x + y` to a hardware `add` because both are `Int`, then a deopt on overflow must restore the exact value the interpreter would have computed, including the slow-path float promotion Arc uses in `doArith`.

Test this by running the same program in the interpreter and the JIT, and asserting byte-for-byte identical output.

#### A.5 Profiling must be conservative

Profile data is a hint, not a proof. Profiles lie:
- New types can appear later (megamorphic).
- Shapes can change (`Shape` transition).
- A "cold" branch can become hot under different inputs.

The JIT must always be prepared to **deopt and recompile** with new data.

### B. Compilation pipeline laws

#### B.1 No `malloc` / `free` in the JIT hot path

Use arena / bump allocators. Every `Node`, `BasicBlock`, `SSAValue`, side-table entry is allocated from a thread-local arena that is bulk-freed at the end of compilation. `malloc` destroys throughput.

#### B.2 No exceptions in the JIT hot path

Compile with `-fno-exceptions` for the JIT proper. Use `std::expected` / `Result<T>` for fallible operations. Errors in the compiler are recoverable: abort the compilation, fall back to a lower tier, keep running.

#### B.3 No RTTI in the JIT hot path

Compile with `-fno-rtti` for the JIT proper. Use `enum class NodeKind` and switch statements. RTTI costs cycles and disables devirtualization.

#### B.4 No `std::shared_ptr` / `std::function` in hot IR mutation code

They allocate. Use raw pointers + stable IDs inside passes.

#### B.5 Every pass must be idempotent

Running the same pass twice must produce the same IR. Otherwise, fixpoint iteration will loop forever.

#### B.6 Every pass must be monotonic decreasing in IR size

A pass either reduces node count, or moves the IR closer to a normal form. If a pass can grow the IR (e.g., loop unrolling), it must run inside a guarded fixpoint with a budget.

### C. Memory / threading laws

#### C.1 Mutator threads must never block on JIT compilation

If a mutator needs compiled code that isn't ready yet, it falls back to the interpreter, **does not** wait.

#### C.2 Mutator threads must never block on GC locks

Thread-local bump arenas for allocation. Global GC synchronization happens **only at safepoints**.

#### C.3 Compiler threads must never block on mutator state

The compiler works on a frozen snapshot of the IR. Mutator updates after the snapshot are picked up by the next compilation.

#### C.4 Memory reclamation must be epoch-based, not lock-based

When the optimizer replaces a Node with a new one, the old node is tagged with an epoch. Once all compiler threads advance past that epoch, the memory is bulk-freed. This avoids both locks and use-after-free.

---

## 3. Sea of Nodes IR design

The graph is the heart of the JIT. It must be designed for:
- fast traversal
- fast mutation
- cheap node allocation
- explicit memory/effect dependencies
- deoptimization
- speculative optimization
- shape/field specialization
- FFI boundaries
- exception flow
- concurrent compilation at the job level
- easy lowering to asmjit

It should **not** be designed primarily for human readability. It is designed for compiler passes.

### 3.1 Core principles

#### Principle 1 — Nodes are compact value objects

Do not make every node a separate C++ class with virtual functions.

```cpp
enum class NodeKind : uint16_t;
enum NodeFlags : uint32_t;
struct NodeId { uint32_t value; };

struct Node {
    NodeKind   kind;
    NodeFlags  flags;
    TypeId     type;
    uint32_t   payload;        // kind-specific small data
    uint32_t   first_input;    // index into edge pool
    uint32_t   first_use;      // index into use pool
    uint16_t   input_count;
    uint16_t   use_count;
};
```

Heavy data lives in side tables or payload pools keyed by `NodeId`.

#### Principle 2 — Use stable `NodeId`s, not raw pointers, for long-lived data

Raw `Node*` is fine inside a single local pass. But for:
- analysis maps
- worklists
- frame states
- deopt metadata
- debug dumps

…use `NodeId`. Pointers are invalidated when the arena grows. IDs are stable until the arena is freed.

#### Principle 3 — Inputs are in a separate edge pool, not inline

Each Node references a contiguous slice of `NodeId`s in a global edge pool. Adding/removing inputs is "rewrite the slice, update `first_input`". This avoids per-node heap allocation and keeps `sizeof(Node)` small (typically 32 bytes).

#### Principle 4 — Data and control edges are first-class

A node can have both:
- **data inputs** — values it consumes
- **control input** — the basic-block / control-flow node it lives under
- **effect input** — the previous effectful node (memory ordering)
- **frame-state input** — for deopt

Mixing these into a single edge list makes passes harder. Keep them as distinct slices.

#### Principle 5 — Memory is explicit via effect edges

Every effectful operation (`STORE`, `CALL`, `ALLOC`, `LOAD` of a mutable object) is linked in an **effect chain**. Pure operations (`ADD`, `CMP`, `CONST`) have no effect edges.

This is what enables code motion: a pure node can be moved anywhere its data inputs dominate.

### 3.2 Node kinds

```cpp
enum class NodeKind : uint16_t {
    // Control
    Region,            // N inputs (one per predecessor)
    If,                // cond + region
    IfTrue, IfFalse,
    Switch,            // value + switch table
    Proj,              // selector for Switch
    Loop,              // back-edge region
    LoopExit,
    Return,
    Unreachable,
    Jump,
    Start,             // function entry
    Stop,              // function exit
    Branch,

    // Frame state (for deopt)
    FrameState,
    Deopt,

    // Constants
    ConstInt,
    ConstFloat,
    ConstNull,
    ConstUndef,
    ConstString,       // global string pool index
    ConstFunction,
    ConstClass,

    // Arithmetic (pure)
    Add, Sub, Mul, Div, Pow, Neg,
    Eq, Ne, Lt, Gt, Lte, Gte,
    And, Or, Not,

    // Memory
    LoadVar,           // global/symbol table load
    StoreVar,
    LoadLocal,
    StoreLocal,
    LoadField,         // instance.field
    StoreField,
    LoadIndex,         // list[i] / string[i]
    StoreIndex,

    // Allocation / shape
    Allocate,          // new Instance/List/String
    ShapeOf,           // get object's shape
    CheckShape,        // guard that object has expected shape

    // Type checks (guards)
    CheckInt,
    CheckFloat,
    CheckNotNull,
    CheckBounds,      // i < length

    // Calls
    Call,             // arc function
    CallNative,       // NativeFunc
    CallKnown,        // statically resolved call target

    // Arc-specific
    ForBegin,         // for-prep: pushes iterable/length/index
    ForNext,          // produces next item or jumps to exit
    TryBegin,
    TryEnd,
    Throw,

    // Type conversions
    ToFloat,          // int → float promotion
    ToBool,

    // Misc
    Phi,
    EffectPhi,
    Parameter,
    ConstantTable,

    // Backend
    MachineOp,        // asmjit-emitted op
};
```

### 3.3 Node flags

```cpp
enum NodeFlags : uint32_t {
    None        = 0,
    Pure        = 1 << 0,   // no side effects, no effect input
    CSEable     = 1 << 1,   // common subexpression elimination candidate
    GVNable     = 1 << 2,   // global value numbering candidate
    Commutative = 1 << 3,   // Add, Mul, Eq, Ne, etc.
    NoThrow     = 1 << 4,
    NoDeopt     = 1 << 5,
    HasFrameState = 1 << 6, // attached FrameState node
    IsAllocated = 1 << 7,  // lives in a register, not on stack
    IsPinned    = 1 << 8,   // cannot be moved by GCM
    IsGuard     = 1 << 9,
    IsControl   = 1 << 10,
    IsEffect    = 1 << 11,
    IsDead      = 1 << 12,  // marked for removal
};
```

### 3.4 Type system

```cpp
enum class TypeId : uint16_t {
    Top,            // unknown / any
    Bottom,         // unreachable
    Int,
    Float,
    Bool,
    Null,
    Undef,
    String,
    List,
    Function,
    NativeFunction,
    Class,
    Instance,
    Object,         // any heap object
    IntOrFloat,      // dual — int or float (after type narrowing)
    NonNullObj,
};

struct TypeLattice {
    // lattice operations: join, meet, isTop, isBottom, isSubtypeOf
    // used by type narrowing passes
};
```

### 3.5 Graph container

```cpp
class Graph {
    std::vector<Node>       nodes_;       // indexed by NodeId
    std::vector<NodeId>    edges_;       // edge pool
    std::vector<NodeId>    uses_;        // use pool
    std::vector<FrameState> frame_states_;
    Arena*                  arena_;
    NodeId                  start_, stop_;
    // ...
};
```

Adding a node = push to `nodes_` + push its inputs to `edges_`. O(1), no per-node allocation.

---

## 4. Full pass list per tier

### 4.1 Tier 1 — Baseline SSA passes

The goal here is **not** maximum speed. It is: compile fast, get off the interpreter, gather better runtime profiles, keep the system responsive while Tier 2 compiles.

1. **Bytecode normalization and stack-to-register lowering** — convert Arc's stack operations to register operations, remove redundant push/pop pairs, map local slots to virtual registers, normalize control flow.
2. **CFG construction** — build basic blocks from the linear bytecode stream; resolve jump targets.
3. **SSA construction** — insert Φ nodes at dominance frontiers; rename variables. Use Cytron's algorithm.
4. **Type feedback incorporation** — annotate each value with the type observed by Tier 0; the JIT uses this to choose between the int fast-path and the float slow-path.
5. **Monomorphic IC inlining** — for each `PROPERTY_ACCESS`, `INDEX_GET`, `CALL` with a single observed shape/target, emit a shape-check + fast-path; fall back to a runtime call on miss.
6. **Linear scan register allocation** — Wimmer–Franz algorithm; O(n log n) in instruction count.
7. **asmjit code emission** — emit x86-64 machine code; emit prologue/epilogue; emit safepoint polls at loop back-edges and call sites.
8. **Deopt table emission** — for each potentially-failing operation, record (bytecode offset, register state) in a side table that the runtime can use to reconstruct interpreter state.

### 4.2 Tier 2 — Sea of Nodes passes

Each pass is a small, idempotent reducer.

**Graph construction passes:**
1. SSA-to-SoN lowering — convert CFG+SSA into SoN nodes with control/effect/data edges.
2. Frame-state attachment — attach a FrameState to every potentially-failing node.

**Cleanup passes:**
3. Dead code elimination (DCE) — remove nodes with no uses and no side effects.
4. Reachability pruning — drop nodes not reachable from `Start` or from `Stop`.

**Speculation passes:**
5. Type narrowing — propagate `TypeId`s through the graph; refine `Int|Float` to `Int` where possible.
6. Guard insertion — emit `CheckInt` / `CheckShape` / `CheckBounds` nodes based on narrowed types.
7. Guard strength reduction — replace strong guards (deopt on miss) with weak guards (fall back to slow path) where the slow path is cheap.

**Strength reduction passes:**
8. Constant folding — fold `ConstInt + ConstInt` → `ConstInt`.
9. Algebraic simplification — `x + 0 → x`, `x * 1 → x`, `x * 0 → 0`, `!!x → x`.
10. Strength reduction — `x * 2^k → x << k`, `x / 2^k → x >> k` (signedness-aware).
11. Bounds-check elimination (BCE) — prove `0 <= i < len` from loop induction variables; drop the `CheckBounds`.
12. Comparison folding — `!(x < y) → x >= y`, `x == x → true`.
13. Branch folding — `if (true) A else B → A`.

**Code motion passes:**
14. Global value numbering (GVN) — `x + y; x + y` → `let t = x + y; t; t`.
15. Loop invariant code motion (LICM) — move pure, loop-invariant operations out of the loop.
16. Global code motion (GCM) — schedule early / schedule late to hide latency and reduce register pressure.

**Allocation passes:**
17. Escape analysis — prove an `Allocate` doesn't escape the function.
18. Scalar replacement — replace a non-escaping `Allocate` with its fields as separate SSA values.
19. Lock elision — if Arc ever adds synchronization, drop locks on non-escaping objects.

**Loop passes:**
20. Loop unrolling — unroll hot loops by a small factor (e.g., 4× or 8×) based on profile count.
21. Loop unswitching — hoist invariant `if` out of loops.
22. Peel + specialize — peel the first iteration to specialize it against the actual types seen.

**Inlining passes:**
23. Call inlining — inline monomorphic `Call` sites whose target fits inside the inline budget.
24. Inline caching stub generation — replace `Call` with shape-check + direct-target-call.

**Lowering passes:**
25. SoN-to-CFG re-linearization — turn the SoN graph back into a CFG of basic blocks.
26. Operand lowering — turn high-level nodes (`Add`) into machine nodes (`x86Add`, `x86Mov`, etc.).
27. Linear scan register allocation — same algorithm as Tier 1, but with the richer SoN-derived live ranges.
28. asmjit emission — emit final machine code; patch in entry trampolines; emit deopt metadata.

---

## 5. Multi-threading & concurrency

A JIT compiler is essentially a massive DAG of tasks. Standard OS threads with mutexes will cause lock contention and kill scalability. We use a **work-stealing task scheduler**.

### 5.1 Scheduler choice

**enkiTS** (vendored under `third_party/enkits`):
- Tiny, blazing-fast work-stealing scheduler used in AAA game engines and high-performance emulators.
- Supports pinned tasks, thread-local storage, priority queues.
- Virtually zero overhead.

### 5.2 Thread pool topology

Three distinct pools, all managed by enkiTS:

**Pool 1 — Mutator Pool (application threads)**
- Runs user Arc code (interpreter, Tier-1, Tier-2 machine code).
- **Rule**: never block on JIT or GC locks. Allocation = thread-local bump arena, falling back to global arena only when full.
- **Rule**: every loop back-edge and call site is a safepoint.

**Pool 2 — Compiler Pool (background)**
- Tier-1 queue: high priority, short tasks.
- Tier-2 queue: lower priority, heavy tasks.
- **Task granularity**: do **not** compile an entire function as one task. For SoN, break it into "build graph", "run GVN", "schedule", "regalloc", "emit" subtasks. This lets multiple compiler threads chew on the same function concurrently.

**Pool 3 — GC Pool (background)**
- Runs concurrent marking and sweeping.
- Triggers a brief mutator pause only at the final fixup phase.

### 5.3 Safepoint / handshake mechanism

Pause-the-world without heavy OS mutexes:

- Atomic state machine, per mutator thread:
  - State 0: `Running`
  - State 1: `SafepointRequested`
  - State 2: `Safepointed`
- GC thread sets all mutators to State 1.
- Every mutator, at its next safepoint (loop back-edge, call, method return), checks the flag.
  - If set: atomically set state to `Safepointed`, increment a "stopped threads" counter, then wait on a thread-local futex.
- When the stopped counter equals the mutator pool size, GC runs.
- GC wakes all mutators via futex.

This is the model used by V8 (stack checks) and OpenJDK (thread-local handshakes).

---

## 6. Memory management for the JIT

### 6.1 Arena bump allocators for IR nodes

Every `Node`, `CFGBlock`, `SSAVariable`, side-table entry is allocated via a thread-local bump allocator.

- Allocating a node = `ptr += sizeof(Node)`. O(1), branchless (modulo alignment).
- Free = `ptr = base`. Bulk free at end of compilation.
- Arenas are pooled and reused across compilations to amortize `mmap` cost.

### 6.2 Epoch-based reclamation (EBR)

When the SoN optimizer replaces an old Node with a new one (during GVN, scalar replacement, etc.), the old node cannot be freed immediately — a concurrent compiler thread might still be reading it.

- Tag deleted nodes with the current epoch.
- Three epochs rotate: 0 → 1 → 2 → 0.
- Each compiler thread announces which epoch it is reading.
- When all threads have advanced past epoch E, nodes tagged with epoch E can be bulk-freed.

This is what `libck`'s `ck_epoch` does. We implement a small in-house version to avoid the C-only dependency.

### 6.3 Code memory

JIT-emitted machine code lives in `mmap`'d pages with `PROT_READ | PROT_WRITE` during emission, then `mprotect`'d to `PROT_READ | PROT_EXEC` for execution. asmjit handles this via its `JitAllocator`. We use its default `virtmem` allocator, which supports W^X by default.

### 6.4 Patchpoints

For Tier-1 IC stubs, we leave small pre-allocated code "slots" that can be patched at runtime to point to a new shape-specific fast path. Patching is done atomically (single 8-byte store on x86-64).

---

## 7. C++23 usage rules

Use C++23 for the compiler infrastructure, but keep the hot JIT graph engine low-level and controlled.

### 7.1 Allowed features

| Feature                       | Use case                                                    |
| ----------------------------- | ----------------------------------------------------------- |
| `std::expected<T, E>`         | fallible compiler API (parse, compile, allocate)           |
| `std::span<T>`                | non-owning views over bytecode / node arrays                |
| `std::print` / `std::println` | debug logging, IR dumps                                     |
| `std::bit_cast`               | type-punning for NaN-boxing experiments                     |
| `std::byteswap`               | endianness for bytecode (Arc is big-endian)                |
| `deducing this`               | CRTP-free builder pattern, e.g. `Graph::add()`              |
| `std::mdspan`                 | 2-D analysis (live ranges × blocks)                        |
| `std::move_only_function`     | pass callbacks to pass manager                              |
| `consteval` / `constinit`     | opcode tables, kind→name maps                               |
| `if consteval`                | dispatch between constexpr and runtime paths                |
| `std::flat_map` / `std::flat_set` | small, cache-friendly analysis maps                     |

### 7.2 Forbidden features in hot paths

- Exceptions — compile with `-fno-exceptions` for the JIT proper.
- RTTI — compile with `-fno-rtti` for the JIT proper.
- `std::shared_ptr`, `std::function`, `std::regex` in IR mutation code.
- Heap-allocating containers in inner loops.
- `std::endl` (use `'\n'`).

### 7.3 Build configuration

```cmake
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# JIT proper: no exceptions, no RTTI
target_compile_options(arcjit_jit PRIVATE
    -fno-exceptions -fno-rtti
    -Wall -Wextra -Wpedantic -Werror
    -O3 -march=native -flto
)

# Runtime glue: exceptions allowed (for FFI error reporting)
target_compile_options(arcjit_runtime PRIVATE
    -O3 -march=native -flto
)
```

### 7.4 Code style

- `.clang-format` with `BasedOnStyle: LLVM`, `ColumnLimit: 100`, `IndentWidth: 4`, `BreakBeforeBraces: Attach`.
- Names: `snake_case` for functions/variables, `PascalCase` for classes/structs/enums, `kCamelCase` for constants.
- Every public header begins with `#pragma once`.
- Every header is self-contained: includes what it uses.

---

## 8. Repository layout

```
arcJIT/
├── docs/
│   ├── ARCHITECTURE.md          ← this file
│   ├── BYTECODE.md              ← Arc bytecode reference (mirrored from upstream)
│   └── PASS_LIST.md             ← detailed pass-by-pass design
├── src/
│   ├── core/                    ← Node, Graph, Arena, EBR, TypeId
│   ├── bytecode/                ← Arc Chunk, OpCode, disassembler
│   ├── interp/                  ← Tier 0 interpreter
│   ├── tier1/                   ← Baseline SSA JIT
│   ├── tier2/                   ← Sea of Nodes JIT
│   ├── passman/                 ← Pass manager, pass pipeline
│   ├── runtime/                 ← ICs, TFVs, safepoints, GC glue
│   ├── codegen/                 ← asmjit wrappers, trampolines
│   └── main.cpp                 ← CLI entry point
├── tests/                       ← unit + integration tests (GoogleTest)
├── examples/                    ← sample Arc programs to JIT
├── cmake/                       ← CMake helpers (FetchContent, sanitizer flags)
├── third_party/                 ← vendored deps (asmjit, enkiTS)
├── CMakeLists.txt
├── CMakePresets.json
└── README.md
```

---

## 9. Definition of done

The project is "done" for the initial milestone when:

1. The Tier-0 interpreter runs every opcode in Arc's `bytecode.md` and produces byte-for-byte identical output to upstream `vmRun`.
2. Tier 1 can compile a non-trivial Arc function (arithmetic, locals, branches, loops, calls) and execute it faster than the interpreter, with deopt working.
3. Tier 2 can take the same function, run GVN + escape analysis + GCM + linear scan, and execute it noticeably faster than Tier 1.
4. The safepoint mechanism works: a request from any thread reaches all mutator threads within a bounded number of bytecode instructions.
5. The compiler pool runs Tier-1 and Tier-2 jobs concurrently across multiple threads without corrupting IR state.
6. All passes are idempotent — running them twice produces the same IR.

Everything beyond that is optimization work.
