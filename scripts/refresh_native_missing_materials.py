"""Refresh original missing-picture materials on all 30 saved campaign maps.

Run in Unreal Editor after compiling AA.RefreshNativeWadMissingMaterials, with
gameplay readers closed and no unsaved level edits. This calls only the bounded
material command, never AA.ImportNativeWad or a geometry rebuild. Each command
saves its map/material assets; failures raise immediately and stop later maps.

Checks source MAP names and the saved sidedef metadata, all level component
transforms/mobility/collision, effective materials and explicit overrides, and
the byte hashes of existing static-mesh assets. Only the Surface slot (index0)
of components with absent named pictures may change. LightingShell slots and
valid-name user overrides must remain unchanged. Re-running is supported.
"""

from collections import Counter
from pathlib import Path
import hashlib
import json
import re
import struct

import unreal


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def object_path(value):
    return value.get_path_name() if value else None


def source_picture_names(path):
    data = path.read_bytes()
    require(len(data) >= 14, f"Truncated source picture archive: {path}")
    magic, index, size, count = struct.unpack_from("<IIIH", data)
    require(magic == 0x21736552 and size == count * 39 and index + size <= len(data),
            f"Invalid source picture index: {path}")
    names = set()
    for entry in range(count):
        position = index + entry * 39
        name = data[position + 4:position + 18].split(b"\0", 1)[0].decode("ascii").strip().upper()
        names.add(name[:8])  # Same normalization as the real map-surface loader.
        if name == "DRK42":
            offset, length = struct.unpack_from("<II", data, position + 18)
            require(offset + length <= len(data), "Truncated DRK42 source payload")
            picture = data[offset:offset + length]
            require(struct.unpack_from("<HH", picture) == (64, 64), "DRK42 source dimensions changed")
            require(hashlib.sha256(picture).hexdigest() ==
                    "7cddd72c05530846eeb225bea3c336cc3cbc10d5524a55b7b45cdc866d68e029",
                    "DRK42 source bytes differ from the audited original fallback")
    require("DRK42" in names, "Original missing-picture fallback DRK42 is unavailable")
    return names


def source_map(path):
    data = path.read_bytes()
    require(len(data) >= 12, f"Truncated source map: {path}")
    magic, count, index = struct.unpack_from("<4sii", data)
    require(magic in (b"PWAD", b"IWAD") and count >= 0 and index >= 0
            and index + count * 16 <= len(data), f"Invalid source map: {path}")
    lumps = {}
    for entry in range(count):
        offset, length, name = struct.unpack_from("<ii8s", data, index + entry * 16)
        require(offset >= 0 and length >= 0 and offset + length <= len(data), f"Invalid lump bounds: {path}")
        name = name.rstrip(b"\0").decode("ascii")
        require(name not in lumps, f"Ambiguous duplicate source lump {name}: {path}")
        lumps[name] = data[offset:offset + length]
    require("SECTORS" in lumps and "SIDEDEFS" in lumps and "LINEDEFS" in lumps,
            f"Missing source topology lumps: {path}")
    require(len(lumps["SECTORS"]) % 26 == 0 and len(lumps["SIDEDEFS"]) % 30 == 0
            and len(lumps["LINEDEFS"]) % 14 == 0, f"Invalid source topology sizes: {path}")

    def texture(value):
        return value.split(b"\0", 1)[0].decode("ascii").strip().upper()

    sectors = [(texture(row[2]), texture(row[3]))
               for row in struct.iter_unpack("<hh8s8shhh", lumps["SECTORS"])]
    sides = list(struct.iter_unpack("<hh8s8s8sH", lumps["SIDEDEFS"]))
    blends = {}
    for row in struct.iter_unpack("<HHHHHHH", lumps["LINEDEFS"]):
        flags, front, back = row[2], row[5], row[6]
        two_sided = bool(flags & 4) and all(
            side < len(sides) and sides[side][5] < len(sectors) for side in (front, back))
        for side in (front, back):
            if side >= len(sides):
                continue
            for band in range(3):
                blends[(side, band)] = (2 if flags & 0x20 else 1) if band == 1 and two_sided else 0
    return sectors, len(sides), blends


def missing_name(name, available):
    normalized = name.strip().upper()[:8]
    return bool(normalized and normalized != "-" and not normalized.startswith("F_SKY")
                and normalized not in available)


