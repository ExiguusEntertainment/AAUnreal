"""Apply/verify only original L75 acid metadata and its required walking collision.

AA.RefreshNativeWadAcidEnvironment <WadPath> <MapName> <MapPackage> is required.
Visible geometry/materials/UV defaults and all unrelated actors/collision are
asserted unchanged. The hidden walking floor and bordering wall collider spans
are the only allowed component changes. Call refresh_acid_environment(False)
in a fresh Unreal process for a saved-map verification without mutation.
"""
from pathlib import Path
import importlib.util
import json
import math
import re
import struct
import unreal


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def original_acid(path):
    data = path.read_bytes()
    magic, count, directory = struct.unpack_from("<4sii", data)
    require(magic in (b"IWAD", b"PWAD") and count > 0 and directory >= 12
            and directory + count*16 <= len(data), "Invalid original L75 MAP")
    lumps = {}
    for i in range(count):
        offset, size, name = struct.unpack_from("<ii8s", data, directory+16*i)
        require(offset >= 0 and size >= 0 and offset+size <= len(data), "Out-of-range source lump")
        lumps[name.rstrip(b"\0").decode("ascii")] = data[offset:offset+size]
    things = list(struct.iter_unpack("<hhHHH", lumps["THINGS"]))
    sectors = list(struct.iter_unpack("<hh8s8sHHH", lumps["SECTORS"]))
    require(things[281] == (960, 392, 0, 1543, 0), "Original acid Thing281 changed")
    require(sum(row[3] == 1543 for row in things) == 1 and not any(row[3] == 1521 for row in things), "Unexpected acid/depth markers")
    require(sectors[166][0] == -20 and sectors[166][6] == 16, "Original acid sector166 changed")
    vertices = list(struct.iter_unpack("<hh", lumps["VERTEXES"]))
    sides = list(struct.iter_unpack("<hh8s8s8sH", lumps["SIDEDEFS"]))
    borders = []
    for i, (a, b, flags, special, tag, front, back) in enumerate(struct.iter_unpack("<7H", lumps["LINEDEFS"])):
        if not any(side != 65535 and sides[side][5] == 166 for side in (front, back)):
            continue
        ax, ay = vertices[a]
        bx, by = vertices[b]
        borders.append({"line": i, "x": (ax+bx)/2, "y": (ay+by)/2,
                        "half_length": math.hypot(bx-ax, by-ay)/2, "yaw": math.degrees(math.atan2(by-ay, bx-ax))})
    return len(sectors), borders


def numeric_transform(actor):
    p, r, s = actor.get_actor_location(), actor.get_actor_rotation(), actor.get_actor_scale3d()
    return ((p.x, p.y, p.z), (r.pitch, r.yaw, r.roll), (s.x, s.y, s.z))


