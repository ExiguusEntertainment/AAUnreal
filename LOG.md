# Development log

Progress is appended here while the unattended playable-loop work continues. PLAN.md remains the feature checklist; a passing focused replay does not imply complete campaign parity.

## 2026-11-27 13:15 CST

- Resumed from `cf98408a` with the carried original-class Priest's successful L61/L40 route.
- Built the Editor module successfully and ran `FinalCampaignReload` in a fresh process. Verified Priest class 4, level 8, 825,122 XP, all seven quest-return maps, consumed Orb of Destruction, 101 gold, and no repeated reward after dismissing the result pages normally.
- Saved `NativeWadOriginalClassCampaignCompleteReview`. Evidence: `Saved/Logs/PlayableLoops-OriginalClassFinalReload.log` and `PlayableLoops-CampaignTown6Chain-OriginalClassL61GroundedInvisibility.log`.
- Committed and pushed milestone 218 as `2f2553cc` (`Verify original-class Destruction return and final save restart`). Loop 5 remains open for missing creature families; loop 13 still requires the final continuous Windows campaign.
- Started parallel source work on missing Destruction creatures, consumable gaps, and the classic Notes/Journal forms. Builds, imports, and gameplay readers remain serialized to avoid sharing live assets or DLLs.
- Confirmed Cure Poison item 807 is skipped because its source uses `SET PLAYER POISON LEVEL` with zero data. A bounded importer fix and use regression are underway.
- Confirmed the current HUD's Journal button occupies the original Notes position. Notes is a separate editable character text field, not the authored journal pages. Restoring both source positions and the original Notes/Journal switches is underway.
- Existing unrelated editor settings, review artifacts, launch scripts, and session handoff files are being preserved.

## 2026-11-27 13:23 CST

- Source changes are ready for verification: both poison cures; classic Notes/Journal editing and persistence; Wyvern, Orange Dragon, Hydra, and Momma Dragon encounters; and unrestricted original Orange Wizard/Gargoyle placements.
- New creature behavior includes the Wyvern's grounded acid attacks, flying dragons that advance while firing, and the Hydra's liquid confinement, dipping/emergence, and sink death. The original Momma Dragon uses DRAGON4 art. Its source treasure key is commented out, so no extra key drop is being invented.
- New regressions cover cure rounding/consumption, Notes editing and the source text limit, and creature behavior. A combined serial Editor build has started; none of these changes is marked verified yet.
- Prepared normal-input replays for partial cure → restart → full cure → second restart, and Notes typing/editing → original Journal switches → map travel → fresh-process reload. Rendered Notes verification will follow the headless checks.
- Jewelry audit found a broader missing system: the original equips one necklace and four rings independently of the ready hand. The current port's passive jewelry is tied to the single ready hand. Equipment slots and persistence must be restored before claiming Bank jewelry parity.

## 2026-11-27 13:25 CST — cross-review findings

- The new creature review found a missing Hydra death transition: the source removes it after sinking, while the first implementation left an exposed corpse. The fix must also retain defeated state in checkpoints so hiding the corpse cannot make it respawn on reload.
- Flight needs a distinct source speed cap. Earlier step 212 correctly removed Unreal's inherited 600-unit default but equated the flying cap with walking step speed. The source separately specifies acceleration and maximum velocity. The new dragons expose this difference; the existing flying families are being audited too.
- Prepared `scripts/refresh_native_encounters.py`. It checks restored actor counts against the original WAD THINGS records and checks that existing native surface mesh references and transforms survive the gameplay refresh. Expected new L40 placements are four Orange Dragons, six Hydras, and one Momma Dragon; L71 has one Wyvern.
- The serial Editor build is still compiling the changed headers and modules. No build/import/gameplay verification result is claimed for this batch yet.

## 2026-11-27 13:27 CST — remaining spell audit

- Compared the native catalog with all 108 source spell definitions. Current coverage is 39: 35 projectile spells, three timed spells, and Rejuvenate. The 69 remaining definitions are now explicitly listed by effect group in PLAN.md.
- Confirmed the original has no extra spell-learning or minimum-level gate for these records: school and owned compatible runes control access. Runes remain physical inventory and are not spent on casting.
- Found a small casting discrepancy to correct: the source clamps a negative success threshold to zero, retaining a one-percent success chance on roll zero. The native generic spell path currently always fizzles when the calculated threshold is negative.
- Selected a bounded next spell group that reuses existing state: Purify Blood, Heal, Arcane Feather Fall, Mage Shield, and Mage Fly. The broader missing timed statuses remain separate work.