def expected_missing(level, sectors, side_count, blends, available):
    # Use a narrow public readonly getter and generated component names. Private
    # native topology UPROPERTY fields have no Editor flags and are intentionally
    # not exposed to Python, even through get_editor_property.
    saved_names = [list(level.get_wad_side_texture_names(index)) for index in range(side_count)]
    require(all(len(names) == 3 for names in saved_names)
            and not level.get_wad_side_texture_names(side_count),
            "Saved native sidedef count/names do not match their original MAP")
    expected = {}
    for component in level.get_components_by_class(unreal.StaticMeshComponent):
        component_name = component.get_name()
        plane = re.fullmatch(r"Sector_(\d+)_(Floor|Ceiling)", component_name)
        side = re.fullmatch(r"Side_(\d+)_(\d+)", component_name)
        if plane:
            index = int(plane[1])
            require(index < len(sectors), f"Native sector outside source MAP: {component_name}")
            name = sectors[index][plane[2] == "Ceiling"]
            if missing_name(name, available):
                expected[component_name] = (name, 0)
        elif side:
            index, band = int(side[1]), int(side[2])
            require(index < len(saved_names) and band < 3, f"Invalid saved sidedef component: {component_name}")
            name = saved_names[index][band]
            if missing_name(name, available):
                require((index, band) in blends, f"Unreferenced saved sidedef band {index}/{band}")
                expected[component_name] = (name, blends[(index, band)])
    return expected


def collision(component):
    body = component.get_editor_property("body_instance")
    responses = body.get_editor_property("collision_responses").get_editor_property("response_array")
    # Include custom response channels, not just the eight public built-in ones.
    channels = tuple((str(entry.get_editor_property("channel")), str(entry.get_editor_property("response")))
                     for entry in responses)
    state = {
        "enabled": str(component.get_collision_enabled()),
        "profile": str(component.get_collision_profile_name()),
        "object_type": str(component.get_collision_object_type()),
        "responses": channels,
        "overlaps": component.get_editor_property("generate_overlap_events"),
        "simulate_physics": body.get_editor_property("simulate_physics"),
    }
    if isinstance(component, unreal.BoxComponent):
        extent = component.get_editor_property("box_extent")
        state["box_extent"] = (extent.x, extent.y, extent.z)
    return state


def snapshot(level):
    state = {}
    for component in level.get_components_by_class(unreal.SceneComponent):
        name = component.get_name()
        require(name not in state, f"Duplicate native component name: {name}")
        location = component.get_editor_property("relative_location")
        rotation = component.get_editor_property("relative_rotation")
        scale = component.get_editor_property("relative_scale3d")
        entry = {
            "class": component.get_class().get_path_name(),
            "location": (location.x, location.y, location.z),
            "rotation": (rotation.pitch, rotation.yaw, rotation.roll),
            "scale": (scale.x, scale.y, scale.z),
            "parent": object_path(component.get_attach_parent()),
            "mobility": str(component.get_editor_property("mobility")),
            "visible": component.get_editor_property("visible"),
            "hidden_in_game": component.get_editor_property("hidden_in_game"),
        }
        if isinstance(component, unreal.PrimitiveComponent):
            entry["collision"] = collision(component)
        if isinstance(component, unreal.StaticMeshComponent):
            mesh = component.get_editor_property("static_mesh")
            entry["mesh"] = object_path(mesh)
            entry["materials"] = tuple(object_path(component.get_material(index))
                                       for index in range(component.get_num_materials()))
            entry["overrides"] = tuple(object_path(value) for value in component.get_editor_property("override_materials"))
            entry["mesh_slots"] = tuple(
                (str(slot.get_editor_property("material_slot_name")), object_path(slot.get_editor_property("material_interface")))
                for slot in mesh.get_editor_property("static_materials")) if mesh else ()
        state[name] = entry
    return state


def mesh_hashes(project, snapshot_state):
    paths = {entry["mesh"] for entry in snapshot_state.values() if entry.get("mesh")}
    result = {}
    for path in sorted(paths):
        if not path.startswith("/Game/"):
            continue
        filename = project / "Content" / (path.split(".", 1)[0][len("/Game/"):] + ".uasset")
        require(filename.is_file(), f"Missing existing mesh asset: {filename}")
        result[path] = hashlib.sha256(filename.read_bytes()).hexdigest()
    return result


