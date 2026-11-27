"""Bounded editor refresh/reopen proof for L151's two original barbarian families.
Run only after the matching C++ patch is built. Does not rebuild level geometry.
To verify a fresh process without mutation, call verify_l151(False).
"""
from pathlib import Path
from collections import Counter
import importlib.util
import json
import struct
import unreal


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def original_things(path):
    data = path.read_bytes()
    signature, count, directory = struct.unpack_from("<4sII", data)
    require(signature in (b"PWAD", b"IWAD") and directory + count * 16 <= len(data), "Invalid original L151 MAP")
    for index in range(count):
        offset, size, name = struct.unpack_from("<II8s", data, directory + index * 16)
        if name.rstrip(b"\0") == b"THINGS":
            require(size % 10 == 0 and offset + size <= len(data), "Invalid original THINGS")
            return list(struct.iter_unpack("<hhhHH", data[offset:offset + size]))
    raise RuntimeError("Original L151 has no THINGS")


def verify_l151(refresh=True):
    project = Path(unreal.Paths.project_dir()).resolve()
    helper_path = project / "Scripts/refresh_native_missing_materials.py"
    spec = importlib.util.spec_from_file_location("aa_l151_geometry", helper_path)
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    source = project / "Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Exe/L151.MAP"
    things = original_things(source)
    expected = {index: kind for index, (_, _, _, kind, _) in enumerate(things) if kind in (3001, 3002)}
    require(Counter(expected.values()) == {3001: 11, 3002: 9} and expected[31] == 3002, "Original L151 family counts changed")
    world = unreal.EditorLoadingAndSavingUtils.load_map(str(project / "Content/Maps/L151.umap"))
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    levels = [a for a in actors.get_all_level_actors() if isinstance(a, unreal.NativeWadLevel)]
    require(world is not None and len(levels) == 1, "Could not load one saved L151 level")
    level = levels[0]
    geometry = helper.snapshot(level)
    meshes = helper.mesh_hashes(project, geometry)

    def actor_transform(actor):
        point = actor.get_actor_location()
        rotation = actor.get_actor_rotation()
        scale = actor.get_actor_scale3d()
        return ((point.x, point.y, point.z), (rotation.pitch, rotation.yaw, rotation.roll),
                (scale.x, scale.y, scale.z))

    def other_actors():
        return {a.get_path_name(): (actor_transform(a), tuple(str(t) for t in a.get_editor_property("tags")))
                for a in actors.get_all_level_actors() if not isinstance(a, unreal.NativeWadGuardian)}

    untouched = other_actors()

    def bindings():
        found = {}
        for actor in actors.get_all_level_actors():
            if not isinstance(actor, unreal.NativeWadGuardian):
                continue
            tags = [str(t) for t in actor.get_editor_property("tags")]
            ids = [int(tag.rsplit(" ", 1)[1]) for tag in tags if tag.startswith("WAD Creature Thing ")]
            if "L151 Gaiseric Magician" in tags:
                require(tags[0] == "L151 Gaiseric Magician" and not ids, "Legacy slice primary identity was replaced or duplicated")
                ids = [31]
            if not ids:
                continue
            require(len(set(ids)) == 1 and ids[0] in expected and ids[0] not in found, "Duplicate or foreign source creature identity")
            found[ids[0]] = actor
        return found

    old = {index: actor.get_path_name() for index, actor in bindings().items()}
    require(31 in old, "The pre-existing L151 slice actor was not found")
    passes = 2 if refresh else 1
    for repeat in range(passes):
        if refresh:
            unreal.SystemLibrary.execute_console_command(world,
                f'AA.RefreshNativeWadL151Encounters "{source.as_posix()}" L151 /Game/Maps/L151')
        current = bindings()
        require(current.keys() == expected.keys(), "Saved native L151 does not contain the exact20 original family placements")
        for index, actor in current.items():
            kind = expected[index]
            point = actor.get_actor_location()
            require((point.x, point.y) == things[index][:2], f"Thing{index} moved away from its original XY")
            movement = actor.get_components_by_class(unreal.CharacterMovementComponent)[0]
            require(abs(movement.get_editor_property("max_walk_speed") - (70 if kind == 3001 else 100)) < 0.01,
                    f"Thing{index} has the wrong original update/step-derived speed")
            capsule = actor.get_components_by_class(unreal.CapsuleComponent)[0]
            require(abs(capsule.get_scaled_capsule_radius() - 20) < 0.01, f"Thing{index} lost source radius20")
            sprites = actor.get_components_by_class(unreal.StaticMeshComponent)
            require(len(sprites) == 1 and sprites[0].get_material(0) is not None, f"Thing{index} lacks saved original art")
            require(index not in old or actor.get_path_name() == old[index], f"Refresh replaced existing Thing{index}")
        require(geometry == helper.snapshot(level) and meshes == helper.mesh_hashes(project, helper.snapshot(level)),
                "Encounter-only refresh changed native geometry, collision, materials, lighting shells or mesh asset bytes")
        require(untouched == other_actors(), "Encounter-only refresh changed pickups, scenery, starts or the level actor")
        old = {index: actor.get_path_name() for index, actor in current.items()}
    result = {"source_counts": dict(Counter(expected.values())), "source_ids": old,
              "retained_slice": old[31], "refresh_passes": passes if refresh else 0,
              "unchanged_geometry_components": len(geometry), "unchanged_mesh_hashes": meshes}
    destination = project / "Saved/ReviewArtifacts/L151EncounterRefresh.json"
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(result, indent=2), encoding="utf-8")
    unreal.log(f"Verified exact20 L151 barbarian placements, preserved slice identity and unchanged geometry; report: {destination}")


if __name__ == "__main__":
    verify_l151()
