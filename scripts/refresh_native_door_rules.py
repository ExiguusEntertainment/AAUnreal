"""Refresh saved campaign door rules from the original MAP markers.

Run in Unreal Editor after building the port. Do not run gameplay readers while
this script saves maps. AA.ImportWadDoors preserves the existing geometry and
requires the native doors to be at their authored closed heights.
"""

from pathlib import Path
import re

import unreal


def refresh_doors():
    project = Path(unreal.Paths.project_dir()).resolve()
    source_root = project / "Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Exe"
    maps = sorted(
        path for path in (project / "Content/Maps").glob("L*.umap")
        if re.fullmatch(r"L\d+", path.stem)
    )
    if len(maps) != 30:
        raise RuntimeError(f"Expected all 30 campaign maps, found {len(maps)}")

    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    total = 0
    for path in maps:
        source = source_root / f"{path.stem}.MAP"
        if not source.is_file():
            raise RuntimeError(f"Missing original map: {source}")
        world = unreal.EditorLoadingAndSavingUtils.load_map(str(path))
        if world is None:
            raise RuntimeError(f"Could not open native map: {path}")
        levels = [actor for actor in actors.get_all_level_actors()
                  if isinstance(actor, unreal.NativeWadLevel)]
        if len(levels) != 1:
            raise RuntimeError(f"Expected one native level actor in {path.stem}")
        unreal.SystemLibrary.execute_console_command(
            world, f'AA.ImportWadDoors "{source.as_posix()}" {path.stem}'
        )
        doors = levels[0].get_editor_property("doors")
        for door in doors:
            strength = door.get_editor_property("lock_strength")
            if not (0 <= strength <= 255 or strength == 65535):
                raise RuntimeError(f"Unconverted lock strength in {path.stem}: {strength}")
            if door.get_editor_property("required_item") > 0 and strength != 65535:
                raise RuntimeError(f"Key door lost its absolute lock in {path.stem}")
        total += len(doors)
        unreal.log(f"Verified saved door rules: {path.stem}, {len(doors)} doors")
    unreal.log(f"Verified saved door rules for all {len(maps)} maps, {total} doors")


refresh_doors()
