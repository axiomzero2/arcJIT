I'll design a comprehensive benchmark suite that isolates every optimization pass and runtime component. Each benchmark has a clear purpose, expected outcome, and maps to specific Gigavolt passes or runtime machinery.

---

# Arc JIT Benchmark Suite: 100+ Benchmarks, 17 Categories

## Category Structure

| # | Category | Benchmarks | Primary Passes Tested |
|---|----------|------------|----------------------|
| 1 | Arithmetic & Numeric | 8 | ConstFold, AlgebraicSimp, StrengthReduce, TypeNarrow |
| 2 | Control Flow | 6 | BranchFold, ReachPrune, DCE |
| 3 | Loops & Iteration | 10 | LICM, LoopUnroll, BCE, InductionVar |
| 4 | Object Allocation | 8 | EscapeAnalysis, ScalarReplace, AllocSink |
| 5 | Field Access | 7 | ShapeSpecialize, LoadElim, StoreForward |
| 6 | Method Dispatch | 6 | InlineCache, Devirtualize, CallInline |
| 7 | Lists & Collections | 8 | ListSpecialize, IterElim, BCE |
| 8 | Functions & Closures | 6 | CallInline, ClosureOpt, ArgSpecialize |
| 9 | Type Polymorphism | 6 | TypeNarrow, GuardStrengthen, Speculate |
| 10 | Shape Polymorphism | 6 | ShapeGuard, PolymorphicIC, MegamorphicFallback |
| 11 | Memory & Aliasing | 5 | AliasAnalysis, MemPRE, DeadStoreElim |
| 12 | FFI Integration | 7 | FFISpecialize, FFIPurity, TransitionOpt |
| 13 | Exceptions & Errors | 6 | ExceptionProfile, LazyErrorAlloc, ColdOutline |
| 14 | Recursion | 5 | TailCallOpt, RecursionInline, StackCheck |
| 15 | Guard & Deopt Stress | 7 | GuardCoalesce, GuardSink, DeoptAnalytics |
| 16 | Mixed Realistic | 8 | Full pipeline integration |
| 17 | Compile-Time & Stress | 5 | CompileBudget, CachePressure, Determinism |

**Total: 110 benchmarks**

---

## Category 1: Arithmetic & Numeric (8 benchmarks)

### 1.1 `arith_const_fold_simple`
```arc
function test()
    return (10 + 20) * 3 + 7 - 42
end
```
**Purpose:** Validate `ConstFold` reduces to single constant.  
**Expected:** 1 node (`ConstInt(48)`), 0.3 ns/op.  
**Passes:** ConstFold, DCE.

### 1.2 `arith_const_fold_chain`
```arc
function test()
    a = 1
    b = a + 2
    c = b + 3
    d = c + 4
    return d + 5
end
```
**Purpose:** Test copy propagation + constant folding through chains.  
**Expected:** All intermediate values folded, final = 15.  
**Passes:** ConstFold, CopyProp, DCE.

### 1.3 `arith_strength_reduce`
```arc
function test(n)
    return n * 8 + n * 16 + n / 2
end
```
**Purpose:** Validate `StrengthReduce` converts mul/div by powers of 2 to shifts.  
**Expected:** No MUL/DIV instructions in assembly, only SHL/SHR.  
**Passes:** StrengthReduce.

### 1.4 `arith_algebraic_identity`
```arc
function test(x)
    a = x + 0
    b = x * 1
    c = x - 0
    d = x | 0
    return a + b + c + d
end
```
**Purpose:** Validate `AlgebraicSimp` removes identity operations.  
**Expected:** All identities eliminated, result = 4*x.  
**Passes:** AlgebraicSimp, DCE.

### 1.5 `arith_type_narrow_int32`
```arc
function test(a, b)
    ; a, b always Int32 in profile
    return a + b
end
```
**Purpose:** Validate `TypeNarrow` unboxes Int32 arithmetic.  
**Expected:** No type guards, native ADD instruction.  
**Passes:** TypeNarrow, GuardElim.

### 1.6 `arith_overflow_check_elim`
```arc
function test()
    sum = 0
    for i = 0 to 100
        sum = sum + i
    end
    return sum
end
```
**Purpose:** Validate `RangeAnalysis` proves no overflow, eliminates checks.  
**Expected:** No overflow guard in hot loop.  
**Passes:** RangeAnalysis, GuardElim.

### 1.7 `arith_float_reassociate`
```arc
function test(a, b, c, d)
    return (a + b) + (c + d)
end
```
**Purpose:** Validate `FastMath` reassociates for better ILP (if enabled).  
**Expected:** Tree reduction instead of linear chain.  
**Passes:** FastMath, Reassociate.

### 1.8 `arith_mixed_types`
```arc
function test(a, b)
    ; a is Int, b is Float
    return a + b
end
```
**Purpose:** Validate correct type promotion without over-specialization.  
**Expected:** Int→Float conversion, Float add, no deopt.  
**Passes:** TypeNarrow, RepresentationSelect.

---

## Category 2: Control Flow (6 benchmarks)

### 2.1 `ctrl_branch_fold_true`
```arc
function test()
    if true then
        return 1
    else
        return 2
    end
end
```
**Purpose:** Validate `BranchFold` eliminates dead branch.  
**Expected:** Single `Return(1)`, else branch removed.  
**Passes:** BranchFold, DCE, ReachPrune.

