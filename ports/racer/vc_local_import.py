#!/usr/bin/env python3
"""
Local-only GTA Vice City -> Stayplaytion/H3531 city packer.

This tool does NOT contain or download any GTA game data. It reads a user's
own installed copy locally and writes a compact C header for the existing
Stayplaytion Racer renderer.

Input chain:
  data/default.dat + data/gta_vc.dat
    -> IDE object definitions
    -> IPL placements
    -> IMG v1 archives (.dir + .img)
    -> DFF meshes via rwfury (MIT)
    -> world transforms / sectorization
    -> vc_city_map.h

First milestone intentionally ignores original TXD textures. It preserves
actual Vice City geometry/placements and assigns compact material colors so we
can answer the hardware question first: can Hi3531 render a real GTA-era city?

Install dependency locally:
  py -m pip install rwfury
"""
from __future__ import annotations

import argparse
import json
import math
import os
import re
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path


try:
    from rwfury import Img, Dff
except Exception:
    Img = Dff = None


def pack1555(r, g, b):
    r=max(0,min(255,int(r))); g=max(0,min(255,int(g))); b=max(0,min(255,int(b)))
    return 0x8000 | ((r>>3)<<10) | ((g>>3)<<5) | (b>>3)


PALETTE = [
    pack1555(188,180,164), pack1555(146,139,129),
    pack1555(173,183,185), pack1555(126,137,142),
    pack1555(196,163,130), pack1555(150,119,93),
    pack1555(187,150,145), pack1555(138,106,104),
    pack1555(115,98,80),   pack1555(82,87,94),
    pack1555(92,102,112),  pack1555(157,157,150),
    pack1555(54,57,61),    pack1555(74,77,81),
    pack1555(113,116,113), pack1555(78,105,72),
]


@dataclass
class IdeObj:
    ident: int
    model: str
    txd: str
    draw_distance: float
    flags: int


@dataclass
class Instance:
    ident: int
    model: str
    interior: int
    pos: tuple[float,float,float]
    scale: tuple[float,float,float]
    quat: tuple[float,float,float,float]


def norm_rel(text: str) -> str:
    return text.strip().strip('"').replace("\\","/")


def find_case(root: Path, rel: str) -> Path | None:
    """Resolve Windows-authored GTA paths case-insensitively on any OS."""
    rel = norm_rel(rel)
    p = root / rel
    if p.exists():
        return p
    cur = root
    for part in Path(rel).parts:
        try:
            entries = {x.name.lower(): x for x in cur.iterdir()}
        except OSError:
            return None
        hit = entries.get(part.lower())
        if hit is None:
            return None
        cur = hit
    return cur


def parse_dat(path: Path) -> dict[str,list[str]]:
    out=defaultdict(list)
    if not path.exists():
        return out
    for raw in path.read_text(encoding="latin-1",errors="ignore").splitlines():
        line=raw.split("#",1)[0].strip()
        if not line:
            continue
        parts=line.split(None,1)
        if len(parts)!=2:
            continue
        key=parts[0].upper()
        if key in {"IDE","IPL","IMG","CDIMAGE","MODELFILE","TEXDICTION","COLFILE"}:
            out[key].append(norm_rel(parts[1]))
    return out


def parse_sectioned_text(path: Path):
    section=""
    if not path.exists():
        return
    for raw in path.read_text(encoding="latin-1",errors="ignore").splitlines():
        line=raw.split("#",1)[0].strip()
        if not line:
            continue
        low=line.lower()
        if low in {"objs","tobj","anim","cars","peds","path","2dfx","inst","cull","pick","occl","zone","grge","enex","auzo","jump","tcyc"}:
            section=low
            continue
        if low=="end":
            section=""
            continue
        yield section,[x.strip() for x in line.split(",")]


def parse_ide(path: Path) -> dict[int,IdeObj]:
    out={}
    for section,p in parse_sectioned_text(path):
        if section not in {"objs","tobj"} or len(p)<6:
            continue
        try:
            ident=int(p[0]); model=p[1]; txd=p[2]
            mesh_count=int(float(p[3]))
            # VC objects can carry 1..3 draw-distance fields before flags.
            draw_fields=max(1,min(3,mesh_count))
            draw=float(p[4])
            flags=int(float(p[4+draw_fields]))
            out[ident]=IdeObj(ident,model,txd,draw,flags)
        except (ValueError,IndexError):
            continue
    return out


