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
