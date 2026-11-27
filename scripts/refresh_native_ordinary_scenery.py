"""Independent all30 source/saved ordinary-prop census and bounded refresh proof.

Run verify_scenery(refresh=True) after building the matching C++ change.
Fresh process: verify_scenery(refresh=False) reopens every saved map, compares
with the durable first-process census and geometry/actor invariants, no writes
to map content. Filters are independently derived from original files; this
never calls the importer to ask what should exist. ELMARK834 is explicit open.
"""
from pathlib import Path
from collections import Counter
import hashlib
import importlib.util
import json
import re
import struct
import unreal


def require(value, message):
    if not value:
        raise RuntimeError(message)


def path(value):
    return value.get_path_name() if value else None


def transform(actor):
    p, r, s = actor.get_actor_location(), actor.get_actor_rotation(), actor.get_actor_scale3d()
    return [p.x, p.y, p.z, r.pitch, r.yaw, r.roll, s.x, s.y, s.z]


def raw_things(filename):
    data = filename.read_bytes()
    magic, count, at = struct.unpack_from('<4sII', data)
    require(magic in (b'PWAD', b'IWAD') and at + count * 16 <= len(data), f'Invalid MAP: {filename}')
    for i in range(count):
        offset, size, name = struct.unpack_from('<II8s', data, at + i * 16)
        if name.rstrip(b'\0') == b'THINGS':
            require(size % 10 == 0 and offset + size <= len(data), 'Invalid THINGS bounds')
            return list(struct.iter_unpack('<hhhHH', data[offset:offset + size]))
    raise RuntimeError(f'Missing THINGS: {filename}')


def original_definitions(project):
    root = project / 'Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor'
    art = root / 'Resources/ArtFinal/OBJS'
    creatures = set(map(int, re.findall(r'\b(\d+)\s*,\s*NAV_LOGIC_', (root / 'Include/CREDATA.H').read_text())))
    require(len(creatures) > 30, 'Original exact CREDATA table unavailable')
    inventory = {int(f.stem[3:]) for f in (art.parent / 'INVDESC').glob('DES*.TXT')}
    resources = {}
    for filename, number in re.findall(r'(\S+\.res)\s+(\d+)', (art / 'OBJS.FL').read_text(), re.I):
        kind = int(number)
        if kind <= 4 or kind == 924 or 1500 <= kind <= 1543 or kind in creatures or kind in inventory:
            continue
        require(kind not in resources, f'Duplicate exact ordinary source type{kind}')
        resources[kind] = filename.replace('\\', '/').split('/')[0].upper()
    result = {}
    for kind, resource in resources.items():
        # Only authored families are required; unused source resources can have extra effects.
        result[kind] = resource
    return root, result


def source_metadata(root, kind, resource):
    directory = root / 'Resources/ArtFinal/OBJS' / resource
    stem = 'DEAD' if resource == 'DEAD01' else resource
    values = [int(s) for line in (directory / (stem + '.INF')).read_text().splitlines()
              if (s := line.split(';')[0].strip())]
    require(len(values) >= 12 and values[0] > 0, f'Bad source INF {resource}')
    if kind == 834:
        require(values[2] == 0x0141, 'ELMARK source translucency definition changed')
        return None
    require(values[5] == 0 and not values[2] & (0x100 | 0x400 | 0x4000), f'Unsupported source effect {resource}')
    require(values[11] in (-1, 0), f'Unimplemented stance transition {resource}')
    first = 8 + 4 * values[0]
    frame_count = values[8]
    pictures = first + 4 * frame_count
    for frame in range(frame_count):
        fields = values[first + frame * 4:first + frame * 4 + 4]
        require(fields[0] in (1, 4, 8) and fields[1:] == [0, 0, 0], f'Unsupported source frame {resource}')
    files = {int(n): name.upper() for name, n in re.findall(r'(\S+\.cpc)\s+(\d+)', (directory / (stem + '.FL')).read_text(), re.I)}
    picture = abs(values[pictures])
    height, width = struct.unpack_from('<HH', (directory / files[picture]).read_bytes())
    require(width > 0 and height > 0, f'Bad original CPC size {resource}')
    return {'radius': values[1], 'attributes': values[2], 'ticks': values[9], 'animation': values[10],
            'frames': frame_count, 'passable': bool(values[2] & 0x2001) or values[1] == 0,
            'hanging': bool(values[7] & 0x200), 'raised': bool(values[7] & 0x400),
            'above_liquid': bool(values[7] & 0x100), 'size': (width, height)}