def parse_ipl(path: Path) -> list[Instance]:
    out=[]
    for section,p in parse_sectioned_text(path):
        if section!="inst":
            continue
        # GTA VC standard: id, model, interior, xyz, scale xyz, quat xyzw = 13 fields.
        # GTA III-style 12 field rows are tolerated with interior=0.
        try:
            if len(p)>=13:
                ident=int(p[0]); model=p[1]; interior=int(float(p[2])); k=3
            elif len(p)>=12:
                ident=int(p[0]); model=p[1]; interior=0; k=2
            else:
                continue
            pos=(float(p[k]),float(p[k+1]),float(p[k+2]))
            scale=(float(p[k+3]),float(p[k+4]),float(p[k+5]))
            quat=(float(p[k+6]),float(p[k+7]),float(p[k+8]),float(p[k+9]))
            out.append(Instance(ident,model,interior,pos,scale,quat))
        except (ValueError,IndexError):
            continue
    return out


def qrot(q, p):
    x,y,z,w=q
    # normalize because some map rows are not exactly unit length after text export
    n=math.sqrt(x*x+y*y+z*z+w*w)
    if n>1.0e-12:
        x/=n;y/=n;z/=n;w/=n
    px,py,pz=p
    # quaternion vector rotation: p + 2w(q x p) + 2(q x (q x p))
    cx=y*pz-z*py; cy=z*px-x*pz; cz=x*py-y*px
    c2x=y*cz-z*cy; c2y=z*cx-x*cz; c2z=x*cy-y*cx
    return (
        px + 2.0*(w*cx+c2x),
        py + 2.0*(w*cy+c2y),
        pz + 2.0*(w*cz+c2z),
    )


def apply_mat4_row_major(m, p):
    if not m or len(m)!=16:
        return p
    x,y,z=p
    # GenericMesh documents row-major transform. Accept affine 4x4.
    return (
        x*m[0]+y*m[1]+z*m[2]+m[3],
        x*m[4]+y*m[5]+z*m[6]+m[7],
        x*m[8]+y*m[9]+z*m[10]+m[11],
    )


def positions_iter(pos):
    # rwfury currently exposes mesh.positions as a sequence. Be tolerant of
    # either [(x,y,z), ...] or flat [x,y,z,...] so importer survives API polish.
    if not pos:
        return []
    first=pos[0]
    if isinstance(first,(tuple,list)):
        return [(float(v[0]),float(v[1]),float(v[2])) for v in pos]
    vals=list(pos)
    return [(float(vals[i]),float(vals[i+1]),float(vals[i+2])) for i in range(0,len(vals)-2,3)]


def indices_iter(idx):
    vals=[int(x) for x in idx]
    return [(vals[i],vals[i+1],vals[i+2]) for i in range(0,len(vals)-2,3)]


def diffuse_to_palette(diffuse, model_name, mesh_no):
    if diffuse and len(diffuse)>=3:
        vals=list(diffuse[:3])
        if max(vals)<=1.01:
            vals=[v*255.0 for v in vals]
        # Preserve hue roughly by choosing nearest fixed console palette entry.
        target=tuple(vals)
        candidates=[
            (188,180,164),(146,139,129),(173,183,185),(126,137,142),
            (196,163,130),(150,119,93),(187,150,145),(138,106,104),
            (115,98,80),(82,87,94),(92,102,112),(157,157,150),
        ]
        return min(range(len(candidates)),key=lambda i:sum((candidates[i][j]-target[j])**2 for j in range(3)))
    h=2166136261
    for ch in (model_name+str(mesh_no)).encode("latin-1",errors="ignore"):
        h=((h^ch)*16777619)&0xffffffff
    return h%12


