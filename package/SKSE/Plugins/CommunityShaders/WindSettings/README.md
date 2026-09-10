# Wind Settings

Place tree-response JSON files in this directory. Open Shaders scans every `.json` file in deterministic filename order at startup; `WindSettings.schema.json` and `*_backup.json` files are ignored. If multiple files define the same parameter for the same mesh, the later file wins and one warning lists every conflicting JSON file.

If this directory contains no settings JSONs, every tree uses the built-in defaults for every response parameter.

In-game edits remain live until saved or reverted. Saving writes only changed properties to the last-scanned JSON containing that mesh, so omitted properties remain omitted. Reverting before a save restores the values loaded from disk.

```json
{
    "$schema": "WindSettings.schema.json",
    "version": 1,
    "trees": [
        {
            "mesh": "meshes/landscape/trees/treepineforest01.nif",
            "bendSensitivity": 1.0,
            "leafAmbientSensitivity": 1.0,
            "upperBendRange": 100,
            "maximumDisplacementPercent": 3,
            "trunkGustInfluence": 0.5,
            "leafGustInfluence": 0.99,
            "transientWindInfluence": 2.01,
            "leafTransientWindInfluence": 5.0,
            "leafTransientFlutterMaximum": 20.0,
            "transientMaximumBendMultiplier": 2.5
        }
    ]
}
```

Every response property is optional. Omit any property to use its coded default; an entry containing only `mesh` is also valid. Use omission rather than JSON `null`.

The coded defaults are:

-   `bendSensitivity`: `1.0` (`0.0`–`4.0`)
-   `leafAmbientSensitivity`: `1.0` (`0.0`–`4.0`)
-   `upperBendRange`: `100` (`5`–`100` percent of measured height)
-   `maximumDisplacementPercent`: `3` (`0`–`10` percent)
-   `trunkGustInfluence`: `0.5` (`0.0`–`2.0`)
-   `leafGustInfluence`: `0.99` (`0.0`–`2.0`)
-   `transientWindInfluence`: `2.01` (`0.0`–`5.0`), scaling trunk response to short-lived impulses such as Unrelenting Force
-   `leafTransientWindInfluence`: `5.0` (`0.0`–`5.0`), scaling leaf response to short-lived impulses independently of the trunk
-   `leafTransientFlutterMaximum`: `20.0` (`0.0`–`20.0`), capping only the transient contribution to vanilla leaf flutter for each mesh
-   `transientMaximumBendMultiplier`: `2.5` (`0.0`–`5.0`), capping the trunk response to those impulses

Mesh matching is case-insensitive and accepts either slash style. The in-game Global Override is runtime-only and can temporarily replace every per-tree response for testing.

Tree spring state is simulated entirely on the GPU using the global Wind settings. Grass and leaf flutter continue to use the unfiltered wind so fast-moving gust fronts retain their detail.
