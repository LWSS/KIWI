Adapted from cod2rad, Credits to https://github.com/7894752

# CoD4Rad reconstruction

This directory contains the standalone CoD4 radiosity compiler reconstruction.
It began as a source-accurate x64 port of CoD2Rad and now reads and writes the
CoD4 `IBSP` v22 tagged-chunk format.

The normal path is intentionally conservative:

- the native triangle tracer is the default;
- the Embree 4 tracer is available only with the hidden `-Embree` extension;
- experimental AO, adaptive-lightmap, and UV-repack switches are hidden and
  disabled unless explicitly requested;
- unknown and currently unused CoD4 BSP chunks are preserved byte-for-byte;
- lightmaps are emitted in CoD4's native 3 MiB-per-page layout (two RGBA
  coefficient maps and one 1024x1024 scalar map).

The radiosity transport core remains the recovered CoD2 four-band solver.  Its
final samples are fitted into CoD4's two-coefficient/dominant-direction
encoding.  This is the principal accepted approximation: output is valid and
visually useful, but lit texels are not expected to hash-match retail CoD4Rad.
x87/CRT floating-point drift is likewise not treated as a blocker.

## Build

Requirements:

- Visual Studio 2022 x64 C/C++ tools
- Embree 4 x64 Windows SDK only when building the optional Embree tracer

Generate the normal Win32 KIWI solution, then build the visible
`KIWI-cod4rad` project from Visual Studio or the command line:

```bat
scripts\mksln.bat Release
cmake --build build --config Release --target KIWI-cod4rad
```

The project shown in the main solution is a utility target: Visual Studio
solutions cannot mix generator architectures, so it transparently configures
`build-cod4rad` as x64 and invokes the real compiler target there.  A direct
x64-only solution can still be generated with `scripts\mksln.bat Release x64`.
It participates in Build Solution and writes `KIWI-cod4rad.exe` beside
`KIWI-mp.exe`, `KIWI-cod4map.exe`, and the other binaries under
`bin\<configuration>`.

To include Embree, configure the x64 tree explicitly:

```bat
cmake -S . -B build-cod4rad -G "Visual Studio 17 2022" -A x64 ^
  -DKIWI_COD4RAD_WITH_EMBREE=ON ^
  -DCOD4RAD_EMBREE_ROOT=C:\path\to\embree-4.4.1.x64.windows
cmake --build build-cod4rad --config Release --target KIWI-cod4rad
```

The CMake build copies the required Embree/TBB DLLs beside the executable.
Without that option, the compiler has no Embree runtime dependency and uses
the recovered native tracer.  The legacy standalone batch build remains
available and always includes Embree:

```bat
set EMBREEDIR=C:\path\to\embree-4.4.1.x64.windows
build_source.bat
```

The batch build writes `bin\cod2rad64_our.exe`; the CMake target is named
`KIWI-cod4rad` and follows the repository's normal configuration output layout.

Run the compiler from a CoD4 game root, for example:

```bat
bin\Release\KIWI-cod4rad.exe -Platform pc maps\your_map
```

The retail-compatible command-line surface contains 22 advertised options;
run without a map argument to print usage.

## Validation

The differential and BSP-inspection utilities live in
`scripts\cod4rad\tools`.  The current smoke set covers an unlit sealed BSP, a
lit BSP, a geometry-heavy lit BSP, single-bounce and converged multi-bounce
paths, and AddressSanitizer builds.
