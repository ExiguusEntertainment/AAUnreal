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


## 2026-11-27 14:58 CST — verified checkpoint pushed; next integration started

- Committed and pushed9180c850, including221 files and188 LFS objects (about82MB). Source-owner/identification/Options/fallback changes and their saved content are online.
- Integrated the reviewed isolated mouse-hand, six-movement-spell and ordinary-treasure patches. Added explicit jump amulet612 import. The next Editor build is running with two compiler workers after confirming about25GB physical/18GB virtual memory available. New features are pending build/runtime verification.
- Dedicated retained-hand rendered/travel/restart reviews and normal-input fixture updates are being finished separately. Creation spells are being implemented in another isolated copy against the now-combined runtime.
- Palette audit corrected a visual assumption: L30 intentionally selects the purple2BACK01 backdrop; FART3/GRASS2 are authored mottled art. Confirmed remaining differences are GAME.PAL versus standard VIEW00.PAL for first224 view colors, and missing source minification/mips. Preserve the authored art; no guessed blue sky or new textures.



## 2026-11-27 15:04 CST — next core build and regressions

- Retained-hand/movement/treasure Editor build succeeds in166seconds with two workers; memory remained available. Imported source rules/art for373 types.
-54/55regressions pass, including full-page retained-hand placement, held restore/exclusivity, all6 movement rune casts, real jump/apex/landing and swept thin-wall Blink, exhaustive treasure tables and actualcollection. OnlyOrdinaryTreasureLives fails: its EasternArcher fixture used normal damage against that family's source normal-damage resistance. Correcting the fixture to prove resistance before legitimate elemental defeat; gameplay remains unchanged.
- Integrated the separate normal-input fixture/rendered hand review and earnedL40tactic patches. Three creation spells are now integrated pending the next build, with source paid occupied-hand refusal and focused casting/use/checkpoint tests.
- Tightened new saved held-slot parsing so malformed or overflowing numeric tokens cannot alias slot0; regression coverage added. Source spell catalog is90/108 in pending code, not yet a verified completion claim.



## 2026-11-27 15:16 CST — 57 regressions and rendered hand state verified

- All57WAD tests pass in Saved/Logs/PlayableLoops-HandMovementCreationTreasure-Automation.log;375source itemrules saved. Both rendered RetainedHand processes pass actual Ready/Equipment swaps, unknown-item rightmouse examination, F1/Notes/banner retention, identification scope, L30-to-L301 travel, restart, normalbagstorage and one equippedshieldowner withoutduplication.
- Inspected held-inventory and equipped-reloaded screenshots. The tiny7x6heldring blendswithauthoredtrees, so independent pixelcomparison checked originalRING17.CPC+GAME.PAL atviewportcenter360,210: all24opaque sourcepixels andall96scaledrenderedpixels match exactly. A repeatable QA script is being added.
- The earnedDestruction retry restored the legitimate Town5 save but stopped before shopping: the seventhinvisibility replacement costs50extra copper beyond its fixed budget. No campaigncompletionclaimed. Nextreview sellsone ofthree surplus earned fullcures807 through the normalStore, retainingtwo, before buyingrequiredsupplies; no addedfunds oritems.
- Invulnerability/Orb954 and sourcefallingdamage/Shock are separate isolated nextpatches; journalpickupmetadata/sharedfinishedpages are still being implemented in isolation. None is shipped yet.



## 2026-11-27 15:22 CST — earned shopping passes; new L32 encounter blocks replay

- Verified normal Store sales of two surplus earned807 cures for200copper each, retainingone. Actual source-price mandatorysupplies cost7850copper; oldTown5purse7600could not fundthem. The sales fundnormal twoelectric/seveninvisibility/threeLavaWalking/manarecovery purchases without item/currencygrants.
- The actual carried Priest enters L32 with restored OrangeWizard/Gargoyle placements. At routepoint17, CreatureThing177's1000rawdamage missile defeats the player. Evidence: Saved/Logs/PlayableLoops-Destruction-RestoredEncounters-EarnedBudgetedSurplusSale.log. This precedesL71/L40; the earlier campaign replays lacked those L32placements. Loop5remainsopen. Futuretactic needs real L32combat/cover or another earnedconcealment allocation; no disabledenemies/freehealth workaround.
- Core retained-hand/movement/creation/treasure milestone is verified separately by57tests andtwo renderedtravel/restartprocesses. Preparingits checkpoint now; pending invulnerability/fall/journalpatches remain excluded.


