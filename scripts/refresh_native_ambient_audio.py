"""Metadata/PCM-only source ambient refresh and fresh-process verification on all30 maps.

Requires AA.RefreshNativeWadAmbientAudio <WadPath> <MapName> <MapPackage>.
Uses readonly DescribeWadAmbientAudio(), public scene properties and numeric
transforms. No private UPROPERTY reflection or gameplay/geometry import.
Call refresh_ambient_audio(False) in a new Unreal process to verify saved data.
For a bounded repair use refresh_ambient_audio(True, ["L72"]); full30 remains the default.
"""
from pathlib import Path
from collections import Counter
import hashlib
import importlib.util
import json
import re
import struct
import unreal

DEFINITIONS = {1508:(3010,255),1509:(3008,255),1510:(3009,255),1511:(3004,128),1512:(3004,255),
    1513:(3003,255),1514:(3007,255),1515:(3019,255),1516:(3006,255),1517:(3001,255),1528:(3000,255),
    1529:(3002,255),1530:(3011,255),1531:(3012,255),1532:(3013,255),1533:(3014,255),1534:(3015,255),
    1535:(3016,255),1536:(3018,255)}


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def original_markers(path):
    data = path.read_bytes()
    magic, count, directory = struct.unpack_from("<4sii", data)
    require(magic in (b"IWAD",b"PWAD") and count > 0 and directory >= 12 and directory+count*16 <= len(data), "Invalid source MAP")
    for i in range(count):
        offset, size, name = struct.unpack_from("<ii8s", data, directory+16*i)
        require(offset >= 0 and size >= 0 and offset+size <= len(data), "Invalid source lump extent")
        if name.rstrip(b"\0") == b"THINGS":
            return {i:{"source_thing":i,"marker_type":t,"sound":DEFINITIONS[t][0],"group":flags,"radius":500,
                       "max_volume":DEFINITIONS[t][1],"loop":True,"position":[x,y]}
                    for i,(x,y,angle,t,flags) in enumerate(struct.iter_unpack("<hhHHH",data[offset:offset+size])) if t in DEFINITIONS}
    raise RuntimeError("No original THINGS")


def original_samples(path):
    data = path.read_bytes()
    require(data[:4] == b"Res!" and len(data) >= 14,"Invalid original sound archive")
    start, size, count = struct.unpack_from("<IIH",data,4)
    require(size == count*39 and start+size <= len(data),"Invalid source sound directory")
    result = {}
    for i in range(count):
        offset = start+39*i
        name = data[offset+4:offset+18].split(b"\0",1)[0].decode("ascii")
        if not re.fullmatch(r"snd#\d+",name):
            continue
        position, length = struct.unpack_from("<II",data,offset+18)
        require(length > 0 and position+length <= start,"Invalid source sample extent")
        result[int(name[4:])] = {"bytes":length,"sha1":hashlib.sha1(data[position:position+length]).hexdigest().upper()}
    return result


def numeric_transform(actor):
    p,r,s = actor.get_actor_location(),actor.get_actor_rotation(),actor.get_actor_scale3d()
    return ((p.x,p.y,p.z),(r.pitch,r.yaw,r.roll),(s.x,s.y,s.z))


