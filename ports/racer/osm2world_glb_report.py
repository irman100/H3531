#!/usr/bin/env python3
"""
Summarize an OSM2World GLB for the Stayplaytion map pipeline.

The report is intentionally renderer-agnostic. It counts nodes, meshes,
primitives, materials, vertices and triangles and records the POSITION bounds.
That lets CI reject accidentally huge maps before we try to convert them into
Hi3531 sector data.
"""
from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path


def load_glb(path: Path):
    data=path.read_bytes()
    if len(data)<20 or data[:4]!=b"glTF":
        raise SystemExit("not a GLB file")
    magic,version,total=struct.unpack_from("<III",data,0)
    if version!=2 or total>len(data):
        raise SystemExit("unsupported GLB")
    off=12
    doc=None
    while off+8<=total:
        length,typ=struct.unpack_from("<II",data,off)
        off+=8
        chunk=data[off:off+length]
        off+=length
        if typ==0x4E4F534A:
            doc=json.loads(chunk.rstrip(b"\x00 \t\r\n").decode("utf-8"))
            break
    if doc is None:
        raise SystemExit("GLB JSON chunk missing")
    return doc,len(data)


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("input_glb")
    ap.add_argument("output_json")
    ap.add_argument("--map-name",default="map")
    args=ap.parse_args()

    path=Path(args.input_glb)
    g,size=load_glb(path)
    accessors=g.get("accessors",[])
    meshes=g.get("meshes",[])
    nodes=g.get("nodes",[])
    materials=g.get("materials",[])
    images=g.get("images",[])
    textures=g.get("textures",[])

    vertices=0
    triangles=0
    primitives=0
    pmins=[]
    pmaxs=[]

    for mesh in meshes:
        for prim in mesh.get("primitives",[]):
            if prim.get("mode",4)!=4:
                continue
            primitives+=1
            attrs=prim.get("attributes",{})
            pos_idx=attrs.get("POSITION")
            if pos_idx is not None and 0<=pos_idx<len(accessors):
                a=accessors[pos_idx]
                vertices+=int(a.get("count",0))
                if "min" in a and "max" in a and len(a["min"])>=3 and len(a["max"])>=3:
                    pmins.append(tuple(float(x) for x in a["min"][:3]))
                    pmaxs.append(tuple(float(x) for x in a["max"][:3]))
            ind_idx=prim.get("indices")
            if ind_idx is not None and 0<=ind_idx<len(accessors):
                triangles+=int(accessors[ind_idx].get("count",0))//3
            elif pos_idx is not None and 0<=pos_idx<len(accessors):
                triangles+=int(accessors[pos_idx].get("count",0))//3

    bounds=None
    if pmins and pmaxs:
        lo=[min(p[i] for p in pmins) for i in range(3)]
        hi=[max(p[i] for p in pmaxs) for i in range(3)]
        bounds={"min":lo,"max":hi,"size":[hi[i]-lo[i] for i in range(3)]}

    report={
        "map_name":args.map_name,
        "file":path.name,
        "bytes":size,
        "nodes":len(nodes),
        "meshes":len(meshes),
        "primitives":primitives,
        "materials":len(materials),
        "textures":len(textures),
        "images":len(images),
        "vertices":vertices,
        "triangles":triangles,
        "bounds":bounds,
        "asset_generator":g.get("asset",{}).get("generator"),
        "scene_count":len(g.get("scenes",[])),
    }
    Path(args.output_json).write_text(json.dumps(report,indent=2),encoding="utf-8")
    print("OSM2WORLD_GLB_REPORT_OK",json.dumps(report,separators=(",",":")))


if __name__=="__main__":
    main()
