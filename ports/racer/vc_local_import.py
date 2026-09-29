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

Stage7.9 preserves DFF UVs and decodes the model TXD dictionaries into a
console-friendly A1R5G5B5 atlas embedded in VCMAP2.BIN. Alpha-tested textures
restore trees, fences, signs, windows and facade detail without shipping any
source GTA asset through GitHub.

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
import struct
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path


try:
    from rwfury import Img, Dff, Txd
except Exception:
    Img = Dff = Txd = None


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
    # rwfury GenericMesh.transform is emitted as a row-major RenderWare frame:
    #
    #   r0 r1 r2 0
    #   r3 r4 r5 0
    #   r6 r7 r8 0
    #   px py pz 1
    #
    # Vertices therefore multiply as ROW vectors [x y z 1] * M.  The previous
    # importer accidentally used column-vector indexing and read translation
    # from m[3]/m[7]/m[11], which scrambled compound DFF atomics.
    return (
        x*m[0] + y*m[4] + z*m[8]  + m[12],
        x*m[1] + y*m[5] + z*m[9]  + m[13],
        x*m[2] + y*m[6] + z*m[10] + m[14],
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


def texcoords_iter(mesh):
    sets=getattr(mesh,"texcoords",None) or []
    if not sets or not sets[0]:
        return []
    vals=list(sets[0])
    if vals and isinstance(vals[0],(tuple,list)):
        return [(float(v[0]),float(v[1])) for v in vals]
    return [(float(vals[i]),float(vals[i+1])) for i in range(0,len(vals)-1,2)]


def diffuse1555(diffuse, model_name="", mesh_no=0):
    if diffuse and len(diffuse)>=3:
        vals=list(diffuse[:3])
        if max(vals)<=1.01:
            vals=[v*255.0 for v in vals]
        return pack1555(vals[0],vals[1],vals[2])
    h=2166136261
    for ch in (model_name+str(mesh_no)).encode("latin-1",errors="ignore"):
        h=((h^ch)*16777619)&0xffffffff
    base=96+(h&63)
    return pack1555(base,base,base)


class TextureAtlas:
    def __init__(self,w=1024,h=1024,max_tex=128):
        self.w=w;self.h=h;self.max_tex=max_tex
        self.pixels=[0]*(w*h)
        self.x=1;self.y=1;self.row_h=0

    @staticmethod
    def _scale_rgba(src,sw,sh,dw,dh):
        out=bytearray(dw*dh*4)
        for y in range(dh):
            sy=min(sh-1,int((y+0.5)*sh/dh))
            for x in range(dw):
                sx=min(sw-1,int((x+0.5)*sw/dw))
                si=(sy*sw+sx)*4
                di=(y*dw+x)*4
                out[di:di+4]=src[si:si+4]
        return bytes(out)

    def add_rgba(self,rgba,sw,sh):
        if sw<1 or sh<1 or not rgba:
            return None
        factor=min(1.0,self.max_tex/max(sw,sh))
        dw=max(1,int(round(sw*factor)))
        dh=max(1,int(round(sh*factor)))
        if dw!=sw or dh!=sh:
            rgba=self._scale_rgba(rgba,sw,sh,dw,dh)

        # 1px transparent gutter keeps adjacent atlas entries from bleeding.
        need_w=dw+2;need_h=dh+2
        if self.x+need_w>self.w:
            self.x=1
            self.y+=self.row_h
            self.row_h=0
        if self.y+need_h>self.h:
            return None
        x0=self.x+1;y0=self.y+1
        meaningful_alpha=False
        for y in range(dh):
            for x in range(dw):
                i=(y*dw+x)*4
                r,g,b,a=rgba[i],rgba[i+1],rgba[i+2],rgba[i+3]
                if a<250: meaningful_alpha=True
                # GTA vegetation/fences are principally alpha-tested. Preserve
                # 1-bit transparency in A1R5G5B5: bit15 clear = transparent.
                if a>=96:
                    self.pixels[(y0+y)*self.w+(x0+x)]=pack1555(r,g,b)
                else:
                    self.pixels[(y0+y)*self.w+(x0+x)]=0
        self.x+=need_w
        self.row_h=max(self.row_h,need_h)
        return (x0,y0,dw,dh,meaningful_alpha)


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


def pack_city(selected, archives, out_header:Path, out_bin:Path, out_report:Path, sector_m:float, scale:float, max_instances:int, center:tuple[float,float]):
    sectors=defaultdict(lambda:{"verts":[],"tris":[]})
    cache={}
    model_stats={}
    missing={}
    used=0
    source_tri=0

    atlas=TextureAtlas(1024,1024,128)
    txd_cache={}
    materials=[]
    material_cache={}
    texture_stats={}

    def add_solid_material(color):
        key=("solid",int(color)&0xffff)
        if key in material_cache:return material_cache[key]
        if len(materials)>=255:return 0
        mid=len(materials)
        materials.append({"x":0,"y":0,"w":0,"h":0,"fallback":int(color)&0xffff,"flags":0,"name":"<solid>"})
        material_cache[key]=mid
        return mid

    # Material 0 is always a valid neutral fallback.
    add_solid_material(pack1555(150,150,150))

    def load_txd(name):
        key=(name or "").lower()
        if key in txd_cache:return txd_cache[key]
        raw,archive=archives.read((name or "")+".txd")
        if raw is None:
            txd_cache[key]=({},None)
            return txd_cache[key]
        try:
            txd=Txd.from_bytes(raw)
            table={t.name.lower():t for t in txd.textures}
            txd_cache[key]=(table,archive)
        except Exception as exc:
            print(f"[vc-import] WARN TXD parse failed {name}: {exc}",file=sys.stderr)
            txd_cache[key]=({},archive)
        return txd_cache[key]

    def material_for(meta,texname,diffuse,model_name,mesh_no):
        fallback=diffuse1555(diffuse,model_name,mesh_no)
        tname=(texname or "").strip().lower()
        if not tname:
            return add_solid_material(fallback)
        key=(meta.txd.lower(),tname)
        if key in material_cache:return material_cache[key]
        if len(materials)>=255:
            return add_solid_material(fallback)

        table,archive=load_txd(meta.txd)
        tex=table.get(tname)
        if tex is None:
            # Some DFFs refer to a texture inherited/shared by another TXD.
            # Keep the mesh visible rather than dropping it.
            mid=add_solid_material(fallback)
            material_cache[key]=mid
            return mid

        try:
            mips,has_alpha=tex.to_rgba()
            rgba=mips[0] if mips else b""
            slot=atlas.add_rgba(rgba,int(tex.width),int(tex.height))
        except Exception as exc:
            print(f"[vc-import] WARN texture decode failed {meta.txd}/{tname}: {exc}",file=sys.stderr)
            slot=None
            has_alpha=False
        if slot is None:
            mid=add_solid_material(fallback)
            material_cache[key]=mid
            return mid

        x,y,w,h,seen_alpha=slot
        mid=len(materials)
        flags=1 | (2 if (has_alpha or seen_alpha) else 0)
        materials.append({
            "x":x,"y":y,"w":w,"h":h,
            "fallback":fallback,"flags":flags,
            "name":f"{meta.txd}/{tname}"
        })
        material_cache[key]=mid
        texture_stats[f"{meta.txd}/{tname}"]={
            "source":[int(tex.width),int(tex.height)],
            "atlas":[x,y,w,h],
            "alpha":bool(has_alpha or seen_alpha),
            "archive":archive,
        }
        return mid

    chosen=selected[:max_instances if max_instances>0 else None]
    for it,meta in chosen:
        # Obvious LOD helper models are useful at long distance in the original
        # engine but harmful in our small-radius test because they duplicate the
        # full model. Keep real billboard geometry (trees/signs); only drop named LODs.
        ml=meta.model.lower()
        if ml.startswith("lod") or ml.endswith("_lod") or "_lod_" in ml:
            continue

        key=ml
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
                    uvs=texcoords_iter(mesh)
                    if len(uvs)<len(verts):
                        uvs=uvs+[(0.0,0.0)]*(len(verts)-len(uvs))
                    transform=getattr(mesh,"transform",None)
                    verts=[apply_mat4_row_major(transform,v) for v in verts]
                    parsed.append((
                        verts,uvs,tris,
                        getattr(mesh,"texture_name","") or "",
                        getattr(mesh,"diffuse_color",None),
                        mi
                    ))
                    tv+=len(verts);tt+=len(tris)
                cache[key]=parsed
                model_stats[key]={"vertices":tv,"triangles":tt,"archive":archive,"txd":meta.txd}
            except Exception as exc:
                print(f"[vc-import] WARN DFF parse failed {meta.model}: {exc}",file=sys.stderr)
                cache[key]=None
                missing[key]=missing.get(key,0)+1
                continue

        parsed=cache[key]
        if not parsed:
            continue

        px,py,pz=it.pos
        sx,sy,sz=it.scale
        for verts,uvs,tris,texname,diffuse,mi in parsed:
            mat=material_for(meta,texname,diffuse,meta.model,mi)
            world=[]
            for vi,(vx,vy,vz) in enumerate(verts):
                local=(vx*sx,vy*sy,vz*sz)
                rx,ry,rz=qrot(it.quat,local)
                gx=px+rx; gy=py+ry; gz=pz+rz
                u,v=uvs[vi] if vi<len(uvs) else (0.0,0.0)
                world.append((gx,gz,gy,float(u),float(v)))

            tri_flags=1 if (meta.flags & 1) else 0
            for a,b,ci in tris:
                if a>=len(world) or b>=len(world) or ci>=len(world):
                    continue
                va,vb,vc=world[a],world[b],world[ci]
                tx=(va[0]+vb[0]+vc[0])/3.0
                tz=(va[2]+vb[2]+vc[2])/3.0
                sec=sectors[sector_key(tx,tz,sector_m)]
                base=len(sec["verts"])
                sec["verts"].extend((va,vb,vc))
                sec["tris"].append((base,base+1,base+2,mat,tri_flags))
                source_tri+=1
        used+=1

    allv=[]; allt=[]; metas=[]
    MAX_CHUNK_VERTS=3800
    MAX_CHUNK_TRIS=7000

    def flush_chunk(sx,sz,cv,ct):
        if not ct:return
        vb0=len(allv);tb0=len(allt)
        allv.extend(cv);allt.extend(ct)
        metas.append((sx,sz,vb0,len(cv),tb0,len(ct)))

    for (sx0,sz0) in sorted(sectors):
        b=sectors[(sx0,sz0)]
        if not b["tris"]:continue
        lut={};cv=[];ct=[]
        for a,bv,ci,m,flags in b["tris"]:
            pts=[b["verts"][a],b["verts"][bv],b["verts"][ci]]
            keys=[
                (round(p[0],5),round(p[1],5),round(p[2],5),round(p[3],6),round(p[4],6))
                for p in pts
            ]
            needed=sum(1 for q in keys if q not in lut)
            if ct and (len(cv)+needed>MAX_CHUNK_VERTS or len(ct)>=MAX_CHUNK_TRIS):
                flush_chunk(sx0,sz0,cv,ct);lut={};cv=[];ct=[]
            ids=[]
            for p,q in zip(pts,keys):
                if q not in lut:
                    lut[q]=len(cv);cv.append(p)
                ids.append(lut[q])
            ct.append((ids[0],ids[1],ids[2],m,flags))
        flush_chunk(sx0,sz0,cv,ct)

    cx,cy=center
    roads=[(it,meta) for it,meta in chosen if (meta.flags & 1)]
    if roads:
        it,meta=min(roads,key=lambda p:(p[0].pos[0]-cx)**2+(p[0].pos[1]-cy)**2)
        spawn_x=float(it.pos[0]);spawn_y=float(it.pos[2])+0.12;spawn_z=float(it.pos[1])
    else:
        spawn_x=float(cx);spawn_y=1.5;spawn_z=float(cy)
    spawn_yaw=0.0

    if metas:
        min_sx=min(x[0] for x in metas);max_sx=max(x[0] for x in metas)
        min_sz=min(x[1] for x in metas);max_sz=max(x[1] for x in metas)
        map_min_x=min_sx*sector_m;map_max_x=(max_sx+1)*sector_m
        map_min_z=min_sz*sector_m;map_max_z=(max_sz+1)*sector_m
    else:
        map_min_x=map_max_x=spawn_x;map_min_z=map_max_z=spawn_z

    # VCM2 header: magic/version, 10 floats, 6 counts = 72 bytes.
    out_bin.parent.mkdir(parents=True,exist_ok=True)
    with out_bin.open("wb") as fp:
        fp.write(struct.pack(
            "<4sI10f6I",
            b"VCM2",2,
            float(scale),float(sector_m),
            spawn_x,spawn_y,spawn_z,spawn_yaw,
            float(map_min_x),float(map_max_x),float(map_min_z),float(map_max_z),
            len(allv),len(allt),len(metas),len(materials),atlas.w,atlas.h
        ))
        # Material: atlas rect x/y/w/h, fallback 1555, flags, pad = 12 bytes.
        for m in materials:
            fp.write(struct.pack(
                "<HHHHHBB",
                m["x"],m["y"],m["w"],m["h"],m["fallback"],m["flags"],0
            ))
        for x,y,z,u,v in allv:
            fp.write(struct.pack("<5f",float(x),float(y),float(z),float(u),float(v)))
        for a,b,ci,m,flags in allt:
            if a>65535 or b>65535 or ci>65535:
                raise SystemExit("VCMAP2 local triangle index exceeds uint16")
            fp.write(struct.pack("<HHHBB",a,b,ci,m&0xff,flags&0xff))
        for sx0,sz0,vb,vc,tb,tc in metas:
            fp.write(struct.pack("<hhIIII",sx0,sz0,vb,vc,tb,tc))
        for px in atlas.pixels:
            fp.write(struct.pack("<H",px&0xffff))

    # Keep the text header as a lightweight diagnostic only; runtime uses BIN.
    out_header.parent.mkdir(parents=True,exist_ok=True)
    out_header.write_text(
        "/* VCMAP2 diagnostic header; runtime data lives in VCMAP.BIN. */\n"
        f"#define VC_CITY_VERTEX_COUNT {len(allv)}u\n"
        f"#define VC_CITY_TRIANGLE_COUNT {len(allt)}u\n"
        f"#define VC_CITY_SECTOR_COUNT {len(metas)}u\n"
        f"#define VC_CITY_MATERIAL_COUNT {len(materials)}u\n"
        f"#define VC_CITY_ATLAS_W {atlas.w}u\n"
        f"#define VC_CITY_ATLAS_H {atlas.h}u\n",
        encoding="utf-8"
    )

    report={
        "format":"VCM2",
        "instances_selected":len(selected),
        "instances_packed":used,
        "unique_models_loaded":len(model_stats),
        "missing_models":missing,
        "source_triangles":source_tri,
        "packed_vertices":len(allv),
        "packed_triangles":len(allt),
        "sectors":len(metas),
        "materials":len(materials),
        "textures_packed":len(texture_stats),
        "texture_stats":texture_stats,
        "atlas":[atlas.w,atlas.h],
        "atlas_bytes":atlas.w*atlas.h*2,
        "models":model_stats,
        "vcmap_bin":str(out_bin),
        "vcmap_bytes":out_bin.stat().st_size,
        "spawn_gta_xyz":[spawn_x,spawn_z,spawn_y],
        "spawn_racer_x_y_z_unscaled":[spawn_x,spawn_y,spawn_z],
        "map_bounds_unscaled":[map_min_x,map_max_x,map_min_z,map_max_z],
    }
    out_report.parent.mkdir(parents=True,exist_ok=True)
    out_report.write_text(json.dumps(report,indent=2),encoding="utf-8")
    print(
        "VC_LOCAL_PACK_OK",
        f"format=VCM2",f"selected={len(selected)}",f"packed={used}",
        f"models={len(model_stats)}",f"triangles={len(allt)}",
        f"vertices={len(allv)}",f"sectors={len(metas)}",
        f"textures={len(texture_stats)}",f"materials={len(materials)}",
        f"atlas={atlas.w}x{atlas.h}",
        f"missing_models={len(missing)}",f"bin_bytes={out_bin.stat().st_size}"
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
    ap.add_argument("--output-bin",default="build/vc-local/VCMAP.BIN")
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

    if Img is None or Dff is None or Txd is None:
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
        Path(args.output_header),Path(args.output_bin),Path(args.output_report),
        args.sector_m,args.world_scale,args.max_instances,
        (args.center_x,args.center_y)
    )


if __name__=="__main__":
    main()
