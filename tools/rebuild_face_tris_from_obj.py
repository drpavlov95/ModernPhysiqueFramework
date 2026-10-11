#!/usr/bin/env python3
"""Rebuild the three RLY head morphs from Outfit Studio OBJ exports.

OBJ exports are in the Outfit Studio scale (one tenth of the base coordinates
stored in FRTRI003).  The neck target is taken from the exact BodySlide source
NIF and OSD used by the HIMBO project.  Only the three RLY blocks are changed;
every other morph remains byte-for-byte compatible.
"""

from __future__ import annotations

import argparse
import collections
import math
import shutil
import struct
from pathlib import Path

from match_neck_to_body import FaceTri, _boundary_components


def read_obj(path: Path) -> list[tuple[float, float, float]]:
    vertices = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("v "):
            fields = line.split()
            if len(fields) < 4:
                raise ValueError(f"malformed vertex in {path}: {line!r}")
            vertices.append(tuple(float(value) for value in fields[1:4]))
    if not vertices:
        raise ValueError(f"OBJ has no vertices: {path}")
    return vertices


def read_osd(path: Path, names: set[str]) -> dict[str, dict[int, tuple[float, float, float]]]:
    """Read selected Outfit Studio slider-data blocks.

    OSD Vector3 values use the same coordinate scale as NIF/TRI morph deltas,
    despite OBJ exports being written at one tenth scale.
    """
    data = path.read_bytes()
    if data[:4] != b"\x00DSO":
        raise ValueError(f"invalid OSD header: {path}")
    _, count = struct.unpack_from("<II", data, 4)
    offset = 12
    result: dict[str, dict[int, tuple[float, float, float]]] = {}
    for _ in range(count):
        name_length = data[offset]
        offset += 1
        name = data[offset:offset + name_length].decode("utf-8")
        offset += name_length
        diff_count = struct.unpack_from("<H", data, offset)[0]
        offset += 2
        if name in names:
            diffs = {}
            for index in range(diff_count):
                vertex, x, y, z = struct.unpack_from("<Hfff", data, offset + index * 14)
                diffs[vertex] = (x, y, z)
            result[name] = diffs
        offset += diff_count * 14
    missing = names - result.keys()
    if missing:
        raise ValueError(f"missing OSD data block(s) in {path}: {', '.join(sorted(missing))}")
    return result


def read_nif_dump(path: Path, shape_name: str) -> tuple[list[tuple[float, float, float]], list[tuple[int, int, int]]]:
    # dump_nif_vertices.exe emits UTF-8 when captured directly, but accepting
    # UTF-16 makes the helper convenient to use from PowerShell too.
    raw = path.read_bytes()
    for encoding in ("utf-8", "utf-16"):
        try:
            lines = raw.decode(encoding).splitlines()
            if any(line.startswith("shape ") for line in lines):
                break
        except UnicodeDecodeError:
            continue
    else:
        raise ValueError(f"unrecognised NIF dump: {path}")

    i = 0
    while i < len(lines):
        if not lines[i].startswith("shape "):
            i += 1
            continue
        fields = lines[i].split()
        count = int(fields[-1])
        name = " ".join(fields[1:-1])
        vertex_lines = lines[i + 1:i + 1 + count]
        vertices = [tuple(float(value) / 10.0 for value in line.split()) for line in vertex_lines]
        tri_line = i + 1 + count
        if tri_line >= len(lines) or not lines[tri_line].startswith("triangles "):
            raise ValueError(f"NIF dump has no triangles for {name}: {path}")
        tri_count = int(lines[tri_line].split()[-1])
        triangles = [tuple(int(value) for value in lines[tri_line + 1 + j].split()) for j in range(tri_count)]
        if name == shape_name:
            return vertices, triangles
        i = tri_line + 1 + tri_count
    raise ValueError(f"shape not found in NIF dump: {shape_name}")


def distance(a: tuple[float, float, float], b: tuple[float, float, float]) -> float:
    return math.sqrt(sum((a[index] - b[index]) ** 2 for index in range(3)))