### 2.2 `ctrl_branch_fold_runtime`
```arc
function test(flag)
    ; flag always true in profile
    if flag then
        return 1
    else
        return 2
    end
end
```
**Purpose:** Validate speculative branch folding with guard.  
**Expected:** Guard on `flag`, fast path returns 1, slow path deopts.  
**Passes:** BranchFold, GuardInsert.

### 2.3 `ctrl_unreachable_code`
```arc
function test()
    return 42
    x = 100  ; unreachable
    y = 200  ; unreachable
    return x + y
end
```
**Purpose:** Validate `ReachPrune` removes unreachable code.  
**Expected:** Only `Return(42)` remains.  
**Passes:** ReachPrune, DCE.

### 2.4 `ctrl_switch_dense`
```arc
function test(x)
    switch x
        case 0: return 10
        case 1: return 20
        case 2: return 30
        case 3: return 40
        default: return 0
    end
end
```
**Purpose:** Validate dense switch lowering to jump table.  
**Expected:** Jump table in assembly, no compare chain.  
**Passes:** SwitchLower.

### 2.5 `ctrl_if_conversion`
```arc
function test(x)
    if x > 0 then
        y = 1
    else
        y = -1
    end
    return y
end
```
**Purpose:** Validate `IfConversion` to conditional move.  
**Expected:** CMOV instruction, no branch.  
**Passes:** IfConversion.

### 2.6 `ctrl_nested_branches`
```arc
function test(a, b, c)
    if a then
        if b then
            return 1
        else
            return 2
        end
    else
        if c then
            return 3
        else
            return 4
        end
    end
end
```
**Purpose:** Validate control flow simplification doesn't break nesting.  
**Expected:** Correct result for all 8 input combinations.  
**Passes:** BranchFold, CFGCleanup.

---

## Category 3: Loops & Iteration (10 benchmarks)

### 3.1 `loop_sum_simple`
```arc
function test(n)
    sum = 0
    for i = 0 to n
        sum = sum + i
    end
    return sum
end
```
**Purpose:** Baseline loop optimization test.  
**Expected:** Induction var recognized, unboxed arithmetic.  
**Passes:** InductionVar, TypeNarrow.

### 3.2 `loop_invariant_hoist`
```arc
function test(arr, n)
    for i = 0 to n
        x = arr.length  ; invariant
        arr[i] = x + i
    end
end
```
**Purpose:** Validate `LICM` hoists invariant load.  
**Expected:** `arr.length` loaded once before loop.  
**Passes:** LICM.

### 3.3 `loop_bounds_check_elim`
```arc
function test(arr)
    for i = 0 to arr.length - 1
        arr[i] = i * 2
    end
end
```
**Purpose:** Validate `BCE` eliminates bounds checks.  
**Expected:** No bounds check in loop body.  
**Passes:** BCE, RangeAnalysis.

### 3.4 `loop_unroll_small`
```arc
function test()
    sum = 0
    for i = 0 to 3
        sum = sum + i
    end
    return sum
end
```
**Purpose:** Validate `LoopUnroll` for small constant bounds.  
**Expected:** Loop fully unrolled, no loop overhead.  
**Passes:** LoopUnroll, ConstFold.

### 3.5 `loop_peel_first`
```arc
function test(arr)
    for i = 0 to arr.length - 1
        if i == 0 then
            arr[i] = 100
        else
            arr[i] = arr[i-1] + 1
        end
    end
end
```
**Purpose:** Validate `LoopPeel` removes first-iteration branch.  
**Expected:** First iteration peeled, main loop branch-free.  
**Passes:** LoopPeel, BranchFold.

### 3.6 `loop_unswitch`
```arc
function test(arr, flag)
    for i = 0 to arr.length - 1
        if flag then
            arr[i] = arr[i] * 2
        else
            arr[i] = arr[i] + 1
        end
    end
end
```
**Purpose:** Validate `LoopUnswitch` moves invariant branch out.  
**Expected:** Two specialized loops, no branch in loop body.  
**Passes:** LoopUnswitch.

### 3.7 `loop_vectorize_add`
```arc
function test(a, b, c, n)
    for i = 0 to n - 1
        c[i] = a[i] + b[i]
    end
end
```
**Purpose:** Validate `Vectorize` generates SIMD code.  
**Expected:** SIMD ADD instructions (SSE/AVX).  
**Passes:** Vectorize, BCE, AliasAnalysis.

### 3.8 `loop_nested`
```arc
function test(mat, n)
    for i = 0 to n - 1
        for j = 0 to n - 1
            mat[i][j] = i + j
        end
    end
end
```
**Purpose:** Validate nested loop optimization.  
**Expected:** Inner loop optimized, outer loop invariant hoisted.  
**Passes:** LICM, BCE, InductionVar.

### 3.9 `loop_induction_strength`
```arc
function test(n)
    x = 0
    for i = 0 to n
        x = x + 5  ; derived induction var
    end
    return x
end
```
**Purpose:** Validate derived induction variable recognition.  
**Expected:** `x` recognized as `5*i`, strength reduced.  
**Passes:** InductionVar, StrengthReduce.

### 3.10 `loop_infinite_guard`
```arc
function test()
    i = 0
    while true
        i = i + 1
        if i > 1000 then break
    end
    return i
end
```
**Purpose:** Validate safepoint insertion in infinite loops.  
**Expected:** Safepoint poll present, no hang.  
**Passes:** SafepointInsert.

