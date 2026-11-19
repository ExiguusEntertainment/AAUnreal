# Project Plan

## Native Unreal Level Import

Use L30 as the first end-to-end import. Verify the geometry and sector movement before tuning materials and Lumen in a saved Unreal level.

1. **Define the imported map data.** Keep stable IDs for sectors, lines, and sides. Preserve floor and ceiling heights, adjacency, wall flags, texture names, source offsets, and sector light levels. Retain sectors that start closed so they can open later.
2. **Fix floor and ceiling geometry.** Build footprints from the original boundary lines, classify outer rings and holes, and triangulate each footprint once for both surfaces. Check sector 163's three cutouts and use the same triangles for the preview and the import.
3. **Build fixed walls from the original lines.** Resolve each side's upper, middle, and lower surfaces. Handle open portals, opaque walls, masked or translucent surfaces, and collision separately. Verify an open passage and one of L30's translucent barriers that blocks movement.
4. **Prove independent floor and ceiling movement.** Give each movable surface its own Unreal component. Keep the walls fixed and handle newly exposed vertical faces as part of the moving floor or ceiling assembly. Verify passage and collision through the full travel of an initially closed sector.
5. **Preserve textures and animation controls.** Match the source texture scale, orientation, and starting offsets. Give each animated surface its own runtime offset so it can scroll independently of surfaces using the same texture.
6. **Save the native level and validate lighting.** Generate modular Static Mesh assets and a `.umap`, then reopen it and repeat the movement checks. Apply the imported sector light level as an adjustable material baseline and use Lumen for bounce and occlusion. Check a dark sector beside a bright one with the opening both closed and open.
7. **Run the authored doors in the native level.** Import the door markers from `THINGS` and use the original ceiling travel and timing. Exercise one door from a reopened L30, including a blocked closing cycle.
8. **Calibrate live sector brightness.** Compare L30 light levels 50, 150, and 255 at fixed exposure and the same door height.
9. **Restore door-driven lighting.** Evaluate sector connections, door openness, and outside light using the original lighting rules. Verify the closed, halfway, open, and reclosed states.
10. **Verify from the player's viewpoint.** Import the original player start, add first-person movement and a use interaction, and test crossing and blocking the door in Play mode.
11. **Verify a second map.** Import L151 and exercise its outdoor lighting in a fresh game session.

**First checkpoint:** Break down steps 1 and 2. Produce floor and ceiling meshes for sector 163 with all three cutouts intact, plus valid footprints for sectors that start closed.

### Step 1 map data and inspection

- `FDoomWadMapData` exposes the parsed records in source order. Each record's zero-based index is its ID within the map. Line adjacency comes from its front and back sidedefs. Heights, offsets, flags, and light values stay in source units.
- In the Unreal console, run `AA.ExportMapJson Content/TestData/L30.MAP ReviewArtifacts/L30.json`. A multi-map WAD takes an explicit marker after the output path. L30 uses the embedded `MAP01` marker.
- The JSON is a diagnostic snapshot from the same parser the preview uses. It includes the geometry, things, BSP data, lump directory, and companion files. `.I` and `.GEN` are text. `BLOCKMAP` and `.LIT` are little-endian word arrays. `REJECT` and other opaque payloads use base64.
- Step 1 is complete for L30. The exported records and companion data were checked against the source files, and all three L30 Unreal automation tests passed. Step 2 can use the original sector IDs to compare the generated footprints with the source.

### Step 2 sector geometry and review

- Build the floor and ceiling from the same triangulated sector footprint. The original linedefs constrain the triangles. Crossing an odd number of boundaries puts a point inside the sector, so inner cutouts stay empty and separate islands can still be filled.
- Keep floor and ceiling meshes for sectors whose heights start equal. The perspective review uses diagnostic colors and hides ceilings to expose the floors.
- Use `AA.ExportTopDownReview <MapPath> <MapName> <OutputPng> [ImageSize] [SectorId]` for a picture drawn from the same triangles used by the mesh builder. Pass `163` as the sector ID to isolate its three cutouts. Use `AA.ExportPerspectiveReview <MapPath> <MapName> <OutputPng> [ImageSize]` for the Unreal render.
- Step 2 is complete for L30. All 179 sectors have a footprint, and all 12 sectors that start closed retain both surfaces. The sector 163 regression checks the three cutouts and the filled area. All four L30 tests passed. The three `L30-step2-*.png` previews are in `ReviewArtifacts`.

### Step 3 fixed walls and collision

