"""Stage1 metadata-only refresh and independent all30/two-process proof.

First editor process: run this script after compiling the matching C++ patch.
It snapshots all actors/components/CPD/material/mesh files and brightness, runs
only AA.RefreshNativeWadSourceLighting twice per map, validates every saved
record against original MAP bytes and current bound prop centers, and writes
Saved/ReviewArtifacts/SourceLightingMetadataRefresh.json. It never opts into
feedback or rebuilds geometry/art/lighting. Close unsaved editor work first.

A DIFFERENT editor process runs the same script with -AASourceLightingVerifyOnly.
That pass opens every map from disk, rederives the original census and compares
the first-process state, with no refresh/save command. Missing prop bindings
are reported explicitly; no actors are created by this metadata milestone.
"""
from pathlib import Path
import hashlib
import importlib.util
import json
import os
import unreal


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def load_module(name, filename):
    spec = importlib.util.spec_from_file_location(name, filename)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def path(obj):
    return obj.get_path_name() if obj else None


def plain(value):
    return json.loads(json.dumps(value))


def transform(actor):
    p, r, s = actor.get_actor_location(), actor.get_actor_rotation(), actor.get_actor_scale3d()
    return [p.x, p.y, p.z, r.pitch, r.yaw, r.roll, s.x, s.y, s.z]


def unpack_metadata(value):
    def fields(row, names):
        return {key: row.get_editor_property(key) for key in names}

    def rule(row):
        return fields(row, ('thing_id', 'marker_type', 'sector', 'animation_type', 'rate', 'center', 'radius'))

    result = fields(value, ('version', 'source_map_name', 'source_map_sha1', 'thing_count', 'line_count'))
    result['source_map_sha1'] = result['source_map_sha1'].upper()
    result['unaddressable_two_sided_lines'] = list(value.get_editor_property('unaddressable_two_sided_lines'))
    result['sectors'] = []
    for sector in value.get_editor_property('sectors'):
        row = fields(sector, ('sector', 'authored_light', 'outside', 'door'))
        row['neighbors'] = list(sector.get_editor_property('neighbors'))
        result['sectors'].append(row)
    result['illuminators'] = []
    for owner in value.get_editor_property('illuminators'):
        row = fields(owner, ('thing_id', 'source_type', 'accessory_data', 'illumination', 'authored_sector', 'center_sector',
                             'uses_edited_center', 'used_source_fallback'))
        for key in ('authored_position', 'center_position'):
            position = owner.get_editor_property(key)
            row[key] = [position.x, position.y]
        row['bound_actor'] = path(owner.get_editor_property('bound_actor'))
        result['illuminators'].append(row)
    result['directives'] = [rule(row) for row in value.get_editor_property('directives')]
    result['animations'] = [rule(row) for row in value.get_editor_property('animations')]
    return result