---

## Category 4: Object Allocation (8 benchmarks)

### 4.1 `alloc_non_escaping`
```arc
function test()
    p = Point(10, 20)
    return p.x + p.y
end
```
**Purpose:** Validate `EscapeAnalysis` + `ScalarReplace` eliminates allocation.  
**Expected:** No allocation, fields in registers.  
**Passes:** EscapeAnalysis, ScalarReplace.

### 4.2 `alloc_escaping_return`
```arc
function test()
    p = Point(10, 20)
    return p
end
```
**Purpose:** Validate allocation retained when object escapes.  
**Expected:** Allocation present, correct return.  
**Passes:** EscapeAnalysis (correctly identifies escape).

### 4.3 `alloc_escaping_call`
```arc
function test()
    p = Point(10, 20)
    print_point(p)
    return 0
end
```
**Purpose:** Validate allocation retained when passed to call.  
**Expected:** Allocation present before call.  
**Passes:** EscapeAnalysis.

### 4.4 `alloc_partial_escape`
```arc
function test(flag)
    p = Point(10, 20)
    if flag then
        return p  ; escapes only on this path
    end
    return p.x + p.y
end
```
**Purpose:** Validate `PEA` materializes only on escape path.  
**Expected:** No allocation on fast path, materialize on slow path.  
**Passes:** PEA, Materialize.

### 4.5 `alloc_array_non_escaping`
```arc
function test()
    arr = [1, 2, 3, 4, 5]
    sum = 0
    for i = 0 to 4
        sum = sum + arr[i]
    end
    return sum
end
```
**Purpose:** Validate array PEA.  
**Expected:** No array allocation, values scalarized.  
**Passes:** EscapeAnalysis, ScalarReplace.

### 4.6 `alloc_nested_objects`
```arc
function test()
    p1 = Point(1, 2)
    p2 = Point(3, 4)
    line = Line(p1, p2)
    return line.start.x + line.end.y
end
```
**Purpose:** Validate nested object PEA.  
**Expected:** All three allocations eliminated.  
**Passes:** EscapeAnalysis, ScalarReplace.

### 4.7 `alloc_loop_temporary`
```arc
function test(n)
    sum = 0
    for i = 0 to n
        p = Point(i, i*2)
        sum = sum + p.x + p.y
    end
    return sum
end
```
**Purpose:** Validate per-iteration allocation elimination.  
**Expected:** No allocation in loop, fields in registers.  
**Passes:** EscapeAnalysis, ScalarReplace.

### 4.8 `alloc_arena_bump`
```arc
function test(n)
    arr = []
    for i = 0 to n
        arr.append(Point(i, i))
    end
    return arr.length
end
```
**Purpose:** Validate arena bump pointer inlining.  
**Expected:** Inline bump allocation, no runtime call.  
**Passes:** ArenaSpecialize.

---

## Category 5: Field Access (7 benchmarks)

### 5.1 `field_mono_load`
```arc
function test(p)
    ; p always has Shape_Point
    return p.x + p.y
end
```
**Purpose:** Validate monomorphic field load specialization.  
**Expected:** Direct offset load, shape guard.  
**Passes:** ShapeSpecialize, IC.

### 5.2 `field_mono_store`
```arc
function test(p)
    p.x = 100
    p.y = 200
end
```
**Purpose:** Validate monomorphic field store specialization.  
**Expected:** Direct offset store, shape guard.  
**Passes:** ShapeSpecialize, IC.

### 5.3 `field_poly_two_shapes`
```arc
function test(obj)
    ; obj is Shape_A 50%, Shape_B 50%
    return obj.value
end
```
**Purpose:** Validate polymorphic IC with two shapes.  
**Expected:** Two shape checks, no megamorphic fallback.  
**Passes:** PolymorphicIC.

### 5.4 `field_megamorphic`
```arc
function test(obj)
    ; obj has 10+ different shapes
    return obj.value
end
```
**Purpose:** Validate megamorphic fallback.  
**Expected:** Hash lookup or vtable dispatch.  
**Passes:** MegamorphicFallback.

### 5.5 `field_load_elim`
```arc
function test(p)
    x1 = p.x
    x2 = p.x
    return x1 + x2
end
```
**Purpose:** Validate `LoadElim` removes redundant load.  
**Expected:** Single load, value reused.  
**Passes:** LoadElim, AliasAnalysis.

### 5.6 `field_store_forward`
```arc
function test(p)
    p.x = 42
    return p.x
end
```
**Purpose:** Validate `StoreForward` bypasses load after store.  
**Expected:** No load, return constant 42.  
**Passes:** StoreForward.

### 5.7 `field_chained_access`
```arc
function test(obj)
    return obj.a.b.c.d
end
```
**Purpose:** Validate chained field access optimization.  
**Expected:** Shape guards for each level, direct loads.  
**Passes:** ShapeSpecialize, LoadElim.

---

## Category 6: Method Dispatch (6 benchmarks)

### 6.1 `call_mono_inline`
```arc
function add(a, b)
    return a + b
end

function test()
    return add(10, 20)
end
```
**Purpose:** Validate monomorphic call inlining.  
**Expected:** `add` inlined, no call instruction.  
**Passes:** CallInline.