def verify_scenery(refresh=True, maps=None):
    project = Path(unreal.Paths.project_dir()).resolve()
    root, resources = original_definitions(project)
    campaign = []
    for quest in range(7):
        text = (root / f'Exe/MAPDESC/QUEST{quest}.INI').read_text()
        # Same campaign order as source MAPDESC, independently of PLAN/importer.
        campaign.extend('L' + n for n in re.findall(r'^mapnumber\s*=\s*(\d+)', text, re.M))
    require(len(campaign) == 30 and len(set(campaign)) == 30, 'Expected exactly30 original campaign maps')
    if maps is not None:
        require(all(m in campaign for m in maps), 'Requested map is outside original campaign')
        campaign = [m for m in campaign if m in maps]
    helper_file = project / 'scripts/refresh_native_missing_materials.py'
    spec = importlib.util.spec_from_file_location('aa_scenery_geometry', helper_file)
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    target = project / 'Saved/ReviewArtifacts/OrdinarySceneryRefresh.json'
    previous = json.loads(target.read_text()) if target.exists() else {}
    require(refresh or previous, 'Fresh-process proof needs the first-process census')
    report = dict(previous) if refresh and target.exists() else {}

    def prop_state(prop):
        sprite = prop.get_components_by_class(unreal.StaticMeshComponent)[0]
        return {'path': path(prop), 'transform': transform(prop), 'tags': [str(t) for t in prop.get_editor_property("tags")],
                'pickup': path(prop.get_pickup()), 'material': path(sprite.get_material(0)),
                'mesh': path(sprite.get_editor_property('static_mesh'))}

    for name in campaign:
        source = root / f'Exe/{name}.MAP'
        things = raw_things(source)
        ordinary = {i: t[3] for i, t in enumerate(things) if t[3] in resources}
        metadata = {kind: source_metadata(root, kind, resources[kind]) for kind in set(ordinary.values())}
        expected = {i: kind for i, kind in ordinary.items() if metadata[kind] is not None}
        unsupported = {i: kind for i, kind in ordinary.items() if metadata[kind] is None}
        world = unreal.EditorLoadingAndSavingUtils.load_map(str(project / f'Content/Maps/{name}.umap'))
        levels = [a for a in actors.get_all_level_actors() if isinstance(a, unreal.NativeWadLevel)]
        require(world and len(levels) == 1, f'{name}: expected one existing saved native level')
        level = levels[0]
        geometry = helper.snapshot(level)
        mesh_hashes = helper.mesh_hashes(project, geometry)

        def untouched():
            result = {}
            for a in actors.get_all_level_actors():
                if isinstance(a, unreal.NativeWadScenery):
                    index = a.get_source_thing_id()
                    tags = [str(t) for t in a.get_editor_property("tags")]
                    if index in expected and f'WAD Scenery Thing {index}' in tags:
                        continue
                    result[path(a)] = prop_state(a)  # Inventory decorations and unsupported ELMARK also remain unchanged.
                else:
                    result[path(a)] = {'transform': transform(a), 'tags': [str(t) for t in a.get_editor_property("tags")]}
            return result

        def bindings():
            found = {}
            for actor in actors.get_all_level_actors():
                tags = [str(t) for t in actor.get_editor_property("tags")]
                ids = [int(t.rsplit(' ', 1)[1]) for t in tags if t.startswith('WAD Scenery Thing ')]
                if not ids or ids[0] not in expected:
                    continue
                require(isinstance(actor, unreal.NativeWadScenery) and len(ids) == 1, f'{name}: conflicting scenery tag class')
                require(ids[0] not in found and actor.get_source_thing_id() == ids[0], f'{name}: duplicate/mismatched Thing identity')
                found[ids[0]] = actor
            return found

        before = {i: prop_state(a) for i, a in bindings().items()}
        actual_missing_before = sorted(set(expected) - set(before))
        others = untouched()
        original_asset_hashes = {}
        # Existing generated sprite assets and user material overrides must not be rewritten.
        for filename in (project / f'Content/Maps/{name}_Assets/Things').glob('*.uasset'):
            original_asset_hashes[str(filename.relative_to(project))] = hashlib.sha256(filename.read_bytes()).hexdigest()
        for repeat in range(2 if refresh else 1):
            if refresh:
                unreal.SystemLibrary.execute_console_command(world,
                    f'AA.RefreshNativeWadOrdinaryScenery "{source.as_posix()}" {name} /Game/Maps/{name}')
                require(unreal.EditorLoadingAndSavingUtils.save_map(world, f'/Game/Maps/{name}'),
                        f'{name}: public save failed; memory-only scenery is not persisted proof')
            found = bindings()
            require(set(found) == set(expected), f'{name}: exact supported ordinary Thing IDs are missing/extra')
            for index, actor in found.items():
                kind = expected[index]
                meta = metadata[kind]
                info = list(actor.get_source_scenery_info())
                require(info == [kind, index, meta['radius'], meta['attributes'], meta['ticks'], meta['animation'],
                                  meta['frames'], int(meta['passable']), int(meta['hanging']), int(meta['raised']), int(meta['above_liquid'])],
                        f'{name}/Thing{index}: source stance/collision/mount metadata differs')
                if name == 'L30' and index == 17:
                    require(actor.get_source_mount_sector() == 120, 'L30/Thing17: original BLOCKMAP mounting sector120 was lost')
                sprite = actor.get_components_by_class(unreal.StaticMeshComponent)[0]
                box = actor.get_components_by_class(unreal.BoxComponent)[0]
                require(sprite.get_material(0) is not None, f'{name}/Thing{index}: no saved original material')
                extent = box.get_unscaled_box_extent()
                require(abs(extent.x - meta['radius']) < 0.01 and abs(extent.y - meta['radius']) < 0.01,
                        f'{name}/Thing{index}: original obstacle radius differs')
                require(box.get_collision_enabled() == (unreal.CollisionEnabled.NO_COLLISION if meta['passable'] else unreal.CollisionEnabled.QUERY_ONLY),
                        f'{name}/Thing{index}: source passability differs')
                require(abs(sprite.get_editor_property('relative_location').z - meta['size'][1] * (-0.5 if meta['hanging'] else 0.5)) < 0.01,
                        f'{name}/Thing{index}: sprite lost bottom/top root')
                state = prop_state(actor)
                if index in before:
                    require(state == before[index], f'{name}/Thing{index}: edited transform/material/tag/pickup or actor identity changed')
                else:
                    require(state['transform'][:2] == list(things[index][:2]), f'{name}/Thing{index}: new actor moved from original XY')
            require(helper.snapshot(level) == geometry and helper.mesh_hashes(project, geometry) == mesh_hashes,
                    f'{name}: scenery-only command changed geometry/collision/materials/lighting shells')
            require(untouched() == others, f'{name}: scenery-only command changed another actor')
            for filename, digest in original_asset_hashes.items():
                require(hashlib.sha256((project / filename).read_bytes()).hexdigest() == digest,
                        f'{name}: existing valid sprite asset was rewritten: {filename}')
            before = {i: prop_state(a) for i, a in found.items()}
        final = {'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
                 'ordinary_source_placements': len(ordinary), 'supported_source_placements': len(expected),
                 'supported_source_types': dict(Counter(expected.values())), 'unsupported_ids': unsupported,
                 'actual_saved_missing_before': actual_missing_before, 'actual_saved_missing_after': [],
                 'actors': before, 'unchanged_geometry': geometry, 'unchanged_mesh_hashes': mesh_hashes,
                 'unchanged_non_scenery_actors': others, 'unchanged_existing_sprite_hashes': original_asset_hashes}
        # Normalize tuple/dict-key representation to the durable JSON format.
        final = json.loads(json.dumps(final))
        if not refresh:
            prior = previous.get(name)
            require(prior is not None, f'{name}: missing durable first-process result')
            for key in ('source_sha256', 'ordinary_source_placements', 'supported_source_placements', 'supported_source_types',
                        'unsupported_ids', 'actors', 'unchanged_geometry', 'unchanged_mesh_hashes', 'unchanged_non_scenery_actors'):
                require(final[key] == prior[key], f'{name}: fresh-process reopen differs in {key}')
        else:
            report[name] = final
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(json.dumps(report, indent=2), encoding='utf-8')
        unreal.log(f'{name}: independently verified {len(expected)} supported ordinary props; actual saved omissions before={len(actual_missing_before)}, after=0; unsupported={unsupported}')
    unreal.log('Ordinary scenery census complete; registration audit1537 is distinct from measured saved omissions.')


if __name__ == '__main__':
    verify_scenery(refresh='-AAOrdinarySceneryVerifyOnly' not in unreal.SystemLibrary.get_command_line())
