#!/usr/bin/env python3
"""
Robust flat OBJ -> small3dlib C header converter.
Project code: OpenAI/Stayplaytion integration.
Input assets may be CC0; this converter ignores textures/materials and keeps
geometry only. Polygons are fan-triangulated.
"""
from __future__ import annotations
import argparse
from pathlib import Path

def parse_face_index(token: str, count: int) -> int:
    head = token.split("/", 1)[0]
    idx = int(head)
    if idx < 0:
        idx = count + idx
    else:
        idx -= 1
    if not 0 <= idx < count:
        raise ValueError(f"OBJ vertex index out of range: {token}")
    return idx

def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--name", default="cc0Ship")
    ap.add_argument("--scale", type=int, default=900)
    args = ap.parse_args()

    verts: list[tuple[float,float,float]] = []
    tris: list[tuple[int,int,int]] = []

    for raw in Path(args.input).read_text(encoding="utf-8", errors="ignore").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("v "):
            p = line.split()
            if len(p) >= 4:
                # small3dlib looks down +Z; flip OBJ Z so common exported ships
                # keep a conventional orientation that we can rotate in engine.
                verts.append((float(p[1]), float(p[2]), -float(p[3])))
        elif line.startswith("f "):
            parts = line.split()[1:]
            if len(parts) < 3:
                continue
            ids = [parse_face_index(t, len(verts)) for t in parts]
            for i in range(1, len(ids)-1):
                tris.append((ids[0], ids[i], ids[i+1]))

    if len(verts) < 4 or len(tris) < 2:
        raise SystemExit("OBJ does not contain enough geometry")

    mins = [min(v[i] for v in verts) for i in range(3)]
    maxs = [max(v[i] for v in verts) for i in range(3)]
    center = [(mins[i]+maxs[i])*0.5 for i in range(3)]
    extent = max(maxs[i]-mins[i] for i in range(3))
    if extent <= 1e-9:
        raise SystemExit("degenerate OBJ extent")

    scaled=[]
    for v in verts:
        scaled.append(tuple(int(round((v[i]-center[i]) * args.scale / extent)) for i in range(3)))

    guard=(args.name+"_MODEL_H").upper()
    out=[]
    out += [f"#ifndef {guard}", f"#define {guard}", ""]
    out += ["/* Generated from a CC0 OBJ at build time. */"]
    out += [f"#define {args.name.upper()}_VERTEX_COUNT {len(scaled)}"]
    out += [f"static const S3L_Unit {args.name}Vertices[{len(scaled)*3}] = {{"]
    for x,y,z in scaled:
        out.append(f"  {x}, {y}, {z},")
    out += ["};", ""]
    out += [f"#define {args.name.upper()}_TRIANGLE_COUNT {len(tris)}"]
    out += [f"static const S3L_Index {args.name}TriangleIndices[{len(tris)*3}] = {{"]
    for a,b,c in tris:
        out.append(f"  {a}, {b}, {c},")
    out += ["};", ""]
    out += [f"static S3L_Model3D {args.name}Model;", ""]
    out += [f"static void {args.name}ModelInit(void)", "{",
            f"  S3L_model3DInit({args.name}Vertices, {args.name.upper()}_VERTEX_COUNT,",
            f"      {args.name}TriangleIndices, {args.name.upper()}_TRIANGLE_COUNT, &{args.name}Model);",
            "}", "", f"#endif /* {guard} */", ""]

    Path(args.output).write_text("\n".join(out), encoding="utf-8")
    print(f"OBJ2S3L_OK vertices={len(scaled)} triangles={len(tris)} input={args.input}")

if __name__ == "__main__":
    main()
