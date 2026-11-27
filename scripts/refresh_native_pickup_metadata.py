"""Refresh and verify source auxiliary payloads on all 30 saved campaign maps.

Run inside Unreal Editor after building the pickup metadata/read APIs. Uses only
AA.RefreshNativeWadPickupMetadata, which validates source topology and saves
actor payloads without rebuilding geometry. No gameplay readers or unsaved level
edits should be open. A repeat run must preserve map files when no payload changes.

The independent bindings below use original THINGS records, public source Thing
tags and unique legacy positions. Private native UPROPERTY fields are not read.
"""

from collections import Counter, defaultdict
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re
import struct

import unreal


def helpers(project):
    # This module has a __main__ guard: importing its invariant readers cannot run
    # the unrelated material refresh or save any assets.
    path = project / "scripts/refresh_native_missing_materials.py"
    spec = importlib.util.spec_from_file_location("_aa_pickup_geometry_readers", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def source_things(path, require):
    data = path.read_bytes()
    require(len(data) >= 12, f"Truncated source MAP: {path}")
    magic, count, directory = struct.unpack_from("<4sii", data)
    require(magic in (b"PWAD", b"IWAD") and count >= 0 and directory >= 0
            and directory + count * 16 <= len(data), f"Invalid source MAP directory: {path}")
    things = None
    for entry in range(count):
        offset, size, name = struct.unpack_from("<ii8s", data, directory + entry * 16)
        require(offset >= 0 and size >= 0 and offset + size <= len(data), f"Invalid source lump: {path}")
        if name.rstrip(b"\0") == b"THINGS":
            require(things is None and size % 10 == 0, f"Ambiguous/malformed THINGS: {path}")
            things = [(x, y, kind & 0xFFFF, flags & 0xFFFF)
                      for x, y, angle, kind, flags in struct.iter_unpack("<hhhhh", data[offset:offset + size])]
    require(things is not None, f"No original THINGS lump: {path}")
    return things


def actor_state(actors, readers):
    geometry = {}
    components = {}
    for actor in actors:
        key = actor.get_path_name()
        position = actor.get_actor_location()
        rotation = actor.get_actor_rotation()
        scale = actor.get_actor_scale3d()
        local = readers.snapshot(actor)
        geometry[key] = {
            "class": actor.get_class().get_path_name(),
            "location": (position.x, position.y, position.z),
            "rotation": (rotation.pitch, rotation.yaw, rotation.roll),
            "scale": (scale.x, scale.y, scale.z),
            "tags": tuple(str(value) for value in actor.get_editor_property("tags")),
            "components": local,
        }
        components.update({f"{key}|{name}": state for name, state in local.items()})
    return geometry, components


def asset_hashes(project, components, readers):
    hashes = dict(readers.mesh_hashes(project, components))
    # Include material files and bulk companions as well as the existing mesh
    # package hashes; all are invariant under an actor-metadata-only refresh.
    paths = set(hashes)
    for state in components.values():
        paths.update(value for value in state.get("materials", ()) if value)
        paths.update(value for value in state.get("overrides", ()) if value)
        paths.update(value for name, value in state.get("mesh_slots", ()) if value)
    for path in sorted(paths):
        if not path.startswith("/Game/"):
            continue
        package = project / "Content" / path.split(".", 1)[0][len("/Game/"):]
        for extension in (".uasset", ".uexp", ".ubulk", ".uptnl"):
            filename = Path(str(package) + extension)
            if filename.is_file():
                hashes[f"{path}:{extension}"] = hashlib.sha256(filename.read_bytes()).hexdigest()
    return hashes


def payload(actor):
    return bool(actor.has_accessory_data()), int(actor.get_source_accessory_data())


def bindings(pickups, level, things, require, map_name):
    by_position = defaultdict(list)
    for index, (x, y, kind, flags) in enumerate(things):
        by_position[(x, y, kind)].append(index)
    origin = level.get_actor_location()
    bound, non_map = {}, {}
    for actor in pickups:
        key = actor.get_path_name()
        tags = [str(value) for value in actor.get_editor_property("tags")]
        if actor.is_dropped() or any(tag.startswith("WAD Generator ") for tag in tags):
            non_map[key] = payload(actor)
            continue
        source_ids = []
        for tag in tags:
            if tag.startswith("WAD Item Thing "):
                match = re.fullmatch(r"WAD Item Thing (\d+)", tag)
                require(match is not None, f"{map_name}: invalid source pickup tag {tag}")
                source_ids.append(int(match[1]))
        require(len(set(source_ids)) <= 1, f"{map_name}: conflicting pickup source tags on {key}")
        if source_ids:
            index = source_ids[0]
        else:
            point = actor.get_actor_location()
            # Match FMath::RoundToInt32, including its negative-half behavior.
            candidates = by_position[(math.floor(point.x - origin.x + 0.5),
                                      math.floor(point.y - origin.y + 0.5), int(actor.get_item_type()))]
            require(len(candidates) <= 1, f"{map_name}: ambiguous legacy pickup placement on {key}")
            if not candidates:
                non_map[key] = payload(actor)
                continue
            index = candidates[0]
        require(0 <= index < len(things), f"{map_name}: source pickup index out of range on {key}")
        x, y, kind, flags = things[index]
        require(kind == int(actor.get_item_type()), f"{map_name}: source type mismatch on {key}")
        bound[key] = {"thing": index, "type": kind, "accessory_data": flags}
    return bound, non_map


def refresh_pickup_metadata():
    project = Path(unreal.Paths.project_dir()).resolve()
    readers = helpers(project)
    require = readers.require
    for method in ("get_item_type", "has_accessory_data", "get_source_accessory_data", "is_dropped"):
        require(hasattr(unreal.NativeWadPickup, method), f"Build the public pickup review API first: missing {method}")
    maps = sorted(path for path in (project / "Content/Maps").glob("L*.umap")
                  if re.fullmatch(r"L\d+", path.stem))
    require(len(maps) == 30, f"Expected all 30 imported maps, found {len(maps)}")
    source_root = project / "Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Exe"
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    report = []
    for path in maps:
        name = path.stem
        source = source_root / f"{name}.MAP"
        readers.source_map(source)  # Independent complete topology-lump validation.
        things = source_things(source, require)
        world = unreal.EditorLoadingAndSavingUtils.load_map(str(path))
        actors = list(subsystem.get_all_level_actors())
        levels = [actor for actor in actors if isinstance(actor, unreal.NativeWadLevel)]
        require(world is not None and len(levels) == 1, f"Could not open one native level in {name}")
        pickups = [actor for actor in actors if isinstance(actor, unreal.NativeWadPickup)]
        expected, non_map = bindings(pickups, levels[0], things, require, name)
        before_payload = {actor.get_path_name(): payload(actor) for actor in pickups}
        before, components_before = actor_state(actors, readers)
        files_before = asset_hashes(project, components_before, readers)
        map_before = hashlib.sha256(path.read_bytes()).hexdigest()
        unreal.SystemLibrary.execute_console_command(world,
            f'AA.RefreshNativeWadPickupMetadata "{source.as_posix()}" {name} /Game/Maps/{name}')
        actors_after = list(subsystem.get_all_level_actors())
        after, components_after = actor_state(actors_after, readers)
        require(before == after, f"{name}: actor/component geometry, collision, visibility, attachment or material state changed")
        require(files_before == asset_hashes(project, components_after, readers), f"{name}: mesh/material asset bytes changed")
        pickups_after = {actor.get_path_name(): actor for actor in actors_after if isinstance(actor, unreal.NativeWadPickup)}
        require(pickups_after.keys() == before_payload.keys(), f"{name}: pickup set changed")
        changed = []
        for key, entry in expected.items():
            require(payload(pickups_after[key]) == (True, entry["accessory_data"]),
                    f"{name}: source Thing {entry['thing']} auxiliary payload was not recorded exactly on {key}")
            if before_payload[key] != payload(pickups_after[key]):
                changed.append(key)
        for key, previous in non_map.items():
            require(payload(pickups_after[key]) == previous, f"{name}: non-map/generated/drop payload changed on {key}")
        if not changed:
            require(hashlib.sha256(path.read_bytes()).hexdigest() == map_before, f"{name}: an unchanged map was unnecessarily rewritten")
        report.append({
            "map": name, "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            "source_things": len(things), "source_type_counts": dict(sorted(Counter(row[2] for row in things).items())),
            "checked_actors": len(before), "checked_components": len(components_before),
            "bound_pickups": expected, "unique_bound_source_ids": len({entry["thing"] for entry in expected.values()}),
            "non_map_pickups": len(non_map), "changed_payloads": changed, "unchanged_asset_sha256": files_before,
        })
        unreal.log(f"Verified pickup metadata {name}: {len(expected)} source bindings, {len(changed)} changed payloads, "
                   f"{len(non_map)} non-map payloads unchanged; {len(components_before)} component states preserved")
    destination = project / "Saved/ReviewArtifacts/PickupMetadataRefresh.json"
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps({"verified_maps": len(report), "maps": report}, indent=2) + "\n", encoding="utf-8")
    unreal.log(f"Verified auxiliary metadata on all {len(report)} maps without geometry rebuilds; report: {destination}")


if __name__ == "__main__":
    refresh_pickup_metadata()