def discover_map(game_root: Path):
    dats=[]
    for rel in ("data/default.dat","data/gta_vc.dat"):
        p=find_case(game_root,rel)
        if p:
            dats.append(p)

    directives=defaultdict(list)
    for p in dats:
        d=parse_dat(p)
        for k,vals in d.items():
            directives[k].extend(vals)

    # VC also has default.ide outside gta_vc.dat in stock installs.
    ide_paths=[]
    p=find_case(game_root,"data/default.ide")
    if p: ide_paths.append(p)
    for rel in directives["IDE"]:
        p=find_case(game_root,rel)
        if p and p not in ide_paths: ide_paths.append(p)

    ipl_paths=[]
    for rel in directives["IPL"]:
        p=find_case(game_root,rel)
        if p: ipl_paths.append(p)

    ide={}
    for p in ide_paths:
        ide.update(parse_ide(p))

    inst=[]
    for p in ipl_paths:
        inst.extend(parse_ipl(p))

    img_paths=[]
    # Stock Vice City always has models/gta3.img even if not explicitly listed.
    p=find_case(game_root,"models/gta3.img")
    if p: img_paths.append(p)
    for rel in directives["IMG"]+directives["CDIMAGE"]:
        p=find_case(game_root,rel)
        if p and p.suffix.lower()==".img" and p not in img_paths: img_paths.append(p)

    return {
        "dat_files":[str(x) for x in dats],
        "ide_files":[str(x) for x in ide_paths],
        "ipl_files":[str(x) for x in ipl_paths],
        "img_files":[str(x) for x in img_paths],
        "ide":ide,
        "instances":inst,
    }


class ArchiveSet:
    def __init__(self, paths):
        if Img is None:
            raise RuntimeError("rwfury is not installed; run: py -m pip install rwfury")
        self.archives=[]
        for p in paths:
            try:
                self.archives.append((p,Img.from_file(str(p))))
            except Exception as exc:
                print(f"[vc-import] WARN cannot open IMG {p}: {exc}",file=sys.stderr)

    def read(self, name):
        lname=name.lower()
        for p,img in self.archives:
            try:
                hit=img.find(lname)
                if hit is not None:
                    return img.read(hit.name),str(p)
            except Exception:
                try:
                    return img.read(lname),str(p)
                except Exception:
                    pass
        return None,None


def choose_instances(instances, ide, center, radius, interior):
    cx,cy=center
    out=[]
    r2=radius*radius
    for it in instances:
        if interior is not None and it.interior!=interior:
            continue
        x,y,_=it.pos
        if (x-cx)*(x-cx)+(y-cy)*(y-cy)>r2:
            continue
        meta=ide.get(it.ident)
        if meta is None:
            # IPL redundantly stores model name; still usable if IDE is missing.
            meta=IdeObj(it.ident,it.model,it.model,300.0,0)
        out.append((it,meta))
    return out


def sector_key(x,z,size):
    return math.floor(x/size),math.floor(z/size)