## 2026-11-27 13:28 CST — build correction

- The first combined build compiled the creature, importer, HUD, cure, and regression changes but failed on one Slate reply API name in Notes input. Corrected `FReply::SetKeyboardFocus` to this engine version's `SetUserFocus`; verification is still pending.
- Applying the cross-review Hydra death/persistence correction and distinct source flight caps before the next build. Grounded walking speeds remain separate. Full discrete flight acceleration, vertical slope, and source edge-escape navigation remain explicit follow-ups.

## 2026-11-27 13:39 CST — verified milestones 219–221

- Corrected build passed. All 22 WAD regressions passed, including both cures, Notes editing, independent flight caps, Hydra movement/death, and a complete level-checkpoint restore that does not resurrect a hidden defeated Hydra.
- Refreshed all five affected maps. Verified 42 restored creature placements against original THINGS records, with all 6,647 checked native surface meshes and transforms preserved. Evidence: `Saved/Logs/PlayableLoops-RestoredEncounters-PublicComponentsImport.log`.
- Imported the six original Notes graphics and refreshed the cure catalog. The first HUD import attempt used an incorrect command alias; the corrected import uses `AA.CreateClassicWadHUD`. Initial encounter-script checks used private/non-reflected APIs; the final script validates public movement, collision, art, and original thing identities.
- Poison-cure gameplay checks passed across three processes: partial cure 5→3, full cure 3→0 after reload, and a second reload with both potions consumed. Health, mana, unrelated inventory, and the 200-water cost were checked.
- Rendered Notes and Journal reviews passed normal typing/editing, tabs/newlines, scrolling, reciprocal buttons, focus return, map travel, and fresh-process reload. The null-RHI button fixture lacked a usable viewport, so interface verification uses real offscreen rendering.
- Inspected the original Notes form capture: [Notes](Saved/PlayableLoops-Rendered-Notes-notes.png) and [reloaded Notes](Saved/PlayableLoops-Rendered-NotesReload-notes.png). The known magenta-sky/palette question remains open in PLAN.md.
- The next parallel assignments are active: early-campaign creature families; necklace/four-ring equipment plus source passives; five additional utility spells and classic Options/Controls. Their source edits remain separate from the built snapshot being committed now.

## 2026-11-27 13:43 CST — checkpoint pushed and L40 replay

- Committed and pushed verified milestones 219–221 as `58addd5c` (`Restore classic Notes, poison cures, and Destruction creatures`), including 523 LFS objects / approximately 25 MB of saved content. The next parallel source changes remain unstaged.
- Replayed the carried Priest's actual L40 arrival with the restored enemies active. Both lifts, both gate switches, flight supplies, ledge crossing, Orb of Destruction, and Key 4 pass. Orange Dragon Thing 168 defeats the character on the return passage before the final door. Evidence: `Saved/Logs/PlayableLoops-CampaignTown6Chain-OriginalClassRestoredL40Encounters.log`. Loop 5 remains open; the earlier completed turn-in was before these encounters were restored.
- The next source batch now covers all four early creature families, jewelry slots and Bank passives, source melee variance/critical rules, five utility spells, classic Options/Controls, and creature Look. Ordinary Elf/Druid treasure drops and remaining conservation effects are also being implemented. These are pending integration build and runtime checks, not completed checklist claims.

## 2026-11-27 13:54 CST — second batch integration

- Froze the parallel source edits and started the next combined Editor build. The batch now includes 98 early-campaign creature placements, Elf/Druid drops, necklace/four-ring equipment, Bank attribute/regeneration passives, conservation items, six new utility spells including Sustenance, and classic Options/Controls/creature Look.
- Added normal UI reviews for original jewelry placement/remove/replace with a readied weapon and for Options/Controls persistence. Updated the elemental melee review to check the source variance/critical roll and exact typed result rather than assuming a fixed hit.
- Cross-review found periodic poison damage ignored poison protection. Added the source guard while retaining poison decay and a real needs-tick regression before the build.
- The build found one Options-slider coordinate type conversion; corrected that line for the next incremental pass. Other pending source audit fixes are the magical shield cap/order, Town ready-hand flight reactivation, and avoiding float-scaled treasure random draws.
- The original has a shared 4,096-entry random table. New treasure helpers preserve the item/quantity decision for each input; native random selection does not yet reproduce that synchronized table or its exact sequence/distribution. This remains a source-parity and future multiplayer consideration.

