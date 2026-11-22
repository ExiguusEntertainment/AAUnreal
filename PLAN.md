# Project Plan

## Full Port Roadmap

The goal is a playable Unreal port of Amulets & Armor from a new game through the original campaign, with working progression, saves, and level transitions.

| Milestone | Completion check | Status |
| --- | --- | --- |
| 1. Native level foundation | Import original geometry, materials, collision, moving sectors, and lighting into saved Unreal levels. | Proven on L30 and L151 at GitHub checkpoint `727f482`. |
| 2. One complete gameplay slice | Play a continuous route from an original spawn through an authored interaction, an encounter, a pickup, and a level transition. | Verified from L30's original spawn to L301 in Steps 16–18. |
| 3. Core game systems | Verify combat, enemy AI, items and inventory, equipment, spells, and character progression against the original. | Steps 19–24 verify a Guardian fight, the ready hand, one spell, and the first priest level-up. |
| 4. Authored levels and events | Import the remaining campaign maps and verify switches, lifts, traps, scripted events, and level exits. Track unsupported features and missing assets by map. | All 30 source maps are native. Staged replays verify Town Hall returns through Destruction. The remaining source asset and gameplay parity work is listed below. |
| 5. Presentation and persistence | Restore sky and lighting behavior, audio, menus, HUD, and save/load. Verify that revisiting a level restores its state. | Disk checkpoints restore settled levels, ordinary door phases, scripted lifts, and pending waits. L301's positional stream and waterfall loops run with the source audio. |
| 6. Campaign and release verification | Play the campaign from a fresh packaged build, resolve parity and performance issues, and verify release reproducibility. Define multiplayer scope and acceptance tests separately. | The fresh character completed Visions of Jugurtha and The Knights of Andrew. The Windows Development package passed Town Hall startup, the Destruction quest, and final save reload. |

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
12. **Verify a fresh checkout.** Skipped at the user's direction. Continue from the current checkout.
13. **Walk L30 from the original spawn to an authored door.** Verify one continuous play session with player movement, collision, and live lighting.
14. **Finish the outdoor lighting pass.** Give L151 a visible sky and connect outside light to the original day/night behavior. Compare the same view at two times of day.
15. **Port another authored interaction.** Verify a source-authored interaction from player input through its effect in the native map.

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
- Visible faces and the closed shells for Lumen use separate material sections. The shell's mask prevents slab skirts and wall caps from competing with the textured stair risers, ceiling lips, and sector edges. Use `AA.RefreshNativeWadSurfaces </Game/MapPackage>` to update previously generated native meshes in place.
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

### Steps 13–15 continuous route, outdoor clock, and map exit

- Step 13 walks from L30's spawn at `(1608, 432)` to the west side of door 147, verifies the closed door blocks the player, presses E, and crosses to the east side. Sector 14 returns to light 84 after reaching 160 with the door open.
- Step 14 reads the backdrop named in the source map's `.I` file. L151 uses `2BACK02.PIC` and the game palette. The clock runs at the original 70 Hz rate of 524,288 ticks per day. Sector 52 receives light 255 at noon and 35 at midnight. The sealed cave stays at 0. `SetTimeOfDayHours` sets and runs the clock, and `SetOutsideLightLevel` overrides it until the clock is set again.
- Step 15 imports type 1518 as a level link on the sector containing its marker. L30's thing 394 links sector 157 to native L301. Walking into that sector triggers travel and spawns the player at L301's source start `(3440, 2944)`. The day clock carries across the transition. The exit test starts beyond the closed scripted gate in sector 129.
- The gallery is `Saved/Wad-Port-Steps13-15-Review.html`. In a fresh standalone game, run `AA.ExportNativeWadPortReview Route <OutputPng> [ImageSize]` or `AA.ExportNativeWadPortReview Link <OutputPng> [ImageSize]` on L30. Run `AA.ExportNativeWadPortReview Sky <OutputPng> [ImageSize]` on L151.

### Steps 16–18 first complete gameplay slice

16. **Restore the route to L30's exit.** The selected `S30.SRP` activities run through the native level's wall use, sector entry, and movement controls. Activities 16–18 start the timed doors and sector 167's pressure plate opens them permanently after the source delay. Activity 40 changes the exit switch, lowers floor 176 from Z80 to Z0, and raises gate 129 from Z0 to Z120. Activity 201 displays the original blocked-slab response. The script port covers these interactions and their movement. Original audio, trap projectiles, and visual effects are still part of the authored-event work.
17. **Add one authored encounter and pickup.** The Elf Guardian (thing 9, type 1018) and berry (thing 210, type 243) use their original L30 positions and palette sprites. The Guardian has basic pursuit and ranged damage. Use left mouse for a melee hit and E to collect the berry. The HUD shows health and berry count, and both carry across native level travel.
18. **Verify the complete gameplay slice.** A standalone Unreal run started at `(1608, 432)`, used door 144, fought the Guardian, collected the berry, operated the timed switches and pressure plate, opened the exit, and reached L301's original spawn. The review drives player inputs throughout and verifies the berry and surviving health on arrival.

- Run `AA.ExportNativeWadPortReview Slice <OutputPng> [ImageSize]` in a fresh standalone L30 game. The seven images are in `Saved/Wad-Gameplay-Slice-Review.html`.

### Steps 19–22 core gameplay and script import