### 6.2 `call_poly_two_targets`
```arc
function test(fn)
    ; fn is add 50%, sub 50%
    return fn(10, 20)
end
```
**Purpose:** Validate polymorphic call site handling.  
**Expected:** Two call targets, no deopt.  
**Passes:** PolymorphicCall.

### 6.3 `call_devirtualize`
```arc
class Shape
    function area() return 0 end
end

class Circle : Shape
    function area() return 3.14 * r * r end
end

function test(c)
    ; c always Circle
    return c.area()
end
```
**Purpose:** Validate devirtualization of method calls.  
**Expected:** Direct call to `Circle.area`, inlined.  
**Passes:** Devirtualize, CallInline.

### 6.4 `call_recursive`
```arc
function fib(n)
    if n < 2 then return n
    return fib(n-1) + fib(n-2)
end

function test()
    return fib(10)
end
```
**Purpose:** Validate recursive call handling.  
**Expected:** No stack overflow, correct result.  
**Passes:** CallInline (with depth limit).

### 6.5 `call_higher_order`
```arc
function apply(fn, x)
    return fn(x)
end

function double(x)
    return x * 2
end

function test()
    return apply(double, 21)
end
```
**Purpose:** Validate higher-order function inlining.  
**Expected:** `apply` and `double` inlined.  
**Passes:** CallInline, ArgSpecialize.

### 6.6 `call_variadic`
```arc
function sum(...)
    total = 0
    for arg in args
        total = total + arg
    end
    return total
end

function test()
    return sum(1, 2, 3, 4, 5)
end
```
**Purpose:** Validate variadic call handling.  
**Expected:** Correct result, no crash.  
**Passes:** CallLowering.

---

## Category 7: Lists & Collections (8 benchmarks)

### 7.1 `list_index_mono`
```arc
function test(arr)
    ; arr always Int list
    return arr[0] + arr[1]
end
```
**Purpose:** Validate list element type specialization.  
**Expected:** Unboxed int loads, no type checks.  
**Passes:** ListSpecialize, TypeNarrow.

### 7.2 `list_append_hot`
```arc
function test(n)
    arr = []
    for i = 0 to n
        arr.append(i)
    end
    return arr.length
end
```
**Purpose:** Validate list append optimization.  
**Expected:** Inline capacity check, fast append.  
**Passes:** ListSpecialize.

### 7.3 `list_iter_elim`
```arc
function test(arr)
    sum = 0
    for x in arr
        sum = sum + x
    end
    return sum
end
```
**Purpose:** Validate iterator allocation elimination.  
**Expected:** No iterator object, indexed loop.  
**Passes:** IterElim, EscapeAnalysis.

### 7.4 `list_bounds_check`
```arc
function test(arr, i)
    return arr[i]
end
```
**Purpose:** Validate bounds check insertion when needed.  
**Expected:** Bounds check present, correct exception on OOB.  
**Passes:** BCE (correctly retains check).

### 7.5 `list_length_hoist`
```arc
function test(arr)
    for i = 0 to arr.length - 1
        arr[i] = i
    end
end
```
**Purpose:** Validate length load hoisted out of loop.  
**Expected:** Length loaded once before loop.  
**Passes:** LICM.

### 7.6 `list_mutation_invalidate`
```arc
function test(arr)
    for x in arr
        if x > 10 then
            arr.append(999)  ; mutates during iteration
        end
    end
end
```
**Purpose:** Validate iterator invalidation on mutation.  
**Expected:** Correct behavior (exception or safe fallback).  
**Passes:** IterElim (correctly bails out).

### 7.7 `list_slice`
```arc
function test(arr)
    return arr[2:5]
end
```
**Purpose:** Validate list slice operation.  
**Expected:** Correct slice, no crash.  
**Passes:** ListLowering.

### 7.8 `list_concat`
```arc
function test(a, b)
    return a + b  ; list concatenation
end
```
**Purpose:** Validate list concatenation.  
**Expected:** Correct result, efficient copy.  
**Passes:** ListLowering, MemCopy.

---

## Category 8: Functions & Closures (6 benchmarks)

### 8.1 `closure_capture_immutable`
```arc
function make_adder(x)
    return function(y) return x + y end
end

function test()
    add5 = make_adder(5)
    return add5(10)
end
```
**Purpose:** Validate closure with immutable capture.  
**Expected:** Capture optimized, no heap allocation if non-escaping.  
**Passes:** ClosureOpt, EscapeAnalysis.

### 8.2 `closure_capture_mutable`
```arc
function make_counter()
    count = 0
    return function()
        count = count + 1
        return count
    end
end

function test()
    c = make_counter()
    c()
    c()
    return c()
end
```
**Purpose:** Validate closure with mutable capture.  
**Expected:** Correct state preservation across calls.  
**Passes:** ClosureOpt.

### 8.3 `closure_inline`
```arc
function apply(fn, x)
    return fn(x)
end

function test()
    return apply(function(x) return x * 2 end, 21)
end
```
**Purpose:** Validate closure inlining.  
**Expected:** Closure inlined into `apply`.  
**Passes:** CallInline, ClosureOpt.

### 8.4 `function_arg_specialize`
```arc
function test(x)
    ; x always Int in profile
    return x * 2
end
```
**Purpose:** Validate argument type specialization.  
**Expected:** Unboxed int parameter, no type check.  
**Passes:** ArgSpecialize, TypeNarrow.