def refresh_acid_environment(refresh=True):
    project = Path(unreal.Paths.project_dir()).resolve()
    source = project/"Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Exe/L75.MAP"
    sector_count, borders = original_acid(source)
    spec = importlib.util.spec_from_file_location("aa_acid_snapshot", project/"scripts/refresh_native_missing_materials.py")
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    world = unreal.EditorLoadingAndSavingUtils.load_map(str(project/"Content/Maps/L75.umap"))
    editor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    levels = [a for a in editor.get_all_level_actors() if isinstance(a, unreal.NativeWadLevel)]
    require(world is not None and len(levels) == 1, "Expected one native L75 level")
    level = levels[0]
    level_path = level.get_path_name()
    floor_names = [c.get_name() for c in level.get_components_by_class(unreal.StaticMeshComponent)
                   if re.fullmatch(r"Sector_0*166_Floor", c.get_name())]
    require(len(floor_names) == 1, "Expected one original sector166 floor component")
    acid_floor_name = floor_names[0]

    def snapshot():
        result = {}
        for actor in editor.get_all_level_actors():
            components = helper.snapshot(actor)
            # Both fields are EditAnywhere in UE5.6 PrimitiveComponent.h/SceneTypes.h.
            # Capture authored UV/lighting defaults as values, not struct-wrapper strings.
            for component in actor.get_components_by_class(unreal.PrimitiveComponent):
                components[component.get_name()]["primitive_defaults"] = tuple(
                    component.get_editor_property("custom_primitive_data").get_editor_property("data"))
            result[actor.get_path_name()] = {"transform": numeric_transform(actor), "components": components}
        return result

    def border_box(entry):
        if "BoxComponent" not in entry["class"] or "box_extent" not in entry.get("collision", {}):
            return False
        x, y, z = entry["location"]
        ex, ey, ez = entry["collision"]["box_extent"]
        yaw = entry["rotation"][1]
        return any(abs(x-edge["x"]) < 1e-4 and abs(y-edge["y"]) < 1e-4
                   and abs(ex-edge["half_length"]) < 1e-4 and abs(ey-0.25) < 1e-6
                   and abs((yaw-edge["yaw"]+180) % 360-180) < 1e-4 for edge in borders)

    before = snapshot()
    flat_before = {f"{a}/{c}": v for a, record in before.items() for c, v in record["components"].items()}
    hashes_before = helper.mesh_hashes(project, flat_before)
    depths_before = [level.get_wad_sector_depth(i) for i in range(sector_count)]
    friction_before = [level.get_wad_sector_friction(i) for i in range(sector_count)]
    acid_before = [level.is_wad_sector_acid(i) for i in range(sector_count)]
    require(level.get_wad_sector_depth(sector_count) == -1, "Native/source sector count mismatch")
    if refresh:
        unreal.SystemLibrary.execute_console_command(world,
            f'AA.RefreshNativeWadAcidEnvironment "{source.as_posix()}" L75 /Game/Maps/L75')
    after = snapshot()
    require(before.keys() == after.keys(), "Acid refresh added/deleted an unrelated actor")
    changed_boxes = []
    new_components = []
    for identity, original in before.items():
        current = after[identity]
        require(original["transform"] == current["transform"], f"Actor transform changed: {identity}")
        if identity != level_path:
            require(original == current, f"Unrelated actor changed: {identity}")
            continue
        old, new = original["components"], current["components"]
        require(old.keys() <= new.keys(), "An existing native component was removed")
        for name, previous in old.items():
            present = new[name]
            if name == acid_floor_name:
                require({k: v for k, v in previous.items() if k != "collision"}
                        == {k: v for k, v in present.items() if k != "collision"}, "Visible acid floor geometry/material/UV changed")
                require({k: v for k, v in previous["collision"].items() if k not in ("enabled", "profile")}
                        == {k: v for k, v in present["collision"].items() if k not in ("enabled", "profile")}, "Unexpected visible acid floor collision change")
            elif name == "WalkingFloor_0166":
                require(present["mesh"] == new[acid_floor_name]["mesh"], "Wrong walking-floor mesh")
            elif border_box(previous):
                require(border_box(present), f"Border collider moved off its original source line: {name}")
                require({k: v for k, v in previous.items() if k not in ("location", "collision")}
                        == {k: v for k, v in present.items() if k not in ("location", "collision")}, f"Border collider noncollision state changed: {name}")
                require(previous["location"][:2] == present["location"][:2], f"Border collider XY changed: {name}")
                if previous != present:
                    changed_boxes.append(name)
            else:
                require(previous == present, f"Unrelated native component changed: {name}")
        for name in new.keys()-old.keys():
            require(name == "WalkingFloor_0166" or border_box(new[name]), f"Unexpected new component: {name}")
            new_components.append(name)
    components = {c.get_name(): c for c in level.get_components_by_class(unreal.SceneComponent)}
    floor = components[acid_floor_name]
    walking = components.get("WalkingFloor_0166")
    require(walking is not None and walking.get_editor_property("static_mesh") == floor.get_editor_property("static_mesh"), "Missing original-mesh walking floor")
    require(abs(walking.get_editor_property("relative_location").z - (floor.get_editor_property("relative_location").z-30)) < 1e-6,
            "Acid walking floor must be exactly30 below the visible source floor")
    require(abs(floor.get_editor_property("relative_location").z+20) < 1e-6
            and abs(walking.get_editor_property("relative_location").z+50) < 1e-6,
            "Original L75 acid floor/walking floor must remain at minus20/minus50")
    require(not walking.get_editor_property("visible") and not walking.get_editor_property("cast_shadow"), "Hidden collision floor affects rendering")
    require(floor.get_collision_enabled() == unreal.CollisionEnabled.NO_COLLISION
            and walking.get_collision_enabled() == unreal.CollisionEnabled.QUERY_AND_PHYSICS, "Acid surface/bottom collision is wrong")
    depths_after = [level.get_wad_sector_depth(i) for i in range(sector_count)]
    acid_after = [level.is_wad_sector_acid(i) for i in range(sector_count)]
    require(depths_after[166] == 30 and acid_after[166] and level.get_wad_sector_friction(166) == 24, "Original acid physical metadata is wrong")
    require(all(depths_after[i] == depths_before[i] and acid_after[i] == acid_before[i] for i in range(sector_count) if i != 166),
            "Unrelated sector physical metadata changed")
    require(sum(acid_after) == 1, "L75 must have exactly one original acid sector")
    require(all(level.get_wad_sector_friction(i) == friction_before[i] for i in range(sector_count) if i != 166),
            "Unrelated sector friction changed")
    flat_after = {f"{a}/{c}": v for a, record in after.items() for c, v in record["components"].items()}
    require(hashes_before == helper.mesh_hashes(project, flat_after), "Saved visible geometry mesh bytes changed")
    if not refresh:
        require(before == after, "Read-only reopen verification mutated the map")
    output = project/"Saved/ReviewArtifacts"/("AcidEnvironmentRefresh.json" if refresh else "AcidEnvironmentReopen.json")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps({"map": "L75", "acid_sector": 166, "source_thing": 281, "depth": depths_after[166],
        "walking_floor_z": walking.get_editor_property("relative_location").z, "changed_border_colliders": changed_boxes,
        "new_components": new_components, "unchanged_mesh_sha256": hashes_before, "refreshed": refresh}, indent=2)+"\n", encoding="utf-8")
    unreal.log(f"Verified original L75 acid environment; visible geometry/materials/UV and unrelated actors preserved: {output}")


if __name__ == "__main__":
    refresh_acid_environment()