def add(a: tuple[float, float, float], b: tuple[float, float, float], scale: float = 1.0):
    return tuple(a[index] + b[index] * scale for index in range(3))


def build_adjacency(triangles: list[tuple[int, int, int]]) -> dict[int, set[int]]:
    adjacency: dict[int, set[int]] = collections.defaultdict(set)
    for a, b, c in triangles:
        adjacency[a].update((b, c))
        adjacency[b].update((a, c))
        adjacency[c].update((a, b))
    return adjacency


def ring_mapping(
    body_vertices: list[tuple[float, float, float]],
    body_triangles: list[tuple[int, int, int]],
    face: FaceTri,
) -> list[tuple[int, int]]:
    components = [component for component in _boundary_components(body_triangles) if len(component) == 26]
    if not components:
        raise ValueError("could not find the 26-vertex HIMBO neck ring")
    body_ring = max(components, key=lambda c: sum(body_vertices[index][2] for index in c) / len(c))

    pairs = []
    used: set[int] = set()
    # FRTRI003 stores its base vertices in NIF scale, while the body dump was
    # already converted to Outfit Studio scale above.
    face_base = [tuple(value * 0.1 for value in point) for point in face.base]
    for body_index in body_ring:
        head_index, d = min(
            ((index, distance(body_vertices[body_index], point)) for index, point in enumerate(face_base)),
            key=lambda item: item[1],
        )
        # HPH/AHPH use the same neck topology with tiny base-coordinate
        # differences; keep the conservative historical 0.25-unit guard.
        if d > 0.25 or head_index in used:
            raise ValueError(f"could not map body/head ring (distance={d:.6f})")
        used.add(head_index)
        pairs.append((body_index, head_index))
    return pairs


def smooth_seam(
    desired: list[tuple[float, float, float]],
    ring: dict[int, tuple[float, float, float]],
    adjacency: dict[int, set[int]],
    loops: int,
) -> tuple[list[tuple[float, float, float]], dict[int, int]]:
    result = list(desired)

    # Preserve the sculpt everywhere except the seam ring and its short fade.
    distances: dict[int, int] = {}
    source_distances: dict[int, dict[int, int]] = {}
    queue = collections.deque()
    for vertex in ring:
        distances[vertex] = 0
        source_distances[vertex] = {vertex: 0}
        queue.append((vertex, vertex))
    while queue:
        source, vertex = queue.popleft()
        dist = source_distances[source][vertex]
        if dist >= loops:
            continue
        for neighbour in adjacency[vertex]:
            if neighbour not in source_distances[source]:
                source_distances[source][neighbour] = dist + 1
                distances[neighbour] = min(distances.get(neighbour, loops + 1), dist + 1)
                queue.append((source, neighbour))

    for vertex, dist in distances.items():
        if vertex in ring:
            result[vertex] = ring[vertex]
            continue
        # Two-loop fade: half strength on the first loop and a small second
        # loop contribution.  This avoids the broad four-loop deformation.
        t = max(0.0, min(1.0, (loops - dist + 1) / (loops + 1)))
        weight = t * t * (3.0 - 2.0 * t)
        nearby = []
        for source, source_map in source_distances.items():
            source_dist = source_map.get(vertex)
            if source_dist is None:
                continue
            source_t = max(0.0, min(1.0, (loops - source_dist + 1) / (loops + 1)))
            source_weight = source_t * source_t * (3.0 - 2.0 * source_t)
            nearby.append((source, source_weight))
        total = sum(source_weight for _, source_weight in nearby)
        if total <= 0.0:
            continue
        correction = tuple(
            sum((ring[source][axis] - desired[source][axis]) * source_weight for source, source_weight in nearby) / total
            for axis in range(3)
        )
        result[vertex] = add(desired[vertex], correction, weight)
    return result, distances


