#!/usr/bin/env python3
"""Match RLY head-neck morph deltas to the HIMBO body _1 RM deltas.

The tool is deliberately conservative: only the named RLY morph blocks are
rewritten, and only in a short topology band above the head neck ring.  The
body NIF/TRI are references; they are never modified.
"""

from __future__ import annotations

import argparse
import collections
import math
import shutil
import struct
from datetime import datetime
from pathlib import Path


def _smoothstep(t: float) -> float:
    t = max(0.0, min(1.0, t))
    return t * t * (3.0 - 2.0 * t)


def _boundary_components(triangles: list[tuple[int, int, int]]) -> list[list[int]]:
    counts: collections.Counter[tuple[int, int]] = collections.Counter()
    for a, b, c in triangles:
        for x, y in ((a, b), (b, c), (c, a)):
            counts[tuple(sorted((x, y)))] += 1

    adjacency: dict[int, set[int]] = collections.defaultdict(set)
    for (a, b), count in counts.items():
        if count == 1:
            adjacency[a].add(b)
            adjacency[b].add(a)

    components: list[list[int]] = []
    seen: set[int] = set()
    for start in adjacency:
        if start in seen:
            continue
        stack = [start]
        seen.add(start)
        component: list[int] = []
        while stack:
            vertex = stack.pop()
            component.append(vertex)
            for neighbour in adjacency[vertex]:
                if neighbour not in seen:
                    seen.add(neighbour)
                    stack.append(neighbour)
        components.append(component)
    return components


def read_nifd(path: Path, shape_name: str) -> tuple[list[tuple[float, float, float]], list[tuple[int, int, int]]]:
    data = path.read_bytes()
    if data[:8] != b"NIFD0002":
        raise ValueError(f"not NIFD0002: {path}")
    offset = 8
    shape_count, = struct.unpack_from("<I", data, offset)
    offset += 4
    for _ in range(shape_count):
        name_len, = struct.unpack_from("<H", data, offset)
        offset += 2
        name = data[offset:offset + name_len].decode("utf-8", "replace")
        offset += name_len
        vertex_count, triangle_count = struct.unpack_from("<II", data, offset)
        offset += 8
        values = struct.unpack_from(f"<{vertex_count * 3}f", data, offset)
        offset += vertex_count * 12
        vertices = list(zip(*[iter(values)] * 3))
        indices = struct.unpack_from(f"<{triangle_count * 3}I", data, offset)
        offset += triangle_count * 12
        triangles = list(zip(*[iter(indices)] * 3))
        if name == shape_name:
            return vertices, triangles
    raise ValueError(f"shape not found in NIFD: {shape_name}")


def read_body_tri(path: Path, shape_name: str) -> dict[str, dict[int, tuple[float, float, float]]]:
    data = path.read_bytes()
    packed = data[:4] == b"PIRT"
    if not packed and data[:4] != b"\0IRT":
        raise ValueError(f"not a RaceMenu TRI/TRIP: {path}")
    offset = 4
    shape_count, = struct.unpack_from("<H" if packed else "<I", data, offset)
    offset += 2 if packed else 4
    for _ in range(shape_count):
        name_len = data[offset]
        offset += 1
        name = data[offset:offset + name_len].decode("utf-8", "replace")
        offset += name_len
        if not packed:
            offset += 4  # shape block size
        morph_count, = struct.unpack_from("<H" if packed else "<I", data, offset)
        offset += 2 if packed else 4
        morphs: dict[str, dict[int, tuple[float, float, float]]] = {}
        for _ in range(morph_count):
            morph_len = data[offset]
            offset += 1
            morph_name = data[offset:offset + morph_len].decode("utf-8", "replace")
            offset += morph_len
            if packed:
                multiplier, count = struct.unpack_from("<fH", data, offset)
                offset += 6
                deltas: dict[int, tuple[float, float, float]] = {}
                for _ in range(count):
                    vertex, x, y, z = struct.unpack_from("<Hhhh", data, offset)
                    offset += 8
                    deltas[vertex] = (x * multiplier, y * multiplier, z * multiplier)
            else:
                _, count = struct.unpack_from("<II", data, offset)
                offset += 8
                deltas = {}
                for _ in range(count):
                    vertex, x, y, z = struct.unpack_from("<Ifff", data, offset)
                    offset += 16
                    deltas[vertex] = (x, y, z)
            morphs[morph_name] = deltas
        if name == shape_name:
            return morphs
    raise ValueError(f"shape not found in body TRI: {shape_name}")