## 2026-11-27 14:20 CST — 35 regressions and rendered checks

- All 35 WAD regressions pass in `Saved/Logs/PlayableLoops-JewelrySpellsFormsCreatures-VerifiedAutomation.log`. The initial utility-spell fixture matched modifier shortcuts for keypad 6/7; matching the complete plain-key chord corrected the fixture. The runtime spell definitions passed afterward.
- Refreshed and validated all ten affected maps, including all 98 early-family placements. The combined checked placement count is 140, and all 11,259 checked native surface mesh references and transforms were preserved. Evidence: `Saved/Logs/PlayableLoops-EarlyCreatures-AllTenMaps-Import.log`.
- The original toggle graphics exist as `.PIC` files although their form references omit the extension. Added the real source-file fallback and completed the HUD/item import. The source Look catalog contains 47 creature descriptions, including packed Elymas identity 50158.
- Rendered jewelry checks pass placement in the original neck/four-ring boxes, weapon retention, independent effects, removal/replacement, save, and new-process restoration. The controlled fixture explicitly sets Accuracy 35 and uses an actual amulet in the neck slot; earlier fixture assumptions about a water-walking ring and legacy stat order were corrected.
- Inspected [worn equipment](Saved/PlayableLoops-VerifiedRendered-WornJewelry-equipment.png). Source art and the five original jewelry locations are visible.
- Rendered Options/Controls passes its in-session controls, gain ratios, toggles, scrolling, and focus return. Its new-process check found a real persistence defect: the custom GameUserSettings section is not written to disk. A dedicated user-config fix is underway; Options is not marked complete yet.
- Creature Look and source elemental-melee rendered checks are continuing independently.
- The next source phase is active and remains separate from the staged built snapshot: temporary effect ownership and additional source spells; full-type item identification/common starter grants; and the source DRK42 missing-texture fallback with material-only refresh support.

## 2026-11-27 14:27 CST — verified checkpoint scope

- Creature Look passed its rendered original portrait/text, source identity, wall-occlusion and mouse-look-return checks. Inspected [creature Look](Saved/PlayableLoops-VerifiedRendered-CreatureLook-creature.png).
- Source elemental melee passed with an actual raw roll of 110 producing 69 damage after chain's electric bonus and one resisted element. A new process retained the scroll effect, observed expiry, and exercised the enchanted mace's ready/unready behavior.
- Found a second real interface issue: inherited Unreal debug keys execute in addition to classic bindings. F1 switched to wireframe and compiled an extra shader; F5/F9 and page keys also conflict. Removed those specific debug bindings in DefaultInput.ini. The rerun stays lit, with no view-mode change: [Controls](Saved/PlayableLoops-KeyProof-Options-controls.png). This is not attributed to the user's still-unreproduced movement hitch.
- Milestones 222–228 are ready as an intermediate checkpoint. Options in-session UI is verified, but disk persistence remains explicitly open until its dedicated-file fix passes the next new-process test. Other pending source changes are excluded from this staged checkpoint.

## 2026-11-27 14:35 CST — third batch integration

- Pushed verified intermediate checkpoint `cebc5cbf` with the early encounters, wearable jewelry, source utility spells and classic forms (677 LFS objects, about 40 MB).
- All three subagents are assigned independent work: strict owner-state validation and remaining spell audit; identification and real Options disk persistence/retry; source missing-picture fallback and an all-map preservation script.
- Started the combined Editor build for owner-aware temporary effects, 36 additional source spell definitions (81 total pending verification), original identified/unidentified descriptions, Identify scrolls/spells, common new-character grants, and DRK42 surface fallback.
- Added a normal-input item/owner integration review that saves, travels L30 to L301, restarts, observes independent natural expiry and saves an explicit empty registry. These new checks are pending, not completed claims.

## 2026-11-27 14:39 CST — source dependencies and integration checks