def replace_block(out: bytearray, face: FaceTri, name: str, values: list[tuple[float, float, float]]) -> None:
    _, block_offset = face.morphs[name]
    multiplier, _ = face.morphs[name]
    maximum = max(abs(value) for point in values for value in point)
    multiplier = max(multiplier, maximum / 32760.0, 1e-8)
    struct.pack_into("<f", out, face.multiplier_offsets[name], multiplier)
    raw = [int(round(value / multiplier)) for point in values for value in point]
    if any(value < -32768 or value > 32767 for value in raw):
        raise ValueError(f"{name} exceeds int16 TRI range")
    struct.pack_into(f"<{len(raw)}h", out, block_offset, *raw)


def process(
    template: Path,
    obj_paths: dict[str, Path],
    body_dump: Path,
    body_osd_path: Path,
    output: Path,
    body_shape: str,
    fade_loops: int,
) -> None:
    body_vertices, body_triangles = read_nif_dump(body_dump, body_shape)
    body_data_names = {f"{body_shape}RM_{morph}" for morph in ("Heavy", "Muscular", "Thin")}
    body_morphs = read_osd(body_osd_path, body_data_names)
    face = FaceTri(template.read_bytes())
    pairs = ring_mapping(body_vertices, body_triangles, face)
    adjacency = build_adjacency(face.triangles)
    out = bytearray(face.data)

    output.mkdir(parents=True, exist_ok=True)
    print(f"{template.name}: ring mapped ({len(pairs)} vertices)")
    for morph in ("Heavy", "Muscular", "Thin"):
        obj_vertices = read_obj(obj_paths[morph])
        if len(obj_vertices) != face.vertex_count:
            raise ValueError(f"{obj_paths[morph]} has {len(obj_vertices)} vertices; TRI has {face.vertex_count}")

        # Outfit Studio writes OBJ coordinates at 0.1 scale, while FRTRI003
        # stores its base vertices and morph deltas at NIF scale.  Therefore
        # the slider delta is OBJ*10 - base (not OBJ - base/10).
        desired = [
            tuple(obj_vertices[index][axis] * 10.0 - face.base[index][axis] for axis in range(3))
            for index in range(face.vertex_count)
        ]
        # The exact seam target is an absolute position from the BodySlide
        # source project: source-body base + RM slider.  Convert that absolute
        # position to a head delta individually, because the five head bases
        # do not coincide exactly with the source-body base ring.
        body = body_morphs[f"{body_shape}RM_{morph}"]
        ring = {
            head: tuple(
                body_vertices[body_index][axis] * 10.0
                + body.get(body_index, (0.0, 0.0, 0.0))[axis]
                - face.base[head][axis]
                for axis in range(3)
            )
            for body_index, head in pairs
        }
        corrected, near = smooth_seam(desired, ring, adjacency, fade_loops)
        ring_errors = [distance(corrected[head], ring[head]) for _, head in pairs]
        obj_errors = [distance(desired[index], corrected[index]) for index in near]
        print(f"  RLY_{morph}: ring residual max={max(ring_errors):.7f}; seam-adjust max={max(obj_errors):.5f}; vertices touched={len(near)}")
        replace_block(out, face, f"RLY_{morph}", corrected)

    candidate = output / template.name
    candidate.write_bytes(bytes(out))
    print(f"  candidate: {candidate}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--template", type=Path, required=True)
    parser.add_argument("--obj-dir", type=Path, required=True)
    parser.add_argument("--body-dump", type=Path, required=True)
    parser.add_argument("--body-osd", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--body-shape", default="HIMBO - Body HP")
    parser.add_argument("--fade-loops", type=int, default=2)
    args = parser.parse_args()
    stem = args.template.stem
    obj_paths = {m: args.obj_dir / f"{stem}_RLY_{m}.obj" for m in ("Heavy", "Muscular", "Thin")}
    missing = [str(path) for path in obj_paths.values() if not path.exists()]
    if missing:
        raise SystemExit("missing OBJ(s): " + ", ".join(missing))
    process(args.template, obj_paths, args.body_dump, args.body_osd, args.output, args.body_shape, args.fade_loops)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
