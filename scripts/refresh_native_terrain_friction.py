"""Refresh only saved sector friction on all 30 native maps; no gameplay/geometry import.

Requires AA.RefreshNativeWadTerrainFriction <WadPath> <MapName> <MapPackage>.
Run in Unreal Python after compiling the runtime/editor changes. To verify a
fresh-process reopen without saving, call refresh_terrain_friction(False).
"""
from pathlib import Path
from collections import Counter
import hashlib
import importlib.util
import json
import re
import struct
import unreal


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def source_friction(path):
    data = path.read_bytes()
    require(len(data) >= 12, f"Truncated MAP: {path}")
    magic, count, index = struct.unpack_from("<4sii", data)
    require(magic in (b"IWAD", b"PWAD") and count > 0 and index >= 12
            and index + count * 16 <= len(data), f"Invalid MAP: {path}")
    lumps = {}
    for entry in range(count):
        offset, length, raw_name = struct.unpack_from("<ii8s", data, index + entry * 16)
        require(offset >= 0 and length >= 0 and offset + length <= len(data), "Out-of-range MAP lump")
        lumps[raw_name.rstrip(b"\0").decode("ascii")] = data[offset:offset+length]
    vertices = list(struct.iter_unpack("<hh", lumps["VERTEXES"]))
    sides = list(struct.iter_unpack("<hh8s8s8sH", lumps["SIDEDEFS"]))
    sectors = list(struct.iter_unpack("<hh8s8sHHH", lumps["SECTORS"]))
    boundaries = [[] for _ in sectors]
    for a, b, flags, special, tag, front, back in struct.iter_unpack("<7H", lumps["LINEDEFS"]):
        require(a < len(vertices) and b < len(vertices), "Invalid source line vertex")
        for side in (front, back):
            if side != 65535:
                require(side < len(sides) and sides[side][5] < len(sectors), "Invalid source line side")
                boundaries[sides[side][5]].append((vertices[a], vertices[b]))

    def inside(edges, x, y):
        hit = False
        for (ax, ay), (bx, by) in edges:
            if (ay > y) != (by > y) and x < ax + (y-ay)*(bx-ax)/(by-ay):
                hit = not hit
        return hit

    values = [12] * len(sectors)
    markers = []
    source_values = {1503: 12, 1504: 2, 1505: 16, 1506: 24, 1507: 14, 1543: 24}
    for thing_id, (x, y, angle, kind, auxiliary) in enumerate(struct.iter_unpack("<hhHHH", lumps["THINGS"])):
        if kind not in source_values:
            continue
        sector = next((i for i, edges in enumerate(boundaries) if inside(edges, x, y)), None)
        require(sector is not None, f"{path.stem}: terrain Thing {thing_id} outside source footprints")
        values[sector] = source_values[kind]  # Last authored marker wins, matching source traversal.
        markers.append({"thing": thing_id, "type": kind, "sector": sector, "friction": source_values[kind]})
    return values, markers


def refresh_terrain_friction(refresh=True):
    project = Path(unreal.Paths.project_dir()).resolve()
    helper_file = project / "scripts/refresh_native_missing_materials.py"
    spec = importlib.util.spec_from_file_location("aa_surface_snapshot", helper_file)
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)  # Reuse the previously verified public component snapshot APIs.
    maps = sorted(p for p in (project / "Content/Maps").glob("L*.umap") if re.fullmatch(r"L\d+", p.stem))
    require(len(maps) == 30, f"Expected all 30 maps, found {len(maps)}")
    editor_actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    report = []
    for path in maps:
        name = path.stem
        source = project / f"Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Exe/{name}.MAP"
        expected, markers = source_friction(source)
        world = unreal.EditorLoadingAndSavingUtils.load_map(str(path))
        actors = editor_actors.get_all_level_actors()
        levels = [a for a in actors if isinstance(a, unreal.NativeWadLevel)]
        require(world is not None and len(levels) == 1, f"{name}: expected one native level")
        level = levels[0]

        def snapshot_all():
            result = {}
            for actor in editor_actors.get_all_level_actors():
                identity = actor.get_path_name()
                location = actor.get_actor_location()
                rotation = actor.get_actor_rotation()
                scale = actor.get_actor_scale3d()
                result[identity] = {"transform": {
                    "location": [location.x, location.y, location.z],
                    "rotation": [rotation.pitch, rotation.yaw, rotation.roll],
                    "scale": [scale.x, scale.y, scale.z],
                }, "components": helper.snapshot(actor)}
            return result

        before = snapshot_all()
        flat_before = {f"{a}/{c}": entry for a, record in before.items() for c, entry in record["components"].items()}
        hashes = helper.mesh_hashes(project, flat_before)
        # New public int32 getter only: no private UPROPERTY access from Python.
        previous = [level.get_wad_sector_friction(i) for i in range(len(expected))]
        require(level.get_wad_sector_friction(-1) == -1 and level.get_wad_sector_friction(len(expected)) == -1,
                f"{name}: terrain getter/source sector count mismatch")
        if refresh:
            unreal.SystemLibrary.execute_console_command(world,
                f'AA.RefreshNativeWadTerrainFriction "{source.as_posix()}" {name} /Game/Maps/{name}')
        actual = [level.get_wad_sector_friction(i) for i in range(len(expected))]
        require(actual == expected, f"{name}: saved terrain friction differs from original markers")
        after = snapshot_all()
        if before != after:
            diagnostic = project / "Saved/ReviewArtifacts" / f"TerrainFriction-{name}-state-difference.json"
            diagnostic.parent.mkdir(parents=True, exist_ok=True)
            changed = {key: {"before": before.get(key), "after": after.get(key)}
                       for key in before.keys() | after.keys() if before.get(key) != after.get(key)}
            diagnostic.write_text(json.dumps(changed, indent=2) + "\n", encoding="utf-8")
            unreal.log_error(f"Terrain metadata state difference: {diagnostic}")
        require(before == after, f"{name}: metadata refresh changed actors/components/transforms/collision/materials")
        flat_after = {f"{a}/{c}": entry for a, record in after.items() for c, entry in record["components"].items()}
        require(hashes == helper.mesh_hashes(project, flat_after), f"{name}: existing mesh asset bytes changed")
        report.append({"map": name, "sectors": len(expected), "friction_counts": dict(Counter(actual)),
                       "changed_sector_metadata": [i for i, (a, b) in enumerate(zip(previous, actual)) if a != b],
                       "markers": markers, "actors_checked": len(before), "unchanged_mesh_sha256": hashes})
        unreal.log(f"Verified {'metadata refresh' if refresh else 'saved reopen'} {name}: {len(expected)} sectors, "
                   f"{len(before)} actors/components unchanged")
    output = project / "Saved/ReviewArtifacts" / ("TerrainFrictionRefresh.json" if refresh else "TerrainFrictionReopen.json")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps({"verified_maps": len(report), "refreshed": refresh, "maps": report}, indent=2)+"\n", encoding="utf-8")
    unreal.log(f"Verified terrain friction in all {len(report)} maps: {output}")


if __name__ == "__main__":
    refresh_terrain_friction()