- Original Store code identifies an item only after a successful purchase. Added that behavior and actual purchase/refusal/preview regression coverage; merchant previews alone remain unknown.
- Audited the remaining 27 spell records: movement 9, Magic Map 8, night vision 5, creation 3, life protection 2. Creation requires a genuinely retained mouse hand; Death Ward requires an alive state independent of zero HP. These remain open.
- The all-map fallback verification now uses a narrow read-only side-texture getter and public component names instead of private reflection, avoiding the access issue previously encountered during encounter imports.
- First integration compile found an Unreal Actor field-shadowing error in an effect-owner parameter. Renamed it; a corrective build is required before any runtime verification. Added real-player normal-rune casting coverage beyond static spell metadata checks.

### Additional source parity findings

- Flight suppression still uses one legacy flag across equipped amulets. Earthbind on amulet A, equipping B, then removing B can incorrectly reactivate A; source suppression belongs to each equipment owner. Source Dispel also reruns equipped READY effects, which should reactivate Earthbound flying equipment. Record both for the next equipment-owner pass; the temporary registry does not close these differences.
- Source Feather Fall's main vertical integration implies a cap near 256.35 map units/second, whereas native currently uses about 16.02. The next movement pass must compare the discrete source integration before claiming exact trajectories.
- Source creature treasure audit distinguishes ordinary defeat from stealing. Mighty Hunter has no ordinary defeat treasure case; Orange Wizard journal loot requires page AccData 407 and GET-trigger handling rather than an ordinary carried scroll.


## 2026-11-27 14:43 CST — parallel implementation without build interference

- Created isolated source copies under Saved/AgentWork; active subagents are implementing retained mouse-hand transfers/persistence, six movement spells with jump rules, and remaining ordinary creature treasure.
- Main Plugins source remains frozen while the current Editor build and verification run. Agents will deliver reviewable patches; none of the next-phase copies are represented as tested or shipped.
- Audited normal earned L40 tactic: replace the unused third electric scroll with a seventh invisibility scroll (+50 copper), and use it before the first flight crossing. This needs a real replay from the legitimate Town provisioning save; no extra item grant or encounter suppression is authorized as evidence.



## 2026-11-27 14:49 CST — 45 regressions pass

- Editor build succeeds and all 45 WAD regressions pass in Saved/Logs/PlayableLoops-OwnersIdentificationFallback-FinalAutomation.log. This includes all33 new timed-status definitions cast normally, all3 Identify spell scopes, strict owner codec/migration tests, actual starter creation/purchase identification, and real-file Options failed-write retry.
- Integration caught missing ring417/418 catalog entries, an engine-outer error in an isolated GameInstance fixture, and absent source feedback after actual Identify rune casts. Corrected the catalog, fixtures and runtime feedback, then reran the full suite.
- Refreshed classic HUD/item descriptions for366 source types (catalog count to be checked against import log). The all30map material-only fallback validation is running next; owner/identification and Options process-restart reviews remain pending.


## 2026-11-27 14:54 CST — restart and all-map milestone verified

- All three owner/identification processes pass: normal identification/potion use and same-owner refresh; L30-to-L301 travel/save; separate owner restoration and independent natural expiry; another process confirms the explicitly empty registry and retained known/unknown item types. Profile: Saved/ItemOwnerReview-20261127-145152/Owners.
- Both rendered Options processes pass, including real disk reload from the dedicated per-user file. Inspected original Controls layout; user gains/toggles persist and F1 stays in the normal lit view. The existing magenta sky/mottled terrain remains a separate presentation audit.
- Material-only fallback refresh passes all30 maps:169 missing-name surface bindings,67,629 preserved component states,11,325 unchanged mesh asset hashes. The first Python pass found a non-reflected overlap getter; the corrected reflected property pass completed. No geometry was rebuilt.
- Rendered native L30 door147 opens in1.5sec, clears the capsule, reverses above the obstruction and recloses after its3.5sec hold. Inspected opening/open captures: original DRK42 brick fallback covers the raised wall bands correctly.
- Updated PLAN milestones229–233 and the remaining source gaps. Parallel MouseHand, Movement and Treasure patches are ready in Saved/AgentWork, still unintegrated/unverified. The separate L40 earned-tactic patch preserves every route action index and has not been run.
