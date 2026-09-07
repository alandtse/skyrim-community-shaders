# VR submit-stage upscaling design

Status: implemented and deployed to MO2 Personal. The user reports reduced
resolution with jagged output; reconstruction is not yet confirmed in-game.

## Scope and evidence

The implementation belongs to worktree `vr-submit-upscaling`, based on
`203cd4733`. The local `csx-wetterness` branch at `2a3e3bf91` is a behavioral
reference only. No CSX code is to be transplanted.

Instructions read: repository `AGENTS.md`, `.claude/CLAUDE.md`,
`AI-INSTRUCTIONS.md`, `.github/copilot-instructions.md`, the supplied personal
rules, and the CSX branch's root instructions. The explicit task restrictions
take precedence over older instructions requesting devbench or shader
validation. Do not modify any `AGENTS.md`.

The existing non-VR path and VR path with render-scale mode disabled retain
their current dispatch timing, resolution policy, post-processing, and
sharpening behavior. This change concerns the restart-gated
`renderAtUpscaleRes` VR path, using its existing preset and explicit-scale
settings. It introduces no live resolution relatching, presentation holds,
frame generation, or separate menu compositor.

## Findings from the two implementations

Open Shaders currently calls `PerformUpscaling()` from
`Upscaling::Main_PostProcessing::thunk`, before the engine post chain.
`PerfMode/ResolutionHook.cpp` reduces the engine's eye allocation size through
`BSOpenVR::GetRenderTargetSize`. PerfMode then owns a display-sized
`testTexture`, enlarged post/UI targets, depth-stencil substitutions,
tonemap/refraction interception, downscaling, menu background bridges, and
viewport/fade corrections. These mechanisms depend on one another; moving
the backend call without retiring those redirects would leave inconsistent
render-target sizes and stale output consumers.

CSX intercepts `IVRCompositor::Submit` and `WaitGetPoses` in
`Features/VR/InSceneOverlay.cpp`. Its `SubmitVRUpscaledFrame`,
`EncodeSubmitStageVRInputs`, and vendor-eye dispatch helpers illustrate the
need to interpret bounds, retain auxiliary inputs, separate eye histories,
and distinguish fallback presentation from successful vendor output.
Its broader load recovery, relatching, menu composition, telemetry, and
foveated orchestration are outside this design.

The existing Open Shaders backend primitives are sufficient starting points:

-   `Streamline::EvaluateDLSS` accepts an eye, resources, explicit input/output
    extents, and an output height, and returns success.
-   `FidelityFX::UpscaleRegion` accepts an eye, resources, explicit dimensions,
    motion-vector scales, and sharpness. It owns runtime-provider/host fallback
    and returns success.
-   `EncodeTexturesCS.hlsl` already splits stereo auxiliary inputs, encodes
    reactive/transparency masks and motion vectors, and has a `DEPTH_OUTPUT`
    permutation for typed depth.

The existing top-level `Upscale()` wrappers are unsuitable for submit-stage
dispatch: they select `kMAIN`, infer output ownership from PerfMode, and merge
per-eye results back into engine or PerfMode targets.

## Component and ownership

`Upscaling/VRSubmitUpscaling.h/.cpp` is owned by `Upscaling`. It is the sole
owner of the submit-stage resolution plan, captured frame validity, per-eye
resources, history-reset decisions, and hook activation state. SDK contexts
remain owned by Streamline and FidelityFX; their lifetime is coordinated
through the existing Upscaling resource lifecycle.

The resolution plan contains render and output dimensions per eye, selected
backend, effective quality mode, and whether explicit scale overrides the
preset. Build it before engine render targets are allocated. Reuse the
existing quality-ratio utility and DLSS render-range clamp. Reject nonfinite
settings and zero, oversized, or incompatible extents before installing the
size override. Preserve the existing even-dimension rule.

Install submission interception successfully before reducing engine target
sizes. A failed installation leaves the original allocation/dispatch path
active. Once reduced targets exist, a later failure cannot pretend that native
targets have been restored: keep the latched plan and submit the original
render-sized image, letting OpenVR present it normally.

Keep the public surface narrow:

-   Install before target creation and expose a read-only resolution plan.
-   Reset resources when engine resources are recreated.
-   Begin/capture a rendered frame before post-processing.
-   Prepare a replacement submission, returning success plus owned output and
    bounds, or return failure without modifying the caller's arguments.
