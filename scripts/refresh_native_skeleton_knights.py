"""Refresh only source1033 Skeleton Knights in L34/L74, retaining all saved geometry.

Requires AA.RefreshNativeWadSkeletonKnights <WadPath> <MapName> <MapPackage>.
Uses public scene/component properties and original Thing tags, never a private
encounter-profile property. Call refresh_skeleton_knights(False) after reopening
in another Unreal process to verify saved placement/art without further import.
"""
from pathlib import Path
import importlib.util
import json
import re
import struct
import unreal


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def source_things(path):
    data = path.read_bytes()
    magic, count, directory = struct.unpack_from("<4sii", data)
    require(magic in (b"IWAD", b"PWAD") and count > 0 and directory >= 12
            and directory + count*16 <= len(data), f"Invalid source MAP: {path}")
    for index in range(count):
        offset, size, name = struct.unpack_from("<ii8s", data, directory+index*16)
        require(offset >= 0 and size >= 0 and offset+size <= len(data), "Invalid source lump extent")
        if name.rstrip(b"\0") == b"THINGS":
            return list(struct.iter_unpack("<hhHHH", data[offset:offset+size]))
    raise RuntimeError(f"Missing THINGS: {path}")


def actor_transform(actor):
    position = actor.get_actor_location()
    rotation = actor.get_actor_rotation()
    scale = actor.get_actor_scale3d()
    return ((position.x, position.y, position.z), (rotation.pitch, rotation.yaw, rotation.roll),
            (scale.x, scale.y, scale.z))


def refresh_skeleton_knights(refresh=True):
    project = Path(unreal.Paths.project_dir()).resolve()
    source = project / "Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Exe"
    spec = importlib.util.spec_from_file_location("aa_skeleton_snapshot", project/"scripts/refresh_native_missing_materials.py")
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    editor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    report = []
    total = 0
    for name, expected_count in (("L34", 30), ("L74", 5)):
        things = source_things(source/f"{name}.MAP")
        expected = {i: row for i, row in enumerate(things) if row[3] == 1033}
        require(len(expected) == expected_count, f"{name}: original Skeleton Knight placement count changed")
        world = unreal.EditorLoadingAndSavingUtils.load_map(str(project/f"Content/Maps/{name}.umap"))
        require(world is not None, f"Could not open {name}")

        def bindings():
            found = {}
            for actor in editor.get_all_level_actors():
                if not isinstance(actor, unreal.NativeWadGuardian):
                    continue
                ids = [int(match[1]) for tag in actor.get_editor_property("tags")
                       if (match := re.fullmatch(r"WAD Creature Thing (\d+)", str(tag))) and int(match[1]) in expected]
                require(len(ids) <= 1, f"{name}: ambiguous source tags on {actor.get_name()}")
                if ids:
                    require(ids[0] not in found, f"{name}: duplicate Skeleton Knight Thing {ids[0]}")
                    found[ids[0]] = actor
            return found

        before_binding = bindings()
        before_identity = {i: (a.get_path_name(), tuple(str(t) for t in a.get_editor_property("tags")), actor_transform(a))
                           for i, a in before_binding.items()}
        all_before = {a.get_path_name(): helper.snapshot(a) for a in editor.get_all_level_actors()}
        others_before = {p: record for p, record in all_before.items() if p not in {a.get_path_name() for a in before_binding.values()}}
        flat_before = {f"{a}/{c}": v for a, record in all_before.items() for c, v in record.items()}
        hashes_before = helper.mesh_hashes(project, flat_before)
        if refresh:
            unreal.SystemLibrary.execute_console_command(world,
                f'AA.RefreshNativeWadSkeletonKnights "{(source/(name+".MAP")).as_posix()}" {name} /Game/Maps/{name}')
        after_binding = bindings()
        require(after_binding.keys() == expected.keys(), f"{name}: missing/duplicate source1033 placements after refresh")
        for index, identity in before_identity.items():
            actor = after_binding[index]
            require((actor.get_path_name(), tuple(str(t) for t in actor.get_editor_property("tags")), actor_transform(actor)) == identity,
                    f"{name}: existing Skeleton Knight identity/tags/transform changed at Thing {index}")
        all_after = {a.get_path_name(): helper.snapshot(a) for a in editor.get_all_level_actors()}
        require(all_before.keys() <= all_after.keys(), f"{name}: refresh deleted an existing actor")
        for identity, before in others_before.items():
            require(all_after[identity] == before, f"{name}: changed a non-Skeleton actor or native geometry: {identity}")
        require(len(all_after)-len(all_before) == len(expected)-len(before_binding), f"{name}: unexpected added actors")
        flat_after = {f"{a}/{c}": v for a, record in all_after.items() for c, v in record.items()}
        require(hashes_before == helper.mesh_hashes(project, flat_after), f"{name}: changed saved geometry mesh bytes")
        for index, actor in after_binding.items():
            movement = actor.get_components_by_class(unreal.CharacterMovementComponent)[0]
            capsule = actor.get_components_by_class(unreal.CapsuleComponent)[0]
            sprite = next(c for c in actor.get_components_by_class(unreal.StaticMeshComponent) if c.get_name() == "GuardianSprite")
            require(movement.get_editor_property("max_walk_speed") == 70.0 and movement.get_editor_property("max_fly_speed") == 70.0,
                    f"{name}/{index}: source movement caps not saved")
            require(capsule.get_unscaled_capsule_radius() == 15.0 and capsule.get_unscaled_capsule_half_height() == 38.5,
                    f"{name}/{index}: source collision size not saved")
            material = sprite.get_material(0)
            require(material is not None and material.get_path_name() == f"/Game/Maps/{name}_Assets/Things/M_SKELKNT.M_SKELKNT",
                    f"{name}/{index}: wrong original standing material")
            scale = sprite.get_editor_property("relative_scale3d")
            require(abs(scale.x-0.76) < 1e-6 and abs(scale.y-0.77) < 1e-6,
                    f"{name}/{index}: source76x77 art dimensions changed")
            if index not in before_binding:
                location = actor.get_actor_location()
                require(abs(location.x-expected[index][0]) < 1e-6 and abs(location.y-expected[index][1]) < 1e-6,
                        f"{name}/{index}: new knight is not at its authored XY")
        total += len(after_binding)
        report.append({"map": name, "source1033_count": len(after_binding), "retained_actor_paths": before_identity,
                       "added_things": sorted(set(after_binding)-set(before_binding)), "unchanged_mesh_sha256": hashes_before})
        unreal.log(f"Verified original Skeleton Knights in {name}: {len(after_binding)} placements; all native geometry/non-Skeleton actors preserved")
    output = project/"Saved/ReviewArtifacts"/("SkeletonKnightRefresh.json" if refresh else "SkeletonKnightReopen.json")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps({"verified_placements": total, "refreshed": refresh, "maps": report}, indent=2)+"\n", encoding="utf-8")
    unreal.log(f"Verified {total} original Skeleton Knight placements across2 maps: {output}")


if __name__ == "__main__":
    refresh_skeleton_knights()
