"""Refresh only L41 trap activities11/20 and missing original projectile art.

Required console hook:
AA.RefreshNativeWadL41ScriptMissiles <WadPath> L41 /Game/Maps/L41

Run refresh_l41_script_missiles(False) in a fresh editor process to verify the
saved baseline, every actor/component/mesh, retained commands/state/materials,
and all seven restored shots without mutation. No private UPROPERTY reflection.
"""
from pathlib import Path
import importlib.util
import json
import unreal


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def normalized(value):
    return json.loads(json.dumps(value, sort_keys=True))


def refresh_l41_script_missiles(refresh=True):
    project = Path(unreal.Paths.project_dir()).resolve()
    spec = importlib.util.spec_from_file_location("aa_l41_snapshot", project/"scripts/refresh_native_missing_materials.py")
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    world = unreal.EditorLoadingAndSavingUtils.load_map(str(project/"Content/Maps/L41.umap"))
    require(world is not None, "Could not open saved native L41")
    editor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    levels = [a for a in editor.get_all_level_actors() if isinstance(a, unreal.NativeWadLevel)]
    require(len(levels) == 1, "Expected exactly one native level actor")
    level = levels[0]
    destination = project/"Saved/ReviewArtifacts/L41-script-missiles-refresh.json"
    destination.parent.mkdir(parents=True, exist_ok=True)

    def snapshot():
        actors = {}
        meshes = {}
        for actor in editor.get_all_level_actors():
            p, r, s = actor.get_actor_location(), actor.get_actor_rotation(), actor.get_actor_scale3d()
            components = helper.snapshot(actor)
            actors[actor.get_path_name()] = {
                "class": actor.get_class().get_path_name(),
                "tags": [str(tag) for tag in actor.tags],
                "transform": ((p.x, p.y, p.z), (r.pitch, r.yaw, r.roll), (s.x, s.y, s.z)),
                "components": components,
            }
            meshes.update(helper.mesh_hashes(project, components))
        return normalized({"actors": actors, "mesh_sha256": meshes})

    def describe():
        value = json.loads(level.describe_wad_script_missiles())
        require("state" in value and value["launched"] == 0, "Refresh must not run gameplay/callbacks")
        return value

    def retained(description):
        out = {}
        for activity in description["activities"]:
            commands = [c["command"] for c in activity["commands"]
                        if not (activity["id"] in (11, 20) and c["missile"] and c["target"] in (4001, 4002, 4007, 4009))]
            out[str(activity["id"])] = commands
        return out

    def verify_saved(description):
        activities = {a["id"]: a for a in description["activities"]}
        require(11 in activities and 20 in activities, "Missing original trap activities")
        expected = {11: [4005, 4007, 4015, 4016, 4009, 4001], 20: [4002]*4}
        for key, types in expected.items():
            shots = [c for c in activities[key]["commands"] if c["missile"]]
            require([c["target"] for c in shots] == types, f"Wrong source activity{key} shot order/count")
            require(all(c["offset"] >= 0 for c in activities[key]["commands"]), "Missing stable source callback positions")
        art = {a["type"]: a["material"] for a in description["art"]}
        for kind in (4001, 4002, 4007, 4009):
            require(art.get(kind) and art[kind] != "None" and unreal.load_asset(art[kind]) is not None,
                    f"No saved original art for missile{kind}")

    if refresh:
        before, old = snapshot(), describe()
        source = project/"Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Exe/L41.MAP"
        unreal.SystemLibrary.execute_console_command(world, f'AA.RefreshNativeWadL41ScriptMissiles "{source}" L41 /Game/Maps/L41')
        require(unreal.EditorLoadingAndSavingUtils.save_map(world, '/Game/Maps/L41'),
                'L41 public map save failed; in-memory scripts are not persisted proof')
        after, new = snapshot(), describe()
        require(before == after, "Refresh changed actor identity, transform, components, geometry, collision or material overrides")
        require(retained(old) == retained(new), "An existing authored command was changed")
        require(old["state"] == new["state"], "Refresh reset script/world state or a pending callback")
        old_art = {a["type"]: a["material"] for a in old["art"] if a["material"] != "None"}
        new_art = {a["type"]: a["material"] for a in new["art"]}
        require(all(new_art.get(kind) == path for kind, path in old_art.items()), "Existing valid projectile art override changed")
        verify_saved(new)
        destination.write_text(json.dumps({"geometry": after, "description": new}, indent=2), encoding="utf-8")
    else:
        require(destination.is_file(), "Run the targeted refresh once before independent reopen verification")
        baseline = json.loads(destination.read_text(encoding="utf-8"))
        require(snapshot() == baseline["geometry"], "Fresh process changed saved actors/components/geometry")
        current = describe()
        require(current == baseline["description"], "Fresh process did not retain script/art/callback metadata/state")
        verify_saved(current)
    unreal.log("Verified L41 seven restored script missiles; retained all actors, geometry, collision, authored overrides, commands and script state")


if __name__ == "__main__":
    refresh_l41_script_missiles()
