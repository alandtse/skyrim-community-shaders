# Open Shaders - Settings Override System

The Settings Override System allows mods to provide custom configuration overrides for Open Shaders features without modifying the main settings file. This enables better mod compatibility and allows multiple mods to adjust different settings independently.

## Directory Structure

Override files should be placed in:

```
Data\SKSE\Plugins\CommunityShaders\Overrides\
```

## File Naming Convention

Override files must follow these naming patterns:

### Feature-Specific Overrides

```
{ModName}_{FeatureShortName}.json
```

Examples:

-   `MyMod_Skylighting.json` - Overrides for Skylighting feature
-   `EnhancedSSGI_ScreenSpaceGI.json` - Overrides for Screen Space GI feature
-   `WaterTweaks_WaterEffects.json` - Overrides for Water Effects feature

### Global Overrides

```
{ModName}_Global.json
```

Examples:

-   `PerformanceOptimizer_Global.json` - Global settings changes
-   `MyMod_Global.json` - Global configuration overrides

## File Format

Override files use JSON format and should contain only the settings you want to override, not the complete feature configuration.

### Feature Override Example

```json
{
    "MaxZenith": 2.0,
    "MinDiffuseVisibility": 0.15,
    "_metadata": {
        "modName": "Enhanced Skylighting",
        "version": "1.2.0",
        "description": "Optimized Skylighting settings for better performance",
        "enabled": true
    }
}
```

### Global Override Example

```json
{
    "General": {
        "Enable Shaders": true,
        "Enable Async": true
    },
    "Advanced": {
        "Log Level": "info",
        "Compiler Threads": 8
    },
    "_metadata": {
        "modName": "Performance Optimizer",
        "version": "1.0.0",
        "description": "Global settings optimized for performance",
        "enabled": true
    }
}
```

## Metadata Section

The optional `_metadata` section describes the override. Its filename identifies the mod and target feature.

-   `modName`: Display name of your mod
-   `version`: Version of your override file
-   `description`: Description of what the override does
-   `enabled`: Whether the override is enabled by default (optional, defaults to true)

## Feature Short Names

To create feature-specific overrides, you need to use the correct feature short name. The full list of feature short names is:

-   `CloudShadows` - Cloud Shadows
-   `DynamicCubemaps` - Dynamic Cubemaps
-   `ExtendedMaterials` - Extended Materials
-   `GrassCollision` - Grass Collision
-   `GrassLighting` - Grass Lighting
-   `HairSpecular` - Hair Specular
-   `IBL` - Image-Based Lighting
-   `LightLimitFix` - Light Limit Fix
-   `LODBlending` - LOD Blending
-   `InteriorSun` - Interior Sun
-   `InverseSquareLighting` - Inverse Square Lighting
-   `ScreenSpaceGI` - Screen Space Global Illumination
-   `ScreenSpaceShadows` - Screen-Space Shadows
-   `Skylighting` - Skylighting
-   `TerrainVariation` - Terrain Variation
-   `SkySync` - Sky Sync
-   `SubsurfaceScattering` - Subsurface Scattering
-   `TerrainBlending` - Terrain Blending
-   `TerrainHelper` - Terrain Helper
-   `TerrainShadows` - Terrain Shadows
-   `VolumetricLighting` - Volumetric Lighting
-   `VR` - VR
-   `WaterEffects` - Water Effects
-   `PerformanceOverlay` - Performance Overlay
-   `WetnessEffects` - Wetness Effects
-   `ExtendedTranslucency` - Extended Translucency

**This list is not exhaustive. It is current as of 11/08/2025. All Feature Short Names will work.**

## How It Works

1. **Discovery**: Override files are automatically discovered when Open Shaders loads
2. **Priority**: Overrides are applied after the main settings are loaded but before features initialize
3. **Merging**: Override values are merged into the existing settings, overwriting only the specified values
4. **Global vs Feature**: Global overrides affect the main settings structure, while feature-specific overrides only affect individual features

## Managing Overrides

### In-Game UI

-   Open **Utilities > Feature Overwrites**, directly before OS Utility.
-   View overwrite files targeting loaded features, including global files.
-   Delete a file after confirmation. This removes it from disk, not just from the list.
-   Deletion reapplies the remaining layers without saving or discarding pending normal feature edits. It removes companion user entries no remaining overwrite controls, deleting empty companion files. Saved personal values remain in normal user settings. Scene Manager drafts remain separate.
-   Use **Export Settings** to choose one feature, then select individual settings from its searchable catalogue-backed list. Arrays are selected as a whole. Exports include unsaved normal feature edits, but exclude applied Scene Manager values and toolbar drafts. Existing same-name files are updated while preserving unselected keys and metadata.
-   Externally changed export targets require a settings reload before export. Exporting another file does not clear this protection. Files changed outside the game are not automatically watched.

### Saving Feature Edits

Edit a normal feature page and save to store changes in normal `SettingsUser.json` and, for overwritten settings, `User/<Feature>.user.json` (or `Global.user.json`). Installed overwrite files are not modified by normal saves. Restoring defaults and saving follows the same behavior. Unedited applied overwrite values and Scene Manager values, including toolbar previews, are excluded from normal user settings.

Global files and their user customizations are applied before feature-specific files and their user customizations. A save only changes the companion controlling each edited setting, preserving shadowed user customizations. A source changed externally must be reloaded before saving. Export is an explicit file-writing action, not a normal settings save.

Removing an overwrite in-game cleans up orphaned companion entries immediately. Removing or disabling its mod outside the game performs that cleanup on settings reload or startup, even when no overwrite files remain. Entries still controlled by another file are retained. Saved personal settings already exist in `SettingsUser.json`, so no transfer is needed when a companion is removed. Values baked into normal settings by older versions are preserved because their original intent cannot be recovered.

Settings that normally require a restart still require one. Set `_metadata.enabled` to `false` in the file or disable the providing mod to disable an overwrite.

## Best Practices for Mod Authors

1. **Use descriptive mod names** in your file names
2. **Include metadata** for better user experience
3. **Only override necessary settings** - don't include unchanged values
4. **Test compatibility** with other override mods
5. **Document your overrides** in your mod description
6. **Version your override files** for easier support

## Troubleshooting

### Override Not Applied

-   Check file naming follows the correct pattern
-   Verify JSON syntax is valid
-   Ensure feature short name is correct
-   Check that the providing mod is enabled and the file does not set `_metadata.enabled` to `false`
-   Look for errors in the Open Shaders log (CommunityShaders.log)

### JSON Validation

Use a JSON validator to ensure your override files have valid syntax:

-   No trailing commas
-   Proper quotation marks around strings
-   Balanced brackets and braces

### Log Messages

Open Shaders logs override discovery and application:

-   Check `CommunityShaders.log` for override-related messages
-   Look for "Discovered X override files" and "Applied X override(s)" messages

## Examples

See the included example override files in the `Overrides` directory for reference implementations.