## 2026-11-27 15:37 CST — protection/fall verified; shared journal import completed

- Invulnerability/OrbDefense954 and physicallandingdamage/Shock buildpasses andall65WADregressions pass (PlayableLoops-InvulnerabilityFall-Automation.log). Sourcecoverage92/108isverified inthatbinary; originaldamage/controlordering, orbactivationcostandDispel, realfalls andsourceShockgatepass.
- Mergedjournalmetadata buildpasses. Imported259finishedoriginalNOTEddddbitmaps into sharedruntimecatalog andrefreshed375itemrules. Expanded68suitehas66passes: itsisolatedHUDRPC fixturefailedtoinitializeaHUD, andtheoldordinary-loot testexpectedplainquiversinthebagratherthancorrectsourceGETammo. Correctedbothfixtures; rerunpending.
- Metadataall30refresh isprepared; addingpublicread-only Actorgettersforscriptverification ratherthanreadingprivateproperties. No geometryrebuildispartofthatrefresh.
- Separateisolatedworknowcovers sourceDeathWard/alive0HP semantics, originalMagicMap, andrealterrainfriction/StickyFeet. Sourceauditfinds nonnormalfrictionmarkers in29of30maps; onlyL34hasnone. Noneofthesenextpatchesisshippedyet.


## 2026-11-27 15:42 CST — Death Ward integration pending verification

- Integrated sourceDeathWard12 andseparate alive/defeatedstate. Strict exact/overkill health-hitmana rules, alive0HP afterexpiry, nativeversion3 persistence andlegacydead migration areunderbuild; sourcecoverage93/108isnotyetclaimedverified.
- The corrected journal fixtures andread-onlymetadata APIsareincluded. A separate three-process Ward0HPsave/travel/expiry/revive reviewisbeing authored. Sourceactive-effect persistenceisadeliberate nativeextension; originalStatsSaveCharacter clears temporaryeffects.
- Previouslyverified65testbinarycoversInvulnerability/Orb954andShock/falling. Journalsharedcatalog259 imported,66/68suitepassedbeforethetwofixturefixes. All30metadatarefresh remains pendingpublicAPIbuild.


## 2026-11-27 15:50 CST - protection and journal verification

- All 72 WAD regressions pass, including source Death Ward, actual falling damage/Shock, Orb954 and journal auxiliary data.
- All 30 saved maps now carry their exact source pickup auxiliary payloads. The refresh verified every bound pickup while preserving geometry, component state and non-map drops. A fresh-process idempotency check follows the rendered reviews.
- Five rendered process reviews are starting: active and expired living-zero Death Ward saves, defeat/revival, and normal journal407 pickup/travel/restart.
- Agents remain active on Magic Map self-review, Night Vision source analysis and remaining authored creature placement coverage. Their isolated patches have not yet been shipped.


- All five rendered Ward/journal processes passed. Inspected the expired0HP alive HUD and restored original NOTE0407 bitmap. Evidence: Saved/Logs/PlayableLoops-WardZero-Rendered.log, WardZeroReload, WardExpiredZeroReload, JournalPickup and JournalPickupReload; profile Saved/WardJournalProcessProfile-20261127-155349.
- Night Vision audit confirms five remaining spells plus carrot244/potion823. Original shading lifts world shade indices while excluding sky; simple global exposure is unsuitable. Source owner start/stop behavior also differs from additive status power. Rendering work remains explicit in PLAN.


- Fresh-process all30 reopen passed with zero changed payloads and identical map-file hashes: 1,942 authored pickup bindings and81,588 component states verified. Protection/journal milestone source coverage is93/108, with72 passing WAD tests and five rendered process reviews. Preparing commit/push for PLAN238-240.

## 2026-11-27 15:59 CST - checkpoint pushed; terrain and treasure under build

- Pushed c78f655d to origin/main, including259 original journal bitmaps and all30 pickup metadata updates. LFS uploaded405 objects/129MB.
- Source terrain friction, Sticky Feet ring432/two spells and Orange Wizard373/407 treasure are integrated and under build. Their new tests and walking-route checks are pending.