### 8.5 `function_return_specialize`
```arc
function get_value()
    return 42  ; always returns Int
end

function test()
    x = get_value()
    return x + 1
end
```
**Purpose:** Validate return type specialization.  
**Expected:** Unboxed int return, no type check at call site.  
**Passes:** ReturnSpecialize, TypeNarrow.

### 8.6 `function_default_args`
```arc
function greet(name, greeting="Hello")
    return greeting + ", " + name
end

function test()
    return greet("World")
end
```
**Purpose:** Validate default argument handling.  
**Expected:** Correct default value insertion.  
**Passes:** ArgLowering.

---

## Category 9: Type Polymorphism (6 benchmarks)

### 9.1 `type_mono_int`
```arc
function test(x)
    ; x always Int
    return x + 1
end
```
**Purpose:** Validate monomorphic int specialization.  
**Expected:** Unboxed int arithmetic, no guard.  
**Passes:** TypeNarrow, GuardElim.

### 9.2 `type_poly_int_float`
```arc
function test(x)
    ; x is Int 70%, Float 30%
    return x + 1
end
```
**Purpose:** Validate polymorphic type handling.  
**Expected:** Two paths (int/float), no deopt.  
**Passes:** TypeSpecialize.

### 9.3 `type_narrow_branch`
```arc
function test(x)
    if is_int(x) then
        return x + 1
    else
        return 0
    end
end
```
**Purpose:** Validate type narrowing in branches.  
**Expected:** `x` known Int in true branch, no redundant check.  
**Passes:** TypeNarrow, GuardElim.

### 9.4 `type_guard_strengthen`
```arc
function test(x)
    ; x always Int32 (subset of Int)
    return x + 1
end
```
**Purpose:** Validate guard strengthening to Int32.  
**Expected:** Int32 guard (cheaper), unboxed 32-bit add.  
**Passes:** GuardStrengthen.

### 9.5 `type_guard_weaken`
```arc
function test(x)
    ; x is Number (Int or Float), only need to know it's numeric
    return x + 1
end
```
**Purpose:** Validate guard weakening when full type not needed.  
**Expected:** Single "is numeric" guard, not separate int/float checks.  
**Passes:** GuardWeaken.

### 9.6 `type_megamorphic`
```arc
function test(x)
    ; x has 10+ different types
    return x + 1
end
```
**Purpose:** Validate megamorphic fallback.  
**Expected:** Generic add with runtime dispatch.  
**Passes:** MegamorphicFallback.

---

## Category 10: Shape Polymorphism (6 benchmarks)

### 10.1 `shape_mono_stable`
```arc
function test(obj)
    ; obj always Shape_A
    return obj.x
end
```
**Purpose:** Validate stable monomorphic shape specialization.  
**Expected:** Direct offset load, single shape guard.  
**Passes:** ShapeSpecialize.

### 10.2 `shape_transition`
```arc
function test()
    obj = {}
    obj.x = 1  ; Shape_A -> Shape_B
    obj.y = 2  ; Shape_B -> Shape_C
    return obj.x + obj.y
end
```
**Purpose:** Validate shape transition handling.  
**Expected:** Correct field access after transitions.  
**Passes:** ShapeTransition.

### 10.3 `shape_poly_stable`
```arc
function test(obj)
    ; obj is Shape_A 60%, Shape_B 40%
    return obj.value
end
```
**Purpose:** Validate stable polymorphic shapes.  
**Expected:** Two shape checks, no deopt.  
**Passes:** PolymorphicIC.

### 10.4 `shape_unstable`
```arc
function test(obj)
    ; obj shape changes every call
    return obj.value
end
```
**Purpose:** Validate unstable shape handling.  
**Expected:** Megamorphic fallback, no deopt storm.  
**Passes:** MegamorphicFallback, DeoptAnalytics.

### 10.5 `shape_dictionary_mode`
```arc
function test()
    obj = {}
    for i = 0 to 100
        obj["field_" + i] = i  ; many fields, dictionary mode
    end
    return obj.field_50
end
```
**Purpose:** Validate dictionary-mode object handling.  
**Expected:** Hash lookup, no crash.  
**Passes:** DictionaryMode.

### 10.6 `shape_field_type_specialize`
```arc
function test(obj)
    ; obj.x always Int
    return obj.x + 1
end
```
**Purpose:** Validate field type specialization.  
**Expected:** Field loaded as unboxed Int, no type check.  
**Passes:** FieldTypeSpecialize.

---

## Category 11: Memory & Aliasing (5 benchmarks)

### 11.1 `mem_no_alias`
```arc
function test(p1, p2)
    ; p1 and p2 never alias
    p1.x = 10
    return p2.x
end
```
**Purpose:** Validate alias analysis proves no alias.  
**Expected:** Load not eliminated (p2.x unknown).  
**Passes:** AliasAnalysis, LoadElim.

### 11.2 `mem_may_alias`
```arc
function test(p1, p2)
    ; p1 and p2 may alias
    p1.x = 10
    return p2.x
end
```
**Purpose:** Validate conservative alias analysis.  
**Expected:** Load retained (may be 10).  
**Passes:** AliasAnalysis.

### 11.3 `mem_dead_store`
```arc
function test(p)
    p.x = 10
    p.x = 20
    return p.x
end
```
**Purpose:** Validate `DeadStoreElim` removes first store.  
**Expected:** Only second store remains.  
**Passes:** DeadStoreElim.