- Resolve upper, middle, and lower faces from the original linedefs and sidedefs. Each side keeps its own facing and texture data. A blank middle leaves the shared opening clear of render geometry. A textured two-sided middle uses a masked or translucent material.
- Build collision separately from the render mesh. Steps and upper bands block by height, and an impassable flag can block a pane or an otherwise open middle. Creature-only spans are kept separate for the future AI collision setup.
- Verified L30 line 11 as an open passage with a blocking upper band, line 590 as a passable masked screen, and line 910 as a translucent barrier that blocks a capsule from either side. All five L30 automation tests passed.
- The Unreal geometry preview is `ReviewArtifacts/L30-step3-perspective.png`. Green highlights identify visible translucent panes. Use `scripts/create_wall_materials.py` in the Unreal Python editor to regenerate the three wall material parents if needed. Texture scale, offsets in the rendered result, and animation controls are covered in step 5.

### Step 4 sector movement

- The floor and ceiling height setters move independently and clamp to the opposite surface. Each change rebuilds only the wall batches containing lines attached to that sector and updates their collision before returning. The wall lines keep their original XY positions.
- Verified L30 sector 147 at ceiling heights 32, 75.2, and 128. The closed doorway blocks from both sides. The partial opening clears a short capsule. The full opening clears the taller capsule. Raising the floor to 56.5 keeps the ceiling at 128 and moves the lower faces and collision. Closing again restores the blockers. All six L30 automation tests passed.
- `AA.ExportSectorMotionReview Content/TestData/L30.MAP L30 ReviewArtifacts/L30-step4.png 147 128 780` saves three images from the same camera. The orange face shows the blocked part of the opening.

### Step 5 textures and scroll controls

- Decode the original column-major pictures with the game palette and retain the cutout alpha. Use the actual image dimensions and one texel per source map unit. Floors and ceilings map from world XY. Walls start from the stored sidedef offsets and keep their vertical anchor when a moving sector reveals more of a wall band.
- `SetSectorTextureScrollTexels` controls one floor or ceiling, and `SetWallTextureScrollTexels` controls one sidedef and wall band. Surfaces can share the same image while their offsets change independently. Unanimated walls stay batched by texture and blend mode. `F_SKY1` keeps its ceiling collision while leaving the sky surface open visually.
- Verified L30's rectangular `FART3` wall texture, the 23-texel offset on sidedef 1513 after raising sector 159's floor, and independent scroll on floor 147 and translucent wall line 910. All seven L30 automation tests passed.
- `AA.ExportPerspectiveReview Content/TestData/L30.MAP L30 ReviewArtifacts/L30-step5-textured.png 1600 Textured` shows the original textures in four cutaway views. `AA.ExportTextureScrollReview Content/TestData/L30.MAP L30 ReviewArtifacts/L30-step5-scroll.png 147 128 850` saves the before and after views at the same camera pose. Pixel comparison confined the change to sector 147's floor strip. The review is in `Saved/L30-Step5-Review.html`.
- These captures use unlit materials. In a fresh offscreen session the first overview can come back blank. The exporter retries once before saving, and the retry was verified on L30.

### Step 6 native level and Lumen

- `AA.ImportNativeWad Content/TestData/L30.MAP L30 /Game/Maps/L30` saves the map and its mesh, texture, and material assets under `Content/Maps`. The command takes a new package path. Set `AA_NATIVE_WAD_MATERIALS=1` when running `scripts/create_wall_materials.py` to rebuild the native material parents.
- The saved actor owns the sector and wall records needed by the height and scroll controls. Each floor and ceiling has a movable Static Mesh. Wall bands stay on their source lines while their visible and collision spans change with sector height. Opaque surfaces have thickness for Lumen, and floor and ceiling collision follows the original triangulated footprint.
- Sector light values set an emissive baseline on the textured surfaces. `SetSectorLightLevel` adjusts a sector at runtime. The saved level enables Lumen global illumination and reflections at fixed exposure.
- Reopened `/Game/Maps/L30` in a fresh Unreal editor process. Sector 147 passed the closed, partial, open, raised-floor, and reclosed collision checks, including traces against the moved floor and ceiling. Independent texture scroll and light adjustment passed on the reopened surfaces. All seven WAD regression tests passed.
- `AA.ExportNativeL30Review ReviewArtifacts/L30-step6.png 850` captures dark sector 14 (light 50) facing bright sector 13 (light 255) through doorway 147. The three frames show the door closed, the door open, and the same open view with Lumen disabled. The foreground floor receives visible indirect light in the open Lumen view. The review is in `Saved/L30-Step6-Review.html`.

### Step 7 authored doors

