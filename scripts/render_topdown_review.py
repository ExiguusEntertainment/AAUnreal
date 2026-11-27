import argparse
import colorsys
import math
import os
import struct
import sys
import zlib


def read_file(path):
    with open(path, "rb") as f:
        return f.read()


def make_upper8(name_bytes):
    return name_bytes.split(b"\x00", 1)[0].decode("ascii", errors="ignore").upper().strip()


def parse_wad(path, map_name):
    data = read_file(path)
    if len(data) < 12:
        raise ValueError("file too small")

    sig, num_lumps, dir_offset = struct.unpack_from("<4sii", data, 0)
    if sig not in (b"IWAD", b"PWAD"):
        raise ValueError(f"bad WAD signature: {sig!r}")

    if dir_offset + (num_lumps * 16) > len(data):
        raise ValueError("directory out of range")

    directory = []
    for i in range(num_lumps):
        file_pos, size, name = struct.unpack_from("<ii8s", data, dir_offset + (i * 16))
        directory.append({"file_pos": file_pos, "size": size, "name": make_upper8(name)})

    wanted = map_name.upper()
    map_index = next((i for i, ent in enumerate(directory) if ent["name"] == wanted), -1)

    lumps = {}
    needed = ("VERTEXES", "LINEDEFS", "SIDEDEFS", "SECTORS")
    if map_index != -1:
        for ent in directory[map_index + 1:]:
            if ent["size"] == 0:
                break
            lumps.setdefault(ent["name"], ent)
    else:
        for ent in directory:
            if ent["size"] <= 0:
                continue
            lumps.setdefault(ent["name"], ent)

    for name in needed:
        if name not in lumps:
            raise ValueError(f"missing required lump {name}")

    def lump_slice(name):
        ent = lumps[name]
        start = ent["file_pos"]
        end = start + ent["size"]
        return data[start:end]

    def read_structs(name, fmt):
        blob = lump_slice(name)
        size = struct.calcsize(fmt)
        if len(blob) % size != 0:
            raise ValueError(f"bad {name} size")
        return [struct.unpack_from(fmt, blob, i) for i in range(0, len(blob), size)]

    vertices = [{"x": x, "y": y} for x, y in read_structs("VERTEXES", "<hh")]
    linedefs = []
    for fr, to, flags, special, tag, side0, side1 in read_structs("LINEDEFS", "<hhhhhhh"):
        linedefs.append({"from": fr, "to": to, "side0": side0, "side1": side1})
    sidedefs = []
    for offx, offy, t1, t2, t3, sector in read_structs("SIDEDEFS", "<hh8s8s8sh"):
        sidedefs.append({"sector": sector})
    sectors = read_structs("SECTORS", "<hh8s8shhh")

    segs = []
    if "SEGS" in lumps:
        for fr, to, angle, linedef, side, offset in read_structs("SEGS", "<hhhhhh"):
            segs.append({"from": fr, "to": to, "linedef": linedef, "side": side})

    ssectors = []
    if "SSECTORS" in lumps:
        for num_segs, first_seg in read_structs("SSECTORS", "<hh"):
            ssectors.append({"num_segs": num_segs, "first_seg": first_seg})

    return {
        "vertices": vertices,
        "linedefs": linedefs,
        "sidedefs": sidedefs,
        "sectors": sectors,
        "segs": segs,
        "ssectors": ssectors,
    }


def sector_color(index):
    hue = ((index * 0.61803398875) % 1.0)
    r, g, b = colorsys.hsv_to_rgb(hue, 0.35, 0.95)
    return (int(r * 255), int(g * 255), int(b * 255), 255)


def point_in_poly(px, py, poly):
    inside = False
    j = len(poly) - 1
    for i in range(len(poly)):
        ax, ay = poly[i]
        bx, by = poly[j]
        if ((ay > py) != (by > py)):
            denom = (by - ay)
            if abs(denom) < 1e-6:
                denom = 1e-6
            cross_x = ((bx - ax) * (py - ay) / denom) + ax
            if px < cross_x:
                inside = not inside
        j = i
    return inside


def draw_line(pixels, width, height, x0, y0, x1, y1, color):
    dx = abs(x1 - x0)
    sx = 1 if x0 < x1 else -1
    dy = -abs(y1 - y0)
    sy = 1 if y0 < y1 else -1
    err = dx + dy
    while True:
        if 0 <= x0 < width and 0 <= y0 < height:
            pixels[(y0 * width) + x0] = color
        if x0 == x1 and y0 == y1:
            break
        e2 = 2 * err
        if e2 >= dy:
            err += dy
            x0 += sx
        if e2 <= dx:
            err += dx
            y0 += sy