def pack_city(selected, archives, out_header:Path, out_report:Path, sector_m:float, scale:float, max_instances:int):
    sectors=defaultdict(lambda:{"verts":[],"tris":[]})
    cache={}
    model_stats={}
    missing={}
    used=0
    source_tri=0

    for it,meta in selected[:max_instances if max_instances>0 else None]:
        key=meta.model.lower()
        if key not in cache:
            raw,archive=archives.read(meta.model+".dff")
            if raw is None:
                cache[key]=None
                missing[key]=missing.get(key,0)+1
                continue
            try:
                dff=Dff.from_bytes(raw)
                meshes=dff.to_generic_meshes()
                parsed=[]
                tv=tt=0
                for mi,mesh in enumerate(meshes):
                    verts=positions_iter(mesh.positions)
                    tris=indices_iter(mesh.indices)
                    mat=diffuse_to_palette(getattr(mesh,"diffuse_color",None),meta.model,mi)
                    transform=getattr(mesh,"transform",None)
                    verts=[apply_mat4_row_major(transform,v) for v in verts]
                    parsed.append((verts,tris,mat,getattr(mesh,"texture_name","") or ""))
                    tv+=len(verts);tt+=len(tris)
                cache[key]=parsed
                model_stats[key]={"vertices":tv,"triangles":tt,"archive":archive}
            except Exception as exc:
                print(f"[vc-import] WARN DFF parse failed {meta.model}: {exc}",file=sys.stderr)
                cache[key]=None
                missing[key]=missing.get(key,0)+1
                continue

        parsed=cache[key]
        if not parsed:
            continue

        # Render coordinate convention in Racer: x horizontal, y height, z forward.
        # GTA map is Z-up, so convert (X,Y,Z) -> (X,Z,Y).
        px,py,pz=it.pos
        sx,sy,sz=it.scale
        for verts,tris,mat,texname in parsed:
            world=[]
            for vx,vy,vz in verts:
                # model scale in GTA coordinates first
                local=(vx*sx,vy*sy,vz*sz)
                rx,ry,rz=qrot(it.quat,local)
                gx=px+rx; gy=py+ry; gz=pz+rz
                world.append((gx,gz,gy))

            for a,b,c in tris:
                if a>=len(world) or b>=len(world) or c>=len(world):
                    continue
                va,vb,vc=world[a],world[b],world[c]
                tx=(va[0]+vb[0]+vc[0])/3.0
                tz=(va[2]+vb[2]+vc[2])/3.0
                sec=sectors[sector_key(tx,tz,sector_m)]
                base=len(sec["verts"])
                sec["verts"].extend((va,vb,vc))
                sec["tris"].append((base,base+1,base+2,mat))
                source_tri+=1
        used+=1

    # Compact exact duplicates within sectors and flatten.
    allv=[]; allt=[]; metas=[]
    for (sx,sz) in sorted(sectors):
        b=sectors[(sx,sz)]
        if not b["tris"]: continue
        lut={}; cv=[]; ct=[]
        for a,bv,c,m in b["tris"]:
            ids=[]
            for old in (a,bv,c):
                p=b["verts"][old]
                q=(round(p[0],5),round(p[1],5),round(p[2],5))
                if q not in lut:
                    lut[q]=len(cv);cv.append(p)
                ids.append(lut[q])
            ct.append((ids[0],ids[1],ids[2],m))
        if len(cv)>65535:
            raise SystemExit(f"sector {(sx,sz)} has too many vertices: {len(cv)}")
        vb0=len(allv);tb0=len(allt)
        allv.extend(cv);allt.extend(ct)
        metas.append((sx,sz,vb0,len(cv),tb0,len(ct)))

    if len(allv)>=2**32:
        raise SystemExit("packed vertex count unexpectedly huge")

    L=[
        "/* Local-only Vice City geometry pack; generated from user's own game files. */",
        "#ifndef VC_CITY_MAP_H","#define VC_CITY_MAP_H","",
        f"#define VC_CITY_WORLD_SCALE {scale:.6f}f",
        f"#define VC_CITY_SECTOR_METERS {sector_m:.6f}f",
        f"#define VC_CITY_SECTOR_WORLD {sector_m*scale:.6f}f",
        f"#define VC_CITY_VERTEX_COUNT {len(allv)}u",
        f"#define VC_CITY_TRIANGLE_COUNT {len(allt)}u",
        f"#define VC_CITY_SECTOR_COUNT {len(metas)}u",
        f"#define VC_CITY_MATERIAL_COUNT {len(PALETTE)}u","",
        "typedef struct { int16_t sx,sz; uint32_t vertex_base,vertex_count,tri_base,tri_count; } vc_city_sector_t;","",
        "static const uint16_t vc_city_mat[VC_CITY_MATERIAL_COUNT]={",
    ]
    L += [f"    0x{x:04x}{',' if i+1<len(PALETTE) else ''}" for i,x in enumerate(PALETTE)]
    L += ["};","","static const v3f_t vc_city_v[VC_CITY_VERTEX_COUNT]={"]
    L += [f"    {{{x*scale:.4f}f,{y*scale:.4f}f,{z*scale:.4f}f}}," for x,y,z in allv]
    L += ["};","","static const tri3d_t vc_city_t[VC_CITY_TRIANGLE_COUNT]={"]
    L += [f"    {{{a},{b},{c},{m}}}," for a,b,c,m in allt]
    L += ["};","","static const vc_city_sector_t vc_city_sector[VC_CITY_SECTOR_COUNT]={"]
    L += [f"    {{{sx},{sz},{vb},{vc},{tb},{tc}}}," for sx,sz,vb,vc,tb,tc in metas]
    L += ["};","","#endif"]
    out_header.parent.mkdir(parents=True,exist_ok=True)
    out_header.write_text("\n".join(L)+"\n",encoding="utf-8")

    report={
        "instances_selected":len(selected),
        "instances_packed":used,
        "unique_models_loaded":len(model_stats),
        "missing_models":missing,
        "source_triangles":source_tri,
        "packed_vertices":len(allv),
        "packed_triangles":len(allt),
        "sectors":len(metas),
        "models":model_stats,
    }
    out_report.parent.mkdir(parents=True,exist_ok=True)
    out_report.write_text(json.dumps(report,indent=2),encoding="utf-8")
    print(
        "VC_LOCAL_PACK_OK",
        f"selected={len(selected)}",f"packed={used}",
        f"models={len(model_stats)}",f"triangles={len(allt)}",
        f"vertices={len(allv)}",f"sectors={len(metas)}",
        f"missing_models={len(missing)}"
    )