class FaceTri:
    def __init__(self, data: bytes):
        if data[:8] != b"FRTRI003":
            raise ValueError(f"not FRTRI003: {data[:8]!r}")
        self.data = data
        offset = 8
        self.vertex_count, = struct.unpack_from("<I", data, offset)
        offset += 4
        values = struct.unpack_from("<13I", data, offset)
        offset += 13 * 4
        polygon_count, self.uv_vertex_count, self.morph_count = values[0], values[4], values[6]
        base_values = struct.unpack_from(f"<{self.vertex_count * 3}f", data, offset)
        offset += self.vertex_count * 12
        self.base = list(zip(*[iter(base_values)] * 3))
        index_values = struct.unpack_from(f"<{polygon_count * 3}I", data, offset)
        offset += polygon_count * 12
        if self.uv_vertex_count:
            offset += self.uv_vertex_count * 8
        normal_indices = struct.unpack_from(f"<{polygon_count * 3}I", data, offset)
        offset += polygon_count * 12
        del normal_indices
        self.triangles = list(zip(*[iter(index_values)] * 3))
        self.morphs: dict[str, tuple[float, int]] = {}
        self.multiplier_offsets: dict[str, int] = {}
        for _ in range(self.morph_count):
            name_len, = struct.unpack_from("<I", data, offset)
            offset += 4
            name = data[offset:offset + name_len].decode("utf-8", "replace").rstrip("\0")
            offset += name_len
            multiplier_offset = offset
            multiplier, = struct.unpack_from("<f", data, offset)
            offset += 4
            self.morphs[name] = (multiplier, offset)
            self.multiplier_offsets[name] = multiplier_offset
            offset += self.vertex_count * 6

    def deltas(self, name: str) -> list[tuple[float, float, float]]:
        multiplier, offset = self.morphs[name]
        raw = struct.unpack_from(f"<{self.vertex_count * 3}h", self.data, offset)
        return [
            (raw[i * 3] * multiplier, raw[i * 3 + 1] * multiplier, raw[i * 3 + 2] * multiplier)
            for i in range(self.vertex_count)
        ]


def _distance(a: tuple[float, float, float], b: tuple[float, float, float]) -> float:
    return math.sqrt(sum((a[i] - b[i]) ** 2 for i in range(3)))


def _add(a: tuple[float, float, float], b: tuple[float, float, float], scale: float = 1.0):
    return tuple(a[i] + b[i] * scale for i in range(3))