def extract_sector_polys(parsed, sector_index):
    vertices = parsed["vertices"]
    linedefs = parsed["linedefs"]
    sidedefs = parsed["sidedefs"]
    segs = parsed["segs"]
    ssectors = parsed["ssectors"]

    if not segs or not ssectors:
        return []

    polys = []
    for ss in ssectors:
        seg_subset = []
        for k in range(ss["num_segs"]):
            seg_index = ss["first_seg"] + k
            if 0 <= seg_index < len(segs):
                seg_subset.append(segs[seg_index])

        if not seg_subset:
            continue

        primary_hits = {}
        any_hits = {}
        chains = []
        for seg in seg_subset:
            fr = seg["from"]
            to = seg["to"]
            if fr == to or not (0 <= fr < len(vertices)) or not (0 <= to < len(vertices)):
                continue
            chains.append((fr, to))
            linedef_index = seg["linedef"]
            if not (0 <= linedef_index < len(linedefs)):
                continue
            line = linedefs[linedef_index]

            seg_side = seg["side"]
            if seg_side == 0:
                side_ref = line["side0"]
                if 0 <= side_ref < len(sidedefs):
                    sector = sidedefs[side_ref]["sector"]
                    primary_hits[sector] = primary_hits.get(sector, 0) + 1
            elif seg_side == 1:
                side_ref = line["side1"]
                if 0 <= side_ref < len(sidedefs):
                    sector = sidedefs[side_ref]["sector"]
                    primary_hits[sector] = primary_hits.get(sector, 0) + 1

            for side_ref in (line["side0"], line["side1"]):
                if 0 <= side_ref < len(sidedefs):
                    sector = sidedefs[side_ref]["sector"]
                    any_hits[sector] = any_hits.get(sector, 0) + 1

        if not chains:
            continue

        dominant_hits = primary_hits or any_hits
        if not dominant_hits:
            continue
        dominant_sector = max(dominant_hits.items(), key=lambda kv: kv[1])[0]
        if dominant_sector != sector_index:
            continue

        local_polys = []
        used = [False] * len(chains)
        for start_idx in range(len(chains)):
            if used[start_idx]:
                continue
            start_v, curr = chains[start_idx]
            loop = [start_v, curr]
            used[start_idx] = True
            guard = 0
            while guard < 100000:
                guard += 1
                if curr == start_v:
                    break
                next_idx = None
                next_v = None
                for i, (a, b) in enumerate(chains):
                    if used[i]:
                        continue
                    if a == curr:
                        next_idx = i
                        next_v = b
                        break
                    if b == curr:
                        next_idx = i
                        next_v = a
                        break
                if next_idx is None:
                    break
                used[next_idx] = True
                if loop[-1] != next_v:
                    loop.append(next_v)
                curr = next_v

            if len(loop) >= 4 and loop[-1] == loop[0]:
                local_polys.append(loop[:-1])

        if not local_polys and len(chains) >= 3:
            loop = [chains[0][0], chains[0][1]]
            for a, b in chains[1:]:
                tail = loop[-1]
                if a == tail:
                    loop.append(b)
                elif b == tail:
                    loop.append(a)
                else:
                    loop.append(a)
            if loop[-1] != loop[0]:
                loop.append(loop[0])
            if len(loop) >= 4:
                local_polys.append(loop[:-1])

        polys.extend(local_polys)

    out = []
    for loop in polys:
        pts = []
        for idx in loop:
            if 0 <= idx < len(vertices):
                v = vertices[idx]
                pts.append((float(v["x"]), float(v["y"])))
        if len(pts) >= 3:
            out.append(pts)
    return out


def extract_sector_boundary_loops(parsed, sector_index):
    vertices = parsed["vertices"]
    linedefs = parsed["linedefs"]
    sidedefs = parsed["sidedefs"]

    directed_edges = []
    for line in linedefs:
        side0 = line["side0"]
        side1 = line["side1"]
        sector0 = sidedefs[side0]["sector"] if 0 <= side0 < len(sidedefs) else -1
        sector1 = sidedefs[side1]["sector"] if 0 <= side1 < len(sidedefs) else -1

        if sector0 == sector_index and sector1 != sector_index:
            directed_edges.append((line["from"], line["to"]))
        elif sector1 == sector_index and sector0 != sector_index:
            directed_edges.append((line["to"], line["from"]))

    if not directed_edges:
        return []

    outgoing = {}
    for edge_index, (a, _b) in enumerate(directed_edges):
        outgoing.setdefault(a, []).append(edge_index)

    used = [False] * len(directed_edges)
    loops = []

    for start_edge in range(len(directed_edges)):
        if used[start_edge]:
            continue

        start_a, start_b = directed_edges[start_edge]
        if not (0 <= start_a < len(vertices) and 0 <= start_b < len(vertices)):
            used[start_edge] = True
            continue

        loop = [start_a, start_b]
        used[start_edge] = True
        current = start_b
        guard = 0

        while guard < len(directed_edges) + 1:
            guard += 1
            if current == start_a:
                break

            next_edge = None
            for candidate in outgoing.get(current, []):
                if not used[candidate]:
                    next_edge = candidate
                    break

            if next_edge is None:
                break

            used[next_edge] = True
            _na, nb = directed_edges[next_edge]
            if loop[-1] != nb:
                loop.append(nb)
            current = nb

        if len(loop) >= 4 and loop[-1] == loop[0]:
            loops.append(loop[:-1])

    out = []
    for loop in loops:
        pts = []
        for idx in loop:
            if 0 <= idx < len(vertices):
                v = vertices[idx]
                pts.append((float(v["x"]), float(v["y"])))
        if len(pts) >= 3:
            out.append(pts)
    return out