def inventory_only(world, out_report:Path):
    ide=world["ide"]; inst=world["instances"]
    models=defaultdict(int); interiors=defaultdict(int)
    xs=[];ys=[];zs=[]
    for it in inst:
        models[it.model.lower()]+=1
        interiors[it.interior]+=1
        xs.append(it.pos[0]);ys.append(it.pos[1]);zs.append(it.pos[2])
    report={
        "dat_files":world["dat_files"],
        "ide_files":world["ide_files"],
        "ipl_files":world["ipl_files"],
        "img_files":world["img_files"],
        "ide_objects":len(ide),
        "instances":len(inst),
        "unique_instance_models":len(models),
        "interiors":dict(sorted(interiors.items())),
        "bounds":{
            "min":[min(xs),min(ys),min(zs)] if xs else None,
            "max":[max(xs),max(ys),max(zs)] if xs else None,
        },
        "most_used_models":sorted(
            ({"model":m,"instances":n} for m,n in models.items()),
            key=lambda x:x["instances"],reverse=True
        )[:100],
    }
    out_report.parent.mkdir(parents=True,exist_ok=True)
    out_report.write_text(json.dumps(report,indent=2),encoding="utf-8")
    print(
        "VC_LOCAL_INVENTORY_OK",
        f"ide={len(ide)}",f"instances={len(inst)}",
        f"models={len(models)}",f"imgs={len(world['img_files'])}"
    )


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--game-root",required=True,help="Your local GTA Vice City installation folder")
    ap.add_argument("--inventory-only",action="store_true")
    ap.add_argument("--center-x",type=float,default=0.0)
    ap.add_argument("--center-y",type=float,default=0.0,help="GTA world Y (horizontal), not height")
    ap.add_argument("--radius",type=float,default=350.0,help="Import radius in GTA world units")
    ap.add_argument("--interior",type=int,default=0)
    ap.add_argument("--sector-m",type=float,default=64.0)
    ap.add_argument("--world-scale",type=float,default=240.0)
    ap.add_argument("--max-instances",type=int,default=0,help="0 = no artificial cap")
    ap.add_argument("--output-header",default="build/vc-local/vc_city_map.h")
    ap.add_argument("--output-report",default="build/vc-local/vc_city_report.json")
    args=ap.parse_args()

    root=Path(args.game_root).resolve()
    if not root.exists():
        raise SystemExit(f"game root does not exist: {root}")

    world=discover_map(root)
    if not world["instances"]:
        raise SystemExit("no IPL instances found; verify this is a PC Vice City installation")

    if args.inventory_only:
        inventory_only(world,Path(args.output_report))
        return

    if Img is None or Dff is None:
        raise SystemExit("rwfury missing; install locally with: py -m pip install rwfury")

    selected=choose_instances(
        world["instances"],world["ide"],
        (args.center_x,args.center_y),args.radius,args.interior
    )
    if not selected:
        raise SystemExit("no instances in requested radius/interior")

    archives=ArchiveSet([Path(x) for x in world["img_files"]])
    pack_city(
        selected,archives,
        Path(args.output_header),Path(args.output_report),
        args.sector_m,args.world_scale,args.max_instances
    )


if __name__=="__main__":
    main()
