# Dynamic Library Loading & Lifetime Testbed: The Clang vs. GCC Segfault Mystery

This repository isolates and investigates a fascinating discrepancy in modern C++ dynamic loading on Linux: **Why a specific dynamic plugin lifecycle bug causes an immediate Segmentation Fault when compiled with Clang, but silently "survives" when compiled with GCC.**

## The Mystery

In plugin architectures (such as older iterations of ROS 2 / `rcutils`), a common lifetime mismatch occurs: the host framework unloads a shared library via `dlclose()` while `std::weak_ptr` control blocks or object trackers are still alive.

When compiled with **Clang**, clearing these remaining trackers causes an immediate **Segmentation Fault**. When compiled with **GCC**, the program mysteriously survives. This project demonstrates how compiler-specific symbol bindings mask critical memory bugs and evaluates `RTLD_NODELETE` as a runtime workaround.

## Core Mechanics Discovered

1. **The GCC Illusion (`STB_GNU_UNIQUE`):** By default, GCC emits unique symbols (`STB_GNU_UNIQUE`) for inline statics and polymorphic type data. The GNU dynamic linker (`ld.so`) interprets this by quietly enforcing an implicit `RTLD_NODELETE` on the library to guarantee process-wide symbol uniqueness. This pins the memory permanently, bypassing your explicit `dlclose()` and masking the bug.
2. **The Clang Reality:** Clang handles inline vtable and template emission cleanly, adhering strictly to standard OS reference counting. This allows `dlclose()` to successfully unmap the plugin's code segment. Consequently, when `weaks.clear()` attempts to clean up the control block, it reaches across unmapped memory and crashes instantly.
3. **The `RTLD_NODELETE` Fix:** Passing the `RTLD_NODELETE` flag to `dlopen()` forces the dynamic linker to pin the library in memory permanently. While this stops the Segmentation Fault across all compilers, it introduces side effects like the inability to refresh or reinitialize internal library states.

## Compilation

```bash
# Compile Host Test Framework (Using GCC)
g++ -g -O0 -o main main.cpp -ldl

# 1. The "Invincible" Plugin (GCC defaults to pinning memory via unique symbols)
g++ -g -O0 -shared -fPIC -o plugin_gcc.so plugin.cpp

# 2. The "Crashing" Plugin (Clang cleanly unmaps, exposing the lifetime bug)
clang++ -g -O0 -shared -fPIC -Wno-return-type-c-linkage -o plugin_clang.so plugin.cpp

# 3. The Controlled Plugin (GCC with unique symbols stripped to force clean unmapping)
g++ -g -O0 -shared -fPIC -fno-gnu-unique -o plugin_gcc_nounique.so plugin.cpp
```

## Running the Tests

### Test 1: The GCC Mask (Implicit `NODELETE`)
```bash
./main ./plugin_gcc.so
```
* **Observation:** Post-cleanup mappings stay at **5** because `dlclose()` was ignored by the OS due to `STB_GNU_UNIQUE`. The program **survives** without a fault, hiding the architectural flaw.

### Test 2: The Clang Reality Check (The Segfault)
```bash
./main ./plugin_clang.so
```
* **Observation:** Clang allows the reference count to drop cleanly. The OS unmaps the code segment (`mappings after dlclose: 0`). When `weaks.clear()` tries to access the control block/vtable context, the application crashes with an immediate **Segmentation Fault**.

### Test 3: The Runtime Workaround Toggle
```bash
./main ./plugin_clang.so --nodelete
# Or using the unique-stripped GCC plugin:
./main ./plugin_gcc_nounique.so --nodelete
```
* **Observation:** Passing `--nodelete` instructs `dlopen()` to include the `RTLD_NODELETE` flag at runtime. The memory mappings stay resident after `dlclose()`. When `weaks.clear()` runs, the library's destructor logic is safely resolved from RAM.
* **Result:** The application **survives with zero faults**, demonstrating exactly how the `RTLD_NODELETE` flag bypasses hardware unmapping to hide lifecycle crashes.

## Behavior Matrix

| Plugin Binary / Flag Mode | Destructor Memory Kept Alive By... | Actual `dlclose` Unmap Occurs? | Safe From Segfault? |
| :--- | :--- | :--- | :--- |
| `./plugin_gcc.so` | Linker-enforced `STB_GNU_UNIQUE` (Implicit Hack) | ❌ No |  Yes (False Sense of Security) |
| `./plugin_clang.so` | None (Exposes raw pointer lifecycle bug) |  Yes | ❌ **No (Segmentation Fault)** |
| `./plugin_clang.so --nodelete` | Explicit `RTLD_NODELETE` (Runtime Workaround) | ❌ No |  Yes (Workaround Active) |
| `./plugin_gcc_nounique.so` | None (Unique bindings stripped) |  Yes | ❌ **No (Segmentation Fault)** |
| `./plugin_gcc_nounique.so --nodelete` | Explicit `RTLD_NODELETE` (Runtime Workaround) | ❌ No |  Yes (Workaround Active) |
