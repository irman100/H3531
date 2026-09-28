#!/usr/bin/env python3
"""
Pack an OSM2World GLB into a compact, sectorized C header for Stayplaytion.

Design goals for Hi3531:
- preserve the real OSM road geometry;
- replace expensive authored building meshes with simple volumes using their
  real OSM2World footprint bounds and height;
- bucket geometry into square sectors for cheap runtime visibility;
- emit only v3f_t / tri3d_t compatible arrays, no runtime GLB parser.

The source GLB remains the authoritative offline map. This packer is a lossy
console-target representation, intentionally similar to old fixed-hardware
level build pipelines.
"""
from __future__ import annotations

import argparse
import json
import math
import struct
from collections import defaultdict
from pathlib import Path

COMP_FMT={5120:"b",5121:"B",5122:"h",5123:"H",5125:"I",5126:"f"}
COMP_SIZE={k:struct.calcsize("<"+v) for k,v in COMP_FMT.items()}
NCOMP={"SCALAR":1,"VEC2":2,"VEC3":3,"VEC4":4}


def load_glb(path: Path):
    data=path.read_bytes()
    if len(data)<20 or data[:4]!=b"glTF":
        raise SystemExit("not a GLB")
    _,version,total=struct.unpack_from("<III",data,0)
    if version!=2:
        raise SystemExit("only GLB v2 supported")
    off=12
    doc=None
    binary=None
    while off+8<=min(total,len(data)):
        ln,typ=struct.unpack_from("<II",data,off)
        off+=8
        chunk=data[off:off+ln]
        off+=ln
        if typ==0x4E4F534A:
            doc=json.loads(chunk.rstrip(b"\x00 \t\r\n").decode("utf-8"))
        elif typ==0x004E4942:
            binary=chunk
    if doc is None or binary is None:
        raise SystemExit("GLB missing JSON/BIN chunk")
    return doc,binary


def read_accessor(doc,binary,index):
    a=doc["accessors"][index]
    bv=doc["bufferViews"][a["bufferView"]]
    ct=a["componentType"]
    fmt=COMP_FMT[ct]
    ncomp=NCOMP[a["type"]]
    item_size=COMP_SIZE[ct]*ncomp
    stride=bv.get("byteStride",item_size)
    base=bv.get("byteOffset",0)+a.get("byteOffset",0)
    unpack="<"+fmt*ncomp
    out=[]
    for i in range(a["count"]):
        out.append(struct.unpack_from(unpack,binary,base+i*stride))
    return out


def sector_key(x,z,size):
    return (math.floor(x/size),math.floor(z/size))


def pack1555(r,g,b):
    r=max(0,min(255,int(r)))
    g=max(0,min(255,int(g)))
    b=max(0,min(255,int(b)))
    return 0x8000|((r>>3)<<10)|((g>>3)<<5)|(b>>3)


PALETTE=[
    pack1555(54,58,62),     # 0 asphalt dark
    pack1555(126,128,128),  # 1 light road / pavement
    pack1555(76,80,84),     # 2 medium road
    pack1555(100,102,102),  # 3 alternate road
    pack1555(191,181,164),  # 4 cream wall
    pack1555(158,170,176),  # 5 cool wall
    pack1555(187,162,135),  # 6 sand wall
    pack1555(174,145,137),  # 7 muted rose wall
    pack1555(105,92,80),    # 8 roof brown
    pack1555(79,84,91),     # 9 roof slate
    pack1555(151,151,145),  # 10 plaza/concrete
    pack1555(63,115,65),    # 11 greenery
]


def building_colors(name):
    h=2166136261
    for ch in name.encode("utf-8",errors="ignore"):
        h=(h^ch)*16777619 & 0xffffffff
    wall=4+(h%4)
    roof=8+((h>>3)&1)
    return wall,roof


def add_box(bucket,cx,miny,cz,w,h,d,wall,roof,roof_cap=True):
    hx=w*0.5
    hz=d*0.5
    base=[
        (cx-hx,miny,cz-hz),(cx+hx,miny,cz-hz),
        (cx+hx,miny,cz+hz),(cx-hx,miny,cz+hz),
        (cx-hx,miny+h,cz-hz),(cx+hx,miny+h,cz-hz),
        (cx+hx,miny+h,cz+hz),(cx-hx,miny+h,cz+hz),
    ]
    faces=[
        (0,1,5),(0,5,4),(1,2,6),(1,6,5),
        (2,3,7),(2,7,6),(3,0,4),(3,4,7),
        (4,5,6),(4,6,7),(0,3,2),(0,2,1),
    ]
    vbase=len(bucket["verts"])
    bucket["verts"].extend(base)
    for i,(a,b,c) in enumerate(faces):
        mat=roof if i in (8,9) else wall
        bucket["tris"].append((vbase+a,vbase+b,vbase+c,mat))

    if roof_cap and h>4.0:
        # Cheap setback roof volume to make silhouettes less box-like.
        rh=max(0.8,min(2.3,h*0.10))
        rw=w*0.82
        rd=d*0.82
        add_box(bucket,cx,miny+h,cz,rw,rh,rd,roof,roof,False)


