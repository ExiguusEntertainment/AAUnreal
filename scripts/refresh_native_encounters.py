"""Refresh restored encounters without rebuilding saved campaign geometry.

Run inside Unreal Editor after building the port, with no gameplay readers open.
Validate placed creature counts against the original THINGS lumps and preserve
every native surface's mesh and transform. Generator pools are checked separately
by the gameplay reviews and are excluded from the placed-actor counts here.
"""

from collections import Counter
from pathlib import Path
import struct

import unreal


def source_types(path):
    data = path.read_bytes()
    signature, count, directory = struct.unpack_from("<4sii", data)
    if signature not in (b"PWAD", b"IWAD"):
        raise RuntimeError(f"Invalid original WAD: {path}")
    for index in range(count):
        offset, size, name = struct.unpack_from("<ii8s", data, directory + index * 16)
        if name.rstrip(b"\0") == b"THINGS":
            return [record[3] for record in struct.iter_unpack("<hhhhh", data[offset:offset + size])]
    raise RuntimeError(f"Missing original THINGS: {path}")


def geometry(level):
    return {
        surface.get_name(): (
            surface.get_editor_property("static_mesh").get_path_name()
            if surface.get_editor_property("static_mesh") else None,
            str(surface.get_editor_property("relative_location")),
            str(surface.get_editor_property("relative_rotation")),
            str(surface.get_editor_property("relative_scale3d")),
        )
        for surface in level.get_components_by_class(unreal.StaticMeshComponent)
        if surface.get_name().startswith(("Sector_", "Side_"))
    }


def refresh_encounters():
    project = Path(unreal.Paths.project_dir()).resolve()
    source = project / "Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Exe"
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    restored_types = (1005, 1006, 1007, 1021, 1022, 1037)
    # The private encounter profile is covered by C++ gameplay regressions.
    # Validate the saved public movement/collision/art here without exposing it.
    profiles = {
        1005: (105.0, 105.0, 55.0, "M_DRAGON"),
        1021: (49.0, 218.75, 50.0, "M_DRAGON2"),
        1022: (70.0, 70.0, 42.0, "M_HYDRA"),
        1037: (49.0, 175.0, 80.0, "M_DRAGON4"),
    }
    for name in ("L40", "L71", "L10", "L32", "L51"):
        path = project / "Content/Maps" / f"{name}.umap"
        world = unreal.EditorLoadingAndSavingUtils.load_map(str(path))
        levels = [actor for actor in actors.get_all_level_actors() if isinstance(actor, unreal.NativeWadLevel)]
        if world is None or len(levels) != 1:
            raise RuntimeError(f"Could not open one native level in {name}")
        before = geometry(levels[0])
        types = source_types(source / f"{name}.MAP")
        original = Counter(types)
        unreal.SystemLibrary.execute_console_command(world, f'AA.ImportWadGameplay "{(source / (name + ".MAP")).as_posix()}" {name}')
        if before != geometry(levels[0]):
            raise RuntimeError(f"Encounter refresh changed native geometry in {name}")
        placed = Counter()
        for actor in actors.get_all_level_actors():
            if not isinstance(actor, unreal.NativeWadGuardian) or not actor.get_actor_label().startswith("WAD Creature Thing "):
                continue
            index = int(actor.get_actor_label().rsplit(" ", 1)[-1])
            kind = types[index]
            placed[kind] += 1
            if kind in profiles:
                movement = actor.get_components_by_class(unreal.CharacterMovementComponent)[0]
                sprites = actor.get_components_by_class(unreal.StaticMeshComponent)
                actual = (
                    movement.get_editor_property("max_walk_speed"),
                    movement.get_editor_property("max_fly_speed"),
                    actor.get_components_by_class(unreal.CapsuleComponent)[0].get_unscaled_capsule_radius(),
                    sprites[0].get_material(0).get_name(),
                )
                if actual != profiles[kind]:
                    raise RuntimeError(f"Wrong source profile in {name}: type {kind}, {actual}")
        for kind in restored_types:
            if placed[kind] != original[kind]:
                raise RuntimeError(f"Missing/duplicate placed type {kind} in {name}: {placed[kind]} != {original[kind]}")
        unreal.log(f"Verified restored encounter counts and preserved {len(before)} surfaces: {name}, "
                   + str({kind: placed[kind] for kind in restored_types if placed[kind]}))
    unreal.log("Verified restored encounters across all five affected maps without geometry changes")


refresh_encounters()