19. **Port the Guardian's combat.** Type 1018 uses the original melee range, damage, windup, and cooldown, plus its ranged bolt and health regeneration. Its original sprites show the attack and hurt poses, and it retreats below 500 health. The route checks a 300-damage bolt, no shots through a closed door over a full cooldown, and melee during the fight. Movement and sight use Unreal collision.
20. **Use inventory and equipment.** E collects the original iron dagger (thing 300) and berry (thing 210). The dagger uses the original ready-weapon damage and speed from `DES00200` and the hit frame from `DAGGER.ANI`. A berry restores up to 200 raw food points from `DES00243`. The walkthrough checks the food cap, the consumed slot, and the equipped dagger in L301. See Step 23 for the current controls.
21. **Cast Rejuvenate and verify XP.** The slice uses a level-one priest. Spell 70 uses the source success roll, 1,000 mana cost on success, 500 on a fizzle, and up to 500 health restored on success. The Guardian awards XP equal to actual damage, including damage to regenerated health. The walkthrough checks the award and carries health, food, mana, XP, and equipment into L301. Step 24 adds rune entry and level-up rewards.
22. **Import activity 40.** The editor reads `S30.SRP` and saves the activity in the Unreal level. The runtime executes the imported switch change, waits, message, two sliders, and the block that prevents repeat execution. The walkthrough measures both slide start times and final heights. Sound and visual effect commands are recognized and reported as deferred during import.

- Run `AA.ExportNativeWadPortReview Slice <OutputPng> [ImageSize]` for the updated walkthrough. The review gallery is `Saved/Wad-Core-Gameplay-Review.html`.

### Steps 23–26 ready hand, progression, and a second gameplay map

23. **Use the original ready-hand controls.** Aim at the item and press E to collect it. Keypad 7 opens the inventory. Click an item and then the ready-hand box. Left Ctrl uses the ready hand, as does a left click in mouse-look mode. The L30 route collects and equips the iron dagger, readies and eats the berry, and readies the dagger again for travel.
24. **Enter runes and gain a level.** Enter keypad 4 and 5, then cast Rejuvenate with right mouse or keypad Enter. Automation checks the heal, the 500-mana fizzle, and the original requirement to have more than 1,000 mana. The L30 walkthrough starts with 1,280 XP so the Guardian fight crosses 2,000 XP and triggers the priest's first level-up. It verifies the source attribute and maximum health and mana gains, then checks that the resulting stats and equipment carry to L301.
25. **Import timed doors and the pressure plate.** The L30 importer follows calls and jumps in `S30.SRP` and saves the executable activities in the native map. The walkthrough verifies activity 17's delay, repeated switch press, and timed close. It presses all three switches, steps on sector 167's pressure plate, and checks that sectors 165, 166, and 168 stay open with clear collision. Step 28 adds the trap and script effects.
26. **Reuse the runtime in L151.** The walkthrough starts at the original spawn, collects and readies the authored Potion of Featherfalling, and lands a dagger hit on the authored Gaiseric Magician. It triggers activity 1 and verifies that the sector 31 lift carries the player from floor Z−250 to Z0 and back. The potion's effect remains to be implemented.

- Run `AA.ExportNativeWadPortReview Slice <OutputPng> [ImageSize]` in standalone L30 with `?WadXP=1280`. Run `AA.ExportNativeWadPortReview SecondMap <OutputPng> [ImageSize]` in standalone L151 with `?WadItems=200?WadReady=0` to start its review with an iron dagger. All nine tests under `AAOriginal.WAD` passed. See `Saved/Wad-Ready-Hand-and-Lift-Review.html` for eight captures.

### Steps 27–30 continuing into L301 and saving a checkpoint

27. **Survey L301.** The supplied `S301.SRP` has an empty initialization activity, and all 196 lines and 48 sectors have activity zero. Player one spawns at `(3440, 2944)` in sector 44. Thing 246 is an Elf Guardian in that same sector at `(2704, 3216)`. The encounter is reachable from arrival on foot and needs only the shared encounter runtime and Guardian art. Beyond it, the source uses water, current, and ambient sound markers. Thing 304 links to L531.
28. **Run the L30 trap and effects.** Activities 16 and 18 call activity 20 after 360 ticks. The activity launches two type-4015 poison missiles from the source positions and sweeps for player and wall impacts. The walkthrough triggers two cycles and checks both shots hit geometry each time. Unreal plays the four sound samples referenced by the imported script. The exit switch runs activity 40's camera quake at its authored time.
29. **Extend the continuous walkthrough into L301.** The replay carries the level-two priest and ready dagger through L30's exit, walks through the L301 arrival area, and lands a dagger hit on thing 246. The capture shows the encounter from the player's viewpoint.
30. **Save and reopen an altered level.** In a settled area, use `WadSave` and `WadLoad` in the Unreal console. The checkpoint records the current map, player, remaining pickups, encounter health and positions, changed sector heights, switch materials, and script variables. A fresh standalone process restored the player's location and stats, the ready dagger, removed pickups, the defeated Guardian, and the raised gate. Using the restored exit switch again verified that its repeat lock survived.

- From L30 with `?WadXP=1280`, run `AA.ExportNativeWadPortReview Campaign <OutputPng> [ImageSize]`. After it writes the review checkpoint, start a fresh standalone L30 with `?WadLoad=NativeWadCheckpointReview` and run `AA.ExportNativeWadPortReview Checkpoint <OutputPng> [ImageSize]`. The four captures are in `Saved/Wad-Campaign-and-Checkpoint-Review.html`.

### Steps 31–34 river route, L531, and level revisits