## 2026-11-27 16:09 CST - terrain verification and completeness audit

- All77 tests passed after the editor topology fix. Terrain refresh diagnostic then exposed only Python Transform wrapper addresses, not geometry changes; numeric transform snapshots now preserve the full invariant. All30 terrain maps were saved; fresh-process reopen and actual walking-route checks are running.
- Independent9,037-THINGS/475-types,443-script-body audit confirms all1,942 inventory placements are bound. PLAN now lists concrete remaining creature, scenery, ambient/light/glass and script registrations. Existing native maps are not being mislabeled as fully populated scenes.
- Small Inn room costs200copper, so the carried Priest remainder150 cannot buy it. L32 already heals to3,200 before its fights. Further Destruction tactics must use real earned resources or combat/cover.

- Terrain saved/reopen checks passed all30 maps (5,683 sectors); door147 cycle passed. The older Slice stopped at action7 Equip(type200), not a walk: its default Priest is now correctly forbidden the dagger. Recording that legacy fixture incompatibility and running the independent actual walking/door/light route. Movement unit tests still pass; no collision/friction tolerance has been weakened.

## 2026-11-27 16:13 CST - terrain and Orange Wizard milestone verified

- All77 WAD tests pass; source spell coverage95/108. Source ground friction/Sticky Feet and Orange Wizard373+407/104 loot are verified. All30 terrain maps (5,683 sectors) reopen correctly without geometry/component/material changes.
- Actual L30 route walks from1608,432 through door147 to2762.6,1515.9; lighting changes84->160->84. Door cycle also passes. The older Slice stops at the class-forbidden Priest dagger equip, not movement; PLAN records fixture migration separately.
- PLAN now includes the all-map source completeness audit and concrete remaining map/type/script omissions. Preparing checkpoint241-242, then original Magic Map, flight-owner and missing encounter integration.

## 2026-11-27 16:15 CST - map and flight integration; source inventory geometry

- Pushed a5aa4777 to origin/main (31 LFS objects/125MB), covering verified terrain/Orange Wizard work and the concrete completeness backlog.
- Original75x75 Magic Map, eight spells/ring435, exploration/control persistence and individual613 flight suppression are merged and under build. Map alpha/visibility sampling remain documented rendering approximations; no global player-light changes are used.
- Inventory audit confirms five6x8 bags, source17x14 spacing and variable bitmap footprints, unlike the current one-cell grouped pages. A pure layout/codec patch is frozen; normal-input/HUD/Town adapters are being implemented separately. Readied-stack refill remains an explicit followup.

## 2026-11-27 16:21 CST - integration checks and parallel packages

- Fixed MagicMap TMap key access, compile-time name shadowing and self-contained review includes; restoration now clears cached map lines so topology rebinds. Rebuild pending; map/flight runtime checks not yet claimed.
- EncounterCoverage.patch combines19 missing L151 Barbarians and5 L74 Skeleton Knights, preserving the existing L151Thing31/L34 identities; AcidTerrain and separate typed-lava fixes are frozen pending integration.
- Active agents now handle original inventory footprint adapters, neutral per-player Night Vision with sky/HUD exclusions, and missing authored ambient marker coverage. Night Vision is explicitly a native rendering approximation; strongest remaining owner policy replaces the source stop-reset interaction.

## 2026-11-27 16:26 CST - map/flight regression pass, rendered fixture correction

- All86 WAD regressions pass, including all8 original map spells, map ownership/visibility/raster checks and four flight-owner tests. Source coverage103/108 is unit-verified, with rendered map verification still pending.
- Corrected the invisible-marker assertion to test actual output pixels, including a visible-object negative control; attributed invisible records are intentionally filtered at rasterization.
- First rendered map run passed initial ring placement, row controls, tier4 cast and unREADY, then its helper toggled an already-open Equipment form incorrectly. Fixed the ordinary button sequence and rerunning the two process reviews. No gameplay permissions or map behavior were weakened.

## 2026-11-27 16:28 CST - original map and flight ownership verified

