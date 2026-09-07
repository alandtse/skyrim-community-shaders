# AGENTS.md

This file provides guidance to coding agents when working with code in this repository.
Claude Code loads it via the `@../AGENTS.md` import in `.claude/CLAUDE.md`.

## Quick Checklist (Most Violated Rules)

-   **PR Title & Commits:** Use Conventional Commits (`type(scope): description`), title <= 50 chars, body wrapped to 72 chars/line. Target branch must be `dev` (never PR directly to `main`).
-   **PR Title Correction:** If a PR title becomes stale before merging, fix it with: `gh pr edit <num> --title "..."` (PR title becomes the release commit message).
-   **Comments:** 0 is the expected line count for most changes — well-named code states what it does, so leave it out if it doesn't add anything. Only write one when the WHY is genuinely non-obvious, and cap it at 2 lines (3-4 only as a rare absolute ceiling for a second clause, never a budget to spend). Describe present code only (never absent/removed code, except for regression-risk warnings). No mid-function tutorials.
-   **Comment Invariants:** The exception is narrow: a fact that, if a future editor doesn't know it, they will silently reintroduce a bug or corrupt state — not why a design was chosen, what alternative was rejected, or how a calculation works (those go in the commit/PR body). Before writing one, name the concrete bug a future edit would cause without it; if you can't, cut it. Never add a "see commit/PR" pointer or name a one-off incident/tool (e.g. "the RenderDoc CTD"). State the invariant and stop.
-   **Minimal Churn:** Do not reformat unrelated code or rename adjacent variables outside the PR scope.
-   **DRY Review:** Check new code against existing shared utilities codebase-wide (e.g. `SetResourceName`, `GetGameSettingValue`, cached `globals::game::*` pointers over raw `GetSingleton()`, serialize/format/filesystem helpers).
-   **DirectX Naming:** Name every D3D11 resource using `Util::SetResourceName`. Canonical implementation is in `Utils/D3D.cpp`; never duplicate the GUID or re-implement inline.
-   **Perf Instrumentation:** Use `CS_GPU_PASS("Feature::Pass")` (RAII `ScopedGpuPass`, `src/GpuPass.h`) at every render-pass entry point, new or ported — see Performance & Profiling Rules for what it wires up. Never hand-roll `state->BeginPerfEvent`/`EndPerfEvent` pairs or raw `TracyD3D11Zone` at a pass entry. **Code transplanted from another fork is a common violation source** — swap raw annotations for `CS_GPU_PASS` during the port, don't carry them over.
-   **VR Maintenance:** Keep VR divergence to the absolute minimum necessary. Resolve merge conflicts in favor of keeping VR.
-   **Devbench Verification:** For a runtime-affecting change (UI, features, shader cache), verify it in-game via devbench (SE and VR) before calling it done — see `docs/development/release-validation.md`. A new feature or settings surface should expose a devbench action alongside it in the same PR, not as a follow-up, so it stays testable without manual UI navigation. Adding or changing a devbench-exposed tool/action means updating that tool's own self-documented `RegisterTool` description/`inputSchema` in the same PR — it drifting out of sync with the handler is what `GET /api/tools` and any agent/scenario reading it will trust.
-   **Git Safety:** Never force-push/rebase shared branches (`main`, `dev`, `hotfix/*`). Never manually create `v*` tags, hand-modify `CMakeLists.txt` version, or run release workflow on `hotfix/X.Y.x` for the current line.
-   **Upstream Sync:** Merge, never cherry-pick. Land sync PRs as merge commits, never squash. Verify ancestry after merging.

---

## Fork Identity & Logo Policy

