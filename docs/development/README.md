# Development Documentation

## Getting Started

-   **[VSCode Setup](./vscode-setup.md)** - IDE configuration, extensions, and auto-deploy
-   **[Shader Workflow](./shader-workflow.md)** - Fast shader iteration and deployment
-   **[Upstream Sync](./upstream-sync.md)** - How Open Shaders merges with upstream community-shaders
-   **[Repository Architecture](./architecture.md)** - Codebase layout, global systems, and multi-runtime targeting
-   **[Release Process](./release-process.md)** - Branch model, semantic version bumps, release stages, and manual packaging

## Quick Links

### Common Tasks

-   **Fast shader deployment:** `cmake --build build/ALL --target COPY_SHADERS`
-   **Full build with deployment:** `.\BuildRelease.bat ALL-WITH-AUTO-DEPLOYMENT`
-   **Run tests:** `cmake --build build/ALL --target run_shader_tests`
-   **Create a worktree with submodules + local preset:** `pwsh ./tools/new-worktree.ps1 -Name my-branch`
-   **Install optional git alias:** `pwsh ./tools/install-worktree-alias.ps1`

### Build Presets and Meanings

-   `ALL` - Universal binary supporting SE/AE/VR runtime detection (default, standard build without auto-deployment)
-   `Dev` / `Dev-Fast` - Fast iteration presets (recommended for development)
-   `ALL-VS2022`, `ALL-DEBUG`, `Debug`, `PR`, `Linux-ClangCL` - see `CMakePresets.json` for what each configures.
-   `ALL-WITH-AUTO-DEPLOYMENT` - **not tracked** — copy `CMakeUserPresets.json.template` to `CMakeUserPresets.json` (gitignored, per-clone) to define it. Extends `ALL` with `AUTO_PLUGIN_DEPLOYMENT=ON` to deploy built plugins and assets directly to your local SE/VR `Data` directories; the preferred preset for a local test-deploy once configured.

`CMakePresets.json` lists every tracked preset. A local `CMakeUserPresets.json` can define more (check for one before assuming a preset name doesn't exist — many devs keep deploy-enabled variants there that won't show up in a `git grep`).

### Build and Development Configuration

To customize builds, set these CMake options (cache variables):

-   `AUTO_PLUGIN_DEPLOYMENT` (default: `OFF`) - Auto-copy build output to `CommunityShadersOutputDir`.
-   `ZIP_TO_DIST` (default: `ON`) - Creates individual feature packages as 7z files in `/dist`.
-   `AIO_ZIP_TO_DIST` (default: `ON`) - Creates all-in-one distribution package as 7z in `/dist`.
-   `TRACY_SUPPORT` (default: `OFF`) - Enables Tracy profiler integration for performance analysis.

#### Auto-Deployment Configuration

Set the `CommunityShadersOutputDir` environment variable to a semicolon-separated list of target Skyrim Data directories:

```
CommunityShadersOutputDir=F:/MySkyrimModpack/mods/CommunityShaders;F:/SteamLibrary/steamapps/common/SkyrimVR/Data;F:/SteamLibrary/steamapps/common/Skyrim Special Edition/Data
```

## Worktrees

Use `tools/new-worktree.ps1` when creating a new worktree for development. The script:

-   Creates the worktree under a sibling `<repo>.worktrees/` directory by default
-   Reuses an existing local branch or creates a new one from `HEAD`
-   Runs `git submodule update --init --recursive` in the new worktree
-   Copies `CMakeUserPresets.json` from the main checkout if it exists there
-   Does not overwrite an existing `CMakeUserPresets.json` unless `-ForcePresetCopy` is passed

Examples:

-   `pwsh ./tools/new-worktree.ps1 -Name reproj_fixes`
-   `pwsh ./tools/new-worktree.ps1 -Name vr-debug -StartPoint dev`
-   `pwsh ./tools/new-worktree.ps1 -Name clean-build -NoSubmodules`

If you want a Git-native command, install the optional repo-local alias:

-   `pwsh ./tools/install-worktree-alias.ps1`
-   Then use `git new-worktree reproj_fixes`

The alias is installed into local Git config by default, so it does not affect other users unless they opt in.

## Build Targets

| Target             | Builds DLL | Runs Tests | Copies Shaders | Use Case               |
| ------------------ | ---------- | ---------- | -------------- | ---------------------- |
| `COPY_SHADERS`     | ❌         | ❌         | ✅             | Fast shader iteration  |
| `DEPLOY_ALL`       | ✅         | ✅         | ✅             | Full deployment (auto) |
| `prepare_shaders`  | ❌         | ✅         | ✅ (AIO only)  | CI shader validation   |
| `run_shader_tests` | ❌         | ✅         | ❌             | Test shaders only      |

## Contributing

When adding new features or documentation, please keep development docs organized under `docs/development/`.
