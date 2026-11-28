"""Migrate exact source shield712 variants from wrongPLH02 to originalPLH01.

Requires ItemInitialFrame + WorldArtCore and the bounded editor console hook:
AA.RefreshNativeWadShieldInitialFrame <MapPackage> [audit]
The optional audit token must call the API with bApply=False. No WAD/geometry
import is used. Original sourceMAPs contain zero authored affected shields, so
all30 saved maps are scanned to verify actual bindings and preserve overrides.
Call refresh_shield_initial_frame(False) in a fresh process for independent
reopen checks. Run before the separate world-palette refresh.
"""
from pathlib import Path
import importlib.util
import hashlib
import json
import re
import struct
import unreal

TYPES = {712, 4808, 8904, 13000, 21192, 25288, 29384, 33480, 37576, 41672, 45768}
OLD_M = "/Game/WadInterface/Inventory/Things/M_SHIELD02_PLH02.M_SHIELD02_PLH02"
NEW_M = "/Game/WadInterface/Inventory/Things/M_SHIELD02_PLH01.M_SHIELD02_PLH01"
OLD_T = "/Game/WadInterface/Inventory/Things/T_SHIELD02_PLH02.T_SHIELD02_PLH02"
NEW_T = "/Game/WadInterface/Inventory/Things/T_SHIELD02_PLH01.T_SHIELD02_PLH01"


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def normalized(value):
    return json.loads(json.dumps(value, sort_keys=True))


def authored_shields(path):
    data = path.read_bytes()
    magic, count, directory = struct.unpack_from("<4sii", data)
    require(magic in (b"IWAD", b"PWAD") and count > 0 and directory >= 12 and directory+count*16 <= len(data), "Invalid sourceMAP")
    for i in range(count):
        offset, size, name = struct.unpack_from("<ii8s", data, directory+16*i)
        require(offset >= 0 and size >= 0 and offset+size <= len(data), "Bad source lump")
        if name.rstrip(b"\0") == b"THINGS":
            require(size % 10 == 0, "Bad sourceTHINGS")
            return [i for i, row in enumerate(struct.iter_unpack("<hhHHH", data[offset:offset+size])) if row[3] in TYPES]
    raise RuntimeError("No originalTHINGS")