def write_png(path, width, height, pixels):
    def chunk(tag, payload):
        tag_bytes = tag.encode("ascii")
        crc = zlib.crc32(tag_bytes + payload) & 0xFFFFFFFF
        return struct.pack(">I", len(payload)) + tag_bytes + payload + struct.pack(">I", crc)

    raw_rows = []
    for y in range(height):
        raw_rows.append(b"\x00")
        row = bytearray()
        for x in range(width):
            r, g, b, a = pixels[(y * width) + x]
            row.extend((r, g, b, a))
        raw_rows.append(bytes(row))
    raw = b"".join(raw_rows)
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    data = b"".join([
        b"\x89PNG\r\n\x1a\n",
        chunk("IHDR", ihdr),
        chunk("IDAT", zlib.compress(raw, 9)),
        chunk("IEND", b""),
    ])
    with open(path, "wb") as f:
        f.write(data)


def render_review_png(parsed, out_path, image_size):
    vertices = parsed["vertices"]
    lines = parsed["linedefs"]
    if not vertices:
        raise ValueError("no vertices to render")

    xs = [v["x"] for v in vertices]
    ys = [v["y"] for v in vertices]
    min_x, max_x = min(xs), max(xs)
    min_y, max_y = min(ys), max(ys)
    span_x = max(max_x - min_x, 1.0)
    span_y = max(max_y - min_y, 1.0)
    margin = max(16, min(128, image_size // 20))
    scale = float(image_size - (2 * margin) - 1) / float(max(span_x, span_y))

    def map_pt(pt):
        x = margin + ((pt[0] - min_x) * scale)
        y = margin + ((pt[1] - min_y) * scale)
        return (x, float(image_size - 1) - y)

    pixels = [(248, 248, 248, 255)] * (image_size * image_size)
    filled_sectors = 0
    filled_loops = 0

    for sector_index in range(len(parsed["sectors"])):
        loops = extract_sector_boundary_loops(parsed, sector_index)
        if not loops:
            continue
        filled_sectors += 1
        fill = sector_color(sector_index)

        pixel_loops = [[map_pt(pt) for pt in poly] for poly in loops]
        min_px = max(0, int(math.floor(min(p[0] for poly in pixel_loops for p in poly))))
        max_px = min(image_size - 1, int(math.ceil(max(p[0] for poly in pixel_loops for p in poly))))
        min_py = max(0, int(math.floor(min(p[1] for poly in pixel_loops for p in poly))))
        max_py = min(image_size - 1, int(math.ceil(max(p[1] for poly in pixel_loops for p in poly))))

        for py in range(min_py, max_py + 1):
            for px in range(min_px, max_px + 1):
                inside_count = 0
                for pixel_poly in pixel_loops:
                    if point_in_poly(px + 0.5, py + 0.5, pixel_poly):
                        inside_count += 1
                if inside_count % 2 == 1:
                    pixels[(py * image_size) + px] = fill

        for pixel_poly in pixel_loops:
            for i in range(len(pixel_poly)):
                ax, ay = pixel_poly[i]
                bx, by = pixel_poly[(i + 1) % len(pixel_poly)]
                draw_line(pixels, image_size, image_size, round(ax), round(ay), round(bx), round(by), (0, 0, 0, 255))
        filled_loops += len(pixel_loops)

    for line in lines:
        a = line["from"]
        b = line["to"]
        if not (0 <= a < len(vertices) and 0 <= b < len(vertices)):
            continue
        ax, ay = map_pt((vertices[a]["x"], vertices[a]["y"]))
        bx, by = map_pt((vertices[b]["x"], vertices[b]["y"]))
        draw_line(pixels, image_size, image_size, round(ax), round(ay), round(bx), round(by), (24, 24, 24, 255))

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    write_png(out_path, image_size, image_size, pixels)
    return filled_sectors, filled_loops


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("wad_path")
    parser.add_argument("map_name")
    parser.add_argument("output_png")
    parser.add_argument("--size", type=int, default=2048)
    args = parser.parse_args()

    parsed = parse_wad(args.wad_path, args.map_name)
    filled_sectors, filled_loops = render_review_png(parsed, args.output_png, max(256, args.size))
    print(
        f"render_topdown_review succeeded: sectors={len(parsed['sectors'])} "
        f"filled_sectors={filled_sectors} boundary_loops={filled_loops} out={args.output_png}"
    )


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"render_topdown_review failed: {exc}", file=sys.stderr)
        sys.exit(1)
