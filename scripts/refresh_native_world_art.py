"""Bounded original-world palette/mip maintenance; never rebuilds map geometry.

Run refresh_world_art() to apply proven requests on all30 imported maps. A missing
creature-frame manifest is explicit: default QA refuses incomplete coverage.
For an intermediate audited pass use allow_unresolved=True, then rerun strict.
Run refresh_world_art(False) in another process for persisted readback. Approved
native GAME interface textures are protected even though original in-level VGA
palette changes affected lower224 entries globally, including UI pixels.
"""
from pathlib import Path
import hashlib
import importlib.util
import json
import re
import unreal


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def canonical_texture_path(path):
    return path if "." in path.rsplit("/", 1)[-1] else path+"."+path.rsplit("/", 1)[-1]


def numeric_transform(actor):
    p, r, s = actor.get_actor_location(), actor.get_actor_rotation(), actor.get_actor_scale3d()
    return ((p.x,p.y,p.z),(r.pitch,r.yaw,r.roll),(s.x,s.y,s.z))


def texture_files(project, report):
    result = {}
    for texture in report["textures"]:
        package = texture["asset"].split(".",1)[0]
        require(package.startswith("/Game/"), f"Unexpected interface package {package}")
        path = project/"Content"/(package[len("/Game/"):]+".uasset")
        require(path.is_file(), f"Missing approved interface asset {path}")
        result[texture["asset"]] = hashlib.sha256(path.read_bytes()).hexdigest()
    return result


