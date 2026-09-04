# Experimental VR dynamic near clip

The VR feature exposes these keys in the existing `VR` object in
`Data/SKSE/Plugins/CommunityShaders/SettingsUser.json` and in VR > General >
Dynamic Near Clip:

```json
{
    "DynamicNearClip": true,
    "NormalNearClip": 5.0,
    "MinimumNearClip": 0.1,
    "NearDistanceScale": 0.25,
    "RestoreSpeed": 0.3,
    "DynamicNearClipReadout": false,
    "FogClearance": true,
    "FogClearanceRadius": 4.0,
    "ReplaceCameraFogWithVolume": true,
    "ForceLocalFog": false,
    "DisableAllFogMeshes": false,
    "LocalFogRadius": 768.0,
    "LocalFogDensity": 0.0015,
    "LocalFogHeightScale": 0.5,
    "LocalFogNoiseScale": 0.006,
    "LocalFogNoiseAmount": 0.65,
    "LocalFogDriftSpeed": 12.0,
    "LocalFogFadeOutSpeed": 1.5,
    "LocalFogColor": [0.85, 0.88, 0.92, 0.18]
}
```

Distances are Skyrim world units. RestoreSpeed is an exponential rate in
inverse seconds: 0.3 closes about 26% of the remaining gap each second, after
a 250 ms hold. These are experimental testing defaults. The feature begins
at the minimum to observe nearby geometry on its first rendered frames.
Turning it off restores the prior engine near distance on the next camera
update. Existing global near-distance settings remain untouched.

## Camera trace

The source trace was checked against static disassembly of the local Skyrim
VR 1.4.15.0 executable. Addresses below are module-relative RVAs, not ASLR
addresses. No debugger or live process inspection is required by the feature.

1. `SceneGraph::GetNearDistance` at `0x5C8610` (VR vtable slot `0x40`)
   returns `customNearDistance` at `+0x168` when `useCustomNear` at `+0x170`
   is set; otherwise `BSSceneGraph::GetNearDistance` at `0xDA0B60` reads
   `fNearDistance`. This getter alone is insufficient for the per-frame VR
   path, which reuses the existing camera frustums.
2. Camera preparation at `0x5B9E90` (address-library ID 35562) reads the
   world camera's first eye frustum at `camera + 0x180`, including near/far
   at frustum `+0x10/+0x14`. At `0x5B9FB8/0x5B9FC1` it publishes the same
   near/far pair, and at `0x5BA018` it sets the camera's far/near ratio.
