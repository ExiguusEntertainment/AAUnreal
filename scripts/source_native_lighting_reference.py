"""Independent original LIGHT.C/3D_IO.C metadata reference. No Unreal imports/writes.

Original fixed-point/BLOCKMAP center-sector selection, full type rules and
ordered terrain/glow setup; no nearest-polygon fallback or importer metadata.
This reference is used by the two-process saved-map proof script.
"""
from pathlib import Path
import hashlib
import math
import re
import struct


def require(condition, message):
    if not condition:
        raise ValueError(message)


def campaign(exe):
    result = ['L' + n for q in range(7) for n in re.findall(
        r'^mapnumber\s*=\s*(\d+)', (exe / f'MAPDESC/QUEST{q}.INI').read_text(), re.M)]
    require(len(result) == 30 and len(set(result)) == 30, 'Expected30 distinct source campaign maps')
    return result


def original(path):
    raw = Path(path).read_bytes()
    require(len(raw) >= 12, 'Truncated original MAP header')
    magic, count, directory = struct.unpack_from('<4sII', raw)
    require(magic in (b'PWAD', b'IWAD') and directory + count * 16 <= len(raw), 'Invalid MAP directory')
    lumps = {}
    for i in range(count):
        offset, length, name = struct.unpack_from('<II8s', raw, directory + i * 16)
        require(offset + length <= len(raw), 'Invalid source lump bounds')
        name = name.split(b'\0')[0].decode('ascii')
        require(name not in lumps, f'Duplicate source lump {name}')
        lumps[name] = raw[offset:offset + length]
    return raw, lumps


