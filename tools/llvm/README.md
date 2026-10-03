# clang: aarch64 DragonFly target

`clang-18-dragonfly-aarch64.patch` teaches clang 18 the `aarch64-unknown-dragonfly`
target. Stock clang knows DragonFly only on x86_64. With an aarch64 triple it falls
back to a bare ELF target: no `__DragonFly__`, and the driver's x86 gcc80 paths. The
patch is about 50 lines, against `clang/` of LLVM 18.1.3. It is LLVM code, so it is
under the Apache-2.0 WITH LLVM-exception license and is meant for upstream.

What it changes:

- **`lib/Basic/Targets.cpp`:** aarch64 + DragonFly gets
  `DragonFlyBSDTargetInfo<AArch64leTargetInfo>`.
- **`lib/Basic/Targets/OSTargets.h`:**
  - `__tune_i386__` is defined only on x86.
  - aarch64 uses `.mcount`.
  - long double stays binary128, the aarch64 default.
- **`lib/Driver/ToolChains/DragonFly.{cpp,h}`:**
  - The gcc80 `-L` and `-rpath` are x86 only. aarch64 has no gcc: its libgcc is
    compiler-rt and its libgcc_eh is LLVM libunwind, both in `/usr/lib`.
  - On aarch64, `/usr/include` comes before clang's resource headers, so that
    DragonFly's `<stddef.h>` and friends win.
  - The default C++ library on aarch64 is libc++ (`<sysroot>/usr/include/c++/v1`).
    x86 keeps libstdc++.

Until a patched clang is installed, `bin/arm-world`'s `cc` shim does the same with
stock clang-18: `-D` predefines and `-nobuiltininc -idirafter`.

## Build and check without a full LLVM build

You only need `clangBasic` and `clangDriver`, linked against the host's
`libLLVM-18.so`. On Ubuntu: `llvm-18-dev`, `clang-18`, cmake, ninja.

1. Extract `clang-18.1.3.src` and `cmake-18.1.3.src` from the LLVM release, as
   `clang/` and `cmake/` side by side. Then apply the patch:
   ```
   cd clang && patch -p1 < clang-18-dragonfly-aarch64.patch
   ```
2. Configure:
   ```
   cmake -G Ninja -S clang -B build -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm \
       -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-18 \
       -DCMAKE_CXX_COMPILER=clang++-18 -DCLANG_INCLUDE_TESTS=OFF \
       -DCLANG_INCLUDE_DOCS=OFF -DLLVM_INCLUDE_TESTS=OFF \
       -DCLANG_ENABLE_STATIC_ANALYZER=OFF -DCLANG_ENABLE_ARCMT=OFF \
       -DLLVM_TABLEGEN_EXE=/usr/lib/llvm-18/bin/llvm-tblgen
   ```
3. Build the two libraries. This is 245 steps, about 45 min at `-j2`:
   ```
   ninja -C build -j2 clangBasic clangDriver
   ```
4. Build the test program:
   ```
   clang++-18 -std=c++17 -fno-rtti -Iclang/include -Ibuild/include \
       -Ibuild/tools/clang/include -I/usr/lib/llvm-18/include dftest.cpp \
       build/lib/libclangDriver.a build/lib/libclangBasic.a \
       -L/usr/lib/llvm-18/lib -lLLVM-18 -Wl,-rpath,/usr/lib/llvm-18/lib -o dftest
   ```
5. Run it:
   - `./dftest [TRIPLE]` prints the predefined macros.
   - `./dftest TRIPLE ARGS...` prints the driver jobs, like `-###`.

Checked 2026-10-03:

- **aarch64:**
  - `__DragonFly__`, `__unix__`, no `__tune_i386__`, 128-bit long double.
  - The link runs `/usr/libexec/ld-elf.so.2` with `-lc -lgcc -lgcc_pic` (static:
    `-lgcc -lgcc_eh`), with no gcc80.
  - C++ links `-lc++`, and the include order is `c++/v1`, `/usr/include`, then the
    resource dir.
- **x86_64 is unchanged:** `__tune_i386__`, gcc80 `-L` and `-rpath`, `-lstdc++`.
