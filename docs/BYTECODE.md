# Arc Bytecode Reference (mirrored from upstream)

> This is a copy of `docs/bytecode.md` from the upstream Arc project at
> https://github.com/VxidDev/Arc — included here so arcJIT can be developed and
> tested without a network round-trip. The canonical source is upstream.

This document lists all opcodes used by the Arc Virtual Machine, their stack effects, and their behavior. Opcodes are defined in `include/compiler.h` and implemented in `src/vm.c` of the Arc repo.

## Operand encoding

- **Constant index**: 3 bytes, big-endian (24-bit). Read via `_read_const_idx`.
- **Jump offset**: 2 bytes, big-endian signed (`int16_t`). Read via `_read_short`.
- **Slot / count / arg count**: 1 byte.

## Data Movement

### OP_LOAD_CONST
- **Stack**: `... -> ..., value`
- **Operand**: 3-byte index into the constant pool.
- **Description**: Pushes a constant (Number, String, or Function) onto the stack. Static constants are pushed by reference; non-static heap objects are deep-copied via `copyValue()`.

### OP_LOAD_VAR
- **Stack**: `... -> ..., value`
- **Operand**: 3-byte index to interned String (name).
- **Description**: Looks up a variable in the current symbol table and pushes its value. Raises `NameError` if undefined.

### OP_STORE_VAR
- **Stack**: `..., value -> ..., value` (peeks)
- **Operand**: 3-byte index to interned String (name).
- **Description**: Assigns the top stack value to a variable in the current symbol table. If the frame has an `instance`, uses `setTableLocal` instead of `setTable`.

### OP_LOAD_LOCAL
- **Stack**: `... -> ..., value`
- **Operand**: 1-byte stack slot index.
- **Description**: Pushes the value of a local variable from the current call frame. If the local is `UNDEF`, raises `NameError` ("Variable used before assignment"). Heap Numbers are unboxed to `VAL_INT` / `VAL_FLOAT` on store.

### OP_STORE_LOCAL
- **Stack**: `..., value -> ..., value` (peeks)
- **Operand**: 1-byte stack slot index.
- **Description**: Assigns the top stack value to a local variable slot. Heap Numbers are unboxed before storing. Old slot value is freed.

### OP_POP
- **Stack**: `..., value -> ...`
- **Description**: Discards the top value on the stack, freeing it if it is a heap object.

## Arithmetic & Logic

### OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_POW
- **Stack**: `..., a, b -> ..., (a op b)`
- **Description**: Binary arithmetic operations.
  - Fast path: `INT + INT` produces `VAL_INT` for `ADD/SUB/MUL`; produces `VAL_FLOAT` for `DIV` (with division-by-zero check) and `POW`.
  - Slow path: any other type combo calls `doArith`, which promotes to `double` if either operand is `Int` or `Float`, performs string concat or string repetition for `String` operands, and otherwise errors.
- `OP_DIV` raises a `ValueError` ("Division by zero") if `b == 0`.

### OP_EQ, OP_NE, OP_LT, OP_GT, OP_LTE, OP_GTE
- **Stack**: `..., a, b -> ..., (0 or 1)`
- **Description**: Comparison operations. Results in an integer `1` (true) or `0` (false). String comparison via `strcmp` for `EQ`/`NE`.

### OP_AND, OP_OR
- **Stack**: `..., a, b -> ..., (0 or 1)`
- **Description**: Logical AND/OR operations. C-style short-circuit semantics do not apply at the bytecode level; both operands are evaluated before the operation.

### OP_NEG
- **Stack**: `..., a -> ..., -a`
- **Description**: Unary arithmetic negation. Promotes `INT` to `FLOAT` (matches `doArith` semantics).

### OP_NOT
- **Stack**: `..., a -> ..., !a`
- **Description**: Logical NOT. Returns `1` if `isTruthy(a)` is false, else `0`.

## Control Flow