def correct(path: Path, body_nifd: Path, body_tri: Path, body_shape: str, band_loops: int, apply: bool) -> None:
    body_vertices, body_triangles = read_nifd(body_nifd, body_shape)
    body_morphs = read_body_tri(body_tri, body_shape)
    targets = ["RM_Heavy", "RM_Thin", "RM_Muscular"]
    for target in targets:
        if target not in body_morphs:
            raise ValueError(f"body TRI is missing {target}")

    face = FaceTri(path.read_bytes())
    face_targets = {name: f"RLY_{name[3:]}" for name in targets}
    for body_name, face_name in face_targets.items():
        if face_name not in face.morphs:
            raise ValueError(f"face TRI is missing {face_name}")

    boundary = _boundary_components(body_triangles)
    body_ring = max(boundary, key=lambda component: sum(body_vertices[i][2] for i in component) / len(component))
    if len(body_ring) != 26:
        raise ValueError(f"unexpected neck ring size for {body_shape}: {len(body_ring)}")

    pairs: list[tuple[int, int, float]] = []
    used: set[int] = set()
    for body_index in body_ring:
        head_index, distance = min(
            ((i, _distance(body_vertices[body_index], point)) for i, point in enumerate(face.base)),
            key=lambda item: item[1],
        )
        if distance > 0.25 or head_index in used:
            raise ValueError(f"could not establish a unique body/head neck mapping: distance={distance:.4f}")
        used.add(head_index)
        pairs.append((body_index, head_index, distance))

    ring_heads = {head_index for _, head_index, _ in pairs}
    adjacency: dict[int, set[int]] = collections.defaultdict(set)
    for a, b, c in face.triangles:
        adjacency[a].update((b, c))
        adjacency[b].update((a, c))
        adjacency[c].update((a, b))

    distances: dict[int, dict[int, int]] = {}
    for source in ring_heads:
        distances[source] = {source: 0}
        queue = collections.deque([source])
        while queue:
            vertex = queue.popleft()
            distance = distances[source][vertex]
            if distance >= band_loops:
                continue
            for neighbour in adjacency[vertex]:
                if neighbour not in distances[source]:
                    distances[source][neighbour] = distance + 1
                    queue.append(neighbour)

    out = bytearray(face.data)
    print(f"{path.name}: body/head ring = {len(pairs)} vertices")
    print(f"  base correspondence distance: mean={sum(d for _, _, d in pairs) / len(pairs):.5f}, max={max(d for _, _, d in pairs):.5f}")

    for body_name, face_name in face_targets.items():
        body_delta = body_morphs[body_name]
        old_delta = face.deltas(face_name)
        _, block_offset = face.morphs[face_name]
        correction_by_head: dict[int, tuple[float, float, float]] = {}
        for body_index, head_index, _ in pairs:
            correction_by_head[head_index] = _add(body_delta.get(body_index, (0.0, 0.0, 0.0)), old_delta[head_index], -1.0)

        new_delta = list(old_delta)
        for vertex in range(face.vertex_count):
            if vertex in ring_heads:
                body_index = next(body for body, head, _ in pairs if head == vertex)
                new_delta[vertex] = body_delta.get(body_index, (0.0, 0.0, 0.0))
                continue
            nearby = [(source, distances[source][vertex]) for source in ring_heads if vertex in distances[source]]
            if not nearby:
                continue
            weights = [_smoothstep((band_loops - dist) / band_loops) for _, dist in nearby]
            total = sum(weights)
            if total <= 0.0:
                continue
            correction = tuple(
                sum(correction_by_head[source][axis] * weight for (source, _), weight in zip(nearby, weights)) / total
                for axis in range(3)
            )
            new_delta[vertex] = _add(old_delta[vertex], correction, 1.0 if total else 0.0)

        multiplier, _ = face.morphs[face_name]
        maximum_component = max(abs(value) for point in new_delta for value in point)
        new_multiplier = max(multiplier, maximum_component / 32760.0)
        struct.pack_into("<f", out, face.multiplier_offsets[face_name], new_multiplier)
        raw_values: list[int] = []
        for point in new_delta:
            for value in point:
                raw = int(round(value / new_multiplier))
                if not -32768 <= raw <= 32767:
                    raise ValueError(f"{face_name} exceeds int16 TRI range")
                raw_values.append(raw)
        struct.pack_into(f"<{len(raw_values)}h", out, block_offset, *raw_values)

        after = FaceTri(bytes(out)).deltas(face_name)
        residuals = []
        for body_index, head_index, _ in pairs:
            target = body_delta.get(body_index, (0.0, 0.0, 0.0))
            residuals.append(_distance(after[head_index], target))
        print(f"  {face_name}: multiplier {multiplier:.9g} -> {new_multiplier:.9g}; "
              f"ring residual max={max(residuals):.6f}, mean={sum(residuals) / len(residuals):.6f}")

    if not apply:
        print("  dry-run: no file written")
        return

    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    backup = path.with_name(path.name + f".pre-neckmatch-{stamp}.bak")
    shutil.copy2(path, backup)
    path.write_bytes(bytes(out))
    print(f"  written: {path}")
    print(f"  backup:  {backup}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("face_tri", type=Path)
    parser.add_argument("--body-nifd", required=True, type=Path)
    parser.add_argument("--body-tri", required=True, type=Path)
    parser.add_argument("--body-shape", default="HIMBO - Body HP")
    parser.add_argument("--band-loops", type=int, default=4)
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    correct(args.face_tri, args.body_nifd, args.body_tri, args.body_shape, args.band_loops, args.apply)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
