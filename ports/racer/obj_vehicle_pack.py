#!/usr/bin/env python3
"""
OBJ sports-car -> compact C geometry for Stayplaytion Racer.

Source model:
  Red Car by J-Toastie, CC BY 3.0, distributed via Poly Pizza.
A pinned copy is fetched from mandaw2014/Rally for reproducible CI.

The OBJ has disconnected components even though it has no useful object/group
names. We detect connected components, identify the four large cylindrical
wheel components geometrically, merge every other component into the body,
preserve UVs, and bake a PNG texture to RGB1555.

Runtime receives no OBJ/PNG parser.
"""
from __future__ import annotations
import argparse, math
from collections import defaultdict, deque
from pathlib import Path
from PIL import Image

def parse_obj(path):
    pos=[]; uv=[]; faces=[]
    for raw in Path(path).read_text(encoding="utf-8",errors="replace").splitlines():
        line=raw.strip()
        if not line or line.startswith("#"): continue
        if line.startswith("v "):
            p=line.split()
            pos.append((float(p[1]),float(p[2]),float(p[3])))
        elif line.startswith("vt "):
            p=line.split()
            uv.append((float(p[1]),float(p[2])))
        elif line.startswith("f "):
            corners=[]
            for tok in line.split()[1:]:
                a=tok.split("/")
                vi=int(a[0]); ti=int(a[1]) if len(a)>1 and a[1] else 0
                if vi<0: vi=len(pos)+vi+1
                if ti<0: ti=len(uv)+ti+1
                corners.append((vi-1,ti-1 if ti else -1))
            for i in range(1,len(corners)-1):
                faces.append((corners[0],corners[i],corners[i+1]))
    if not pos or not faces:
        raise SystemExit("OBJ has no usable geometry")
    return pos,uv,faces

def connected_components(pos,faces):
    by_v=defaultdict(list)
    for fi,f in enumerate(faces):
        for vi,_ in f: by_v[vi].append(fi)
    seen=[False]*len(faces)
    out=[]
    for root in range(len(faces)):
        if seen[root]: continue
        q=[root]; seen[root]=True; fs=[]; vs=set()
        while q:
            fi=q.pop(); fs.append(fi)
            for vi,_ in faces[fi]:
                vs.add(vi)
                for nf in by_v[vi]:
                    if not seen[nf]:
                        seen[nf]=True;q.append(nf)
        pts=[pos[i] for i in vs]
        mn=[min(p[k] for p in pts) for k in range(3)]
        mx=[max(p[k] for p in pts) for k in range(3)]
        cen=[sum(p[k] for p in pts)/len(pts) for k in range(3)]
        out.append({
            "faces":fs,"verts":vs,"centroid":cen,
            "min":mn,"max":mx,
            "extent":[mx[k]-mn[k] for k in range(3)]
        })
    return out

def deindex_part(pos,uv,faces,face_ids,center,scale):
    mapping={}; verts=[]; tex=[]; tris=[]
    for fi in face_ids:
        tri=[]
        for vi,ti in faces[fi]:
            key=(vi,ti)
            if key not in mapping:
                mapping[key]=len(verts)
                x,y,z=pos[vi]
                verts.append(((x-center[0])*scale,
                              (y-center[1])*scale,
                              (z-center[2])*scale))
                if ti>=0 and ti<len(uv):
                    u,v=uv[ti]
                    tex.append((u,1.0-v)) # OBJ UV origin -> image top-left
                else:
                    tex.append((0.0,0.0))
            tri.append(mapping[key])
        tris.append(tuple(tri))
    return verts,tex,tris

def pack_texture(path,size):
    resampling=getattr(getattr(Image,"Resampling",Image),"LANCZOS")
    src=Image.open(path).convert("RGBA")
    dst=src.resize((size,size),resampling)
    tex=[]
    for r,g,b,a in dst.getdata():
        alpha=0x8000 if a>=128 else 0
        tex.append(alpha|((r>>3)<<10)|((g>>3)<<5)|(b>>3))
    return src.size,dst.size,tex