def verify_material_changes(before, after, expected, map_name):
    require(before.keys() == after.keys(), f"{map_name}: component set changed")
    changed = []
    for name, previous in before.items():
        current = after[name]
        require(previous.keys() == current.keys(), f"{map_name}/{name}: component state shape changed")
        for key in previous:
            if name in expected and key in ("materials", "overrides"):
                require(previous[key][1:] == current[key][1:],
                        f"{map_name}/{name}: non-Surface material or LightingShell override changed")
            else:
                require(previous[key] == current[key], f"{map_name}/{name}: unexpected change in {key}")
        if name in expected:
            source, blend = expected[name]
            require(current["mesh_slots"] and current["mesh_slots"][0][0] == "Surface",
                    f"{map_name}/{name}: generated Surface is not slot0")
            package = f"/Game/Maps/{map_name}_Assets/Materials/MI_{blend}_{source}"
            material = package + f".MI_{blend}_{source}"
            require(current["materials"] and current["materials"][0] == material,
                    f"{map_name}/{name}: expected DRK42-backed authored-name material {material}")
            require(current["overrides"] and current["overrides"][0] == material,
                    f"{map_name}/{name}: bounded refresh did not persist its component Surface override")
            if previous["materials"] != current["materials"] or previous["overrides"] != current["overrides"]:
                changed.append(name)
    return changed


def verify_fallback_assets(map_name, expected):
    for source, blend in sorted(set(expected.values())):
        root = f"/Game/Maps/{map_name}_Assets"
        material_path = f"{root}/Materials/MI_{blend}_{source}.MI_{blend}_{source}"
        material = unreal.load_asset(material_path)
        require(isinstance(material, unreal.MaterialInstanceConstant), f"Missing saved fallback material: {material_path}")
        texture = unreal.MaterialEditingLibrary.get_material_instance_texture_parameter_value(material, "SurfaceTexture")
        require(object_path(texture) == f"{root}/Textures/T_{source}.T_{source}",
                f"{material_path}: authored-name fallback texture is not bound")
        require((texture.blueprint_get_size_x(), texture.blueprint_get_size_y()) == (64, 64),
                f"{material_path}: fallback dimensions differ from original DRK42")


def refresh_missing_materials():
    project = Path(unreal.Paths.project_dir()).resolve()
    source_root = project / "Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor"
    pictures = source_picture_names(source_root / "Resources/ArtFinal/PICS.RES")
    maps = sorted(path for path in (project / "Content/Maps").glob("L*.umap")
                  if re.fullmatch(r"L\d+", path.stem))
    require(len(maps) == 30, f"Expected all 30 imported campaign maps, found {len(maps)}")
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    report = []
    total_missing = Counter()
    for path in maps:
        name = path.stem
        source = source_root / "Exe" / f"{name}.MAP"
        sectors, side_count, blends = source_map(source)
        world = unreal.EditorLoadingAndSavingUtils.load_map(str(path))
        levels = [actor for actor in actors.get_all_level_actors() if isinstance(actor, unreal.NativeWadLevel)]
        require(world is not None and len(levels) == 1, f"Could not open one native level in {name}")
        level = levels[0]
        expected = expected_missing(level, sectors, side_count, blends, pictures)
        before = snapshot(level)
        require(expected.keys() <= before.keys(), f"{name}: missing expected surface components")
        mesh_before = mesh_hashes(project, before)
        map_before = hashlib.sha256(path.read_bytes()).hexdigest()
        unreal.SystemLibrary.execute_console_command(world,
            f'AA.RefreshNativeWadMissingMaterials "{source.as_posix()}" {name} /Game/Maps/{name}')
        after = snapshot(level)
        changed = verify_material_changes(before, after, expected, name)
        require(mesh_before == mesh_hashes(project, after), f"{name}: a saved mesh asset was rewritten")
        if not expected:
            require(hashlib.sha256(path.read_bytes()).hexdigest() == map_before,
                    f"{name}: a map with no missing surface names was unnecessarily rewritten")
        verify_fallback_assets(name, expected)
        counts = Counter(source for source, blend in expected.values())
        total_missing.update(counts)
        report.append({"map": name, "checked_components": len(before), "missing_names": dict(sorted(counts.items())),
                       "rebound_components": changed, "unchanged_mesh_sha256": mesh_before})
        unreal.log(f"Verified material-only fallback refresh {name}: {len(expected)} missing-name surfaces, "
                   f"{len(changed)} changed bindings; {len(before)} component states and {len(mesh_before)} mesh assets preserved")
    destination = project / "Saved/ReviewArtifacts/MissingTextureRefresh.json"
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps({"verified_maps": len(report), "missing_name_surfaces": dict(sorted(total_missing.items())),
                                       "maps": report}, indent=2) + "\n", encoding="utf-8")
    unreal.log(f"Verified missing-picture fallback on all {len(report)} maps without geometry rebuilds; report: {destination}")


if __name__ == "__main__":
    refresh_missing_materials()
