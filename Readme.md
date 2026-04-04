# Cando Scripting Language

Cando is a powerful, C-style scripting language implemented in C11. It is designed to be a lightweight yet feature-rich language that compiles source code into bytecode for execution on a high-performance, stack-based virtual machine.

---

### ⚠️ AI-Generated Code Disclaimer
**Important:** While the architecture, language specifications, and overall design of Cando were conceptualized by a human, **100% of the source code was generated using Artificial Intelligence.** This project serves as a primary "stress test" for modern AI coding capabilities, utilizing tools such as **Ollama, Gemini, ChatGPT**, and predominantly **Claude AI**. The goal is to explore the current limits of AI in generating complex, interconnected software systems.

---

## 🚀 Quick Start

### Building the Project
```bash
make          # Build all test binaries and the cando executable
make cando    # Build only the cando executable
make test     # Build and run all tests
```

### Running a Script
```bash
./cando script.cdo          # Execute a script
./cando script.cdo --disasm # Execute and view disassembled bytecode
```

---

## 🏗️ Architecture Overview

Cando utilizes a modern end-to-end execution pipeline:
1.  **Lexer**: Converts raw `.cdo` text into a stream of tokens.
2.  **Parser/Compiler**: A recursive-descent and Pratt expression parser that emits bytecode directly into a `CandoChunk` without an intermediate AST.
3.  **VM**: A stack-based interpreter using **GCC computed gotos** for efficient instruction dispatch.

### The Two-Layer Value System
Cando implements a unique parallel value representation:
* **VM/Core Layer**: Uses `CandoValue`, a small tagged union for the stack and native functions. Objects are stored as `HandleIndex` (u32) to allow the GC to relocate them without breaking script values.
* **Object Layer**: Manages `CdoObject`, the heap-resident type for objects, arrays, and functions. It includes a hash table for key-value pairs and dense storage for arrays.

---

## 💎 Language Features

### Core Syntax
* **Variable Declarations**: Use `VAR` for mutable variables and `CONST` for immutable constants.
* **Block Scoping**: Variables are scoped to their enclosing `{ }` braces.
* **Comments**: Supports both `//` single-line and `/* */` multi-line comments.

### Data Types
| Type | Description |
| :--- | :--- |
| **Number** | IEEE 754 doubles (e.g., `42`, `3.14`) |
| **String** | Immutable heap strings; supports double-quotes, single-quotes (multi-line), and backticks (interpolation) |
| **Boolean** | `TRUE` and `FALSE` literals |
| **Array** | Ordered, numeric-indexed lists (e.g., `[1, 2, 3]`) |
| **Object** | Key-value stores with prototype-based inheritance |

### Advanced Control Flow
* **Multi-Comparison**: Test a value against multiple options in one statement: `IF x == 1, 2, 3 { ... }`.
* **Range Loops**: Easily iterate ranges with `FOR i OF 1 -> 5` (ascending) or `FOR i OF 5 <- 1` (descending).
* **Unpacking & Masks**: Functions can return multiple values. Use masks like `(~.~)` to select specific return values for comparison or assignment.

### Functional Programming
* **First-Class Functions**: Functions can be assigned to variables and passed as arguments.
* **Closures**: Supports full lexical scoping with captured upvalues.
* **Method Syntax**: Use the colon operator for method calls: `"hello":toUpper()`.

---

## 🛠️ Internal Specifications

* **VM Stack**: 2048 slots.
* **Call Stack**: 256 frames.
* **Upvalues**: Implemented via an intrusive linked list for open upvalues, closing onto the heap when a frame exits.
* **Error Handling**: Integrated `TRY / CATCH / FINALLY` system with stack unwinding.
* **Native API**: Easily extend Cando with C-based native functions via the `CandoNativeFn` signature.

---

## 📁 Repository Layout
* `source/core/`: Low-level primitives (values, strings, memory).
* `source/object/`: Object-layer implementation (arrays, classes).
* `source/parser/`: Lexer and Pratt parser.
* `source/vm/`: Bytecode interpreter and bridge layer.
* `source/lib/`: Built-in standard libraries.