- All86 WAD regressions pass;103/108 original spells verified. Both rendered MagicMap processes passed. Inspected tier4 and actual opened-door frames; line79 stayed unseen behind closed door147, then ordinary E revealed it. Controls/owners/exploration survive L301 travel and process restart. Profile: Saved/MagicMapProcessProfile-20261127-162600.
- Individual Flying Amulet suppression, legacy migration, index remapping and source Dispel reREADY pass actual save/disk tests alongside Ward/Orb.
- PLAN243-244 are checked; full roadmap now explicitly distinguishes historical seven-quest archive proof from the current restored encounters. Preparing commit/push before encounter/hazard work.

## 2026-11-27 16:33 CST - next binary built; encounter/hazard content under verification

- Pushed59377afc with verified original MagicMap and flight owners.
- EncounterCoverage, acid terrain, typed lava and the pure source bag-layout/codec model now compile. MSVC required splitting exported constexpr declarations; unrelated actor proofs now compare numeric transforms. Four-action build stayed bounded with~25GB physical/~20GB commit headroom and completed compilation in118seconds.
- Targeted L34/L74 Skeleton, L151 Barbarian and L75 acid refresh/reopen proofs are running before the expanded100-test suite. Expected visible geometry remains unchanged; acid may change only its hidden bottom and bordering collision spans. Bag UI integration remains separate and unclaimed.

## 2026-11-27 16:36 CST -100 regressions and saved-content proofs pass

- All100 WAD tests pass, including source20 L151 Barbarians/35 Skeleton Knights acrossL34/L74, actual typed acid/lava behavior, save/revisit and four source bag-layout/order/codec groups.
- Targeted encounter imports and independent reopen preserve legacy actor identities and geometry. Acid first proof used a zero-padded floor-name assumption; corrected unique sector166 lookup and restored only our modified L75 to committed baseline before rechecking the actual collision change. Both refresh and fresh reopen now pass with visible mesh/material/UV bytes preserved and14 bordering colliders updated.
- Rechecking real L30 door/walking collision after shared collision-code extraction before checkpoint. NightVision and all334 ambient markers are frozen candidates; inventory HUD/Town adapters continue separately.

## 2026-11-27 16:38 CST - encounter/hazard/model milestone complete

- All100 tests pass. Targeted content refresh and independent reopen pass for L34/L74 Skeletons, all20 L151 Barbarians and L75 acid. Existing identities/visible mesh bytes remain intact.
- Both real L30 regressions pass after collision refactoring: door147 holds/recloses3.5seconds and player walks original spawn through it with84->160->84 lighting.
- PLAN245-247 checked, explicitly separating the tested bag model from pending visible UI wiring. Preparing commit/push, then NightVision/ambient/inventory integration.

## 2026-11-27 16:49 CST - next integration under final linking

- fa1a4222 is pushed. Integrated original five-bag UI/player/Town grids, neutral private-camera Night Vision and all334 source ambient markers. Candidate source catalog now contains all108 definitions, but final five shader/use proofs are not yet claimed.
- Resolved missing MaterialDomain include, test fixture Cast name shadowing and extreme-coordinate bag codec overflow. Added the RHI dependency required for material feature-level inspection; final link is running.
- Pending validation: saved PP material/rules import, expanded111-test suite, real GPU swatch/two-view/HUD comparisons, rendered inventory/Bank/Store/restart, all30 ambient import/reopen and explicitly muted live audio transport. Modern bag save corruption hardening and its rendered fixture are being finalized separately.
- Agents also have frozen L41 script missile restoration/migration, and near-complete original scenery coverage3377 placements across99 types;5 translucent ELMARK placements remain explicit. L30 outside-polygon TreeThing17 was resolved using original BLOCKMAP to sector120, without moving its authored XY.

## 2026-11-27 17:02 CST - save/cold-resource defects caught before shipping

- First111 suite passed108: two bag tests exposed real footprint changes from asynchronous texture placeholders; now uses saved original icon dimensions everywhere. Modern bag save preflight/atomicity and full-catalog source-size regression are compiled;114-suite rerun starts now. Night CPU reference assertions used Equals(...,0), whose strict tolerance check rejects even exact equality; corrected to exact equality without changing shader math.
- Night GPU proof failed: even bright diagnostic cards were black; source shader-permutation readiness/fallback is the current hypothesis. Added diagnostic/readiness candidate retains all image thresholds; no visual success claimed.
- Ambient import verified all334 in memory, but independent reopen caught L72 disk data still old. Source log proves one temporary-package-hash write failed;29 maps persisted, L72 did not. Strengthening explicit save-success proof and retrying that map before full readonly reopen. This is not being counted as complete from memory-only checks.

