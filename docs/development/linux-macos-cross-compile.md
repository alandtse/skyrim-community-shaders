# Linux/macOS-Host Cross-Compile

Build-only path, no VS/Windows tooling required:

```bash
cmake --preset Linux-ClangCL && cmake --build --preset Linux-ClangCL
```

Cross-compiles to the same Windows PE/MSVC-ABI output as the native presets
via `clang-cl`+`lld-link` against an `xwin`-generated Windows SDK/CRT
sysroot. The toolchain and DirectXTK's Wine-based shader-compile workaround
both come from the `extern/CommonLibSSE-NG` submodule; see
`extern/CommonLibSSE-NG/examples/linux-cross-compile/README.md` for
one-time host setup (xwin, wine, llvm-mingw, vcpkg).

**Validation boundary:** proves the toolchain compiles clean and produces a
working `CommunityShaders.dll`/`.lib`/`.pdb`. It does not produce an
in-game-runnable package (no AIO packaging, no deployment), and
struct-layout/vtable mismatches between clang-cl and real MSVC would only
surface as an in-game crash, not a build failure.

## Overlay ports

Three vcpkg overlay ports beyond CommonLibSSE-NG's own example were needed:

-   **`cppwinrt`** — the official port requires real Windows SDK `.winmd`
    metadata that `xwin` doesn't stage. This codebase only uses
    `winrt::com_ptr` (base COM interop), so this port runs `cppwinrt.exe
-base` under Wine instead, which needs no SDK metadata.
-   **`detours`** — the official port's NMake-based build needs a real
    `nmake.exe` (unavailable in an `xwin` sysroot); this one compiles the
    same source-file set via a small vendored `CMakeLists.txt`.
-   **`directxtex`** — bundles its own BC6H-encode compute shaders,
    compiled the same way DirectXTK's (already-solved) shaders are, reusing
    the identical `fxc2`-under-Wine stand-in.

## FidelityFX_SC.exe under Wine (resolved)

`FidelityFX-SDK`'s bundled shader compiler initially appeared to have
deterministic HLSL parser bugs under Wine — e.g. treating `pass` as a
reserved keyword (colliding with its own use as a parameter name in
`ffx_cacao_callbacks_hlsl.h`), and rejecting some `switch`/`case`/
`globallycoherent` patterns. Reproducible with a fresh Wine prefix and
`WINEESYNC=0`/`WINEFSYNC=0`, ruling out flakiness.

The actual root cause: Wine ships its own partial reimplementation of
`d3dcompiler_47.dll`, silently used instead of a real Microsoft compiler
DLL unless `FidelityFX_SC.exe` is explicitly pointed at one via `-d3ddll=`.
Once wired to a real `d3dcompiler_47.dll` (downloaded checksummed, wrapped
in a small launcher script), every one of the above "parser bugs" resolved
at once. See [alandtse/FidelityFX-SDK-DX11#1](https://github.com/alandtse/FidelityFX-SDK-DX11/pull/1).

Frame interpolation/optical flow (`FFX_FI`/`FFX_OF`) are disabled via the
preset — confirmed unused anywhere in this codebase — as an unrelated,
worthwhile scope reduction.
