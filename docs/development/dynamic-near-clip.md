# Experimental VR dynamic near clip

The VR settings expose an adaptive near plane for both eyes:

```json
{
    "DynamicNearClip": true,
    "NormalNearClip": 5.0,
    "MinimumNearClip": 0.1,
    "NearDistanceScale": 0.25,
    "RestoreSpeed": 0.3,
    "DynamicNearClipReadout": false
}
```

Distances are Skyrim world units. The depth probe samples the existing world
prepass with a small central grid for each eye, rejects sky and invalid depth,
and uses the smaller relevant distance for both eyes. The target is
`clamp(nearestDistance * NearDistanceScale, MinimumNearClip, NormalNearClip)`.
Lowering responds immediately; restoration waits through a short hysteresis
deadband and then approaches the normal value exponentially.

The hook runs after Skyrim prepares the world camera and before it constructs
the eye projections and combined culling frustum. It writes the selected near
distance into both source eye frustums; Skyrim then rebuilds its normal
projection, culling, depth and history data. No render pass, reversed-Z path,
far-plane change, FOV change, or shadow-camera change is introduced.

The existing prepass and pre-water depth-copy hooks provide depth without a
full-resolution reduction. Readback uses a small asynchronous staging ring and
never waits for the GPU. The optional headset readout reports the requested,
observed and sampled distances; logs are throttled.

Projection validation checks both jittered and unjittered eye matrices before a
sample is accepted. If another camera path leaves the engine projection out of
sync, the feature holds the engine near plane until the matrices agree.

The implementation is VR-only and installs only on Skyrim VR 1.4.15. If camera,
projection, depth or resource setup is invalid, it restores the engine camera
and continues without the adaptive override.