def refresh_shield_initial_frame(refresh=True):
    project = Path(unreal.Paths.project_dir()).resolve()
    spec = importlib.util.spec_from_file_location("aa_shield_snapshot", project/"scripts/refresh_native_missing_materials.py")
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    editor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    maps = sorted(p for p in (project/"Content/Maps").glob("L*.umap") if re.fullmatch(r"L\d+", p.stem))
    require(len(maps) == 30, "Expected all30 imported campaign maps")
    destination = project/"Saved/ReviewArtifacts/ShieldInitialFrame/baseline.json"
    destination.parent.mkdir(parents=True, exist_ok=True)
    baseline = {} if refresh else json.loads(destination.read_text(encoding="utf-8"))
    def legacy_files():
        return {prefix: hashlib.sha256((project/f"Content/WadInterface/Inventory/Things/{prefix}_SHIELD02_PLH02.uasset").read_bytes()).hexdigest()
                for prefix in ("T", "M")}
    retained_legacy = legacy_files()
    nonart = None
    final_catalog = None
    total_icons = total_worlds = total_bindings = total_preserved = 0
    for path in maps:
        name = path.stem
        require(not authored_shields(project/f"Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Exe/{name}.MAP"),
                "The original source shield placement census changed; review scope before migrating")
        world = unreal.EditorLoadingAndSavingUtils.load_map(str(path))
        require(world is not None, f"Could not open{name}")

        def snapshot():
            actors, meshes = {}, {}
            for actor in editor.get_all_level_actors():
                p, r, s = actor.get_actor_location(), actor.get_actor_rotation(), actor.get_actor_scale3d()
                components = helper.snapshot(actor)
                actors[actor.get_path_name()] = {"class": actor.get_class().get_path_name(),
                    "tags": [str(t) for t in actor.tags], "transform": ((p.x,p.y,p.z),(r.pitch,r.yaw,r.roll),(s.x,s.y,s.z)),
                    "components": components}
                meshes.update(helper.mesh_hashes(project, components))
            return normalized({"actors": actors, "meshes": meshes})

        report_path = destination.parent/f"{name}.json"
        before = snapshot()
        if report_path.exists():
            report_path.unlink()  # Remove only this exact stale report, never assets.
        unreal.SystemLibrary.execute_console_command(world, f'AA.RefreshNativeWadShieldInitialFrame /Game/Maps/{name}' + ("" if refresh else " audit"))
        require(report_path.is_file(), f"Shield refresh/audit did not finish{name}")
        report = json.loads(report_path.read_text(encoding="utf-8"))
        require(report["applied"] == refresh, "Wrong console apply/audit dispatch")
        require(report["old_pixels_valid"], "Legacy generated texture differs from source; retained override requires explicit review")
        require(report["nonart_before"] == report["nonart_after"], "Migration changed rules, prices, classes, stock, sizes, or unrelated art")
        nonart = nonart or report["nonart_after"]
        require(report["nonart_after"] == nonart, "Catalog non-art state changed between maps")
        after = snapshot()
        if refresh:
            require(before["meshes"] == after["meshes"], "An existing mesh asset was rewritten")
            require(set(before["actors"]) == set(after["actors"]), "An actor was created or removed")
            allowed = set(report["changed_components"])
            seen = set()
            for actor_path, old_actor in before["actors"].items():
                new_actor = after["actors"][actor_path]
                require({k:v for k,v in old_actor.items() if k != "components"} == {k:v for k,v in new_actor.items() if k != "components"},
                        "Actor identity/transform/tags changed")
                require(set(old_actor["components"]) == set(new_actor["components"]), "Components were created/replaced")
                for component, old in old_actor["components"].items():
                    new = new_actor["components"][component]
                    key = actor_path+"."+component
                    if key not in allowed:
                        require(old == new, f"Unrelated component changed:{key}")
                        continue
                    seen.add(key)
                    for field, value in old.items():
                        expected = list(value) if field in ("materials", "overrides") else value
                        if field in ("materials", "overrides") and expected and expected[0] == OLD_M:
                            expected[0] = NEW_M
                        require(new[field] == expected, f"Change exceeded exact material slot0:{key}/{field}")
            require(seen == allowed, "Reported changed component is absent from snapshot")
            old_catalog = {r["type"]:r for r in report["catalog_before"]}
            for row in report["catalog_after"]:
                old = old_catalog[row["type"]]
                require(row.get("width") == old.get("width") and row.get("height") == old.get("height"), "Original padded dimensions changed")
                require(row["icon"] == old["icon"] or (old["icon"] == OLD_T and row["icon"] == NEW_T), "Unproven/custom icon replaced")
                require(row.get("world") == old.get("world") or (old.get("world") == OLD_M and row.get("world") == NEW_M), "Unproven/custom world material replaced")
            baseline[name] = after
            total_icons += report["icons"]
            total_worlds += report["world_refs"]
            total_bindings += report["actor_bindings"]
            if path == maps[0]:
                total_preserved = report["preserved_overrides"]
        else:
            require(before == after == baseline[name], f"Fresh{name} did not preserve saved geometry/bindings")
            require(report["icons"] == report["world_refs"] == 0, "Fresh catalog retains eligible legacy source references")
        final_catalog = report["catalog_after"]
        # Unknown or custom old refs are preserved and counted, never blindly replaced.
        require(report["remaining_old_component_refs"] == 0, f"{name} retains an unexpected legacy material reference; review the override")
    require(legacy_files() == retained_legacy, "Migration rewrote a retained legacy texture/material asset")
    if refresh:
        baseline["legacy_files"] = retained_legacy
        baseline["catalog"] = final_catalog
        baseline["nonart"] = nonart
        destination.write_text(json.dumps(baseline, indent=2), encoding="utf-8")
    else:
        require(retained_legacy == baseline["legacy_files"] and final_catalog == baseline["catalog"] and nonart == baseline["nonart"], "Fresh process changed catalog pixels/provenance/rules")
    unreal.log(f"Verified shield initial frame across30 maps: icons{total_icons}, world refs{total_worlds}, saved actor bindings{total_bindings}, preserved overrides{total_preserved}; no geometry import")


if __name__ == "__main__":
    refresh_shield_initial_frame()