def refresh_ambient_audio(refresh=True, map_names=None):
    project = Path(unreal.Paths.project_dir()).resolve()
    source = project/"Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Exe"
    archive = original_samples(source/"SOUNDS.RES")
    spec = importlib.util.spec_from_file_location("aa_ambient_snapshot",project/"scripts/refresh_native_missing_materials.py")
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    maps = sorted(p for p in (project/"Content/Maps").glob("L*.umap") if re.fullmatch(r"L\d+",p.stem))
    require(len(maps) == 30,"Expected all30 imported maps")
    if map_names is not None:
        requested = [map_names] if isinstance(map_names,str) else list(map_names)
        require(requested and all(isinstance(n,str) and re.fullmatch(r"L\d+",n) for n in requested),
                "Restricted maps must contain original map names such as L72")
        require(len(requested) == len(set(requested)),"Restricted map list contains duplicates")
        require(set(requested) <= {p.stem for p in maps},"Restricted map list contains an unavailable map")
        maps = [p for p in maps if p.stem in set(requested)]
    full_campaign = len(maps) == 30
    editor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    report,total,new_total,missing_total = [],0,0,0
    for path in maps:
        name = path.stem
        expected = original_markers(source/f"{name}.MAP")
        world = unreal.EditorLoadingAndSavingUtils.load_map(str(path))
        levels = [a for a in editor.get_all_level_actors() if isinstance(a,unreal.NativeWadLevel)]
        require(world is not None and len(levels) == 1,f"{name}: expected one native level")
        level = levels[0]

        def snapshot():
            return {a.get_path_name():{"transform":numeric_transform(a),"components":helper.snapshot(a)}
                    for a in editor.get_all_level_actors()}

        before = snapshot()
        flat = {f"{a}/{c}":v for a,record in before.items() for c,v in record["components"].items()}
        geometry_hashes = helper.mesh_hashes(project,flat)
        old = json.loads(level.describe_wad_ambient_audio())
        old_samples = {int(s["id"]):s for s in old["samples"]}
        existing = {int(e["source_thing"]):e for e in old["emitters"] if int(e["source_thing"]) >= 0}
        legacy_ids = [i for i,e in expected.items() if e["marker_type"] in (1513,1514)]
        if len(old["emitters"]) == len(legacy_ids) and all(e["source_thing"] == -1 and e["marker_type"] == 0 for e in old["emitters"]):
            existing.update(zip(legacy_ids,old["emitters"]))  # Exact previous serialized source order.
        if refresh:
            unreal.SystemLibrary.execute_console_command(world,
                f'AA.RefreshNativeWadAmbientAudio "{(source/(name+".MAP")).as_posix()}" {name} /Game/Maps/{name}')
        after = snapshot()
        require(before == after,f"{name}: audio-only refresh changed actors/components/geometry/transforms/materials/collision")
        flat_after = {f"{a}/{c}":v for a,record in after.items() for c,v in record["components"].items()}
        require(geometry_hashes == helper.mesh_hashes(project,flat_after),f"{name}: mesh asset bytes changed")
        current = json.loads(level.describe_wad_ambient_audio())
        entries = {}
        for entry in current["emitters"]:
            index = int(entry["source_thing"])
            if index < 0:
                continue  # Explicit unbound custom emitters are retained separately.
            require(index not in entries,f"{name}: duplicate source ambient binding {index}")
            entries[index] = entry
        if entries.keys() != expected.keys():
            diagnostic = project/"Saved/ReviewArtifacts"/f"AmbientAudio-{name}-Mismatch.json"
            diagnostic.parent.mkdir(parents=True,exist_ok=True)
            diagnostic.write_text(json.dumps({"map":name,"refreshed":refresh,
                "expected_ids":sorted(expected),"current_ids":sorted(entries),
                "missing_ids":sorted(expected.keys()-entries.keys()),
                "unexpected_ids":sorted(entries.keys()-expected.keys()),
                "expected":expected,"current":current},indent=2)+"\n",encoding="utf-8")
            require(False,f"{name}: missing/unexpected original ambient placements; exact IDs: {diagnostic}")
        samples = {int(s["id"]):s for s in current["samples"]}
        for sample, record in old_samples.items():
            require(samples.get(sample) == record,f"{name}: existing PCM/sample override changed: {sample}")
        missing = []
        for index, definition in expected.items():
            entry = entries[index]
            require(entry["marker_type"] == definition["marker_type"],f"{name}/{index}: source marker identity changed")
            if index in existing:
                prior = existing[index]
                for key in ("position","sound","group","radius","max_volume","loop"):
                    require(entry[key] == prior[key],f"{name}/{index}: discarded saved emitter override {key}")
            else:
                for key in ("sound","group","radius","max_volume","loop"):
                    require(entry[key] == definition[key],f"{name}/{index}: incorrect original {key}")
                require(entry["position"][:2] == definition["position"],f"{name}/{index}: wrong original XY")
            sound = int(entry["sound"])
            if sound not in old_samples:
                if sound in archive:
                    require(sound in samples and {k:samples[sound][k] for k in ("bytes","sha1")} == archive[sound],
                            f"{name}/{index}: saved PCM differs from actual source sample{sound}")
                else:
                    require(sound not in samples,f"{name}/{index}: a missing source sample acquired invented replacement data")
                    missing.append({"thing":index,"sound":sound})
        if refresh:
            # The native console command cannot return its SavePackage result to Python.
            # Require a public positive disk-save result; validating memory alone is insufficient.
            require(unreal.EditorLoadingAndSavingUtils.save_map(world,f"/Game/Maps/{name}"),
                    f"{name}: public map save failed; in-memory ambient metadata is not persisted proof")
        else:
            require(old == current,f"{name}: readonly reopen verification mutated audio metadata")
        added = sum(e["marker_type"] not in (1513,1514) for e in expected.values())
        total += len(expected)
        new_total += added
        missing_total += len(missing)
        report.append({"map":name,"original_markers":len(expected),"additional_marker_families":added,
                       "missing_samples":missing,"source_types":dict(Counter(e["marker_type"] for e in expected.values())),
                       "audio":current,"positive_public_save":bool(refresh),"unchanged_mesh_sha256":geometry_hashes})
        unreal.log(f"Verified ambient metadata/PCM {name}: {len(expected)} original markers; {len(missing)} missing source samples; all geometry retained")
    if full_campaign:
        require(total == 334 and new_total == 203,"Original campaign ambient-marker counts changed")
    suffix = "" if full_campaign else "-"+"-".join(p.stem for p in maps)
    output = project/"Saved/ReviewArtifacts"/(("AmbientAudioRefresh" if refresh else "AmbientAudioReopen")+suffix+".json")
    output.parent.mkdir(parents=True,exist_ok=True)
    output.write_text(json.dumps({"verified_maps":len(maps),"complete_campaign":full_campaign,"original_markers":total,"additional_marker_families":new_total,
        "missing_source_sample_placements":missing_total,"refreshed":refresh,"maps":report},indent=2)+"\n",encoding="utf-8")
    unreal.log(f"Verified{total} original ambient markers ({new_total} additional), {missing_total} missing-source placements: {output}")


if __name__ == "__main__":
    refresh_ambient_audio()