-   **Keep as `CommunityShaders` (Do NOT rename C++ runtime identity):** Keep CMake `PROJECT_NAME`, DLL name, `SKSE/Plugins/CommunityShaders/` directory, `CommunityShaders.log`, ImGui window ID after `###`, asset paths under `package/Interface/CommunityShaders/`, and HLSL include paths.
-   **Use "Open Shaders" for public identity:** Use in in-game menu titles, READMEs/instructions, Nexus filenames, GitHub release names, and Welcome/FAQ/About text.
-   **Link Upstream Explicitly:** Link upstream references to `community-shaders/skyrim-community-shaders` (Nexus 86492). Do not link dead `doodlum` paths. Open Shaders' own Nexus is 180419.
-   **AIO Bundling Semantics:**
    -   The Nexus upload workflow ships **ONLY** the AIO archive; there is no per-feature matrix distribution.
    -   The per-feature matrix upload is gated on `autoupload = true` (in the feature's `.ini` `[Info]` section).
    -   Use `aio = true` (with `autoupload = false`) to bundle a third-party feature redistributed with permission without uploading it standalone.
    -   A runtime-core feature (`IsCore()` returns `true`) must **also** carry a `CORE` marker file, or its shaders are excluded from the AIO bundle and it ships broken (e.g. shared includes).
    -   Partition logic is handled by `feature_in_aio` in `CMakeLists.txt`. For local development, configure `AIO_INCLUDE_NON_AUTOUPLOAD=ON` to include everything.
-   **No Logo:** `cs-logo.png` is intentionally absent (non-GPL). Do not "fix" this or restore upstream assets. Logo draws are null-safe fallback (retries colored fallback in `IconLoader.cpp` and gates draw on null check in `Menu.cpp`, `MenuHeaderRenderer`, and `HomePageRenderer`).
    -   _Exception to comment rules:_ A regression-risk warning naming removed code so a future maintainer doesn't restore it (e.g. "do not restore cs-logo.png") is load-bearing and stays.

---

## Comment & Documentation Standards

-   **Doxygen:** Use Doxygen-style comments for all public declarations and API methods (especially graphics-related functions).
-   **Present Code Only:** Comments must describe the present file's code, never absent or deleted code — except a regression-risk warning naming removed code so a future maintainer doesn't restore it (see Fork Identity & Logo Policy's `cs-logo.png` note for the canonical example of this exception).
-   **Length & default:** See the Quick Checklist's Comments/Comment Invariants entries — 0 lines by default, 2 as the cap when one is genuinely warranted, and the narrow invariant test before reaching for the exception.

---

## Code Quality & Architecture Standards

-   **No Placeholders:** Never ship TODO, FIXME, or incomplete implementations unless explicitly requested for planning.
-   **Minimal Churn:** PRs must touch only the lines required for the change. Do not reformat unrelated code, clean up surrounding structures, or rename adjacent variables.
-   **Descriptive Naming:** Use domain-specific names that clearly indicate graphics/rendering purpose (e.g. `screenSpaceAmbientOcclusion` not `ssao`, `UpdateShadowCascades()` not `UpdateSC()`).
-   **Single Responsibility:** Each feature class must handle exactly one graphics technique. Break up C++ functions longer than ~200 lines into focused helper methods.
-   **Centralize Constants:** Extract magic numbers and UI theme settings to named constants in appropriate classes (e.g., `ThemeManager::Constants`).
-   **DRY Codebase-Wide:** Do not reinvent existing functionality. Check your changes against the codebase and use shared utility libraries in `src/Utils/` (e.g., `Serialize.h` for JSON, `Format.h` for strings, `FileSystem.h` for paths, `UI.h` for ImGui) and `bshoshany-thread-pool` for parallel operations. Always reuse:
    -   `Util::SetResourceName` for resource naming.
    -   `Util::GetGameSettingValue` for reading game settings.
    -   The cached `globals::game::*` pointers (`Globals.h`) over calling the equivalent `RE::*::GetSingleton()` directly — e.g. `globals::game::player` instead of `RE::PlayerCharacter::GetSingleton()`, `globals::game::isVR` instead of `REL::Module::IsVR()`.
-   **New shared HLSL concerns get their own file (Include What You Use):** When adding a genuinely new fork-specific shared concern to `package/Shaders/Common/`, give it its own new `.hlsli` sibling rather than appending to an existing shared header (matches the existing convention: `ColorSpaces.hlsli`, `FoveatedMask.hlsli`, `BlurDither.hlsli`, `DirectionalShadow.hlsli` are all standalone). A broad `#include`d header that mixes unrelated concerns forces every consumer's CI shader-validation to re-run on any edit to any concern it bundles, and a header shared with upstream additionally risks a merge conflict on every future sync. This does not mandate retroactively splitting existing shared headers — only that new additions default to a new file, not growing an existing one.
-   **ImGui Integration:**
    -   Pair `ImGui::BeginTable()` with `ImGui::EndTable()` (orphaned `TableNextColumn()` calls cause layout bugs and crashes).
    -   Use the RAII pattern for ImGui style changes; avoid manual save/restore states.
    -   Use central Theme constants for UI spacing and padding instead of hardcoded values.
    -   Use callbacks to access private methods from modular UI components rather than making methods public. Keep UI state managed centrally in the `Menu` class.
-   **Restart-Gated Config Fields:** Use `Util::Settings::BootSnapshot` + `kRestartFields` metadata to diff boot-latched vs selected settings values. This drives `Util::Text::RestartNeeded` banners and MCP/menu introspection (Upscaling acts as the canary).

---

## DirectX & D3D11 Resource Naming

-   **Debuggability:** Every D3D11 resource must be named for RenderDoc debuggability using `Util::SetResourceName(ptr, "Feature::ResourceDescription")` after raw `device->Create*` calls.
-   **Wrappers:** For wrapper types (`Texture2D`, `Buffer`, `ConstantBuffer` in `Buffer.h`), pass the name to the constructor. Views are named automatically.
-   **Conventions:** Use `"Feature::Name"` for the resource and `"Feature::Name SRV"` / `"Feature::Name UAV"` for views.
-   **Implementation:** The canonical implementation lives in `Util::SetResourceName` (`Utils/D3D.cpp`). Never duplicate the GUID or re-implement this call inline.

---

## VR & Cross-Platform Policy

-   **Non-VR is Primary:** Treat non-VR (flatrim: SE/AE) as the primary code path. VR is a minimal divergence kept to the absolute minimum necessary.
-   **Divergence Rules:**
    -   Diverge only where behavior is genuinely VR-specific (stereo projection, VR-only resources, different engine data paths).
    -   _C++:_ Use runtime checks. Universal binaries use runtime detection. Prefer the cached `globals::game::isVR` over calling `REL::Module::IsVR()` directly (reserve direct calls for early-init paths).
    -   _HLSL:_ Diverge via `#if defined(VR)` compiler checks per shader permutation.
    -   Keep VR branches small and localized. Avoid VR branching to work around side-effects; unify paths where possible.
-   _For detailed multi-runtime member offset and virtual relocations, see [Repository Architecture](docs/development/architecture.md)._

---

## Performance & Profiling Rules

-   **Pass Instrumentation:** Wrap every new render pass entry point with the `CS_GPU_PASS("Feature::Pass")` macro (RAII `ScopedGpuPass`) — wires up the internal profiler, Tracy CPU/GPU zones, and RenderDoc/PIX annotation in one call. Do not use direct `TracyD3D11Zone` or `State::BeginPerfEvent` at pass entry sites.
-   **Sub-Dispatch Annotations:** Raw/legacy zones (such as `TracyD3D11Zone`) are only appropriate for sub-dispatches within a pass where profiler timer granularity is not required.
-   **Ported Code:** When transplanting a pass from another fork, its own raw `BeginPerfEvent`/`EndPerfEvent` or `TracyD3D11Zone` calls do not carry the fork's meaning here — replace them with `CS_GPU_PASS` as part of the port, not a follow-up. Give the two dispatch paths of a runtime-vs-fallback feature distinct pass names (e.g. `Feature::RuntimeDispatch` vs `Feature::HostDispatch`) so they can be A/B'd against each other via Tracy.
-   **Justifying Speedups:** PRs claiming performance speedups (or `perf:` commits) must state a measured number in their description:
    -   Justify at the PR level, not per commit.
    -   Normalize cost/savings as a percentage of the target frame budget (VR 90fps ≈ 11.1ms; flatrim 60fps ≈ 16.7ms). Do not quote raw wall-clock deltas.
    -   Measure GPU per-pass cost using Tracy (compile preset with `-DTRACY_SUPPORT=ON`, connect server to port 8086).
    -   A/B test a feature: toggle it (via in-game menu or devbench `openshaders.feature`), capture the same scene/camera both ways, and diff the zone times holding scene, resolution, and upscaler fixed.
    -   Micro-optimizations that cannot be isolated/benchmarked must be labeled `refactor:`, not `perf:`.

---

## Error Handling & Memory Management

-   **Graceful Degradation:** Features must disable cleanly on shader compilation failures or DirectX errors, falling back to a working render path rather than crashing or corrupting state.

---

## Git & Release Invariants

-   **PR Branch Targets:** All PRs must target the `dev` branch. Never PR directly to `main`.
-   **Approval:** Never push to shared branches without explicit OK.
-   **Commit Versioning Traps:**
    -   Mislabeling a build/CI/test change as `fix:` burns a patch release on a non-user-visible change. Use `build:`, `ci:`, or `test:`.
    -   Mislabeling a refactor as `feat:` forces a minor bump.
    -   A performance improvement on internal code is `refactor:`, not `perf:`.
    -   `chore:` is a catch-all; prefer the specific type when one fits.
-   **Branch Lineage:** `main` must remain an ancestor of `dev`. Following a hotfix staging promotion, `dev` is auto-rebase-reconciled (force-pushed by the workflow) to absorb the `chore(release):` commit.
-   **Prohibitions (Never do these without explicit user direction):**
    -   Do not force-push or rebase `main`, `dev`, or any `hotfix/*` branch (except release workflow auto-reconciliations).
    -   Do not manually create tags matching `v*` (semantic-release owns these).
    -   Do not manually bump `CMakeLists.txt`'s `VERSION` field.
    -   Do not PR a feature branch directly into `main`.
    -   Do not run `Release: Semantic Version` on `hotfix/X.Y.x` for the current line (fails with "cannot be published as it is out of range"). Use `ff_target` into `main` instead.
-   **Upstream Sync Rules:**
    -   Pull changes from upstream `community-shaders/skyrim-community-shaders` by merge (`git merge <upstream-ref>`), never cherry-pick.
    -   Land sync PRs as merge commits, never squash.
    -   Resolve conflicts in favor of keeping VR. Revert upstream VR removals.
    -   Verify ancestry after landing: `git merge-base --is-ancestor <upstream-sha> HEAD` must pass.
    -   **Upstream never tests VR, so a clean (non-conflicting) sync can still silently break it.** After landing, scan the sync's own diff for new runtime-version gating (`REL::Module::IsAE()`/`IsAtLeast(...)`, new "legacy compatibility" layers) and ask explicitly for each one: does VR need this too? A 2-arg `RelocationID(se, ae)` is the correct, preferred pattern for a function VR can share — once its address is registered in `skyrim_vr_address_library`'s `database.csv`, VR resolves it automatically with no C++ change needed, so the constructor itself is not a red flag. The actual risk is a runtime/version gate (e.g. a flat-runtime-only helper) silently keeping VR from ever reaching an otherwise-correct call, or an id that's genuinely missing its VR row in `database.csv` (a hard fatal plugin-load abort on VR, not a graceful skip). Check both — is VR excluded by a gate that shouldn't exclude it, and does the id actually have a VR address registered (see PR #586 for a case where it did, twice, in the same sync).
-   _For conventional commit mappings, staging/RC workflows, release stages (Alpha/Beta ini flags), and manual packaging targets, see [Release Process](docs/development/release-process.md) and [Upstream Sync Guide](docs/development/upstream-sync.md)._

---

## Security & Input Validation

-   **Shaders:** Validate shader parameters and buffer sizes to prevent GPU driver crashes.

---

## Environment & Build Reference Summary

-   **WSL/Linux Note:** For Windows SDK compilation, run via PowerShell:
    `powershell.exe -Command "./BuildRelease.bat [PRESET_NAME]"`
-   **Linux/macOS-host cross-compile (build-only, no VS/Windows tooling):** `cmake --preset Linux-ClangCL && cmake --build --preset Linux-ClangCL`. Proves the toolchain compiles clean and produces a working `CommunityShaders.dll`; does not produce a runnable-in-game package. See [Linux/macOS Cross-Compile](docs/development/linux-macos-cross-compile.md) for setup, overlay ports, and validation boundary.
-   **Primary Build Command:** `./BuildRelease.bat [PRESET_NAME]`
    -   _Tracked presets (`CMakePresets.json`):_ `ALL` (default, universal SE/AE/VR binary), `Dev`, `Dev-Fast`, `ALL-VS2022`, `ALL-DEBUG`, `Debug`, `PR`, `Linux-ClangCL`.
    -   _Local presets:_ many devs keep a gitignored `CMakeUserPresets.json` (start from `CMakeUserPresets.json.template`) defining deploy-enabled variants such as `ALL-WITH-AUTO-DEPLOYMENT` — sets `AUTO_PLUGIN_DEPLOYMENT=ON` and deploys straight to the local SE/VR `Data` dirs via `CommunityShadersOutputDir`; the preferred preset for a local test-deploy. Check for this file (`Test-Path CMakeUserPresets.json`) before assuming a preset name doesn't exist — it's per-clone and not committed, so its presets won't show up in `CMakePresets.json` or a `git grep`.
-   **clangd setup:** Generate compilation database after configuring `ALL`:
    `pwsh tools/gen-clangd-db.ps1`
-   **Shader Refactor Verification:**
    -   _Identical DXBC:_ Provable no-op (`tools/verify-shader-refactor.ps1`).
    -   _Legitimate op-reordering:_ Use runtime A/B frame diffing (`tools/taa-renderdoc-ab.py`).
-   **Feature Workflow:** Start from `template/`, implement the C++ class inheriting from `Feature` (`DrawSettings()`, `LoadSettings()`, `SaveSettings()`, and feature-specific rendering hooks), and register in appropriate source files and `globals::features`.
-   **Fast Iteration (avoid needless shader recompiles):** Match the build to what changed: `Dev-Fast` for C++-only, building just the `CommunityShaders` target under a `*-WITH-AUTO-DEPLOYMENT` preset for a DLL-only deploy (no shaders/tests touched), `COPY_SHADERS` for shader-only. Deploying the AIO `Shaders/` tree is content-based (`cmake/SyncShaderDeploy.cmake`), so an unchanged shader keeps its mtime across a branch switch instead of forcing an in-game recompile. For branch-swap A/B testing, use a separate build directory (or worktree) per branch rather than switching branches inside one build dir. See [Shader Development Workflow](docs/development/shader-workflow.md#fast-iteration-without-paying-a-full-shader-recompile) for the full mechanism and the two in-game toggles (Skip Unchanged Shaders, File Watcher).
-   _For detailed setup, options, and commands, see [VSCode Setup](docs/development/vscode-setup.md), [Development README](docs/development/README.md), [Shader Development Workflow](docs/development/shader-workflow.md), [In-game A/B Testing](docs/development/shader-runtime-ab.md), and [Repository Architecture](docs/development/architecture.md)._

---

## Maintaining This File

-   **Update in the same PR, not a follow-up.** When a convention here changes, edit this file in the PR that changes it — don't leave documentation drift to a later docs-only pass.
-   **Audit every 3-6 months.** Scan `NEVER`/`ALWAYS`/`DO NOT`/ALL-CAPS bans for ones that now just restate a capable model's native taste or contradict current practice, and cut or soften them.
-   **Prefer a doc link over inline detail** once a section would exceed ~8 lines — point to `docs/development/` rather than duplicating it here.