31. **Import L301's water and currents.** The importer uses nine water markers, eight current sectors, and twelve scrolling sides. It reads the floor and sidedef scroll rates and current vectors from `MAP.ANI`. The water stays at its original visible height, with collision 20 units below. Current strengths are scaled to the native first-person movement speed. The replay verified southwest drift and texture motion in sector 37. Step 35 adds the ambient loops.
32. **Import L531 as a native Unreal level.** The saved level contains 25 sectors and 137 lines, the source player start, a door, and the link to L31. After verifying arrival, the review stages the player in the teleport hallway and walks into sector 21. Activity 201 from `S531.SRP` sends the player to `(1263, 1200)` and preserves the view direction. See Step 37 for the L31 import.
33. **Walk L301 to L531.** The replay starts at the original L301 spawn with a ready dagger supplied for the review. It crosses the river, jumps from sector 16 onto sector 47, and enters thing 304's link. L531 spawns the player at `(-144, -232)` with the same XP, health, and ready dagger.
34. **Preserve settled level state across visits.** The game archives a settled level on departure through a WAD link. Disk checkpoints carry those prior maps along with the current map. A test return from L531 restored L301's defeated Guardian and the carried character, both in the same session and after loading the L531 checkpoint in a fresh Unreal process. The existing single-map L30 checkpoint also loaded successfully.

- From standalone L301 with `?WadItems=200?WadReady=0`, run `AA.ExportNativeWadPortReview River <OutputPng> [ImageSize]`. To repeat the restart check, start standalone L531 with `?WadLoad=NativeWadRiverReview` and use the `RiverReload` mode. The review gallery is `Saved/Wad-River-and-Level-Revisit-Review.html`.

### Steps 35–38 spatial audio and the route from L531 to L31

35. **Restore L301's river audio.** The saved map has 13 stream and two waterfall emitters. They use samples 3003 and 3007 from `SOUNDS.RES` and the authored 500-unit horizontal falloff. Group zero plays independent emitters. Each nonzero group plays its nearest emitter. The audio-enabled replay verified both sounds remained active beyond one full sample and stopped when the listener moved outside their radius.
36. **Follow L531 from the original start.** The replay jumped into sectors 11, 12, and 14, pressed E to open door sector 24 to Z128, crossed it, and entered thing 103's exit in sector 23. Movement, jumps, doors, and travel ran through the normal first-person inputs and collision.
37. **Import L31 as a playable native level.** The map saves 174 sector records and 746 lines. It retains the five unused sector IDs, and the 169 sectors with boundaries have meshes. All eight activities come from `S31.SRP`, including the startup activity that continues into activity 1, its moving floors, two teleport triggers, switch texture, and sounds. The original start is `(2544, 2775)`. Thing 235 links to L22, which still needs a native import.
38. **Walk into L31 and use its authored controls.** The L531 replay began with a ready dagger and 2,105 XP supplied for the test. Arrival preserved the ready dagger, XP, health, mana, and food. The replay opened sector 59 with E, then walked onto sector 107's pressure plate. Imported activity 3 lowered floor 135 from Z192 to Z88.

- Run `AA.ExportNativeWadPortReview Ambient <OutputPng> [ImageSize]` in standalone L301 with audio enabled. For the route, start standalone L531 with `?WadItems=200?WadReady=0?WadXP=2105` and use the `Ravine` mode. See `Saved/Wad-Ravine-and-L31-Review.html` for the four route captures.

### Steps 39–48 L22, trap effects, and checkpoints during motion

39. **Trace L31's exit route.** The route from the first pressure plate follows sectors 106 and 68 to the south, then approaches door 168 from sector 96. Thing 235 in sector 26 links to L22. The source route uses the existing stairs and door. The lift and teleporter puzzles can run on separate branches.
40. **Verify L31's startup lift.** Activity 0 continues into activity 1. The walkthrough verified floor 136 rose from Z88 to Z188 in three seconds and returned to Z88 at nine seconds.
41. **Verify the L31 pressure-plate reset.** Walking onto sector 107 ran activity 3. The walkthrough observed floor 135 lower from Z192 to Z88 and return to Z192.
42. **Run the L22 trap projectiles.** Imported magic dart type 4011 and paralyze missile type 4016 with their source sprites and speeds. The dart deals 100 damage. The other missile halts player locomotion for ten seconds.
43. **Save L22 as a native level.** Preserve 201 sectors, 812 lines, the original start, authored doors, and eight activities from `S22.SRP`. Thing 113 points to the original town screen, campaign form 20004. The import stores that destination as a form ID.
44. **Walk from L31 to L22.** The replay began at the original L31 spawn with a supplied dagger and 2,105 XP at level 2. It used both authored doors, walked through sector 26's link, and confirmed that the ready dagger, XP, health, mana, and food carried into L22.
45. **Use the first L22 door.** From the source spawn at `(-2432, -228)`, the replay used E on door sector 134. It measured the opening at Z176 and walked through.
46. **Save ongoing sector motion and delays.** The checkpoint records scripted slides, ordinary door phases, and remaining waits by activity and command. A new Unreal process resumed L31 door 59 from a partially open state, held it, and closed it on schedule. The older L30 checkpoint also loaded successfully. Save from the ground once projectiles and screen effects have cleared.
47. **Resume an active lift after restart.** The L151 checkpoint saved floor 31 at Z−187.5 with 1.75 seconds remaining on its script wait. The restart replay checked the saved height and carried the player up and back down. After the recorded wait, the replay staged the player on the original approach, walked back onto the lift, and verified a second cycle started.
48. **Test L22's corridor trap.** The replay staged the player near sector 188 and entered off-axis. Both shots hit map geometry. On the second entry the paralyze missile held the player still with W down for ten seconds. The following dart dealt 100 damage and the player moved again when paralysis ended. All nine WAD regression tests passed.

- Start standalone L31 with `?WadItems=200?WadReady=0?WadXP=2105?WadLevel=2?WadNextXP=36000` and run `AA.ExportNativeWadPortReview L31ToL22 <OutputPng> [ImageSize]`. Run `L22Trap` from L22 and `LiftSave` from L151. For fresh-process checks, use `LiftReload` in L151 with `?WadLoad=NativeWadMotionReview`, or `DoorReload` in L31 with `?WadLoad=NativeWadDoorReview`. The four captures are in `Saved/Wad-L22-and-Motion-Review.html`.