### OP_JUMP
- **Operand**: 2-byte signed offset.
- **Description**: Unconditionally adds the offset to the Instruction Pointer (`ip`).

### OP_JUMP_IF_FALSE
- **Stack**: `..., condition -> ...` (pops)
- **Operand**: 2-byte signed offset.
- **Description**: Jumps if `isTruthy(condition)` is false.

### OP_FOR_PREP
- **Stack**: `..., iterable -> ..., iterable, length, index(0)`
- **Description**: Prepares a `FOR` loop by pushing the iterable's length and an initial index onto the stack. Works on `List` and `String`.

### OP_FOR_ITER
- **Stack**: `..., iterable, length, index -> ..., iterable, length, index, item` (if continuing)
- **Operand**: 2-byte signed offset (to exit).
- **Description**: Checks if `index < length`. If so, pushes the next item and increments the index. If not, jumps to exit.

### OP_BREAK / OP_CONTINUE
- **Description**: Internal markers returned to the VM loop to handle loop control flow. The compiler emits `JUMP` instructions targeting the loop's break/continue patch points.

## Functions & Execution

### OP_CALL
- **Stack**: `..., callee, arg1, ..., argN -> ..., result`
- **Operand**: 1-byte argument count `N`.
- **Description**: Invokes an Arc function or Native function. Pushes a new `CallFrame`, sets `localsBase`, and starts executing the callee's chunk. If the callee is `OBJ_NATIVE_FUNCTION`, invokes the C function pointer directly.

### OP_RETURN
- **Stack**: `..., result -> (caller stack)`
- **Description**: Returns from the current function frame with the top stack value. Pops the call frame, restores caller state. `OBJ_RETURN` is the internal sentinel.

### OP_PROPERTY_ACCESS
- **Stack**: `..., instance -> ..., value`
- **Operand**: 3-byte index to interned String (property name).
- **Description**: Accesses a property value from an `Instance`. Raises `TypeError` if target is not an `Instance`. Raises `NameError` if the instance has no such property.

### OP_PROPERTY_SET
- **Stack**: `..., instance, value -> ..., value`
- **Operand**: 3-byte index to interned String (property name).
- **Description**: Sets a property value on an `Instance` via `setTableLocal`.

## Collections & Indexing

### OP_BUILD_LIST
- **Stack**: `..., item1, ..., itemN -> ..., [list]`
- **Operand**: 1-byte count `N`.
- **Description**: Creates a new `List` containing the top `N` stack items (in order). Items are deep-copied.

### OP_INDEX_GET
- **Stack**: `..., target, index -> ..., value`
- **Description**: Retrieves an element from a `String` or `List` at the given index. Raises `TypeError` for non-indexable targets. Raises `ValueError` for out-of-bounds.

### OP_INDEX_SET
- **Stack**: `..., target, index, value -> ..., 1`
- **Description**: Sets an element in a `String` or `List`. Pushes `1` on success.

### OP_STORE_INDEX
- **Stack**: `..., index, value -> ..., 1`
- **Operand**: 3-byte index to interned String (target name).
- **Description**: Specialized assignment for list/string elements: `target[index] = value`. Pushes `1` on success.

## Exceptions

### OP_TRY_PUSH
- **Operand**: 2-byte signed offset (to catch block).
- **Description**: Pushes the catch block address onto the VM's `tryStack`. On any subsequent error, `HANDLE_ERROR` pops the try-stack and resumes at the catch offset.

### OP_TRY_POP
- **Description**: Removes the top entry from the `tryStack` — emitted at the end of a `try` block.

## Modules

### OP_IMPORT
- **Operand**: 3-byte index to constant String (path).
- **Description**: Loads and executes an external `.arc` file or native module. Native modules register their functions into the global symbol table via `NativeModuleInit`.

## Termination

### OP_HALT
- **Description**: Stops the VM execution loop. Emitted at the end of the top-level chunk.