- The six `THINGS` markers of type 1500 define doors in sectors 144, 145, 147, 149, 151, and 155. Each opens to the lowest adjacent ceiling minus 16, travels for 1.5 seconds, and holds for two seconds. A closing door reopens if the moving ceiling encounters a blocker.
- New native imports include the door definitions. `AA.ImportWadDoors Content/TestData/L30.MAP L30` adds them to the open native editor level when its topology and the doors' closed heights match the source. Use `AA.UseWadDoor 147` in Play mode or call `UseDoor` from Blueprint to activate a door.
- Verified all six definitions after reopening `/Game/Maps/L30` in a fresh game process. Door 147 traveled from Z 32 to Z 112, cleared the standing capsule, reversed at Z 68.44 above an obstruction, and closed again after the obstruction was removed. All seven L30 regression tests passed. `AA.ExportNativeL30DoorReview ReviewArtifacts/L30-step7.png 720` captures the cycle in a running game. The frames are in `Saved/L30-Step7-Review.html`.

### Steps 8–11 live lighting and playable checks

- The native level has an adjustable sector emissive strength, set to 1.5 for these maps. The L30 calibration fixes the exposure, camera, and door height while overriding sector 14 at 50, 150, and 255. A foreground floor patch measured 61, 98, and 123 on an 8-bit brightness scale. At light 150 the same patch measured 89 with Lumen off. Sector 146 keeps its calculated light value throughout.
- Runtime lighting uses the original integer weights, including light from beyond a door scaled by its opening percentage. Each change starts from the authored values and runs two passes so equivalent door heights produce the same result when reopened or reclosed. Outdoor sectors take the explicit outside light level. `SetSectorLightLevel` temporarily overrides one sector, and `ClearSectorLightOverride` restores its calculated value.
- L30's door-driven light in sector 14 tracks `84 → 119 → 160 → 84` through the closed, halfway, open, and reclosed states. Door sector 147 tracks `0 → 63 → 138 → 0`.
- Native maps use a map-local first-person mode. L30 uses the source player-one start at `(1608, 432)`. Use WASD to move, the mouse to look, and E while facing a door. The Unreal walk-through verifies the imported spawn before positioning the player at the doorway for a repeatable crossing using W and E. The player blocks the closing door, crosses once it reverses, and the closed light level returns.
- `/Game/Maps/L151` verifies that the same rules apply to a second map. Its outdoor sector 52 follows the outside light from 255 to 90 while the sealed cave remains dark. The native import saved 55 sectors and 271 lines. All seven L30 import regression tests passed after importing both maps.
- The image gallery is `Saved/Wad-LiveLighting-Review.html`. Run `AA.ExportNativeWadLightingReview <OutputPng> [ImageSize]` from a running L30 or L151. Run `AA.ExportNativeL30PlayerReview <OutputPng> [ImageSize]` from a fresh L30 game to repeat the walk-through.

### Asset follow-ups

- [ ] Find the original `MARBGRAY` texture and restore it to the L30 import. The supplied `PICS.RES` lacks it, so the affected lower wall bands use the default material when exposed. Check a raised floor at sector 147 after restoring the art.

## 2026-04-11 Review Findings

Issues recorded during the April review. The current checkout now creates separate floor and ceiling roots for sectors it builds. The native import outline above covers the remaining movement work.

1. `ASectorMeshBuilderActor::SetSectorFloorZ` and `SetSectorCeilingZ` are non-functional because `FloorRoots` and `CeilRoots` are declared but never populated.
2. `bUseSubsectorPolysForFloors` does not actually control behavior; subsector polys are always queried and used when present.
3. `AutoTest.bat` is malformed and cannot launch the smoke test as committed.
4. The legacy-map automation test expects `NumSectors > 0`, but the modern legacy parser intentionally reports zero sectors for now, so the test will fail once a real modern legacy sample is exercised.

## 2026-04-11 Immediate Fix Plan

- Wire per-sector floor and ceiling roots into `ASectorMeshBuilderActor` and attach generated PMCs to them.
- Restore the `bUseSubsectorPolysForFloors` flag so it meaningfully switches between subsector polys and linedef-loop fallback.
- Repair `AutoTest.bat` and make it resilient to the local DDC environment by forcing memory cache for headless smoke runs.
- Adjust the legacy-map automation test so it matches the current loader contract instead of asserting unavailable sector data.

## Current Environment Note

- Unreal headless startup currently fails on this machine without a DDC workaround because the default cache graph has no writable node. Use `-DDC-ForceMemoryCache` for local verification until the machine-level cache config is fixed.