### L22 exit and town

49. **Trace the authored L22 ending.** Follow the level geometry and the imported `S22.SRP` activities from the original start to Thing 113 in sector 184. Thing 113 selects campaign form 20004.
50. **Exercise L22's ordinary doors along the route.** The standalone replay opened eleven distinct doors with E and walked through while their ceilings were raised.
51. **Use the L22 switch.** Activate line 762 with E to run activity 1. It changes the switch texture, raises ceiling 15 from Z0 to Z100, and blocks another run.
52. **Open the exit with the L22 pressure plate.** Walk into sector 163 to run activity 7. It raises ceiling 193 from Z280 to Z380 and prints the source message.
53. **Trace original town form 20004.** Use `HARDFORM.C`, `TOWNUI.C`, `CLIENT.C`, and `MAPDESC/QUEST*.INI` for the single-player form, quest selection, and completion rules. L22 closes *Visions of Jugurtha*. The next quest in the selector is *The Knights of Andrew*, which starts at L25.
54. **Import the original single-player form resources.** Use the original town and prompt art, bitmap fonts, palette, and the seven quest descriptions. Build the town map and its content assets with `AA.CreateNativeWadTown`.
55. **Finish the source quest through L22's exit.** Import the journal at L22 thing 123. Returning to form 20004 with the journal turns it in and grants ten gold. Returning empty-handed displays the source failure bulletin.
56. **Use the original quest selector.** Previous and Next browse the authored list. Quest opens the original Accept and Cancel dialogue for a new adventure. A quest in progress uses its saved map. Map-by-map diagnostic access is available through the standalone review commands.
57. **Save the original town result and continue to L25.** A fresh process loaded the town checkpoint with ten gold, the successful quest result, and the archived L22 switches and gate heights. After selecting *The Knights of Andrew* and accepting Lady Marg's briefing, the replay arrived at the authored L25 spawn at `(298, 1636)`.
58. **Replay the standalone town loop.** Both journal outcomes were verified from the original L22 spawn using regular movement and use controls. The first-quest briefing also launched L30 with the character and clock intact. The old L30 checkpoint loaded successfully and all nine WAD regression tests passed.

- Run `AA.ExportNativeWadPortReview TownTour <OutputPng> [ImageSize]` in standalone L22 with `?WadItems=200?WadReady=0?WadXP=2105?WadLevel=2?WadNextXP=36000`. Use `TownFailure` with the same start for the empty-handed case. Start `/Game/Maps/WadTown?WadLoad=NativeWadTownReview` in a fresh process and run `TownReload`. Use `TownStart` in a fresh town for the original first-quest launch.
- The screenshots are in `Saved/Wad-L22-Town-Review.html`.
- The Inn, Store, and Bank each have a separate original form. The town displays their original buttons. Those forms are follow-up work.

### Town scrolling and the Church of Andrew

59. **Verify original Town Hall scrolling.** The first quest briefing moved one line for the Down key and one for the mouse wheel. The up-arrow hit target and Up key reversed those moves. The final screenshot matched the initial one pixel for pixel.
60. **Run L25's timed passages.** Imported all eleven activities from `S25.SRP`. E on line 852 lowered bridge floor 158 from Z−72 to Z−208. Sector 105's pressure plate opened gate 100 from Z−44 to Z72. The player crossed both while they were open, and each returned to its initial height after the authored nine-second cycle.
61. **Run L25's paired pressure plates.** Verified the startup heights, the ignored repeat entry into sector 15, and the reset from sector 17. Entering both valid plates in sectors 15 and 18 raised ceiling 130 from Z136 to Z240 and activated generator 0.
62. **Generate and use L25's authored key.** A fresh process restored the pending generator and released item 258 once. The test confirmed door 148 refused E without the key, then consumed the collected key on use. A second restart checked the permanent unlock.

- The L25 walkthrough starts at the authored spawn, then stages the player at the original approaches for the timed passages, plates, and key door. Each crossing, plate entry, and use action runs through normal player controls.
- Run `AA.ExportNativeWadPortReview TownScroll <OutputPng> [ImageSize]` in standalone Town Hall and `ChurchPassages` in standalone L25. With the knights active, use the review character options `?WadMaxHealth=10000?WadHealth=10000` for the timed waits. Start L25 with `?WadLoad=NativeWadChurchKeyReview` for `ChurchKeyReload` and `?WadLoad=NativeWadChurchDoorReview` for `ChurchDoorReload`. The older L30 checkpoint, the original Town Hall checkpoint, and all nine WAD regression tests passed.
- The screenshots are in `Saved/Wad-Town-Scroll-and-L25-Review.html`.

### The Church of Andrew and the route into L20

63. **Place both authored knight variants.** Imported two mounted knights and seven crossbowmen in L25 from Thing types 1008 and 1009. Their original sprite poses, collision sizes, 3,000 health, bolt artwork, and combat values run through the native encounter code.
64. **Verify the L25 encounters.** Staged approaches isolated source Things 3 and 6. Each knight hit the player for 300 with its own bolt. Using a readied dagger against the crossbowman reduced its health and awarded experience.
65. **Build native L20 and run its original activities.** Imported 142 sectors, 668 lines, 15 ordinary doors, 16 knight placements, and all seventeen activities from `S20.SRP`. Player input verified floor 16's nine-second cycle to Z−300 and back to Z−16, and the piercing bolt from sector 41.
66. **Continue the active quest through L25's authored exit.** The replay accepted *The Knights of Andrew* in Town Hall, loaded the tested key-door state, then walked through door 148 and into sector 149. L20 received the character and clock at its authored spawn `(-1084, 4630)`. A fresh process restored the quest resume point and archived L25 state.