### 11.4 `mem_store_sink`
```arc
function test(p, flag)
    p.x = 10
    if flag then
        return p.x
    end
    return 0
end
```
**Purpose:** Validate `StoreSink` moves store to needed path.  
**Expected:** Store only on true branch.  
**Passes:** StoreSink.

### 11.5 `mem_pre`
```arc
function test(p, flag)
    if flag then
        x = p.x
    else
        x = p.x
    end
    return x
end
```
**Purpose:** Validate `MemPRE` hoists common load.  
**Expected:** Single load before branch.  
**Passes:** MemPRE.

---

## Category 12: FFI Integration (7 benchmarks)

### 12.1 `ffi_simple_call`
```arc
function test()
    return native_add(10, 20)
end
```
**Purpose:** Validate basic FFI call.  
**Expected:** Correct result, minimal overhead.  
**Passes:** FFILowering.

### 12.2 `ffi_specialize_sig`
```arc
function test(a, b)
    ; native_add(int, int) -> int
    return native_add(a, b)
end
```
**Purpose:** Validate FFI signature specialization.  
**Expected:** Direct call, no marshalling overhead.  
**Passes:** FFISpecialize.

### 12.3 `ffi_loop_hot`
```arc
function test(n)
    sum = 0
    for i = 0 to n
        sum = sum + native_identity(i)
    end
    return sum
end
```
**Purpose:** Validate FFI in hot loop.  
**Expected:** Minimal transition overhead per call.  
**Passes:** FFISpecialize, TransitionOpt.

### 12.4 `ffi_pure_annotate`
```arc
function test(x)
    ; native_sqrt marked @pure
    return native_sqrt(x) + native_sqrt(x)
end
```
**Purpose:** Validate FFI purity annotation enables CSE.  
**Expected:** Single sqrt call, result reused.  
**Passes:** FFIPurity, GVN.

### 12.5 `ffi_side_effect`
```arc
function test()
    native_print("hello")
    native_print("world")
end
```
**Purpose:** Validate FFI side effects prevent reordering.  
**Expected:** Both calls present in order.  
**Passes:** FFISideEffect.

### 12.6 `ffi_pointer_arg`
```arc
function test(buf)
    ; buf is Arc list, passed as pointer
    native_fill(buf, 100)
    return buf.length
end
```
**Purpose:** Validate pointer argument marshalling.  
**Expected:** Correct pointer passed, no crash.  
**Passes:** FFIMarshall.

### 12.7 `ffi_callback`
```arc
function my_callback(x)
    return x * 2
end

function test()
    return native_with_callback(my_callback, 21)
end
```
**Purpose:** Validate FFI callback to Arc function.  
**Expected:** Callback invoked correctly, no crash.  
**Passes:** FFICallback.

---

## Category 13: Exceptions & Errors (6 benchmarks)

### 13.1 `exception_no_throw`
```arc
function test()
    try
        x = 10 + 20
    catch e
        return -1
    end
    return x
end
```
**Purpose:** Validate try/catch with no exception.  
**Expected:** Fast path, no exception overhead.  
**Passes:** ExceptionProfile, ColdOutline.

### 13.2 `exception_throw_catch`
```arc
function test()
    try
        throw "error"
    catch e
        return 42
    end
end
```
**Purpose:** Validate exception throw and catch.  
**Expected:** Correct catch, result = 42.  
**Passes:** ExceptionLowering.

### 13.3 `exception_lazy_alloc`
```arc
function test()
    try
        if false then
            throw Error("never")  ; cold path
        end
    catch e
        return e.message
    end
    return 0
end
```
**Purpose:** Validate `LazyErrorAlloc` defers error object creation.  
**Expected:** No error allocation on hot path.  
**Passes:** LazyErrorAlloc.

### 13.4 `exception_nested_try`
```arc
function test()
    try
        try
            throw "inner"
        catch e
            throw "outer"
        end
    catch e
        return e
    end
end
```
**Purpose:** Validate nested try/catch.  
**Expected:** Correct exception propagation.  
**Passes:** ExceptionLowering.

### 13.5 `exception_finally`
```arc
function test()
    x = 0
    try
        x = 1
        throw "error"
    finally
        x = 2
    end
    return x
end
```
**Purpose:** Validate finally block execution.  
**Expected:** finally runs, x = 2.  
**Passes:** FinallyLowering.

### 13.6 `exception_position_info`
```arc
function test()
    try
        invalid_function()
    catch e
        return e.line_number  ; position-aware error
    end
end
```
**Purpose:** Validate position-aware error reporting.  
**Expected:** Correct line number in error.  
**Passes:** ErrorPosition.

---

## Category 14: Recursion (5 benchmarks)

### 14.1 `recursion_fib`
```arc
function fib(n)
    if n < 2 then return n
    return fib(n-1) + fib(n-2)
end

function test()
    return fib(20)
end
```
**Purpose:** Validate recursive function performance.  
**Expected:** Correct result, no stack overflow.  
**Passes:** CallInline (depth-limited).

### 14.2 `recursion_tail`
```arc
function factorial(n, acc=1)
    if n == 0 then return acc
    return factorial(n-1, n*acc)
end

function test()
    return factorial(100)
end
```
**Purpose:** Validate tail call optimization.  
**Expected:** No stack growth, correct result.  
**Passes:** TailCallOpt.