def refresh_world_art(refresh=True, map_names=None, allow_unresolved=False):
    project = Path(unreal.Paths.project_dir()).resolve()
    spec = importlib.util.spec_from_file_location("aa_world_art_snapshot",project/"scripts/refresh_native_missing_materials.py")
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    maps = sorted(p for p in (project/"Content/Maps").glob("L*.umap") if re.fullmatch(r"L[0-9]+",p.stem))
    require(len(maps)==30,"Expected all30 imported maps")
    if map_names is not None:
        requested = [map_names] if isinstance(map_names,str) else list(map_names)
        require(requested and len(requested)==len(set(requested)) and set(requested)<={p.stem for p in maps},"Invalid restricted map list")
        maps = [p for p in maps if p.stem in set(requested)]
    folder = project/"Saved/ReviewArtifacts/WorldArt"
    folder.mkdir(parents=True,exist_ok=True)
    original_ui = json.loads(unreal.NativeWadArtEditor.describe_interface_art())
    ui_files = texture_files(project,original_ui)
    editor = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    reports = []
    for map_file in maps:
        name, package = map_file.stem, "/Game/Maps/"+map_file.stem
        world = unreal.EditorLoadingAndSavingUtils.load_map(str(map_file))
        require(world is not None,f"{name}: could not load native map")

        def snapshot():
            return {a.get_path_name():{"transform":numeric_transform(a),"components":helper.snapshot(a)} for a in editor.get_all_level_actors()}

        before = snapshot()
        flat = {f"{a}/{c}":v for a,row in before.items() for c,v in row["components"].items()}
        mesh_hashes = helper.mesh_hashes(project,flat)
        before_art = json.loads(unreal.NativeWadArtEditor.describe_world_art(package))
        previous = {r["asset"]:r for r in before_art["textures"]}
        report_path = folder/(name+".json")
        if refresh:
            require(unreal.NativeWadArtEditor.refresh_world_art(package,str(report_path),True),f"{name}: source staging/save failed; inspect {report_path}")
        require(report_path.is_file(),f"{name}: prior saved refresh report is missing")
        report = json.loads(report_path.read_text(encoding="utf-8-sig"))
        require(report["map"]==name and report["positive_saves"],f"{name}: no positive saved source-art result")
        after = snapshot()
        require(before==after,f"{name}: world-art refresh changed actors/components/transforms/material slot paths/collision")
        flat_after = {f"{a}/{c}":v for a,row in after.items() for c,v in row["components"].items()}
        require(mesh_hashes==helper.mesh_hashes(project,flat_after),f"{name}: mesh asset bytes changed")
        current = json.loads(unreal.NativeWadArtEditor.describe_world_art(package))
        textures = {r["asset"]:r for r in current["textures"]}
        for row in report["assets"]:
            if row["status"]=="retained_unused_legacy":
                require(name=="L72" and row["asset"]=="/Game/Maps/L72_Assets/Things/T_PlasmaBolt.T_PlasmaBolt",
                        "Unexpected unproven legacy exclusion")
                registry=unreal.AssetRegistryHelpers.get_asset_registry()
                options=unreal.AssetRegistryDependencyOptions(True,True,True,True,True)
                owner="/Game/Maps/L72_Assets/Things/M_PlasmaBolt"
                require(set(map(str,registry.get_referencers(row["asset"].split(".",1)[0],options)))=={owner}
                        and not registry.get_referencers(owner,options),"Legacy projectile gained a saved world reference")
            if row["status"] in ("custom_override","retained_unused_legacy") and row["asset"] in previous:
                require(textures.get(row["asset"])==previous[row["asset"]],f"{name}: modified protected texture override {row['asset']}")
            if row["status"] not in ("updated","unchanged"):
                continue
            path = canonical_texture_path(row.get("target") or row["asset"])
            require(path in textures,f"{name}: saved world texture absent {path}")
            texture = textures[path]
            require(texture.get("palette")==1 and texture.get("source_sha1") and texture["filter"]==0,
                    f"{name}: wrong world domain/source/point sampler {path}")
            require(texture["pixel_sha1"]==texture.get("expected_mip_sha1"),f"{name}: saved pixels differ from recorded generated source {path}")
            if row.get("expected_mip_sha1") is not None:
                require(texture["pixel_sha1"]==row["expected_mip_sha1"],f"{name}: persisted pixels differ from original refresh request {path}")
            if row.get("expected_source_types") is not None:
                require(texture.get("full_source_types")==row["expected_source_types"],f"{name}: persisted full source identity changed {path}")
                require(texture.get("mirrored_binding")==row["expected_mirrored_binding"],f"{name}: persisted source view binding changed {path}")
            if row.get("authored_mips") is not None:
                require(texture["mip_count"]==row["authored_mips"],f"{name}: authored mip count changed {path}")
        protected_materials = {r["asset"] for r in report["assets"] if r["status"]=="custom_override"}
        for material in current["materials"]:
            if material["asset"] in protected_materials:
                continue
            texture = textures.get(material["texture"])
            if texture and texture.get("palette")==1 and texture.get("authored_mips"):
                require(material["authored_mip_policy"] and material["has_mip_info"],f"{name}: source LOD policy missing {material['asset']}")
                require(material["mip_info"][:3]==[texture["width"],texture["height"],texture["mip_count"]-1],
                        f"{name}: per-surface mip dimensions differ {material['asset']}")
        require(json.loads(unreal.NativeWadArtEditor.describe_interface_art())==original_ui,f"{name}: approved interface pixels/provenance/settings changed")
        require(texture_files(project,original_ui)==ui_files,f"{name}: approved interface asset file bytes changed")
        (folder/(name+("-current.json" if refresh else "-reopened.json"))).write_text(json.dumps(current,indent=2)+"\n",encoding="utf-8")
        reports.append({"map":name,"unresolved_sources":report["unresolved_sources"],"custom_overrides":report["custom_overrides"],"retained_unused_legacy":report.get("retained_unused_legacy",0),"unchanged_mesh_sha256":mesh_hashes})
        require(allow_unresolved or report["complete_source_coverage"],f"{name}: exact original frame identities remain unresolved; inspect {report_path}")
        unreal.log(f"Verified {name} source world art; {report['unresolved_sources']} unresolved identities; geometry and approved UI unchanged")
    summary = {"verified_maps":len(maps),"complete_campaign":len(maps)==30,"refreshed":refresh,
               "complete_source_coverage":all(r["unresolved_sources"]==0 for r in reports),"maps":reports,"approved_interface_sha256":ui_files}
    output = folder/("All30-Refreshed.json" if refresh else "All30-Reopened.json")
    if len(maps)!=30:
        output = folder/(("Refreshed-" if refresh else "Reopened-")+"-".join(p.stem for p in maps)+".json")
    output.write_text(json.dumps(summary,indent=2)+"\n",encoding="utf-8")
    unreal.log(f"Verified source world art on {len(maps)} maps: {output}")


if __name__=="__main__":
    refresh_world_art()