## 2026-11-27 17:13 CST -114 tests, inventory process reviews and persisted ambience

- All114 regressions pass. InventoryGrid andInventoryGridReload both pass actual normal clicks, manual2+1 potion stacks,64x64 source footprint, five pages, Equipment/F1/Notes retention, L30->L301 travel and separate-process exact positions/order restoration. Profile Saved/InventoryGridProcessProfile-20261127-170819; PNGs saved. Original type712 bitmap is mostly padded; separate source-frame selection audit is ongoing, not presumed from pixels alone.
- Explicit public-save L72 repair passed. Independent fresh full30 reopen now verifies334 original ambient bindings,203additional,zero missing samples, unchanged geometry/meshbytes. Full persisted result is Saved/ReviewArtifacts/AmbientAudioReopen.json. Muted live-device playback still needs its separate check.
- Night shader resources are confirmed incomplete, explaining fallback captures; ordered DDC cache completion then shader job submission/finish is now under build, preserving all completeness and GPU thresholds. No NightVision visual success yet.

## 2026-11-27 17:20 CST - inventory/Town and live ambient checks verified

- Both rendered TownServices processes passed: original Bank deposits/withdrawals, Store affordability/buy/sell, footprint grids, page/equipment/close controls, purse/bank/item persistence. Screenshots reviewed. Profile Saved/InventoryTownProcessProfile-20261127-171758; earlier two inventory processes also passed. PLAN248 records this verified goal.
- Separate audio-enabled AmbientMutedPlayback passed on the actual XAudio2 device, with fixture master gain0 throughout. This exercised real procedural loop play, radius stop/restart and EndPlay release, not the no-device skip. Together with fresh all30 persisted census, PLAN249 is verified. Exact original panning/mixer behavior remains open.
- The Night shader diagnostic was too strict for UE5.6's default on-demand shader job cache: engine source intentionally leaves legacy compilation-finalized false for complete ordinary maps. Replacing that diagnostic with actual rendering-thread fallback identity proof; all image thresholds remain unchanged. GPU success is still not claimed.
- Three agents are active on shader/runtime readiness, source world palette/mip provenance, and the11-variant shield initial-frame fix. Frozen scenery/L41/packed-Druid packages await integration after this checkpoint.

## 2026-11-27 17:28 CST - Night Vision GPU proof passed

- Saved RHI import now proves the actual rendering resource is complete with no fallback. UE5.6 on-demand job-cache mode deliberately leaves the legacy finalized-valid flag false; no engine completion flag was changed.
- Controlled normal rune cast + two-view GPU review passes: dark grey69.757->116.337, bright242.588->243.588, neutral sky109.132 unchanged, original sky159.198 unchanged, independent viewerB max channel difference0, and45180 static original HUD pixels unchanged.
- Fixture corrections retained every threshold: raised sub-code-value test greys into a measurable range; scoped/restored r.Test.FreezeTemporalSequences to compare identical sampling; copied the actual player camera aspect constraints instead of ACameraActor's default letterbox. No gameplay shader/exposure was altered for those fixture repairs.
- Native vision is neutral SDR shadow gain with constant sky classification and private per-camera power, not exact indexed palette shading. Runtime cold-parent pending guard is the last change before the verified checkpoint commit.

## 2026-11-27 17:32 CST - verified checkpoint250 ready for Git

- Night cold/pending guard compiles. All115 WAD tests pass (final stdout has the complete last test; the abslog omitted its final flush). Separate real-RHI PendingLifecycle passes cold proof, genuine PostEditChange invalidation and fresh renderer proof. Final normal-cast GPU review passes again with the guard active and all45180 static HUD pixels unchanged.
- PLAN248�250 now records original bag/Town grids, all334 persisted campaign ambient markers and private-camera Night Vision. Native behavior covers all108 source spell definitions; exact original indexed shading, original feedback and full current campaign remain open.
- Committing source, required shared effects/catalog and all30 map assets; user Editor settings and unrelated local review files are excluded. Next source packages are ordinary scenery, L41 missiles, packed Druid variants and source-art corrections.