-   Invalidate histories after skipped frames, dispatch failures, and changes
    in resource identity or backend.

## Frame flow

1. At the existing jitter point, use the latched render/output ratio for phase
   count. Engine dynamic-resolution scale stays at identity because its
   targets already have the render dimensions.
2. At the existing pre-post-processing hook, capture fresh per-eye depth,
   motion vectors, masks, jitter, and the camera/frame identity needed by the
   temporal backend. Do not run vendor upscaling or display-sized depth repair
   there on the submit-stage path.
3. Run engine post-processing and UI composition against the render-sized
   engine targets. No display-sized target substitutions or post-chain
   viewport expansion are active in this path.
4. Intercept the final OpenVR eye submission. Validate the actual submitted
   resource and bounds, then copy the final eye color into component-owned
   input storage. Use the same frame's captured auxiliary inputs.
5. Dispatch the existing backend with explicit per-eye extents. Publish an
   output handle only after successful dispatch. Apply requested DLSS
   sharpening to that owned output using the existing RCAS backend; FSR
   sharpness continues through its dispatch primitive.
6. Forward exactly one submission for each intercepted call, preserving eye,
   flags, color-space semantics, and the original compositor return value.
   Release no output resource while the compositor can still consume it.

Use compositor-cycle identity to deduplicate input capture, admit submission,
and advance temporal history. Desktop Present
increments `State::frameCount`, so it must not invalidate the inputs retained
for an OpenVR cycle. Camera constants and jitter belong to that capture;
submit does not compare them against mutable next-frame globals.
Do not equate an eye call with a new frame. A duplicate
must reuse a valid result or pass through; it must not advance history twice.
Do not wait for the second eye before forwarding the first.

## Resource, state, and fallback contracts

The supported contract is a single-mip, single-slice, single-sample D3D11
`R8G8B8A8_UNORM` side-by-side submission using `Submit_Default`, with
left/right rectangles `(0, 0, 0.5, 1)` and `(0.5, 0, 1, 1)` respectively.
Reversed horizontal or vertical bounds preserve their original orientation
when mapped onto the reconstructed per-eye texture.
Dimensions must exactly match the latched plan. Linear, Gamma and Auto color
spaces are supported; Auto is treated as gamma for the 8-bit input. Validate
resource type through COM, owning device, format, array/mip/sample counts,
finite bounds, rectangle alignment, eye mapping, and exact dimensions.
Unsupported arrays, alternate APIs, unusual flags, cropped layouts, and
foreign devices pass through unchanged. Never reinterpret an arbitrary
submission as the engine's stereo layout.

For a verified complete stereo source, prepare both eyes before publishing
either backend result so a vendor failure can fall back for the pair. Reuse
the pair only when cycle, source identity, and frame capture match. If a
later eye presents a different unsupported source, forward that source;
never suppress it or substitute an unrelated cached eye.

Final submitted color is after tone mapping. Backend HDR/LDR options and
resource formats must describe that color, not `kMAIN`'s pre-tonemap format.
`SubmitColorCS.hlsl` converts gamma inputs to linear FP16 eye textures and
converts the vendor output back for the original OpenVR color-space contract.
Both vendor paths receive linear floating-point color with HDR processing
enabled. Output alpha is opaque, as required by the supported ordinary scene
submission contract. This is a narrow extension to backend dispatch
parameters, not a global change to flat rendering options.

All allocations use RAII and existing resource-name utilities. Construct a
complete resource set before publishing it; allocation failure leaves no
half-ready set. Preserve the full D3D11 context state across submission work,
including compute bindings and any bindings disturbed by SDK dispatch.
`FullscreenPassScope` alone does not cover this boundary. The existing
Effects11 backup intentionally omits shader stages, so its coverage must not
be assumed sufficient for arbitrary compositor-entry state.

The first post-processing capture establishes the owning render thread;
startup resource allocation does not establish render-thread identity.
Keep GPU work on that thread. Reentrant or unexpected-thread
submissions pass through without touching immediate-context resources.
Resource reset, shader-cache invalidation, and backend reset invalidate all
capture/result stamps before the next dispatch. Reset temporal histories
after a gap or failure; do not resume accumulation against stale depth or
motion. Device removal disables replacement work until resource lifecycle
reinitialization. Report a failure transition once rather than logging each
eye each frame.