def emit_part(out,name,verts,uvs,tris):
    macro=name.upper()
    out.write(f"#define {macro}_VERTEX_COUNT {len(verts)}\n")
    out.write(f"#define {macro}_TRIANGLE_COUNT {len(tris)}\n")
    out.write(f"static const v3f_t {name}_v[{len(verts)}]={{\n")
    for x,y,z in verts: out.write(f" {{{x:.3f}f,{y:.3f}f,{z:.3f}f}},\n")
    out.write("};\n")
    out.write(f"static const v2f_t {name}_uv[{len(uvs)}]={{\n")
    for u,v in uvs: out.write(f" {{{u:.7f}f,{v:.7f}f}},\n")
    out.write("};\n")
    out.write(f"static const tri3d_t {name}_t[{len(tris)}]={{\n")
    for a,b,c in tris: out.write(f" {{{a},{b},{c},0}},\n")
    out.write("};\n\n")

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("obj")
    ap.add_argument("texture")
    ap.add_argument("output")
    ap.add_argument("--target-size",type=float,default=1050.0)
    ap.add_argument("--texture-size",type=int,default=512)
    args=ap.parse_args()

    pos,uv,faces=parse_obj(args.obj)
    comps=connected_components(pos,faces)
    comps.sort(key=lambda c:len(c["faces"]),reverse=True)

    print("OBJ_COUNTS positions",len(pos),"uv",len(uv),"triangles",len(faces),"components",len(comps))
    for i,c in enumerate(comps[:12]):
        print("OBJ_COMPONENT",i,"faces",len(c["faces"]),"verts",len(c["verts"]),
              "centroid",tuple(round(x,5) for x in c["centroid"]),
              "extent",tuple(round(x,5) for x in c["extent"]))

    # This model has one large body component, then four ~cylindrical wheels.
    candidates=[]
    for i,c in enumerate(comps[1:],start=1):
        ex,ey,ez=c["extent"]
        circular=max(ey,ez)/max(1e-9,min(ey,ez))
        if ex < max(ey,ez)*0.65 and circular < 1.25 and len(c["faces"])>=100:
            candidates.append(i)
    if len(candidates)<4:
        raise SystemExit("could not identify four wheel components")
    wheel_ids=candidates[:4]

    wheels=[comps[i] for i in wheel_ids]
    # Sort by z, then x. Larger z is front for this model.
    front=sorted(wheels,key=lambda c:c["centroid"][2],reverse=True)[:2]
    rear=sorted(wheels,key=lambda c:c["centroid"][2])[:2]
    fl=min(front,key=lambda c:c["centroid"][0])
    fr=max(front,key=lambda c:c["centroid"][0])
    rl=min(rear,key=lambda c:c["centroid"][0])
    rr=max(rear,key=lambda c:c["centroid"][0])
    named={"wheel_fl":fl,"wheel_fr":fr,"wheel_rl":rl,"wheel_rr":rr}

    wheel_set={id(c) for c in wheels}
    body_face_ids=[]
    for c in comps:
        if id(c) not in wheel_set: body_face_ids.extend(c["faces"])

    pivots={k:tuple(c["centroid"]) for k,c in named.items()}
    origin=tuple(sum(p[i] for p in pivots.values())/4.0 for i in range(3))

    allpts=pos
    extent=max(max(p[0] for p in allpts)-min(p[0] for p in allpts),
               max(p[2] for p in allpts)-min(p[2] for p in allpts))
    scale=args.target_size/extent

    body=deindex_part(pos,uv,faces,body_face_ids,origin,scale)
    wheel_parts={}
    pivot_out={}
    for k,c in named.items():
        p=pivots[k]
        wheel_parts[k]=deindex_part(pos,uv,faces,c["faces"],p,scale)
        pivot_out[k]=tuple((p[i]-origin[i])*scale for i in range(3))

    front_z=(pivot_out["wheel_fl"][2]+pivot_out["wheel_fr"][2])*0.5
    rear_z=(pivot_out["wheel_rl"][2]+pivot_out["wheel_rr"][2])*0.5
    left_x=(pivot_out["wheel_fl"][0]+pivot_out["wheel_rl"][0])*0.5
    right_x=(pivot_out["wheel_fr"][0]+pivot_out["wheel_rr"][0])*0.5
    wheelbase=abs(front_z-rear_z)
    track=abs(right_x-left_x)

    radii=[]
    for k,(vv,_,_) in wheel_parts.items():
        radii.append(max(math.sqrt(y*y+z*z) for _,y,z in vv))
    radius=sum(radii)/len(radii)

    print("OBJ_PIVOTS",pivot_out)
    print("OBJ_DIMENSIONS wheelbase",wheelbase,"track",track,"radius",radius,
          "front_z",front_z,"rear_z",rear_z)
    if front_z<=rear_z:
        raise SystemExit("unexpected sports-car orientation")

    srcsz,dstsz,texels=pack_texture(args.texture,args.texture_size)
    print("OBJ_TEXTURE source",srcsz,"packed",dstsz,"texels",len(texels))

    with Path(args.output).open("w",encoding="utf-8") as out:
        out.write("#ifndef STAYPLAYTION_SPORTS_VEHICLE_H\n#define STAYPLAYTION_SPORTS_VEHICLE_H\n\n")
        out.write("/* Generated from Red Car by J-Toastie (CC BY 3.0). */\n")
        out.write(f"#define SPORTS_VEHICLE_WHEELBASE {wheelbase:.3f}f\n")
        out.write(f"#define SPORTS_VEHICLE_TRACK {track:.3f}f\n")
        out.write(f"#define SPORTS_VEHICLE_WHEEL_RADIUS {radius:.3f}f\n")
        out.write(f"#define SPORTS_COLORMAP_W {args.texture_size}\n")
        out.write(f"#define SPORTS_COLORMAP_H {args.texture_size}\n\n")
        out.write(f"static const uint16_t sports_colormap[{len(texels)}]={{\n")
        for i in range(0,len(texels),16):
            out.write(" "+",".join(f"0x{x:04x}" for x in texels[i:i+16])+",\n")
        out.write("};\n\n")
        emit_part(out,"sports_body",*body)
        for k in ("wheel_fl","wheel_fr","wheel_rl","wheel_rr"):
            emit_part(out,"sports_"+k,*wheel_parts[k])
            x,y,z=pivot_out[k]
            out.write(f"static const v3f_t sports_{k}_pivot={{{x:.3f}f,{y:.3f}f,{z:.3f}f}};\n\n")
        out.write("#endif\n")

    print("OBJ_PACK_OK scale",scale,"origin",origin)

if __name__=="__main__":
    main()