- Run `AA.ExportNativeWadPortReview ChurchKnights <OutputPng> [ImageSize]` in L25 with `?WadItems=200?WadReady=0`. Run `ChurchJourney` in Town Hall with `?WadLoad=NativeWadTownReview` after generating the key-door checkpoint with `ChurchKeyReload`. Run `ChurchL20Reload` in L20 with `?WadLoad=NativeWadChurchL20Review`. All nine WAD regression tests passed, and the older L30 checkpoint still loaded.
- The screenshots are in `Saved/Wad-L25-Knights-and-L20-Review.html`.

### Church scenery and the route into L60

67. **Restore the original scenery placements.** Imported 102 placements in L25 and 82 in L20 from the source Things, including the churchyard trees and tombs, the L20 groves and columns, and the original furnishings and torches. The original object definitions supply sprite dimensions, viewing angles, animation timing, and mounting flags.
68. **Check scenery collision in play.** First-person input stopped at L25's solid courtyard tree and crossed L20's passable rock. The native bolt collision sweep respected both objects' passability. The captures show the church grounds, L20 columns, and grove.
69. **Open L20's authored exit.** Walking into sector 108 ran activity 12 from `S20.SRP`. It raised ceiling 87 from Z112 to Z190 in the authored three seconds and displayed the original exit message. The player crossed the open gate and followed Thing 138 in sector 131 to L60.
70. **Import L60 and resume the active quest there.** Built all 258 sectors and 1,276 lines, the original spawn, 84 scenery placements, 25 knights, and startup activity 0 from `S60.SRP`. A fresh Unreal process restored the quest and the archived L20 gate from a checkpoint at L60's original spawn `(-3076, -3)`.

- The walkthrough stages the player at the original pressure-plate and gate approaches. The plate entry, gate crossing, and level transition use regular player movement.
- Run `AA.ExportNativeWadPortReview ChurchScenery <OutputPng> [ImageSize]` in L25 and `ChurchL20Scenery` in L20. Use `ChurchL20Exit` in L20 with `?WadLoad=NativeWadChurchL20Review`, and `ChurchL60Reload` in a fresh L60 process with `?WadLoad=NativeWadChurchL60Review`.
- The L25 knight combat and L20 timed-floor and trap replays still pass, along with all nine WAD regression tests. The screenshots are in `Saved/Wad-Scenery-and-L60-Review.html`.

### Classic gameplay HUD

- The native gameplay HUD uses the original `MAINBACK` frame, palette, `FontTiny`, potion frames, Priest rune buttons, item sprites, and the inventory and statistics panels. It appears at level entry, including on maps without scripted activities.
- The banner reads live health, mana, food, water, carried weight, the ready hand, and queued runes. The original inventory grid and statistics panel display the current character. The remaining original banner forms are pending.
- Numpad 7 opens the inventory. Tab releases the mouse for the banner buttons. Select an inventory item and click the ready-hand box to equip it. The USE and CAST buttons use the same gameplay handlers as their keyboard controls.
- Run `AA.CreateClassicWadHUD` in Unreal Editor to regenerate the assets under `Content/WadInterface`. Run `AA.ExportNativeWadPortReview ClassicHud <OutputPng>` from a fresh standalone L301 game to capture the gameplay frame, inventory, statistics, and runes.
- The L301 replay readied the dagger and cast Rejuvenate through the banner, spending 1,000 mana and restoring the player's 300 missing health. Town Hall scrolling and all nine WAD regression tests passed. The captures are in `Saved/Wad-Classic-HUD-Review.html`.

### The Knights of Andrew quest completion

71. **Restore L60's original puzzle activities.** Imported 19 activities from `S60.SRP` and four knight generators from `L60.GEN`. The replay worked the timed lift, entered the teleport and healing pool, turned all five statues from their pressure plates, and walked across a raised floor. Checkpoints carry statue angles, script state, moving sector heights, and generator progress.
72. **Recover the Necklace of Andrew.** Imported the necklace from Thing 505 on the original pedestal. Walking onto the pedestal lowers the exit gate and activates the crossbowman generator. The replay collected the necklace with E and crossed the gate into the exit corridor.
73. **Verify the Town Hall turn-in.** The original exit returns the player to Town Hall. Lady Marg takes the necklace and awards 20 gold. Returning empty-handed shows the original failure dialogue, awards zero gold, and leaves the pickup in the archived L60 state.
74. **Replay the quest across the four maps and restart in L60.** Linked the Town Hall, L25, L20, L60, and return-to-Town segments through checkpoints. Fresh Unreal processes restored the solved statue crossing and later restored the necklace, open exit gate, and generator progress. The review stages the player at selected approaches and uses regular controls for the interactions, crossings, and map transitions.

- Run `ChurchJourney` and `ChurchL20Exit` as above. Run `AA.ExportNativeWadPortReview ChurchL60Puzzles <OutputPng> [ImageSize]` in L60 with `?WadLoad=NativeWadChurchL60Review`. Run `ChurchL60Relic` and `ChurchL60Empty` from `NativeWadAndrewPuzzleReview`, then `ChurchL60Return` from `NativeWadAndrewRelicReview`.
- All nine WAD regression tests passed. The classic HUD and both Town Hall outcomes are visible in `Saved/Wad-Andrew-Quest-Review.html`.

### Elmore's Retreat and scenery facing

