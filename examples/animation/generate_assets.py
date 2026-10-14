"""Rebuild the original demonstration assets; only Python's standard library."""
from pathlib import Path
import math
import struct

ROOT = Path(__file__).parent / "assets"
vertices, indices, bindings = [], [], []


def box(center, half, color, joint=0):
    for axis in range(3):
        for sign in (-1, 1):
            other = [(axis + 1) % 3, (axis + 2) % 3]
            first = len(vertices)
            corners = [(-1, -1), (1, -1), (1, 1), (-1, 1)]
            if sign < 0:
                corners.reverse()
            for a, b in corners:
                p = list(center)
                p[axis] += sign * half[axis]
                p[other[0]] += a * half[other[0]]
                p[other[1]] += b * half[other[1]]
                normal = [0.0, 0.0, 0.0]
                normal[axis] = float(sign)
                vertices.append((*p, *normal, (a + 1) / 2, (b + 1) / 2, color))
                bindings.append(((joint, 0, 0, 0), (1.0, 0.0, 0.0, 0.0)))
            indices.extend(first + i for i in (0, 1, 2, 0, 2, 3))


def arm():
    # A continuous sleeve: neighbouring rings share blended elbow influences.
    xs = [-0.25, -0.55, -0.65, -0.75, -1.05]
    for face in range(4):
        first = len(vertices)
        for ring, x in enumerate(xs):
            for edge in range(2):
                angle = (face + edge) * math.pi / 2 + math.pi / 4
                y, z = 0.09 * math.cos(angle), 1.3 + 0.09 * math.sin(angle)
                normal_angle = (face + 0.5) * math.pi / 2 + math.pi / 4
                vertices.append((x, y, z, 0.0, math.cos(normal_angle),
                                 math.sin(normal_angle), ring / 4, edge, 0xFFFFB94C))
                weight = max(0.0, min(1.0, (-x - 0.55) / 0.20))
                bindings.append(((1, 2, 0, 0), (1 - weight, weight, 0.0, 0.0)))
        for ring in range(4):
            start = first + ring * 2
            indices.extend(start + i for i in (0, 2, 3, 0, 3, 1))


box((0, 0, 1.12), (0.23, 0.13, 0.30), 0xFFFFB94C)
box((0, 0, 1.62), (0.14, 0.13, 0.18), 0xFFEBDDD2)
box((0, -0.136, 1.64), (0.105, 0.015, 0.047), 0xFFFFCC55)
for x in (-0.12, 0.12):
    box((x, 0, 0.48), (0.08, 0.09, 0.36), 0xFFB07036)
    box((x, -0.04, 0.07), (0.095, 0.16, 0.07), 0xFF52332C)
box((0.40, 0, 1.15), (0.08, 0.09, 0.28), 0xFFFFB94C)
arm()

# Split a body triangle without changing its shape. The odd vertex count also
# exercises the demo's aligned workspace layout, independent of model topology.
a, b, c = indices[:3]
middle = len(vertices)
vertices.append(tuple(sum(vertices[i][component] for i in (a, b, c)) / 3
                      for component in range(8)) + (vertices[a][8],))
bindings.append(bindings[a])
indices[:3] = (a, b, middle, b, c, middle, c, a, middle)

sections = [
    (b"VERT", b"".join(struct.pack("<8fI", *v) for v in vertices), len(vertices)),
    (b"INDX", struct.pack(f"<{len(indices)}I", *indices), len(indices)),
    (b"PART", struct.pack("<4I", 0, len(indices), 0, 7), 1),
    (b"STRS", b"checker", 0),
    (b"BNDS", struct.pack("<6f", *(min(v[a] for v in vertices) for a in range(3)),
                                      *(max(v[a] for v in vertices) for a in range(3))), 1),
    (b"SKIN", struct.pack("<2I", 3, len(vertices)) + b"".join(
        struct.pack("<4I4f", *j, *w) for j, w in bindings), len(vertices)),
]
offset = 40 + len(sections) * 16
directory, payload = bytearray(), bytearray()
for tag, data, count in sections:
    padding = (-offset) % 4
    payload.extend(b"\0" * padding)
    offset += padding
    directory.extend(tag + struct.pack("<3I", offset, len(data), count))
    payload.extend(data)
    offset += len(data)
header = struct.pack("<4sHHIIQIIII", b"LMD1", 1, 40, len(sections), offset, 0, 0, 7, 0, 0)
model_path = ROOT / "models" / "Demo.lop" / "robot.lo"
model_path.parent.mkdir(parents=True, exist_ok=True)
model_path.write_bytes(header + directory + payload)

channels, keys = bytearray(), bytearray()
for target, angles in ((1, (0.0, 0.7, 1.2, 0.4, 0.0)),
                       (2, (0.0, -0.3, -1.4, -0.5, 0.0))):
    first_key = len(keys) // 24
    channels.extend(struct.pack("<5I", 2, target, 1, first_key, len(angles)))
    for index, angle in enumerate(angles):
        keys.extend(struct.pack("<5fI", index * 0.5, 0.0,
                                math.sin(angle / 2), 0.0, math.cos(angle / 2), 0))
joints = bytearray()
for parent, translation, world in ((0xFFFFFFFF, (0, 0, 0), (0, 0, 0)),
                                  (0, (-0.25, 0, 1.3), (-0.25, 0, 1.3)),
                                  (1, (-0.40, 0, 0), (-0.65, 0, 1.3))):
    inverse_bind = (1, 0, 0, -world[0], 0, 1, 0, -world[1], 0, 0, 1, -world[2])
    joints.extend(struct.pack("<I22f", parent, *translation, 0, 0, 0, 1, 1, 1, 1,
                              *inverse_bind))
key_offset = 48 + len(channels)
joint_offset = key_offset + len(keys)
animation = struct.pack("<4s6If4I", b"LAK1", 1, 48, joint_offset + len(joints),
                        2, len(keys) // 24, 3, 2.0, 48, key_offset, joint_offset, 0)
clip_path = ROOT / "animations" / "Demo.lkp" / "wave.lk"
clip_path.parent.mkdir(parents=True, exist_ok=True)
clip_path.write_bytes(animation + channels + keys + joints)
for category, extension in (("models", "lop"), ("animations", "lkp")):
    (ROOT / category / "active.txt").write_text(f"Demo.{extension}\n", encoding="utf-8")
print(f"{len(vertices)} vertices, {len(indices) // 3} triangles, 3 joints")