### 14.3 `recursion_mutual`
```arc
function is_even(n)
    if n == 0 then return true
    return is_odd(n-1)
end

function is_odd(n)
    if n == 0 then return false
    return is_even(n-1)
end

function test()
    return is_even(100)
end
```
**Purpose:** Validate mutual recursion.  
**Expected:** Correct result, no infinite loop.  
**Passes:** CallLowering.

### 14.4 `recursion_deep`
```arc
function deep(n)
    if n == 0 then return 0
    return deep(n-1) + 1
end

function test()
    return deep(10000)
end
```
**Purpose:** Validate stack check insertion.  
**Expected:** Stack overflow prevented, correct result or error.  
**Passes:** StackCheck.

### 14.5 `recursion_inline_shallow`
```arc
function helper(x)
    return x * 2
end

function test(n)
    sum = 0
    for i = 0 to n
        sum = sum + helper(i)
    end
    return sum
end
```
**Purpose:** Validate shallow recursion inlining.  
**Expected:** `helper` inlined into loop.  
**Passes:** CallInline.

---

## Category 15: Guard & Deopt Stress (7 benchmarks)

### 15.1 `guard_coalesce`
```arc
function test(x)
    if is_int(x) then
        if is_int(x) then  ; redundant
            return x + 1
        end
    end
    return 0
end
```
**Purpose:** Validate `GuardCoalesce` removes redundant guards.  
**Expected:** Single type guard.  
**Passes:** GuardCoalesce.

### 15.2 `guard_sink`
```arc
function test(x, flag)
    check_int(x)
    if flag then
        return x + 1
    end
    return 0
end
```
**Purpose:** Validate `GuardSink` moves guard to needed path.  
**Expected:** Guard only on true branch.  
**Passes:** GuardSink.

### 15.3 `guard_hoist_loop`
```arc
function test(arr)
    for i = 0 to arr.length - 1
        check_int(arr[i])
        arr[i] = arr[i] + 1
    end
end
```
**Purpose:** Validate guard hoisted out of loop.  
**Expected:** Single guard before loop (if provable).  
**Passes:** GuardHoist, LICM.

### 15.4 `deopt_shape_mismatch`
```arc
function test(obj)
    ; compiled for Shape_A, called with Shape_B
    return obj.x
end
```
**Purpose:** Validate deopt on shape mismatch.  
**Expected:** Correct deopt, fallback to baseline.  
**Passes:** Deopt, ShapeGuard.

### 15.5 `deopt_type_mismatch`
```arc
function test(x)
    ; compiled for Int, called with Float
    return x + 1
end
```
**Purpose:** Validate deopt on type mismatch.  
**Expected:** Correct deopt, correct result after fallback.  
**Passes:** Deopt, TypeGuard.

### 15.6 `deopt_storm_prevent`
```arc
function test(x)
    ; x type changes every call
    return x + 1
end
```
**Purpose:** Validate deopt storm prevention.  
**Expected:** After N deopts, stop optimizing, stay in baseline.  
**Passes:** DeoptAnalytics, SpeculationPolicy.

### 15.7 `deopt_materialize_virtual`
```arc
function test()
    p = Point(10, 20)  ; virtualized by PEA
    if rare_condition() then
        return p  ; forces materialization
    end
    return p.x + p.y
end
```
**Purpose:** Validate virtual object materialization on deopt.  
**Expected:** Correct object reconstructed.  
**Passes:** PEA, Materialize, Deopt.

---

## Category 16: Mixed Realistic (8 benchmarks)

### 16.1 `real_json_parse`
```arc
function test(json_str)
    obj = json_parse(json_str)
    return obj.name + ": " + obj.value
end
```
**Purpose:** Validate realistic JSON parsing workload.  
**Expected:** Correct parse, good performance.  
**Passes:** Full pipeline.

### 16.2 `real_matrix_multiply`
```arc
function test(a, b, n)
    c = zeros(n, n)
    for i = 0 to n-1
        for j = 0 to n-1
            sum = 0
            for k = 0 to n-1
                sum = sum + a[i][k] * b[k][j]
            end
            c[i][j] = sum
        end
    end
    return c
end
```
**Purpose:** Validate numeric kernel performance.  
**Expected:** Vectorization, loop opts active.  
**Passes:** Vectorize, LICM, BCE.

### 16.3 `real_entity_update`
```arc
function test(entities, dt)
    for e in entities
        e.x = e.x + e.vx * dt
        e.y = e.y + e.vy * dt
    end
end
```
**Purpose:** Validate game-like entity update loop.  
**Expected:** Field access optimized, loop vectorized.  
**Passes:** ShapeSpecialize, Vectorize.

### 16.4 `real_tree_traverse`
```arc
function traverse(node)
    if node == null then return 0
    return node.value + traverse(node.left) + traverse(node.right)
end

function test(root)
    return traverse(root)
end
```
**Purpose:** Validate recursive tree traversal.  
**Expected:** Correct sum, no stack overflow.  
**Passes:** CallInline, NullCheck.

### 16.5 `real_event_dispatch`
```arc
function test(events)
    for e in events
        handler = handlers[e.type]
        handler(e.data)
    end
end
```
**Purpose:** Validate polymorphic dispatch in realistic scenario.  
**Expected:** IC optimization active.  
**Passes:** PolymorphicIC, CallInline.

