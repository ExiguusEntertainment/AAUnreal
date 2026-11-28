"""Create opt-in source-feedback copies; prove all campaign sources unchanged.

After stage1's metadata-only proof, run this in the editor. Default candidates
are L30, L64 and L151; existing destinations are NEVER overwritten. This checks
source/copy actor transforms, components, material/collision/mesh identities,
attached pickups and default CPD, all existing asset-file hashes, original-map
hashes, full metadata and original cooked lookup bytes. A fresh editor process
with -AASourceLightingCandidateVerifyOnly verifies saved candidate schema/data
without creating/saving anything. Actual clock/performance/render/restart proof
runs separately in game with SourceLightingB/SourceLightingBReload review modes.
"""
from pathlib import Path
import hashlib
import importlib.util
import json
import os
import unreal


def require(value, message):
    if not value:
        raise RuntimeError(message)


def module(name, file):
    spec = importlib.util.spec_from_file_location(name, file)
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def digest(file):
    return hashlib.sha256(file.read_bytes()).hexdigest()


def plain(value):
    return json.loads(json.dumps(value))


def create_candidates(maps=('L30', 'L64', 'L151'), verify_only=False):
    project = Path(unreal.Paths.project_dir()).resolve()
    reference = module('aa_feedback_original', project / 'scripts/source_native_lighting_reference.py')
    geometry = module('aa_feedback_geometry', project / 'scripts/refresh_native_missing_materials.py')
    metadata_helper = module('aa_feedback_metadata', project / 'scripts/refresh_native_source_lighting.py')
    exe = project / 'Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Exe'
    campaign = reference.campaign(exe)
    require(len(set(maps)) == len(maps) and all(name in campaign for name in maps), 'Invalid/duplicate candidate source list')
    actor_api = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    target = project / 'Saved/ReviewArtifacts/SourceLightingCandidates.json'
    previous = json.loads(target.read_text()) if target.exists() else {}
    if verify_only:
        require(previous and previous['process_id'] != os.getpid(), 'Candidate disk proof needs a DIFFERENT editor process')
    report = {'process_id': os.getpid(), 'maps': {}}
    source_files = {name: project / f'Content/Maps/{name}.umap' for name in campaign}
    original_hashes = {name: digest(file) for name, file in source_files.items()}

    def normalize(value, world_path):
        if isinstance(value, str):
            return value.replace(world_path, '$WORLD')
        if isinstance(value, dict):
            return {normalize(key, world_path): normalize(item, world_path) for key, item in value.items()}
        if isinstance(value, (tuple, list)):
            return [normalize(item, world_path) for item in value]
        return value

    def open_map(package):
        filename = project / ('Content/' + package[len('/Game/'):] + '.umap')
        world = unreal.EditorLoadingAndSavingUtils.load_map(str(filename))
        require(world is not None, f'Could not open {package}')
        actors = list(actor_api.get_all_level_actors())
        levels = [actor for actor in actors if isinstance(actor, unreal.NativeWadLevel)]
        require(len(levels) == 1, f'{package}: expected one native level')
        return world, actors, levels[0]

    def snapshot(world, actors):
        result = {}
        for actor in actors:
            components = geometry.snapshot(actor)
            defaults = {component.get_name(): json.loads(unreal.NativeWadArtEditor.describe_primitive_data(component))["defaults"]
                        for component in actor.get_components_by_class(unreal.PrimitiveComponent)}
            entry = dict(transform=metadata_helper.transform(actor), tags=[str(t) for t in actor.get_editor_property('tags')],
                         components=components, default_cpd=defaults)
            if isinstance(actor, unreal.NativeWadScenery):
                entry['pickup'] = metadata_helper.path(actor.get_pickup())
            result[actor.get_name()] = entry
        return plain(normalize(result, world.get_path_name()))

    # Confirm old maps stay legacy, even when only a subset gets a review copy.
    for name in campaign:
        _, _, level = open_map(f'/Game/Maps/{name}')
        require(not level.is_source_light_feedback_enabled(), f'{name}: a campaign map was unexpectedly opted into B')

    for name in maps:
        source = f'/Game/Maps/{name}'
        candidate = f'/Game/Maps/SourceLightingCandidates/{name}_SourceB'
        world, actors, level = open_map(source)
        before = snapshot(world, actors)
        centers, bindings = {}, {}
        for owner in reference.metadata(exe / (name + '.MAP'))['illuminators']:
            index = owner['thing_id']
            found = [actor for actor in actors if f'WAD Scenery Thing {index}' in [str(tag) for tag in actor.get_editor_property('tags')]
                     or isinstance(actor, unreal.NativeWadScenery) and actor.get_source_thing_id() == index]
            require(len(found) <= 1, f'{name}: duplicate bound owner {index}')
            if found:
                actor = found[0]
                local = unreal.MathLibrary.inverse_transform_location(level.get_actor_transform(), actor.get_actor_location())
                centers[index], bindings[index] = [local.x, local.y], actor.get_path_name()
        metadata_before = plain(normalize(reference.metadata(exe / (name + '.MAP'), centers, bindings), world.get_path_name()))
        runtime_before = {actor.get_name(): {component.get_name(): json.loads(unreal.NativeWadArtEditor.describe_primitive_data(component))['runtime']
                                            for component in actor.get_components_by_class(unreal.PrimitiveComponent)} for actor in actors}
        assets = {str(file.relative_to(project)): digest(file) for file in (project / f'Content/Maps/{name}_Assets').rglob('*.uasset')}
        existed = unreal.EditorAssetLibrary.does_asset_exist(candidate)
        if not existed and not verify_only:
            filename = exe / (name + '.MAP')
            unreal.SystemLibrary.execute_console_command(world,
                f'AA.CreateNativeWadSourceLightingCandidate "{filename.as_posix()}" {name} {source} {candidate}')
        require(existed or not verify_only, f'{name}: missing saved candidate')
        require(snapshot(world, actors) == before, f'{name}: candidate command changed the original world in memory')
        runtime_after = {actor.get_name(): {component.get_name(): json.loads(unreal.NativeWadArtEditor.describe_primitive_data(component))['runtime']
                                           for component in actor.get_components_by_class(unreal.PrimitiveComponent)} for actor in actors}
        require(runtime_after == runtime_before, f'{name}: candidate changed original runtime CPD')
        copied_world, copied_actors, copied_level = open_map(candidate)
        require(copied_level.is_source_light_feedback_enabled(), f'{candidate}: candidate opt-in was not saved')
        if not existed and not verify_only:
            require(unreal.EditorLoadingAndSavingUtils.save_map(copied_world,candidate), f'{candidate}: explicit public candidate save failed')
        require(snapshot(copied_world, copied_actors) == before, f'{candidate}: duplication changed source geometry/actor/art/default-CPD state')
        copied_metadata = plain(normalize(metadata_helper.unpack_metadata(copied_level.get_source_lighting_metadata()), copied_world.get_path_name()))
        require(copied_metadata == metadata_before, f'{candidate}: source metadata or internal owner references did not duplicate exactly')
        cooked = bytes(copied_level.get_cooked_source_lighting_lookup_bytes())
        original = (exe / (name + '.MAP')).read_bytes()
        require(cooked == original, f'{candidate}: cooked center lookup differs from exact original MAP')
        require(hashlib.sha1(cooked).hexdigest().upper() == copied_metadata['source_map_sha1'], f'{candidate}: source identity mismatch')
        for owner in copied_level.get_source_lighting_metadata().get_editor_property('illuminators'):
            actor = owner.get_editor_property('bound_actor')
            require(not actor or actor.get_path_name().startswith(copied_world.get_path_name() + ":"), f'{candidate}: illumination owner still refers to original world')
        for filename, before_hash in assets.items():
            require(digest(project / filename) == before_hash, f'Candidate rewrote shared art/geometry: {filename}')
        require({key: digest(file) for key, file in source_files.items()} == original_hashes, 'Candidate command changed a campaign-map file')
        final = dict(candidate=candidate, source_hash=original_hashes[name], metadata=copied_metadata, untouched=before,
                     cooked_lookup_sha256=hashlib.sha256(cooked).hexdigest(), unchanged_assets=assets)
        if verify_only:
            require(final == previous['maps'].get(name), f'{name}: candidate fresh-process disk proof differs')
        report['maps'][name] = final
        if not verify_only:
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(json.dumps(report, indent=2), encoding='utf-8')
        unreal.log(f'{candidate}: opt-in copied metadata/lookup/identities verified; all30 original campaign maps remain legacy and byte unchanged')


if __name__ == '__main__':
    create_candidates(verify_only='-AASourceLightingCandidateVerifyOnly' in unreal.SystemLibrary.get_command_line())