def source_sector(lumps, x, y):
    """Active original IFindSectorNum including row search; None=invalid lookup."""
    vertices = list(struct.iter_unpack('<hh', lumps['VERTEXES']))
    lines = list(struct.iter_unpack('<HHHHHHH', lumps['LINEDEFS']))
    sides = list(struct.iter_unpack('<hh8s8s8sH', lumps['SIDEDEFS']))
    raw = lumps['BLOCKMAP']
    require(len(raw) >= 8 and len(raw) % 2 == 0, 'Invalid BLOCKMAP size')
    words = struct.unpack('<' + 'H' * (len(raw) // 2), raw)
    ox, oy, columns, rows = struct.unpack_from('<hhHH', raw)
    require(columns > 0 and rows > 0 and columns * rows <= len(words) - 4, 'Invalid BLOCKMAP dimensions')
    column, row = (math.floor(x) - ox) >> 7, (math.floor(y) - oy) >> 7
    x, y = math.floor(x), math.floor(y)
    if not (0 <= column < columns and 0 <= row < rows):
        return None

    def side(px, py, line):
        require(0 <= line < len(lines), 'Invalid BLOCKMAP line')
        a, b = (vertices[lines[line][i]] for i in (0, 1))
        value = (a[1] - b[1]) * (px - b[0]) - (a[0] - b[0]) * (py - b[1])
        require(-2147483648 <= value <= 2147483647, 'Original line test would overflow signed32')
        return 0 if value < 0 else 1 if value > 0 else 2

    found, last, last_side = 65534, 65535, 0
    history = []
    while found == 65534:
        at = 1 + words[4 + row * columns + column]
        require(4 + columns * rows <= at < len(words), 'BLOCKMAP cell offset outside line lists')
        while True:
            require(at < len(words), 'Unterminated BLOCKMAP line list')
            line = words[at]
            at += 1
            if line == 65535:
                break
            which = side(x, y, line)
            if which == 2:
                which = 0 if found == 65534 else 1 - last_side
            candidate = lines[line][5 + which]
            new = False
            if candidate != 65535:
                require(candidate < len(sides), 'Invalid sidedef in BLOCKMAP')
                new = found == 65534 or side(*vertices[lines[line][0]], last) == last_side or side(*vertices[lines[line][1]], last) == last_side
            for old, old_side in history:
                if side(*vertices[lines[line][0]], old) != old_side and side(*vertices[lines[line][1]], old) != old_side:
                    new = False
                    break
            if new:
                if last != 65535:
                    history.append((last, last_side))
                    require(len(history) <= 20, 'Original BLOCKMAP history capacity exceeded')
                found, last, last_side = candidate, line, which
        if found == 65534:
            row += 1
            if row >= rows:
                return None
    return sides[found][-1] if found not in (65534, 65535) else None


def illumination(kind, data):
    if kind == 924:
        return data if data < 32768 else data - 65536
    if kind == 27:
        return 128
    if kind == 38:
        return 255
    if kind & 4095 == 41:
        return 64
    return None


def directive(index, kind, sector):
    animation, rate, center, radius = {1526: (1, 70, 190, 60), 1506: (1, 40, 128, 64), 1543: (1, 20, 192, 32)}.get(kind, (0, 0, 0, 0))
    return dict(thing_id=index, marker_type=kind, sector=sector, animation_type=animation, rate=rate, center=center, radius=radius)


def metadata(path, centers=None, bindings=None):
    path = Path(path)
    raw, lumps = original(path)
    centers, bindings = centers or {}, bindings or {}
    things = list(struct.iter_unpack('<hhHHH', lumps['THINGS']))
    source_sectors = list(struct.iter_unpack('<hh8s8sHHH', lumps['SECTORS']))
    sides = list(struct.iter_unpack('<hh8s8s8sH', lumps['SIDEDEFS']))
    sectors = [dict(sector=i, authored_light=s[4] & 255, outside=s[3].startswith(b'F_SKY') or bool(s[6] & 1), door=False, neighbors=[])
               for i, s in enumerate(source_sectors)]
    neighbors = [set() for _ in sectors]
    unaddressable = []
    lines = list(struct.iter_unpack('<HHHHHHH', lumps['LINEDEFS']))
    for index, row in enumerate(lines):
        if not row[2] & 4:
            continue
        if any(side >= len(sides) or sides[side][-1] >= len(sectors) for side in (row[5], row[6])):
            unaddressable.append(index)
            continue
        a, b = sides[row[5]][-1], sides[row[6]][-1]
        require(a < len(sectors) and b < len(sectors), 'Invalid source sector adjacency')
        neighbors[a].add(b)
        neighbors[b].add(a)
    for i, values in enumerate(neighbors):
        sectors[i]['neighbors'] = sorted(values)
    owners, directives, final = [], [], {}
    for index, (x, y, angle, kind, data) in enumerate(things):
        power = illumination(kind, data)
        if power is None and kind not in (1500, 1503, 1504, 1505, 1506, 1507, 1526, 1543):
            continue
        authored_sector = source_sector(lumps, x, y)
        if power is not None:
            current = list(centers.get(index, (x, y)))
            require(all(math.isfinite(v) and -32768 <= v <= 32767 for v in current), 'Edited source XY out of range')
            sector = source_sector(lumps, *current)
            owners.append(dict(thing_id=index, source_type=kind, accessory_data=data, illumination=power,
                               authored_sector=0 if authored_sector is None else authored_sector,
                               center_sector=0 if sector is None else sector, authored_position=[x, y], center_position=current,
                               uses_edited_center=current != [x, y], used_source_fallback=sector is None, bound_actor=bindings.get(index)))
        else:
            require(authored_sector is not None, 'Source special marker has invalid center lookup')
            if kind == 1500:
                sectors[authored_sector]['door'] = True
            else:
                rule = directive(index, kind, authored_sector)
                directives.append(rule)
                final[authored_sector] = rule
    return dict(version=1, source_map_name=path.stem.upper(), source_map_sha1=hashlib.sha1(raw).hexdigest().upper(),
                thing_count=len(things), line_count=len(lines), unaddressable_two_sided_lines=unaddressable, sectors=sectors, illuminators=owners, directives=directives,
                animations=[rule for sector, rule in sorted(final.items()) if rule['animation_type']])