def verify_source_lighting(refresh=True, maps=None):
    project = Path(unreal.Paths.project_dir()).resolve()
    reference = load_module('aa_source_light_reference', project / 'scripts/source_native_lighting_reference.py')
    geometry_helper = load_module('aa_source_light_geometry', project / 'scripts/refresh_native_missing_materials.py')
    exe = project / 'Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Exe'
    campaign = reference.campaign(exe)
    if maps is not None:
        require(all(name in campaign for name in maps), 'Requested map is outside original campaign')
        campaign = [name for name in campaign if name in maps]
    # Parse/preflight every original file before the first saved-map mutation.
    authored_maps = {name: reference.metadata(exe / (name + '.MAP')) for name in campaign}
    destination = project / 'Saved/ReviewArtifacts/SourceLightingMetadataRefresh.json'
    previous = json.loads(destination.read_text()) if destination.exists() else {}
    require(refresh or previous, 'Fresh-process proof needs first-process durable report')
    if not refresh:
        require(previous['process_id'] != os.getpid(), 'Reopen proof must run in a new editor process')
    report = {'process_id': os.getpid(), 'stage': 'metadata only; feedback model not enabled', 'maps': {}}
    editor_actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    totals = dict(owners=0, markers924=0, props=0, raw_glow=0, effective_animations=0, source_self_edges=0, unaddressable_edges=0)

    for name in campaign:
        source = exe / (name + '.MAP')
        authored = authored_maps[name]
        world = unreal.EditorLoadingAndSavingUtils.load_map(str(project / f'Content/Maps/{name}.umap'))
        actors = list(editor_actors.get_all_level_actors())
        levels = [a for a in actors if isinstance(a, unreal.NativeWadLevel)]
        require(world and len(levels) == 1, f'{name}: expected one saved native level')
        level = levels[0]
        initial_version = level.get_source_lighting_metadata().get_editor_property('version')

        def bindings():
            centers, bound = {}, {}
            for owner in authored['illuminators']:
                index, kind = owner['thing_id'], owner['source_type']
                tag = f'WAD Scenery Thing {index}'
                candidates = [actor for actor in actors if tag in [str(t) for t in actor.get_editor_property('tags')]
                              or isinstance(actor, unreal.NativeWadScenery) and actor.get_source_thing_id() == index]
                require(len(candidates) <= 1, f'{name}/Thing{index}: duplicate owner identity before refresh')
                if not candidates:
                    continue
                actor = candidates[0]
                require(isinstance(actor, unreal.NativeWadScenery) and actor.get_source_thing_id() == index
                        and tag in [str(t) for t in actor.get_editor_property('tags')]
                        and actor.get_source_type() in (0, kind), f'{name}/Thing{index}: mismatched source owner')
                local = unreal.MathLibrary.inverse_transform_location(level.get_actor_transform(), actor.get_actor_location())
                centers[index] = [local.x, local.y]
                bound[index] = path(actor)
            return centers, bound

        def state():
            result = {}
            for actor in actors:
                entry = dict(transform=transform(actor), tags=[str(t) for t in actor.get_editor_property('tags')],
                             components=geometry_helper.snapshot(actor))
                if isinstance(actor, unreal.NativeWadScenery):
                    entry['pickup'] = path(actor.get_pickup())
                cpdata = {}
                for primitive in actor.get_components_by_class(unreal.PrimitiveComponent):
                    cpdata[primitive.get_name()] = json.loads(unreal.NativeWadArtEditor.describe_primitive_data(primitive))
                entry['custom_primitive_data'] = cpdata
                lights = {}
                for light in actor.get_components_by_class(unreal.LightComponent):
                    lights[light.get_name()] = dict(intensity=light.get_editor_property('intensity'), color=str(light.get_editor_property('light_color')))
                entry['lights'] = lights
                result[path(actor)] = entry
            return plain(dict(actors=result, feedback_enabled=level.is_source_light_feedback_enabled(),
                              sector_lights=[level.get_sector_light_level(i) for i in range(len(authored['sectors']))],
                              outside_light=level.get_editor_property('outside_light')))

        def asset_hashes():
            # Existing referenced meshes/materials and all generated sprite assets must stay byte identical.
            paths = set((project / f'Content/Maps/{name}_Assets/Things').glob('*.uasset'))
            for actor in actors:
                for component in actor.get_components_by_class(unreal.StaticMeshComponent):
                    refs = [component.get_editor_property('static_mesh')]
                    refs += [component.get_material(i) for i in range(component.get_num_materials())]
                    for value in refs:
                        obj = path(value)
                        if obj and obj.startswith('/Game/') and ':' not in obj:
                            filename = project / ('Content/' + obj.split('.')[0][len('/Game/'):] + '.uasset')
                            require(filename.is_file(), f'Missing existing mesh/material asset: {filename}')
                            paths.add(filename)
            return {str(filename.relative_to(project)): hashlib.sha256(filename.read_bytes()).hexdigest() for filename in sorted(paths)}

        before, hashes_before = state(), asset_hashes()
        centers, bound = bindings()
        expected = reference.metadata(source, centers, bound)
        if refresh:
            old_metadata = unpack_metadata(level.get_source_lighting_metadata())
            unreal.SystemLibrary.execute_console_command(world,
                f'AA.PreflightNativeWadSourceLighting "{source.as_posix()}" {name} /Game/Maps/{name}')
            require(unpack_metadata(level.get_source_lighting_metadata()) == old_metadata and state() == before,
                    f'{name}: dry-run preflight mutated metadata or outputs')

        for repeat in range(2 if refresh else 1):
            if refresh:
                unreal.SystemLibrary.execute_console_command(world,
                    f'AA.RefreshNativeWadSourceLighting "{source.as_posix()}" {name} /Game/Maps/{name}')
                require(unreal.EditorLoadingAndSavingUtils.save_map(world,f'/Game/Maps/{name}'),
                        f'{name}: explicit public metadata map save failed')
            actual = unpack_metadata(level.get_source_lighting_metadata())
            require(actual == expected, f'{name}: saved metadata differs from independent original+actual-center reference')
            require(state() == before, f'{name}: metadata-only refresh changed actor/component/collision/light/CPD/mode output')
            require(asset_hashes() == hashes_before, f'{name}: metadata-only refresh rewrote an existing mesh/material/sprite asset')
        missing = [owner['thing_id'] for owner in actual['illuminators'] if owner['source_type'] != 924 and owner['bound_actor'] is None]
        final = plain(dict(source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(), metadata=actual,
                           untouched=before, unchanged_asset_hashes=hashes_before, unbound_prop_owners=missing))
        if not refresh:
            require(final == previous['maps'].get(name), f'{name}: fresh-process disk restore changed metadata/identity/outputs/assets')
        else:
            report['maps'][name] = final
            report['initial_versions'] = report.get('initial_versions', {}) | {name: initial_version}
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(json.dumps(report, indent=2), encoding='utf-8')
        totals['owners'] += len(actual['illuminators'])
        totals['markers924'] += sum(owner['source_type'] == 924 for owner in actual['illuminators'])
        totals['props'] += sum(owner['source_type'] != 924 for owner in actual['illuminators'])
        totals['raw_glow'] += sum(row['marker_type'] == 1526 for row in actual['directives'])
        totals['effective_animations'] += len(actual['animations'])
        totals['source_self_edges'] += sum(row['sector'] in row['neighbors'] for row in actual['sectors'])
        totals['unaddressable_edges'] += len(actual['unaddressable_two_sided_lines'])
        unreal.log(f'{name}: metadata matches source/current centers; mode unchanged={before["feedback_enabled"]}; missing prop bindings={missing}; excluded undefined source lines={len(actual["unaddressable_two_sided_lines"])}')
    if maps is None:
        require(totals == dict(owners=655, markers924=200, props=455, raw_glow=78, effective_animations=151, source_self_edges=3, unaddressable_edges=175),
                f'Independent all30 source census changed: {totals}')
    unreal.log(f'Source lighting stage1 {"refresh" if refresh else "NEW PROCESS REOPEN"} verified: {len(campaign)}maps {totals}; no model activation/output change')


if __name__ == '__main__':
    verify_source_lighting(refresh='-AASourceLightingVerifyOnly' not in unreal.SystemLibrary.get_command_line())