Missing/frozen world inputs and paused/menu presentation submit the original
image for that frame. Unsupported layouts, allocation/shader failures, vendor
failures, and rejected reconstructed submissions latch original-image
presentation until resource setup or shader-cache reset. A failed context or
device remains unsupported. Do not hold black frames, replay a prior scene, suppress
OpenVR submissions, or change engine allocations in these failure paths.
Jitter policy must account for a latched fallback to avoid ongoing jitter
without temporal reconstruction. Menu transitions require conservative
history resets because the final color contains UI absent from world depth.

## Integration map

| Area                    | Required change                                                                                                                                        |
| ----------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Upscaling               | Own the component; route enabled VR capture/submit lifecycle; preserve the existing branch for other paths.                                            |
| PerfMode                | Its header and five implementation files are removed; the new component owns the enabled path.                                                         |
| Hooks.cpp               | Install the plan before target allocation; reset the component after recreation; remove enabled-path enlarge windows and associated hook installation. |
| Globals.cpp             | Remove installation of the PerfMode fade Draw detour.                                                                                                  |
| Streamline / FidelityFX | Reuse per-eye dispatch; add only explicit reset/color-domain inputs needed at submission. Preserve existing callers' defaults.                         |
| State / VR API          | Redirect existing read-only scale and restart reporting to the same plan.                                                                              |
| Effects11               | Consume render-sized engine color before submit; remove reliance on PerfMode's private display output for this path.                                   |
| FoveatedRender          | Leave the current disabled-render-scale route unchanged; submit-stage scaling crops owned eye inputs and reuses stretch/blend operations.              |

Read-only resolution consumers use `vrSubmit`; no compatibility getters
pretend that the removed PerfMode display texture exists. Effects11 consumes
engine color before submission. The existing pre-post-processing foveated
route remains available when render scaling is disabled. Submit-stage scaling
has its own per-eye crop orchestration using the same region and
peripheral-quality controls.

## Initial C++ verification

The initial C++ build used an isolated `build/Dev-Fast` directory, the universal
SE/AE/VR definitions, and Effects11 enabled. Deployment, archives, and
`BUILD_SHADER_TESTS` are off. The tracked FidelityFX integration forces shader
generation as a dependency even of Ninja's PCH target; it is therefore not a
safe C++-only target in this checkout.

During delegated build preparation, Luna mistakenly invoked that PCH target.
It started FidelityFX shader generation and was interrupted. This was a
violation of the task's no-shader-compilation restriction. The resulting
files are confined to the ignored build directory. No shader validation or
shader tests were run, and those generated files are not validation evidence
for the new shader.

Subsequent compilation invokes MSVC directly using the exported translation
unit commands. It retains the forced `include/PCH.h` include, disables PCH
consumption, and omits Ninja's generated module-map argument. `/WX-` keeps
existing dependency and project warnings visible without treating them as
errors. No further build-target traversal or shader generation is permitted.

C++ compilation passed for all 16 affected and signature-dependent translation
units: `Upscaling`, `VRSubmitUpscaling`, `Streamline`, `FidelityFX`,
`FidelityFX/RuntimeUpscaler`, `RCAS`, `FoveatedRender`,
`FoveatedRender/{Params,Modes,Postprocess}`, `DX12SwapChain`,
`Effects11/EffectManager`, `Hooks`, `Globals`, `State`, and
`VRAPI/CSpluginapi`. MSVC produced object files for each. Existing warnings
were C4245 in CommonLib's `ContextHook.h`, C4099 in `Utils/UI.h`, and C4100
in existing `Feature` template parameters; no new warnings were reported.

The final lifecycle fixes in `VRSubmitUpscaling.cpp` were compiled again with
exit code 0. Its object is
`build/Dev-Fast/CMakeFiles/CommunityShaders.dir/src/Features/Upscaling/VRSubmitUpscaling.cpp.obj`,
SHA-256 `076023B939061611908409A6E800D3F27C7E80225FF174B4A4E7EE311D7CD60B`.
Shader-cache resets are applied at frame capture, preserving an already
reconstructed stereo pair until that boundary. A changed render-context
identity disables reconstruction before any GPU work is attempted.
That initial check did not link a DLL; the pristine checkout lacked generated
FidelityFX permutation headers needed to build the linked SDK normally.