def emit_header(out_path, sectors, scale, sector_m, spawn_x, spawn_z, spawn_yaw, clip_m):
    # Rebase each sector's triangle indices so queue_world_static_mesh can use
    # pointer slices directly.
    all_v=[]
    all_t=[]
    meta=[]
    for (sx,sz) in sorted(sectors):
        b=sectors[(sx,sz)]
        if not b["tris"]:
            continue

        # compact only vertices actually referenced
        used={}
        compact=[]
        tris=[]
        for a,bv,c,m in b["tris"]:
            idxs=[]
            for old in (a,bv,c):
                if old not in used:
                    used[old]=len(compact)
                    compact.append(b["verts"][old])
                idxs.append(used[old])
            tris.append((idxs[0],idxs[1],idxs[2],m))

        vbase=len(all_v)
        tbase=len(all_t)
        all_v.extend(compact)
        all_t.extend(tris)
        meta.append((sx,sz,vbase,len(compact),tbase,len(tris)))

    if len(all_v)>=65535:
        raise SystemExit(f"too many packed vertices: {len(all_v)}")
    if any(vcount>4096 or tcount>8192 for _,_,_,vcount,_,tcount in meta):
        bad=[m for m in meta if m[3]>4096 or m[5]>8192]
        raise SystemExit(f"sector exceeds runtime scratch limits: {bad[:3]}")

    lines=[]
    lines.append("/* Auto-generated by osm2world_sector_pack.py. */")
    lines.append("#ifndef OSM_CITY_MAP_H")
    lines.append("#define OSM_CITY_MAP_H")
    lines.append("")
    lines.append(f"#define OSM_CITY_WORLD_SCALE {scale:.6f}f")
    lines.append(f"#define OSM_CITY_SECTOR_METERS {sector_m:.6f}f")
    lines.append(f"#define OSM_CITY_SECTOR_WORLD {(sector_m*scale):.6f}f")
    lines.append(f"#define OSM_CITY_CLIP_METERS {clip_m:.6f}f")
    lines.append(f"#define OSM_CITY_SPAWN_X {(spawn_x*scale):.6f}f")
    lines.append("#define OSM_CITY_SPAWN_Y 0.0f")
    lines.append(f"#define OSM_CITY_SPAWN_Z {(spawn_z*scale):.6f}f")
    lines.append(f"#define OSM_CITY_SPAWN_YAW {spawn_yaw:.9f}f")
    lines.append(f"#define OSM_CITY_VERTEX_COUNT {len(all_v)}")
    lines.append(f"#define OSM_CITY_TRIANGLE_COUNT {len(all_t)}")
    lines.append(f"#define OSM_CITY_SECTOR_COUNT {len(meta)}")
    lines.append(f"#define OSM_CITY_MATERIAL_COUNT {len(PALETTE)}")
    lines.append("")
    lines.append("typedef struct {")
    lines.append("    int16_t sx,sz;")
    lines.append("    uint16_t vertex_base,vertex_count;")
    lines.append("    uint32_t tri_base,tri_count;")
    lines.append("} osm_city_sector_t;")
    lines.append("")

    lines.append("static const uint16_t osm_city_mat[OSM_CITY_MATERIAL_COUNT]={")
    for i,c in enumerate(PALETTE):
        lines.append(f"    0x{c:04x}{',' if i+1<len(PALETTE) else ''}")
    lines.append("};")
    lines.append("")

    lines.append("static const v3f_t osm_city_v[OSM_CITY_VERTEX_COUNT]={")
    for x,y,z in all_v:
        lines.append(f"    {{{x*scale:.4f}f,{y*scale:.4f}f,{z*scale:.4f}f}},")
    lines.append("};")
    lines.append("")

    lines.append("static const tri3d_t osm_city_t[OSM_CITY_TRIANGLE_COUNT]={")
    for a,b,c,m in all_t:
        lines.append(f"    {{{a},{b},{c},{m}}},")
    lines.append("};")
    lines.append("")

    lines.append("static const osm_city_sector_t osm_city_sector[OSM_CITY_SECTOR_COUNT]={")
    for sx,sz,vb,vc,tb,tc in meta:
        lines.append(f"    {{{sx},{sz},{vb},{vc},{tb}u,{tc}u}},")
    lines.append("};")
    lines.append("")
    lines.append("#endif")
    out_path.write_text("\n".join(lines)+"\n",encoding="utf-8")
    return len(all_v),len(all_t),len(meta)


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("input_glb")
    ap.add_argument("output_header")
    ap.add_argument("--scale",type=float,default=240.0)
    ap.add_argument("--sector-m",type=float,default=48.0)
    ap.add_argument("--clip-m",type=float,default=270.0)
    ap.add_argument("--spawn-x",type=float,default=-19.535)
    ap.add_argument("--spawn-z",type=float,default=42.699)
    ap.add_argument("--spawn-yaw",type=float,default=2.369313148)
    args=ap.parse_args()

    doc,binary=load_glb(Path(args.input_glb))
    sectors=defaultdict(lambda:{"verts":[],"tris":[]})
    road_source_tri=0
    road_packed_tri=0
    buildings=0

    for node in doc.get("nodes",[]):
        name=node.get("name","")
        mesh_index=node.get("mesh")
        if mesh_index is None:
            continue
        mesh=doc["meshes"][mesh_index]
        if not mesh.get("primitives"):
            continue
        prim=mesh["primitives"][0]
        if prim.get("mode",4)!=4:
            continue

        pos_idx=prim.get("attributes",{}).get("POSITION")
        ind_idx=prim.get("indices")
        if pos_idx is None or ind_idx is None:
            continue

        acc=doc["accessors"][pos_idx]
        amin=acc.get("min")
        amax=acc.get("max")
        if not amin or not amax:
            continue

        cx=(float(amin[0])+float(amax[0]))*0.5
        cz=(float(amin[2])+float(amax[2]))*0.5

        if name.startswith("Building"):
            if abs(cx)>args.clip_m or abs(cz)>args.clip_m:
                continue
            miny=max(0.0,float(amin[1]))
            maxy=max(miny+3.0,float(amax[1]))
            w=max(3.0,float(amax[0])-float(amin[0]))
            d=max(3.0,float(amax[2])-float(amin[2]))
            h=max(3.0,maxy-miny)
            key=sector_key(cx,cz,args.sector_m)
            wall,roof=building_colors(name)
            add_box(sectors[key],cx,miny,cz,w,h,d,wall,roof)
            buildings+=1
            continue

        if not name.startswith("Road"):
            continue

        verts=read_accessor(doc,binary,pos_idx)
        inds=read_accessor(doc,binary,ind_idx)
        flat=[int(v[0]) for v in inds]
        road_source_tri+=len(flat)//3
        mat=int(prim.get("material",0))
        # Preserve the original rough luminance grouping without depending on
        # OSM2World material names/configuration.
        road_mat={0:0,1:1,2:2,3:3}.get(mat,0)

        for i in range(0,len(flat)-2,3):
            a,b,c=flat[i],flat[i+1],flat[i+2]
            va=verts[a];vb=verts[b];vc=verts[c]
            tx=(va[0]+vb[0]+vc[0])/3.0
            tz=(va[2]+vb[2]+vc[2])/3.0
            if abs(tx)>args.clip_m or abs(tz)>args.clip_m:
                continue
            key=sector_key(tx,tz,args.sector_m)
            bucket=sectors[key]
            base=len(bucket["verts"])
            bucket["verts"].extend([
                (float(va[0]),float(va[1])+0.015,float(va[2])),
                (float(vb[0]),float(vb[1])+0.015,float(vb[2])),
                (float(vc[0]),float(vc[1])+0.015,float(vc[2])),
            ])
            bucket["tris"].append((base,base+1,base+2,road_mat))
            road_packed_tri+=1

    vc,tc,sc=emit_header(
        Path(args.output_header),sectors,args.scale,args.sector_m,
        args.spawn_x,args.spawn_z,args.spawn_yaw,args.clip_m
    )
    print(
        "OSM2WORLD_SECTOR_PACK_OK",
        f"buildings={buildings}",
        f"road_source_tri={road_source_tri}",
        f"road_packed_tri={road_packed_tri}",
        f"vertices={vc}",
        f"triangles={tc}",
        f"sectors={sc}",
        f"spawn=({args.spawn_x:.3f},{args.spawn_z:.3f},{args.spawn_yaw:.6f})"
    )


if __name__=="__main__":
    main()
