"""Restore66 verified exact packed creature placements on seven native maps.

Requires enabled profile/loot package + shared creature-art core. Register:
AA.RefreshNativeWadPackedEncounterFamilies <WadPath> <MapName> <MapPackage>
Run AA.RefreshWadItemRules first for new packed loot rules/world art. This hook
updates only the five families; it never reinstalls scripts, generators, quest
pickups, geometry or collision components belonging to unrelated actors.
Call refresh_packed_encounter_families(False) in a fresh process to verify saved
actors/art/geometry against the captured baseline, without mutations.
"""
from pathlib import Path
import hashlib
import importlib.util
import json
import struct
import unreal

TYPES = {16368,24560,33803,37886,54268}
EXPECTED = {"L153":{37886:2},"L231":{16368:10,24560:1},"L75":{16368:7},"L10":{24560:15},
            "L51":{33803:22},"L44":{37886:5},"L61":{37886:3,54268:1}}


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def norm(value):
    return json.loads(json.dumps(value, sort_keys=True))


def source_rows(path):
    data = path.read_bytes()
    magic, count, directory = struct.unpack_from("<4sii", data)
    require(magic in (b"IWAD",b"PWAD") and count > 0 and directory >= 12 and directory+count*16 <= len(data), "Invalid sourceMAP")
    for i in range(count):
        offset, size, name = struct.unpack_from("<ii8s",data,directory+16*i)
        require(offset >= 0 and size >= 0 and offset+size <= len(data), "Invalid source lump")
        if name.rstrip(b"\0") == b"THINGS":
            require(size % 10 == 0, "Invalid sourceTHINGS")
            return {i:row for i,row in enumerate(struct.iter_unpack("<hhHHH",data[offset:offset+size])) if row[3] in TYPES}
    raise RuntimeError("No sourceTHINGS")