`git diff --check` passes. Runtime image quality, menu transitions, stereo
bounds, SDK execution, and compositor lifetime remain unverified. In
particular, UI composited into a live scene is reconstructed with world
motion/depth, so HUD quality needs runtime assessment. The desktop mirror
continues to show engine render-size color. No performance claim is made.

No devbench, shader tests, deployment, commit, or remote publication was
performed during the initial C++ check.

## Personal deployment and follow-up

At the user's request, Luna subsequently linked a Release DLL in `build/ALL`
using matching cached FidelityFX outputs and deployed the full package to
MO2 Personal's `Open Shaders - VR Submit Upscaling` mod. The deployed DLL hash
was `3CA4C66BCCAE11FDE4B16463A14936765B5BE05052ED86494AC6E4942C9A4627`.
A staging target incorrectly launched shader unit tests; that process was
stopped. The corrected build disabled `BUILD_SHADER_TESTS` and
`DEVBENCH_BRIDGE`, and the completed overlay avoided the test target.

The user then reported lower resolution with jagged output. Static inspection
found that submit compared capture identity with `State::frameCount`, which
the desktop Present hook increments. The follow-up uses the OpenVR cycle
for capture, submit admission, and temporal history; it also establishes
render-thread ownership at post-processing and preserves reversed eye bounds.
The VR scale controls display the latest submission result or fallback reason
and a cumulative reconstructed stereo-pair count. This identifies whether
vendor reconstruction ran without inferring it from the resolution setting.

Luna compiled and linked the correction through direct MSBuild with project
references and post-build events disabled: zero errors, 86.74 seconds.
The corrected DLL was deployed to the same Personal mod and its SHA-256
matched the Release output:
`6835146FD9FBD0764E4EDE2692D294C0A070FB49C4B5CFD0619037E7CFFD6E85`.
Only DLL/PDB changed in this follow-up; shaders and Streamline directories
were untouched. The correction still needs an in-game check.

## Submit-stage foveation

The submit owner now retains independent crop textures and periphery histories
for each eye. `VRSubmitUpscalingFoveation.cpp` implements that part of the same
component; it introduces no additional hooks or engine target redirections.
The existing boot-latched enable, stereo region presets, stretch, temporal
smoothing, visualization, and edge blending controls are used. Submit-stage
foveation uses isolated crops for every DLSS preset. The older Faster mode is
inapplicable to these per-eye inputs and is disabled in the controls.

Before vendor dispatch, resolve and validate both eye rectangles, allocate
owned crops, copy captured inputs, and fill both output backgrounds through
the existing stretch operation. Input crop extents use even dimensions and
are checked against DLSS's supported render range. Asymmetric eye sizes and
off-center regions have independent resources. Full Eye or unsupported crop
parameters select full-eye reconstruction without changing engine resolution.
Resource or stretch failures latch full-eye reconstruction until reset.

DLSS receives crop-adjusted captured projection/reprojection matrices and
motion-vector scale computed from the actual rounded crop. FSR uses the
existing host crop path and full-eye motion-vector pixel extents. A change of
crop dimensions, position, or full-eye/foveated route resets temporal history
and recreates DLSS contexts before evaluation. No eye is evaluated twice in
one compositor cycle. A vendor failure retains the original pair fallback.

The periphery uses a single-eye permutation of the existing temporal shader,
with owned ping-pong history reset at cycle gaps, region changes, and resource
resets. The original SBS permutation remains unchanged. Shader failure uses
the current peripheral color. Existing hard-copy, feather, or dither blending
places each crop into its owned full-eye output; a failed blend falls back to
a valid hard copy. Sharpening and final color-space conversion follow the
existing submit path. Status distinguishes foveated and full-eye submissions.

Paused-menu behavior is unchanged. Foveated runtime quality and performance
still require in-game verification; no shader compilation or validation is
performed by this implementation task.

Luna's direct Release C++ build passed with zero errors and five existing
MSB8028 intermediate-directory warnings. The new translation unit compiled
and linked. DLL SHA-256:
`112F807DB6F0FE79C6ED7EDDC844C9F1084D0EE313D860012288A8838956D406`.
Build log: `build/open-shaders-deploy/foveation-cpp-20260907.log`.
Numeric checks confirmed crop-corner mapping and motion-vector displacement
for centered, nasal, and rounded crop extents. No deployment or commit was
performed for the foveation addition.
