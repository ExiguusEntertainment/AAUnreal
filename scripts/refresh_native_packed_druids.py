"""Restore only L31/L22 exact packed Druids and their original palette art.

Required hook: AA.RefreshNativeWadPackedDruids <WadPath> <MapName> <MapPackage>.
Refresh item catalog first with AA.RefreshWadItemRules (new treasure415/825 and
existing mace234 need source world art). Call refresh_packed_druids(False) in a
fresh editor process to verify saved source counts and the captured invariants.
"""
from pathlib import Path
import hashlib
import importlib.util
import json
import struct
import unreal


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def normalized(value):
    return json.loads(json.dumps(value, sort_keys=True))


def things(path):
    data = path.read_bytes()
    magic, count, directory = struct.unpack_from("<4sii", data)
    require(magic in (b"IWAD", b"PWAD") and count > 0 and directory >= 12
            and directory+count*16 <= len(data), "Invalid original map directory")
    for i in range(count):
        offset, size, name = struct.unpack_from("<ii8s", data, directory+16*i)
        require(offset >= 0 and size >= 0 and offset+size <= len(data), "Invalid source lump bounds")
        if name.rstrip(b"\0") == b"THINGS":
            require(size % 10 == 0, "Invalid source THINGS size")
            return {i: row for i, row in enumerate(struct.iter_unpack("<hhHHH", data[offset:offset+size]))
                    if row[3] in (33787, 46075)}
    raise RuntimeError("No original THINGS")


def refresh_packed_druids(refresh=True):
    project = Path(unreal.Paths.project_dir()).resolve()
    spec = importlib.util.spec_from_file_location("aa_packed_snapshot", project/"scripts/refresh_native_missing_materials.py")
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    editor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    destination = project/"Saved/ReviewArtifacts/PackedDruids-refresh.json"
    destination.parent.mkdir(parents=True, exist_ok=True)
    report = {} if refresh else json.loads(destination.read_text(encoding="utf-8"))
    for name in ("L31", "L22"):
        source = project/f"Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Exe/{name}.MAP"
        expected = things(source)
        require(sum(row[3] == 33787 for row in expected.values()) == (5 if name == "L31" else 8)
                and sum(row[3] == 46075 for row in expected.values()) == (0 if name == "L31" else 1), "Source placement counts changed")
        world = unreal.EditorLoadingAndSavingUtils.load_map(str(project/f"Content/Maps/{name}.umap"))
        require(world is not None, f"Could not open native {name}")

        def snapshot():
            actors, meshes = {}, {}
            for actor in editor.get_all_level_actors():
                p, r, s = actor.get_actor_location(), actor.get_actor_rotation(), actor.get_actor_scale3d()
                components = helper.snapshot(actor)
                state = {"class": actor.get_class().get_path_name(), "label": actor.get_actor_label(),
                         "tags": [str(t) for t in actor.tags], "components": components,
                         "transform": ((p.x, p.y, p.z), (r.pitch, r.yaw, r.roll), (s.x, s.y, s.z))}
                if isinstance(actor, unreal.NativeWadGuardian):
                    state.update(source=actor.get_source_type(), health=actor.get_encounter_health())
                actors[actor.get_path_name()] = state
                meshes.update(helper.mesh_hashes(project, components))
            return normalized({"actors": actors, "meshes": meshes})

        def verify():
            identities = {}
            for actor in editor.get_all_level_actors():
                if not isinstance(actor, unreal.NativeWadGuardian) or actor.get_source_type() not in (33787, 46075):
                    continue
                matching = [i for i in expected if f"WAD Creature Thing {i}" in [str(t) for t in actor.tags]]
                require(len(matching) == 1 and matching[0] not in identities, "Duplicate/ambiguous packed source actor")
                i = matching[0]
                require(actor.get_source_type() == expected[i][3], "Packed source ID was flattened or swapped")
                require(actor.get_portrait_material() is not None, "No saved original palette art")
                identities[i] = actor.get_path_name()
            require(set(identities) == set(expected), f"{name}: missing exact source placements")
            return identities

        if refresh:
            before = snapshot()
            unreal.SystemLibrary.execute_console_command(world, f'AA.RefreshNativeWadPackedDruids "{source}" {name} /Game/Maps/{name}')
            identities = verify()
            after = snapshot()
            target_paths = set(identities.values())
            require(set(before["actors"]) <= set(after["actors"]), "An existing actor was removed/replaced")
            require(set(after["actors"])-set(before["actors"]) <= target_paths, "Unexpected unrelated actors were spawned")
            for path, state in before["actors"].items():
                current = after["actors"][path]
                if path not in target_paths:
                    require(current == state, f"Unrelated actor/components/geometry/materials changed: {path}")
                    continue
                require(current["class"] == state["class"] and current["label"] == state["label"]
                        and current["transform"] == state["transform"] and current["health"] == state["health"],
                        "Existing packed/legacy boss actor lost its identity, transform or health")
                require(not state["tags"] or current["tags"][0] == state["tags"][0], "Old primary checkpoint tag was replaced")
                require(set(state["tags"]) <= set(current["tags"]), "An existing boss/Thing tag was removed")
                require(set(current["components"]) == set(state["components"]), "Existing target component identities changed")
                for component, old in state["components"].items():
                    if component not in ("Sprite", "GuardianSprite", "CapsuleComponent"):
                        require(current["components"][component] == old, "A target's unrelated component changed")
            for path, digest in before["meshes"].items():
                filename = project/"Content"/(path.split(".",1)[0][len("/Game/"):] + ".uasset")
                require(hashlib.sha256(filename.read_bytes()).hexdigest() == digest, f"Existing mesh asset rewritten: {path}")
            require(unreal.EditorLoadingAndSavingUtils.save_map(world, f'/Game/Maps/{name}'),
                    f'{name}: public map save failed; in-memory actors are not persisted proof')
            report[name] = {"saved": after, "source_identities": identities,
                            "preserved_mesh_sha256": before["meshes"]}
        else:
            require(snapshot() == report[name]["saved"], f"{name}: saved actors/art/geometry differ in fresh process")
            require({str(k):v for k,v in verify().items()} == report[name]["source_identities"], "Fresh source actor identities changed")
            for path, digest in report[name]["preserved_mesh_sha256"].items():
                filename = project/"Content"/(path.split(".",1)[0][len("/Game/"):] + ".uasset")
                require(hashlib.sha256(filename.read_bytes()).hexdigest() == digest, "Fresh process found rewritten original mesh")
        unreal.log(f"Verified {name}: {len(expected)} exact packed Druid placements, retained prior actor/quest identities and all unrelated geometry/components")
    if refresh:
        destination.write_text(json.dumps(report, indent=2), encoding="utf-8")


if __name__ == "__main__":
    refresh_packed_druids()