75. **Launch Elmore's Retreat from Town Hall.** Continued from the completed Knights of Andrew checkpoint. The original quest selector and briefing led to L64's authored spawn. Startup activity 0 granted journal pages 423 and 424, which appear in the original journal frame.
76. **Import L64 and restore its mechanisms.** Built 363 native sectors and placed 117 scenery objects. The replay used the original pressure plate and wall switch. After a restart the lift finished returning and reset both switch faces. Both flood activities moved the sectors to their authored heights.
77. **Take L64's authored exit into L65.** Walking into the sector for Thing 227 reached L65's spawn at `(-852, 2282)` with the active quest, journal and archived L64 state.
78. **Import L65 and prove its first teleporter.** Built 183 native sectors and placed 84 scenery objects. Line 847 enabled the sector 115 teleporter. Reload restored the destination at `(1628, 2370)` and its switch state. The replay inspected the floor barrel and hanging brazier from two player views.

- Floor scenery turns horizontally around its bottom center. Hanging scenery turns around its top center. Both follow their authored mounting surface, including moving sectors. The displayed sprite can change frames without moving that point.
- The replay stages the player near selected mechanisms and uses regular movement and use inputs for the tested switches, crossings, and map transitions. Start `ElmoreLaunch` in Town Hall from `NativeWadAndrewTownReview`, which `ChurchL60Return` writes. Then use `ElmoreL64`, `ElmoreL64Reload`, `ElmoreL65`, and `ElmoreL65Reload` with the checkpoints from each preceding mode.
- The captures are in `Saved/Wad-Elmore-Retreat-Review.html`.

### The L65 exit and the lighthouse

79. **Restore the barbarian encounters.** Imported 28 barbarians in L64 and 31 in L65 across four authored types. Verified the Bronze Barbarian's 350-damage melee hit and the White Barbarian's 100-damage magic dart using the native encounter system.
80. **Complete the L65 teleporter and lift route.** From the first teleporter checkpoint, line 533 enabled the second teleporter to `(2234, 1600)`. The replay opened both doors, rode the lift down, and crossed Thing 239 into L34.
81. **Import the L34 lighthouse.** Built 186 sectors and 915 lines, placed 88 scenery objects and 30 skeleton knights, and imported all nine script activities. The replay took the pressure trap to Z-200 and back and used line 914 to lower the exit floor from Z104 to Z32.
82. **Carry Elmore's Retreat through L34 into L151.** Crossed the opened lighthouse gate and entered the exit for Thing 180. A fresh Unreal process restored L151's authored spawn with the active quest, both journal pages, and the archived L65 switch and L34 exit.

- Run `AA.ExportNativeWadPortReview ElmoreCombat64 <OutputPng>` in L64 and `ElmoreCombat65` in L65. For campaign travel, use `ElmoreL65Exit` from `NativeWadElmoreTeleportReview`, then `ElmoreL34` from `NativeWadElmoreL34Review` and `ElmoreL151Reload` from `NativeWadElmoreL151Review`.
- The replay uses regular movement and interaction from selected approaches. It verifies traversal of the opened L34 gate, then stages the player near the exit beyond the skeletons and solid glowers.
- All nine WAD regression tests passed. The encounter and travel screenshots are in `Saved/Wad-Lighthouse-Review.html`.
- L34's Shadow and Lich encounters remain to be ported.

### The reservoir and the Orb of Defense

83. **Take the original cave exit from L151.** Restored the previous campaign checkpoint, collected Key 5 from Thing 223, opened door 48, and crossed Thing 84 into L152 at its original spawn. A separate older lift checkpoint resumed its pending timer and completed two lift cycles after the pickup update.
84. **Import L152 and activate the reservoir mechanism.** Imported 72 sectors, 306 lines, and all three script activities. Collected Key 6 and pressed the hidden switch to lower floor 27 from Z24 to Z-476. Sector 69 fired its five lock missiles and locked door 70. Thing 266 took the player to L153 with both earlier keys and the archived L152 state.
85. **Import L153 and recover the Orb of Defense.** Imported 225 sectors, 1,340 lines, all 34 script activities, four key doors, and the two local keys. The replay reopened the western gate, verified the vault rejected an incorrect combination, waited through its two-second reset, and entered the vault after solving the correct combination. It collected and saved the original orb from Thing 206.
86. **Complete Elmore's Retreat at Town Hall.** A fresh Unreal process restored the saved orb and carried it through L153's return teleporter. Captain Argus took the orb and awarded 150,000 experience. The same exit reached the original failure outcome when used empty-handed and awarded no experience.

- The replay uses regular controls from staged approaches. Run `ElmoreL151Exit` from `NativeWadElmoreL151Review`, then `ElmoreL152` and `ElmoreL153Puzzles` from the arrival checkpoints. Run `ElmoreOrb` and `ElmoreOrbEmpty` from `NativeWadElmoreVaultReview`, and `ElmoreOrbReturn` from `NativeWadElmoreOrbReview`.
- All nine WAD regression tests passed. Vault, orb, and Town Hall screenshots are in `Saved/Wad-Orb-of-Defense-Review.html`.
- Elmore's encounter in L152, the Gargoyles in L153, and a continuous walkthrough through all four key doors remain to be verified or ported.

### Exiguus and the Undead