def refresh_packed_encounter_families(refresh=True):
    project = Path(unreal.Paths.project_dir()).resolve()
    spec = importlib.util.spec_from_file_location("aa_family_snapshot",project/"scripts/refresh_native_missing_materials.py")
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    editor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    report_path = project/"Saved/ReviewArtifacts/PackedEncounterFamilies-refresh.json"
    report_path.parent.mkdir(parents=True,exist_ok=True)
    report = {} if refresh else json.loads(report_path.read_text(encoding="utf-8"))
    total = 0
    for name, counts in EXPECTED.items():
        source = project/f"Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Exe/{name}.MAP"
        expected = source_rows(source)
        require({t:sum(row[3] == t for row in expected.values()) for t in counts} == counts and len(expected) == sum(counts.values()), "Source creature census changed")
        world = unreal.EditorLoadingAndSavingUtils.load_map(str(project/f"Content/Maps/{name}.umap"))
        require(world is not None,f"Could not open saved{name}")

        def assets():
            return {str(path.relative_to(project)):hashlib.sha256(path.read_bytes()).hexdigest()
                    for path in (project/f"Content/Maps/{name}_Assets").rglob("*.uasset")}

        def snapshot():
            actors, meshes = {}, {}
            for actor in editor.get_all_level_actors():
                p,r,s = actor.get_actor_location(),actor.get_actor_rotation(),actor.get_actor_scale3d()
                components = helper.snapshot(actor)
                state = {"class":actor.get_class().get_path_name(),"label":actor.get_actor_label(),"tags":[str(t) for t in actor.tags],
                         "transform":((p.x,p.y,p.z),(r.pitch,r.yaw,r.roll),(s.x,s.y,s.z)),"components":components}
                if isinstance(actor,unreal.NativeWadGuardian):
                    material = actor.get_portrait_material()
                    state.update(source=actor.get_source_type(),health=actor.get_encounter_health(),portrait=material.get_path_name() if material else None)
                actors[actor.get_path_name()] = state
                meshes.update(helper.mesh_hashes(project,components))
            return norm({"actors":actors,"meshes":meshes})

        def verify():
            identities = {}
            for actor in editor.get_all_level_actors():
                if not isinstance(actor,unreal.NativeWadGuardian) or actor.get_source_type() not in TYPES:
                    continue
                ids = [i for i in expected if f"WAD Creature Thing {i}" in [str(t) for t in actor.tags]]
                require(len(ids) == 1 and ids[0] not in identities,"Duplicate or unbound full source identity")
                i = ids[0]
                require(actor.get_source_type() == expected[i][3],"A packed identity was flattened/swapped")
                require(actor.get_portrait_material() is not None,"Missing original source art")
                identities[i] = actor.get_path_name()
            require(set(identities) == set(expected),f"{name}: exact source placements missing")
            return identities

        levels=[a for a in editor.get_all_level_actors() if isinstance(a,unreal.NativeWadLevel)]
        require(len(levels)==1,"Expected one native level")
        def script_state():
            value=json.loads(levels[0].describe_wad_script_missiles())
            require("generator_bindings" in value,"Missing saved generator ownership diagnostics")
            # Checkpoint export is a top-level named-field tuple. Preserve all
            # script/door/slide/generator/pickup state; only encounter membership
            # may grow when restoring the omitted original creature placements.
            text=value.pop("state")
            require(text.startswith("(") and text.endswith(")"),"Unexpected checkpoint export format")
            fields=[];start=1;depth=0;quoted=False;escaped=False
            for i,c in enumerate(text[1:-1],1):
                if quoted:
                    if escaped: escaped=False
                    elif c=="\\": escaped=True
                    elif c=='"': quoted=False
                elif c=='"': quoted=True
                elif c=="(": depth+=1
                elif c==")": depth-=1
                elif c=="," and depth==0: fields.append(text[start:i]);start=i+1
            fields.append(text[start:-1])
            require(depth==0 and not quoted,"Unbalanced checkpoint export")
            value["state_without_encounters"]=[f for f in fields if not f.lstrip().startswith("Encounters=")]
            return value

        if refresh:
            before, old_assets, old_script = snapshot(),assets(),script_state()
            unreal.SystemLibrary.execute_console_command(world,f'AA.RefreshNativeWadPackedEncounterFamilies "{source}" {name} /Game/Maps/{name}')
            require(unreal.EditorLoadingAndSavingUtils.save_map(world,f"/Game/Maps/{name}"),f"{name}: explicit public world save failed")
            identities = verify()
            after, new_assets = snapshot(),assets()
            require(script_state()==old_script,"Script state/callbacks/generators/quest pickups changed")
            target_paths = set(identities.values())
            require(set(before["actors"]) <= set(after["actors"]),"Existing actor identity was removed/replaced")
            require(set(after["actors"])-set(before["actors"]) <= target_paths,"Unexpected non-family actors were created")
            for path, old in before["actors"].items():
                current = after["actors"][path]
                if path not in target_paths:
                    require(current == old,f"Unrelated actor/geometry/quest object changed:{path}")
                    continue
                require(current["class"] == old["class"] and current["label"] == old["label"] and current["transform"] == old["transform"]
                        and current["health"] == old["health"],"Existing packed/boss identity, transform or health changed")
                require(not old["tags"] or current["tags"][0] == old["tags"][0],"Old primary checkpoint identity was replaced")
                require(set(old["tags"]) <= set(current["tags"]),"Existing source/boss tag was removed")
                require(set(old["components"]) == set(current["components"]),"Existing target component identity changed")
                if old.get("source") == current["source"] and old.get("portrait"):
                    require(current["portrait"] == old["portrait"],"Existing valid art override was replaced")
                for component, state in old["components"].items():
                    if component not in ("GuardianSprite","Sprite","CapsuleComponent"):
                        require(current["components"][component] == state,"Unrelated target component changed")
            rewritten=[path for path,digest in old_assets.items() if new_assets.get(path)!=digest]
            require(not rewritten,f"Existing named art/mesh asset was rewritten: {rewritten}")
            require(all(after["meshes"].get(path) == digest for path,digest in before["meshes"].items()),"Existing geometry mesh was replaced/rewritten")
            report[name] = {"state":after,"assets":new_assets,"identities":identities,"source_rows":expected,"script":script_state()}
        else:
            require(snapshot() == report[name]["state"],f"{name}: saved actors/art/geometry changed on fresh reopen")
            require(script_state() == report[name]["script"],"Saved script/callback/generator state changed on fresh reopen")
            require(assets() == report[name]["assets"],f"{name}: saved source art files differ on fresh reopen")
            require({str(k):v for k,v in verify().items()} == report[name]["identities"],"Saved source identity mapping changed")
        total += len(expected)
        unreal.log(f"Verified{name}: {len(expected)} exact packed encounters; existing actor tags/art overrides, quests, scripts and level geometry retained")
    require(total == 66,"Expected all66 authored placements")
    if refresh:
        report_path.write_text(json.dumps(report,indent=2),encoding="utf-8")
    unreal.log("Verified all66 exact packed encounters across seven saved maps without geometry/script/generator import")


if __name__ == "__main__":
    refresh_packed_encounter_families()