### 16.6 `real_string_concat`
```arc
function test(n)
    s = ""
    for i = 0 to n
        s = s + "x"
    end
    return s.length
end
```
**Purpose:** Validate string operation performance.  
**Expected:** Efficient concatenation (builder pattern if available).  
**Passes:** StringOpt.

### 16.7 `real_hash_table`
```arc
function test(keys, values)
    table = {}
    for i = 0 to keys.length - 1
        table[keys[i]] = values[i]
    end
    return table["target"]
end
```
**Purpose:** Validate hash table operations.  
**Expected:** Correct lookup, reasonable performance.  
**Passes:** HashLowering.

### 16.8 `real_mixed_workload`
```arc
function test(data)
    result = []
    for item in data
        if item.type == "A" then
            result.append(process_a(item))
        else if item.type == "B" then
            result.append(process_b(item))
        end
    end
    return result
end
```
**Purpose:** Validate mixed realistic workload.  
**Expected:** Correct result, good overall performance.  
**Passes:** Full pipeline.

---

## Category 17: Compile-Time & Stress (5 benchmarks)

### 17.1 `compile_large_function`
```arc
function test()
    ; 1000+ lines of straight-line code
    x = 1
    x = x + 1
    x = x + 1
    ; ... 1000 times
    return x
end
```
**Purpose:** Validate compile time for large functions.  
**Expected:** Compile < 10ms, correct result.  
**Passes:** CompileBudget.

### 17.2 `compile_many_functions`
```arc
function f1() return 1 end
function f2() return 2 end
; ... 100 functions
function test()
    return f1() + f2() + ... + f100()
end
```
**Purpose:** Validate code cache pressure.  
**Expected:** All functions compiled, no eviction issues.  
**Passes:** CodeCache.

### 17.3 `compile_deopt_recompile`
```arc
function test(x)
    return x + 1
end

; Call with Int 100 times, then Float
```
**Purpose:** Validate recompilation after deopt.  
**Expected:** Polymorphic code generated, no infinite recompile.  
**Passes:** DeoptAnalytics, SpeculationPolicy.

### 17.4 `compile_determinism`
```arc
function test(x)
    return x * 2 + 1
end
```
**Purpose:** Validate deterministic compilation.  
**Expected:** Same IR/code generated on repeated compiles.  
**Passes:** Determinism.

### 17.5 `compile_memory_pressure`
```arc
function test()
    ; Allocate many objects to pressure GC/arena
    arr = []
    for i = 0 to 10000
        arr.append(Point(i, i))
    end
    return arr.length
end
```
**Purpose:** Validate compilation under memory pressure.  
**Expected:** No OOM, correct result.  
**Passes:** ArenaManagement.

---

## Benchmark Execution Framework

### Harness Structure

```cpp
struct BenchmarkResult {
    std::string name;
    uint64_t iterations;
    double ns_per_op;
    uint64_t compile_time_us;
    uint32_t deopt_count;
    uint32_t guard_count;
    bool correctness_pass;
    std::string expected_output;
    std::string actual_output;
};

class BenchmarkSuite {
public:
    void run_all();
    void run_category(const std::string& category);
    void run_single(const std::string& name);
    
    void compare_tiers();  // Spark vs Jolt vs Surge
    void validate_correctness();
    void report_statistics();
    
private:
    std::vector<Benchmark> benchmarks;
    BenchmarkResult run_benchmark(const Benchmark& b, Tier tier);
};
```

### Execution Protocol

For each benchmark:

1. **Warmup:** Run 100 iterations in Spark (interpreter)
2. **Baseline:** Run 1000 iterations in Jolt (baseline JIT)
3. **Optimized:** Run 10000 iterations in Surge (optimizing JIT)
4. **Correctness:** Compare output across all tiers
5. **Performance:** Measure ns/op, compile time, deopt count
6. **Validation:** Check expected pass activity (e.g., `ConstFold.changed > 0`)

### Reporting Format

```
=== BENCHMARK RESULTS ===

Category: Arithmetic & Numeric
┌─────────────────────────┬──────────┬──────────┬──────────┬─────────┐
│ Benchmark               │ Spark    │ Jolt     │ Surge    │ Speedup │
├─────────────────────────┼──────────┼──────────┼──────────┼─────────┤
│ arith_const_fold_simple │ 150 ns   │ 2.1 ns   │ 0.3 ns   │ 500x    │
│ arith_strength_reduce   │ 148 ns   │ 2.0 ns   │ 0.4 ns   │ 370x    │
│ arith_type_narrow_int32 │ 152 ns   │ 2.2 ns   │ 0.5 ns   │ 304x    │
└─────────────────────────┴──────────┴──────────┴──────────┴─────────┘

Pass Activity:
  ConstFold: 45/50 benchmarks active (90%)
  TypeNarrow: 38/50 benchmarks active (76%)
  GuardElim: 32/50 benchmarks active (64%)

Issues Detected:
  ⚠️  loop_sum_simple: BCE not firing (expected bounds check elimination)
  ⚠️  alloc_non_escaping: PEA not eliminating allocation
  ❌ field_poly_two_shapes: Wrong result (correctness bug)
```

This suite will give you complete visibility into which optimizations are working, which are broken, and where to focus next. Start by running all benchmarks and fixing any correctness failures (Rule 45), then systematically address performance gaps.