3. Inside that eye loop, `0x5BA107` calls `BSVRInterface::GetProjectionRaw`
   (vtable slot 8) to get each eye's tangents. `BSOpenVR::GetProjectionRaw`
   at `0xC53C20` forwards to `IVRSystem` slot 2. Near/far are supplied by
   Skyrim; the headset supplies the off-axis field of view. OpenVR also has
   a full-matrix API (`BSOpenVR` slot 7, wrapper at `0xC53B50`), but this
   camera path uses the raw API. The
   [OpenVR matrix API](https://github.com/ValveSoftware/openvr/wiki/IVRSystem::GetProjectionMatrix)
   also takes near/far from its caller.
4. `NiCamera`'s per-eye frustum setter at `0xCAAB50` clamps near against
   `far / maximumFarNearRatio` and `minimumNear`, writes the eye frustum,
   and calls the combined-frustum builder at `0xCAACB0`. The two limits
   are at camera `+0x1EC/+0x1E8`, respectively. The pinned CommonLib's
   `GetRuntimeData2()` offset does not match these instructions; this
   implementation isolates the two verified fields in `ProjectionLimits`
   and installs only on VR 1.4.15 with an intact call-site signature.
5. At `0x5BA281` camera preparation calls `HighActorCuller`'s camera update
   (`0x69F4E0`), which copies the combined frustum from camera `+0x1CC`.
   The near/far members at the culler's `+0x40/+0x44` are exactly the
   `globals::game::cameraNear/cameraFar` used by Open Shaders. The caller
   of camera preparation also copies this combined frustum into culling
   processes immediately after `0x5B9403`.
6. `BSGraphics::State::SetCameraData` at `0xDCF4F0` populates its camera
   cache through `0xDD1320`, which passes each actual eye frustum to the
   projection builder at `0xDD0230`. Its native row-vector Z coefficients
   are `m33 = far / (far - near)`, `m34 = 1`, `m43 = -near * m33`, and
   `m44 = 0`. `0xDD16D0` copies the resulting view data into renderer state.
   Native near extraction is `-m43 / m33`; the GPU frame-buffer upload is
   transposed and uses `-m34 / m33`. The feature uses these explicit layouts.

The hook wraps the FOV setup call at `0x5B9F38`, after that call completes
and before step 2. It writes one chosen near distance into both source eye
frustums and their primary buffer, and relaxes the verified minimum clamp.
The engine then sets the ratio from the new pair, constructs both frustums,
updates culling, and generates its ordinary projection/inverse/history data.
No completed matrix, world transform, far distance, FOV, or jitter is patched.

## Depth and temporal consumers

-   `Utils/Game.cpp::GetCameraData()` constructs `SharedData::CameraData`
    from the engine's near/far pair. `SharedData::GetScreenDepth` is used by
    SSGI, SSR, screen-space shadows, fog, terrain/water effects and other
    depth consumers. Light Limit Fix, underwater DoF and fog also read the
    pair directly on the CPU.
-   `Common/FrameBuffer.hlsli` carries each eye's current, inverse,
    unjittered and previous matrices. Matrix-based world reconstruction,
    stereo reprojection and camera motion vectors consume those engine data.
    The previous matrices retain the previous frame's actual projection.
    Changing only the Z coefficients does not change projected XY or FOV.
-   Streamline uses the engine near/far and separate current/previous
    view-projection matrices for each eye. Both FidelityFX routes use the
    same current near/far. They receive the actual rendered projection.
-   Shadow cameras are not patched. The main combined frustum and
    near-sensitive shadow-caster tests see the new near value; changes to
    shadow coverage near the camera remain an in-game verification item.
-   VR depth-buffer culling retains its existing setting. Its path at
    `0x1323250` prepares depth/camera data through `0x1322D80` and submits
    geometry visibility tests through `0x13561F0`; the CPU consumes queued
    visibility results. Frustum changes do not retroactively change those
    results. Check newly exposed meshes with depth-buffer culling both on and
    off; this experiment does not claim to remove its existing temporal lag.
-   SSGI geometry history stores linear view depth and uses previous view
    transforms. Exponential Height Fog's conservative depth history and
    previous clip W are also linear distances. Its logarithmic volume slice
    distribution nevertheless depends on near: using the current distribution
    to sample the previous volume would select the wrong fog depth during
    adaptation. The fog now saves its grid Z parameters with each successful
    history copy and uses those parameters for history lookup. Current-frame
    reconstruction and compositing continue to use the current distribution.
-   Raw-depth history rejection, including vanilla volumetric lighting,
    can reject history as depth encoding changes. The controller does not
    rewrite historical depth or force whole-frame TAA resets each frame.
    Visible temporal effects require headset testing; this is not a claim
    of validated compatibility with every upscaler or third-party effect.

Before probing, the feature reads both current eye projections from the
renderer shadow state, using `Util::GetCameraData(eye)`. Both jittered and
unjittered near distances must agree with the engine reconstruction near.
If they do not, that depth sample is skipped and checked again next frame;
the override stays active. A later engine clamp that leaves the actual
projection and reconstruction consistent permits sampling using that actual
near/far pair. The requested near is applied again at the next preparation.
The GPU upload cache is diagnostic only, since it has no camera identity or
preparation generation. A mismatch logs the requested, engine, frustum,
native and cached near values and native Z/W coefficients at most once
every two seconds. Both eyes are measured before deciding, and the readout
retains the last projection measurements across camera restoration.

Invalid source frustums or projection limits are a recoverable waiting
condition, checked again on every camera preparation. They do not latch a
permanent failure during loading or camera initialization. The menu guard
also checks live main/loading menu state because the render-frame cache can
lag camera preparation. The readout and throttled log report the source
near/far values for both eyes and the camera's projection limits; an inactive
override is labeled as such instead of displaying zero as the current near.

## Detector and controller

The probe samples the existing world depth prepass before fog and effect
meshes contribute depth. Only the controlled world camera is sampled.
This avoids collision-mesh raycasts missing visual faces, weapons or clutter.
If the prepass hook cannot be installed, the pre-water copy in
`Deferred::Main_RenderWorld_BlendedDecals` provides the original fallback
after opaque geometry, terrain blending and stereo repair. That fallback
cannot guarantee fog exclusion; the installation warning identifies it.

Two compute groups each sample a 16 by 16 grid over the central 30% of an
eye, with a 2 by 2 texel neighborhood at each grid point: 2,048 depth loads
total, independent of rendering resolution. Sky/clear depth, NaNs, infinities
and out-of-range depth are rejected. Each group reduces to one linear
view-axis distance and a valid sample count. Dynamic resolution scales the
packed render region; physical render-size mode already reports that size.

Only 16 bytes are copied into a three-slot staging ring. CPU reads use
`D3D11_MAP_FLAG_DO_NOT_WAIT`; busy slots are retained, never overwritten,
and the GPU is never flushed or waited on. Distances are linearized using
the near/far from the frame that produced them, before asynchronous copying.

The closest eye governs both eyes. The distance scale supplies advance
margin, and the observed closing speed extrapolates over readback age plus
one 90 Hz frame. A close sample disappearing causes a conservative minimum
near plane so the next ordinary frame can rediscover a clipped surface.
Reductions are immediate. Increases wait 250 ms, ignore 5% release noise,
then follow exponential recovery. Missing or stale readback holds the
current near; completed sky samples can restore normal. Loading/menu scenes,
camera replacement and long frame gaps discard old measurements and restart
conservatively.

There is no geometry pass, full-resolution reduction, reversed Z, or CPU
collision traversal. GPU cost is exposed as `VR::NearClipProbe`; it has not
been measured in-headset. Debug logs are limited to one sample every two
seconds. The optional helper HUD and settings readout show requested near,
both observed eye near distances, nearest samples and readback age.

### Fog mesh exclusion

The 314 exact NIF basenames in `VR/NearClipIgnoredModels.h` are excluded
automatically while dynamic near clip is active. Paths are normalized with
`Util::FixFilePath`; directories and filename case do not affect matching.
The implementation reuses the tree patcher's model-identification method
from the `fushrodah` branch: `TESModelDB::TESProcessor::PostCreate` attaches
`OS_NearClipIgnore` integer extra data to each matching model's geometry.
Skyrim copies this metadata when it clones the loaded model. Draw hooks
check that metadata without resolving paths or references each frame.

The list includes `fxcameraattachblowingfog.nif`, `fxcameraattachfog.nif`,
`fxcameraattachgroundfog.nif`, `fxcameraattachturbulentfog.nif`,
`fxcameraattachwispyfog.nif` and `fxambblowingfog01.nif`. The local Skyrim VR
`Skyrim.esm` maps `FXCameraAttachBlowingFogObject` (ARTO `0005592C`) to
`Effects\FXCameraAttachBlowingFog.nif`; `FXCameraAttachBlowingFogEffect`
(RFCT `0005592D`) references that Art Object. `FXAmbBlowingFog01`
(MSTT `00035267`) uses `Effects\FXAmbBlowingFog01.nif`. These editor IDs
are covered through their actual model paths, rather than treated as
filenames. Mods that replace these records' model paths need the replacement
basename added separately. The same metadata scopes the optional headset fog
clearance described below.

The exact list also covers camera-attached fine mist and heavy smoke,
general fog and mist, Soul Cairn and Apocrypha mist, ambient fog/mist/smoke
beams, Clear Skies fog particles, and the `CloudDistant`/`CloudShape` families
including inverted and DLC variants. Solid objects, spell/impact effects,
menu assets, and unrelated steam/water effects are not selected by broad
filename patterns. These names were checked against the installed Skyrim VR
archive catalogs; additional names do not establish whether the visible fog
responds correctly to projection changes.

Tagged Utility draws in the existing world depth prepass are postponed in
a fixed 512-entry queue. Each entry retains a render-pass value and the
batch's viewport, depth, stencil, raster and alpha settings. The existing
tiny probe runs after solid geometry has written depth and before those
fog draws. Each queued draw then runs exactly once, with its original
settings; the engine's batch cache and render settings are restored afterward.
This changes draw ordering, without duplicating geometry or adding a depth
texture, stencil mask or full-resolution reduction.

Standard `BSEffectShaderProperty` inherits an empty `GetRenderDepthPass`,
so those meshes are already absent from the prepass even when their later
effect draw writes depth. The probe therefore runs at the prepass on every
frame, including frames with zero queued fog draws. Metadata exclusion also
covers listed NIF variants that use a depth-prepass material.

The VR 1.4.15 prepass at `0x1323250` is entered through the call at
`0x5B961E`. The probe and postponed draws finish at its `CopyResource` call
at `0x13235BD`, before the engine makes its depth copy or submits occlusion
queries. The seven-byte indirect call is signature checked. The batch-cache
reset routine at `0x1349770` is also checked; it clears the active shader,
technique and material cache. These hooks are limited to the main world
depth prepass. Shadows and visible/color passes keep their existing draws,
and downstream depth effects receive the completed fog-inclusive depth.

When Terrain Blending is active, the probe reads its original main depth
and unoffset terrain depth directly before the regular blend dispatch.
Taking the closer of those two surfaces costs at most 2,048 additional
texel loads per frame with Terrain Blending. The terrain target is restored
before fog draws. Frames using this early sample do not probe again after stereo
repair. A busy readback ring or queue overflow holds the near plane instead
of substituting fog-inclusive depth; all original geometry draws still run.

The readout reports postponed fog depth draws for the frame and tagged model
loads since startup. Standard effect meshes can legitimately give zero
postponed draws because they never enter the depth prepass. The model-load
count is cumulative, not a visible-instance count.
`VR::ExcludedFogDepth` measures the postponed existing draws. In-headset
verification should check those counts while looking through the listed
mist meshes, then approach a solid object behind the mist. Neither visual
equivalence nor runtime cost has been measured in the headset for this path.

### Headset fog clearance

`FogClearance` defaults to `true`, with `FogClearanceRadius = 4.0` Skyrim
units. Both settings appear under **VR > Dynamic Near Clip (Experimental)**;
the cutoff works independently of the adaptive near-plane toggle. The headset
readout shows the selected radius. Values are clamped to 0.1–32 units.

Listed fog fragments inside a sphere around the midpoint of the two rendered
eye positions are discarded. Whole meshes remain loaded, so walking into a
large fog bank clears only its nearby portion. Camera-attached fog eligible for
the local volumetric replacement is the exception described below. The initial
clearance implementation uses a hard radius boundary. It does not move meshes
or change particle emitters.

`Common/FogClearance.hlsli` uses interpolated camera-relative positions in the
Effect and Lighting pixel shaders. Eye origins are transformed into the same
coordinate frame before averaging, so both views evaluate the same physical
sphere. Utility world-depth shaders reconstruct their own fragment position
from raster depth and the current inverse view-projection matrix, accounting
for packed stereo and dynamic resolution. No scene-depth texture is sampled.
The pixel shader uses [HLSL clip](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-clip)
to discard nearby fragments, including their depth writes.

The existing per-draw permutation buffer carries the radius in unused padding;
its size stays 32 bytes. Render-pass hooks set the radius only for tagged models
using the world camera and clear it afterward, including postponed depth draws.
Reflections, fullscreen menus, other shader types and unlisted models receive
zero. This adds no geometry pass, textures, projection changes or spawning work.
The effect requires the Open Shaders replacement pixel shaders; native shader
fallbacks retain their existing rendering.

An offline scan of the 314 vanilla NIFs found 306 with only
`BSEffectShaderProperty`, six Soul Cairn mist models containing both Effect and
Lighting properties, and two without a BS shader-property string. The latter
may reference separate particle geometry; filename tags do not guarantee that
every modded or dynamically generated child uses a covered shader path.

Test by approaching a listed fog bank, looking at a hand while the near plane
changes, then turning the clearance toggle off and on. Check both eyes and the
remaining distant fog. Clearing nearby fragments does not establish that all
other near-plane-dependent fog behavior is correct.

### Local volumetric camera fog

`ReplaceCameraFogWithVolume` defaults to `true`. It applies only to the nine
camera-attached fog basenames in `kCameraAttachedFogNames`; the broader list of
placed mist, light beams and distant cloud meshes keeps its existing rendering.
The setting is independent of dynamic near clip and the global Exponential
Height Fog settings. The Exponential Height Fog shader module must be loaded
because its material fog hooks carry the world-space integration.

`ForceLocalFog` keeps the local volume active without a camera-attached mesh
trigger. It defaults to `false` and provides a deterministic visual test in any
location. The menu reports the current blend and whether the world-space path is
active.

`DisableAllFogMeshes` defaults to `false`. When enabled, the render hooks skip
every geometry draw tagged from `kIgnoredNames` before it reaches the engine draw
call. This CPU-side diagnostic does not depend on the fog-clearance shader. The
menu reports matching model loads and the number of skipped draws so mesh
classification can be distinguished from shader behavior.

The model-load hook stores a distinct metadata value on camera fog geometry.
When one of those draws appears through the main world camera, the current frame
is recorded without a reference or filename lookup. The following frame uploads
a player-centered local density field through the shared feature buffer. The
camera fog draw is discarded once that field is active.

The density is an ellipsoid centered at the midpoint of the rendered eye world
positions. Its boundary follows the player, while smooth procedural noise uses
absolute world position plus time-based drift. The existing headset clearance
radius forms a soft empty sphere inside the volume.

Each shaded pixel integrates four deterministic density samples along the
physical ray from that eye to the shaded surface, clipped to the ellipsoid. Both
eyes use the same absolute density field and headset center. The local path does
not sample scene depth, allocate a screen-aligned grid, reproject history or copy
results between eyes. First-person hands and weapons therefore terminate their
own rays without clearing a coarse tile behind them. Global Exponential Height
Fog can still use its separate froxel pipeline when enabled.

The original camera plane remains for its first detected frame because the
visibility trigger is recorded by that plane's draw. It is replaced from the
next active frame onward. Headset testing is required to tune density, noise,
ambient strength and the fixed four-sample integration cost.

## Depth-effect audit

This is a source audit, not a claim of in-game compatibility. Correct current
projection matrices alone do not preserve thresholds expressed in raw depth.

| Consumer                                    | Finding                                                                                                                                                                                                                                                                                                                                                                                  |
| ------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| SSGI / AO                                   | `prefilterDepths.cs.hlsl` converts the current full-precision depth to linear view depth. `radianceDisocc.cs.hlsl` stores/compares that linear depth and uses the previous view transform. No stale near coefficient found in this history path.                                                                                                                                         |
| Light Limit Fix                             | `LightLimitFix::UpdateStructure` reads near/far and rebuilds both cluster bounds and assignments each frame. Shader cluster selection reads the same pair. The logarithmic grid does not persist across near changes.                                                                                                                                                                    |
| Subsurface scattering                       | `SeparableSSS.hlsli` linearizes both center and neighbor depths before determining blur radius and rejecting cross-surface samples.                                                                                                                                                                                                                                                      |
| Cinematic DoF                               | `PostProcessing/DoF/dof.cs.hlsl` stores autofocus distance in kilometres. History is a physical distance, not raw depth. Underwater DoF also uploads current near/far.                                                                                                                                                                                                                   |
| Dynamic cubemaps                            | `UpdateCubemapCS.hlsl` reconstructs current world positions; accumulated cubemap position data is in world space. No saved raw-depth reconstruction coefficient found.                                                                                                                                                                                                                   |
| Vanilla TAA / motion vectors                | `ISTemporalAA.hlsl` uses current depth for nearest-neighbor selection and velocity/color for history. Near changes preserve depth ordering. Current/previous projection matrices remain engine-owned; near-only changes do not alter projected XY. Newly revealed geometry still exercises ordinary disocclusion handling.                                                               |
| DLSS / FSR                                  | Streamline receives current near/far and per-eye current-to-previous clip transforms. FSR dispatches receive current near/far. The inspected host FSR3 path reconstructs its previous-coordinate depth from current-frame input before using it for disocclusion. Proprietary runtime behavior and temporal quality remain unverified.                                                   |
| Exponential Height Fog                      | Previous volume slice parameters are now saved alongside history; see the earlier section. Linear depth alone was insufficient because the slice distribution depends on near.                                                                                                                                                                                                           |
| SSR                                         | `ISReflectionsRayTracing.hlsl` reconstructs positions using current matrices, but compares raw ray/scene depth against `SSRParams.y`. Its effective hit tolerance changes with near. This is an unresolved compatibility concern, not a verified visual failure.                                                                                                                         |
| Stereo optimization / stereo effect filters | `VRStereoOptimizations/StencilCS.hlsl` uses a relative raw-depth disocclusion test. `Common/VRReproject.hlsli` and stereo shadow/blend consumers also use raw-depth agreement, edge and Gaussian thresholds. Smaller near compresses raw depth differences and can admit different surfaces more easily. Linear edge/near-camera gates elsewhere do not make all these checks invariant. |
| Screen-space contact shadows                | The Bend raymarch scales most thickness tests by `1 - depth`, but floors that scale at `1e-4`. Lower near makes that floor apply at closer world distances. Stereo postfilters have the separate raw-depth issue above.                                                                                                                                                                  |
| Vanilla volumetric lighting / vanilla DoF   | Volumetric lighting compares raw depth across frames. Vanilla DoF contains near-sensitive raw-depth cutoffs and engine-supplied constants. Full compatibility is not established.                                                                                                                                                                                                        |
| Engine water / soft particles               | These also use engine-owned `CameraDataWater` / `CameraDataEffect` constants. Their upload paths have not been individually verified against the adaptive override. Treat them as outstanding runtime checks.                                                                                                                                                                            |

While the adaptive camera override is present, `GetCurrentSceneDepthSRV`
uses Terrain Blending's existing R32 depth even for callers that normally
prefer R16_UNORM. This avoids another quantization step for shared depth,
fog and subsurface scattering without allocating another texture or adding a
pass. Hardware depth precision still decreases while near is small. For
example, with a far plane of 100,000 units and near 0.1, geometry at 20,000
units has raw depth about 0.999996, which rounds to the sky value 1.0 in
R16_UNORM. That extra rounding is avoidable with the existing R32 copy.

The probe also rejects exact zero, used by VR hidden-area masking, as well
as sky/clear depth 1.0. These source-level corrections do not resolve the
remaining raw-threshold and engine-constant audit items above.

## Testing limits and headset checks

Depth cannot see geometry already clipped before the first usable sample,
off-center geometry, surfaces without depth writes, or backfaces when the
camera is inside a mesh. Startup/recovery at minimum and approach prediction
reduce this limitation but cannot guarantee zero clipping after arbitrarily
fast head movement. A central sparse probe may miss a thin object. No
performance, mountain-flicker, eye-matching or temporal-artifact result is
claimed without running the feature in Skyrim VR.

For in-headset verification, enable the readout and check distant mountains,
then approach a wall, skull, NPC face and held weapon, including one-eye-only
proximity. Verify both projection readouts follow the same near, recovery is
gradual, and the camera returns to normal after moving away. Repeat with TAA,
DLSS/FSR, dynamic resolution, terrain blending and stereo optimization as used
in the actual setup. Check loading, teleporting and disabling the feature.
If another camera mod writes near later in the frame, the consistency failure
is a compatibility diagnostic rather than a reason to patch more matrices.