87. **Trace the next Town Hall quest.** `QUEST3.INI` defines Castle Arius (L231), the underground traps (L75), the next keep (L10), and the cemetery (L12). Exiguus drops item 955, the Orb of the Undead. King Gregor takes the orb and awards 50 gold.
88. **Import L231 and launch from Town Hall.** Bring the completed Elmore character into the next quest at the original start `(-2208, 2416)`. Startup activity 0 adds journal page 403. The map has 272 sectors, 1,122 lines, and 12 imported activities.
89. **Verify the castle lift and exit.** Step onto the plate for floor 263, check that it rises from Z0 to Z320 and returns, then cross Thing 141 into L75 at `(2096, 3034)`.
90. **Import L75 and its traps.** Import 196 sectors, 979 lines, and 17 activities, including crushing ceilings, timed floors, and the Evil Knight trap.
91. **Exercise the traps and move on to L10.** Verify the crusher in sector 105, the wall switch and first exit lift, the two knights released by sector 148, and travel through Thing 140 to `(-1268, -531)`.
92. **Import L10 and the moving key platform.** Import 242 sectors, 1,051 lines, and 27 activities. Attach the local Key 6 pickup to its original moving floor.
93. **Verify L10's two switches and key door.** Check that floor 120 lowers first to Z100 and then to Z0, collect Key 6, cross door 200, and use Thing 234 to reach L12 at `(4530, 1171)`.
94. **Import L12 and its cemetery mechanisms.** Import 304 sectors, 1,368 lines, 15 activities, and the four electric skeleton generators. The original exit plate lowers floor 294 and pauses the generators.
95. **Verify the cemetery key door and Exiguus's quest drop.** Use L12's Key 1 at door 81. Verify that Exiguus drops the Orb of the Undead only on defeat and that the collected orb survives a checkpoint reload.
96. **Verify both Town Hall outcomes.** Use Thing 576 to return to King Gregor. Turn-in consumes the orb for 50 gold and no quest XP. Return empty-handed to verify the failure result and no reward.

- The replay drives movement and interactions from staged approaches. The Exiguus drop review applies a controlled finishing hit.
- Use `AA.ExportNativeWadPortReview` with `UndeadLaunch`, `UndeadCastle`, `UndeadTraps`, `UndeadKeep`, `UndeadBoss`, `UndeadReturn`, and `UndeadEmpty`. `ElmoreOrbReturn` writes the starting Town Hall checkpoint, `NativeWadElmoreTownReview`. `UndeadReturn` writes the completed checkpoint, `NativeWadUndeadTownReview`.
- All nine WAD automation tests passed. The gallery is `Saved/Wad-Exiguus-Review.html`.
- A continuous route, combat balance, Exiguus's summoning, other creature combat effects, the boulders, the flying-potion generator, and the Blue Wizard generators remain follow-ups.

### Temple of Astarte, L51 and L52

97. **Trace the Temple of Astarte quest.** `QUEST4.INI` gives the route L51 → L52 → L41 → L44. The original L44 encounter with Elymas drops his notes (item 380). John takes the notes and awards 35 gold and 200,000 experience.
98. **Import L51 and launch from Town Hall.** Preserve the completed Exiguus character and enter the Temple at the source start `(-160, -189)`. L51 has 173 sectors, 787 lines, and 13 imported activities.
99. **Verify the poison corridor and L51 exit.** Run activity 10 from the original sector, fire the two type 887 bolts, and check player damage and poison. Press the switch on line 674, cross gate 115, and reach L52's source start `(760, 3540)` through Thing 234.
100. **Import L52 and verify its timed gate and pit rescue.** Import 171 sectors, 774 lines, and 16 activities. Step on all three plates and check the ceiling rises to Z25, Z50, and Z75, then returns to Z0 and resets the plate variables. Verify the pit teleporter and save before the original L41 exit.

- Sector 154 had a one-unit linedef overlapping a longer boundary. The importer splits and normalizes overlapping edges when the original footprint fails. The same repaired outline is saved for runtime sector membership. The WAD regression checks the courtyard's exact area, all three plate cutouts, and the outside point near the overlap.
- Use `AA.ExportNativeWadPortReview` with `TempleLaunch`, `TempleL51`, `TempleL52`, and `TempleL52Reload`. Begin with `NativeWadUndeadTownReview` and continue from each saved arrival. The final checkpoint, `NativeWadTempleExitReview`, is beside the L41 exit.
- The replay uses regular controls from staged approaches. All ten WAD automation tests passed. The gallery is `Saved/Wad-Temple-Astarte-Review.html`.

### Temple of Astarte, L41 and L44

101. **Continue from the L52 checkpoint.** Cross the original exit at Thing 238 and enter L41 at `(7104, 4152)`.
102. **Import L41 and exercise its gate and key.** Preserve 358 sectors, 1,358 lines, 41 activities, and the two source keys. Press the timed gate switch, collect the local Key 6, use its door, and take Thing 323 to L44.
103. **Import L44 and recover Elymas's notes.** Preserve 116 sectors, 465 lines, and 17 activities. Trigger the three pressure plates, check gate 68, and verify that Elymas's packed type 50158 drops item 380 on defeat. Collect and use the local Key 3 at the final passage.
104. **Turn in the notes.** Use L44's original Town Hall exit and check the notes are consumed for 35 gold and 200,000 experience. Save the result for The Lost City.

- The verified replays use `CampaignL52`, `CampaignL41`, and `CampaignL44Chain`. The L44 replay applies a controlled finishing hit to verify the defeat and pickup. The Town Hall checkpoint is `NativeWadCampaignTown4Review`.

### The Lost City

105. **Launch The Lost City.** Accept the quest from the Temple result and reach L74's original start at `(-1120, -454)`.
106. **Import L74 and verify its switch.** Preserve 191 sectors and 1,009 lines. Raise gate 104 before taking the original exit into L73.
107. **Import L73 and verify the three plates.** Preserve 252 sectors and 1,120 lines. Raise gate 118 through Z175, Z195, and Z232, then take the original exit into L72.
108. **Import L72 and return with the wand.** Preserve 340 sectors and 1,642 lines. Lower the hidden floor, collect the original Wand of Disintegration, and use L72's exit. The Town Hall turn-in consumes the wand and awards 75 gold.

- Sector 184 in L73 has two crossing linedefs. The loader splits and triangulates the boundary at the exact fractional intersections. The saved runtime outline retains those positions. The repaired L52 package still uses its saved integer outline.

### Destruction

