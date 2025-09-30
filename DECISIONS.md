# Project Decisions & Rules (Summary)

## Context
Unreal Engine 5.6 project (UnrealAA). C++14, .NET 8 toolchain present but .NET AutomationTool projects are NOT a build goal for day-to-day iteration. Plugin: `AmuletsAndArmorPort` with legacy reference sources under `Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor` (reference-only, not compiled).

---
## Active Rules
[RULE] External legacy source directory is reference-only; never added to include paths or compiled.  
[RULE] No direct inclusion of legacy headers in new runtime code (e.g. `LegacyMapLoader` must stand alone).  
[RULE] Focus native build on Unreal targets (e.g. `UnrealAAEditor`); .NET Automation / *Automation.csproj failures can be ignored.  
[RULE] Do not globally suppress or modify Engine / AutomationTool vulnerability or compatibility warnings; only scope suppressions to project-specific builds when required.  
[RULE] Keep custom global MSBuild props minimal and scoped (only affect `UnrealAA`, not Engine rule / automation projects).  
[RULE] Use solution filters or UBT command line to avoid building unwanted .NET projects.  
[RULE] Introduce new functionality via modern Unreal C++ wrappers (Exec handlers, BlueprintFunctionLibraries) instead of retrofitting legacy procedural code.  
[RULE] Provide test coverage through Unreal Automation Tests (EditorContext + EngineFilter).  
[RULE] All future prompts should begin with an explicit [TASK] and may optionally embed [RULES] and [DECISIONS] blocks for context continuity.  

---
## Map Loading Decisions
[DECISION] Retained `DoomWadLoader` (multi-map WAD concept) for experimental loading.  
[DECISION] Replaced original direct legacy `View3dLoadMap` dependency with a modern placeholder `LegacyMapLoader` parsing a simplified custom binary format (magic `AAM1` + counts).  
[DECISION] Removed earlier attempt to include legacy external headers/lib from build after rule change.  
[RATIONALE] Avoid tight coupling to unmaintained DOS-era code; enable iterative modernization while permitting later true parser implementation.  

---
## Legacy Stats & Interfaces
[DECISION] Added `FDoomWadLoadStats` and `FLegacyMapLoadStats` structs plus C-style exported accessors for testability.  
[DECISION] Tests assert presence of non-zero counts after a successful load.  

---
## Testing
[DECISION] Added automation tests:
- CRC32 vectors (`AAOriginal.Unit.CRC32.*`)
- Version sanity check (`AAOriginal.Unit.Version.Sanity`)
- WAD load basic (skips if file absent)
- Legacy map load basic (modern placeholder format)
[DECISION] Tests must skip gracefully (warnings) when required sample assets are not present.  
[RATIONALE] Keep CI / local runs deterministic without forcing asset commits.  

---
## Build Configuration
[DECISION] `AAOriginal.Build.cs`: `PCHUsageMode.NoPCHs`, `bUseUnity=false`, no RTTI, no exceptions (lean build aligned with legacy C style).  
[DECISION] Removed linkage to legacy static lib (commented out) after rule to keep external code reference-only.  
[DECISION] Directory.Build.props mutated to:  
- Scope NoWarn / WarningsNotAsErrors only to `UnrealAA` project.  
- Removed broad suppression for Engine automation projects.  
[RATIONALE] Prevent unintended side effects on Engine & AutomationTool restore/analysis.  

---
## .NET / Automation Projects
[DECISION] Do not invest time resolving NU1901/2/3 vulnerability warnings or missing AutomationUtils cascade for routine native iteration.  
[DECISION] Recommended workflow: build via UBT / solution filter excluding *Automation projects.  
[ALTERNATIVE] (Deferred) Introduce per-project conditional suppression target if future native build automation requires clean solution builds.  

---
## Exec / Console Integration
[DECISION] Implemented module-level `Exec` handler (`AA.LOADWAD`) instead of maintaining a separate console commands CPP.  
[RATIONALE] Reduces dependency churn & simplifies runtime command discovery.  

---
## Refactoring & Modernization Path
[DECISION] Placeholder parser approach chosen over immediate full fidelity port of legacy map system.  
[DEFERRED] Future tasks: implement authentic map parsing (lines / sectors / sides) using a clean, documented modern format or adapter layer.  

---
## Prompting & Process Guidelines
[GUIDELINE] Each future request should include:  
```
[TASK] <concise objective>
[CONTEXT] (optional new info)
[REFERENCES] (if specific files / prior decisions)
```
[GUIDELINE] When requesting iterative fixes: explicitly say “continue” with new error block to allow chained corrections.  
[GUIDELINE] Maintain `DECISIONS.md`—append new decisions with date + short rationale.  

---
## Open Items / TODO
[TODO] Provide sample `AAM1` map test asset to exercise legacy loader stats path in CI.  
[TODO] Consider a structured plugin command registry for future content loading commands.  
[TODO] Evaluate whether to convert placeholder legacy format into a UAsset importer pipeline.  

---
## Change Log (Recent)
- Added Exec loader command, stats structs, automation tests.  
- Introduced and then refactored `LegacyMapLoader` to standalone parser.  
- Scoped MSBuild warning suppressions to avoid engine pollution.  
- Adopted rule excluding external legacy sources from compilation.  

---
## Usage in Future Prompts
Include a short snippet like:
```
[RULES] External legacy = reference-only; build native only; use placeholder AAM1 map format.
[DECISIONS] LegacyMapLoader standalone; ignore AutomationTool warnings.
[TASK] <your new request>
```
This ensures continuity without re-reading full file.

---
*End of DECISIONS.md*