109. **Launch Destruction.** Accept the quest from the Lost City result, arrive at L32, and receive journal page 603.
110. **Import L32 and open the paired gates.** Preserve 250 sectors, 1,158 lines, and 13 activities. Press the original delayed switch and verify ceilings 108 and 233 before the exit to L71.
111. **Import L71 and operate the platform.** Preserve 122 sectors, 639 lines, and five activities. Enter sector 86 to raise floor 84 to Z200 and enable its switch. Use the switch to lower the floor to Z16, verify the timed return to Z200, and take the original L70 exit.
112. **Import L70 and raise gate 56.** Preserve 103 sectors, 651 lines, and 12 activities. Press the original switch and take the exit to L61.
113. **Import L61 and check the ceiling plate.** Preserve 191 sectors, 789 lines, and four activities. Enter the authored pressure plate, verify ceilings 143 and 62, and collect the local Key 8 before proceeding to L40.
114. **Import L40 and recover the Orb of Destruction.** Preserve 129 sectors, 712 lines, and 16 activities. Verify the two-switch gate, collect the original orb and Key 4, and open door 123.
115. **Return to Town Hall.** Exercise the original L40 exit and verify the orb is consumed for 100 gold and 350,000 experience.
116. **Audit the imports.** Match all 30 original campaign map files to native `.umap` packages. Verify the package signatures and that every map has generated Unreal assets, with 15,438 assets across the 30 map folders.

- The L71 generator parser skips the stray character on its comment line, following the original parser's rule for recognizing generator records. The L61 torch on a one-sided wall uses the wall's source sector and retains its exact authored coordinates.
- `CampaignTown6Chain` starts from `NativeWadCampaignTown5Review`. `CampaignL71Chain` resumes from the recorded L71 arrival. The successful Town Hall return saves `NativeWadCampaignTown6Review` with the orb consumed.
- All 11 WAD automation tests passed, including the L52 overlapping boundary and the L73 crossed boundary. The final three quest replays use staged approaches and regular controls, with a controlled finishing hit for Elymas. The screenshots are in `Saved/Wad-Campaign-Finale-Review.html`.

### Campaign playthrough and standalone verification

117. **Start Visions of Jugurtha with a fresh character.** The `FreshCampaign` review opens Town Hall with no save, starts L30 with zero XP and no items, and walks L30, L301, L531, L31, and L22 with the same character. The replay fights both Guardians using regular controls and turns in the journal for 10 gold. `NativeWadFreshCampaignTown0` records the actual Town Hall return.
118. **Continue through the campaign in quest order.** The same character accepted The Knights of Andrew, crossed L25's timed bridge and gate, activated both pressure plates, and used the generated key. In L20 the replay worked the switch wing and saved at the reopened timed gate. From that checkpoint it jumped onto the return step, opened the exit through the original pressure plate, and crossed into L60. The L60 replay walked from the original spawn, rotated all five statues, climbed the raised crossing, and recovered the Necklace of Andrew. Lady Marg took the necklace and awarded 20 gold. Continue with Elmore's Retreat from `NativeWadCampaignTown1Review`.
119. **Verify the Windows build.** The archive manifest contains Town Hall and all 30 campaign levels. The packaged executable starts in Town Hall and launches L30. In the same build, the Destruction replay starts from the verified Lost City checkpoint, runs all five finale maps, and earns 100 gold and 350,000 experience. The build saves the quest result, and a new process restores that save. The archive is `Saved/Builds/Campaign-Win64/Windows`.

- The L20 review resumes the same character and records movement frames over 100 ms. The offscreen Play-in-Editor sample started in 0.553 seconds and reached the south door with 1,854 movement frames sampled. The longest was 36.2 ms, and the log showed no NavMesh rebuild. The reported hitch, wall visibility, and stuck position still need a reproduction on the affected route. The WAD automation suite passed all 13 tests, including camera near-plane clearance and wall penetration recovery.
- The staged L60 checks also verified the timed lift, teleport, and healing pool from `NativeWadCampaignL60Review`. `CampaignL60Chain` walks the continuous route from the original spawn, with rests in cover and normal Rejuvenate inputs.

### Gameplay parity follow-ups

- [ ] Port L41's remaining wizard generators and scripted projectile effects. Match Elymas's homing projectile and palette in L44.
- [ ] Match L73's acid projectile and the mana drain in the recovery sector.
- [ ] Port L32's homing missile and the remaining creature generators in L71, L70, and L61. Implement L61's deferred scripted effects.

### Asset follow-ups

- [ ] Find the original `MARBGRAY` texture and restore it to the L30 import. The supplied `PICS.RES` lacks it, so the affected lower wall bands use the default material when exposed. Check a raised floor at sector 147 after restoring the art.
- [ ] Restore `MARBGRAY` and `AASHITTY` in L31 when the source textures are available. The L31 importer uses the default material for those surfaces.
- [ ] Restore `MARBGRAY` in L22. The import uses the default material for the affected surfaces.
- [ ] Restore `AASHITTY` in L25 when the source texture is available. The native import uses the default material for that surface.
- [ ] Restore `MARBLE1` and `MARBGRAY` in L60 when the source textures are available. The import uses the default material on the affected surfaces.
- [ ] Restore `ASHWALL2` and `AASHITTY` in L64 and `MARBGRAY` in L65. Find authored sound samples 5, 1508, and 1510 referenced by their scripts.
- [ ] Restore `STARTAN3` and `AASHITTY` in L34 when the source textures are available.
- [ ] Find L153's authored sound sample 15. The supplied sound archive also lacks L151's sample 1514, so reimport preserves any lift sample already embedded in the native map.
- [ ] Restore `BLOOD3` in L51 and `MARBGRAY` in L51, L52, L41, L44, L32, and L40 when the textures are available.
- [ ] Restore `CEIL3_2` in L74, `AASHITTY` in L74, L71, and L61, and `FLOOR4_8` in L72 and L61 when the textures are available.

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
