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
    from rwfury import Img, Dff, Txd, Col
except Exception:
    Img = Dff = Txd = Col = None


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
    num_atomics: int = 1
    lod_distances: tuple[float,...] = ()
    first_damaged: int = 0


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
        if key in {"IDE","IPL","IMG","CDIMAGE","MODELFILE","HIERFILE","TEXDICTION","COLFILE"}:
            value=parts[1].strip()
            # GTA III/VC DAT syntax is "COLFILE <slot> <path>".
            # The old importer treated "<slot> <path>" as one filename and
            # therefore never opened any of the city's collision archives.
            if key=="COLFILE":
                bits=value.split(None,1)
                if len(bits)==2 and bits[0].lstrip("+-").isdigit():
                    value=bits[1]
            out[key].append(norm_rel(value))
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
        if low in {"objs","tobj","anim","hier","cars","peds","path","2dfx","inst","cull","pick","occl","zone","grge","enex","auzo","jump","tcyc","txdp"}:
            section=low
            continue
        if low=="end":
            section=""
            continue
        yield section,[x.strip() for x in line.split(",")]


def parse_txdp(path: Path) -> dict[str,str]:
    """Return child->parent TXD mappings from IDE txdp sections."""
    out={}
    for section,p in parse_sectioned_text(path):
        if section!="txdp" or len(p)<2:
            continue
        child=p[0].strip().lower()
        parent=p[1].strip().lower()
        if child and parent and child!=parent:
            out[child]=parent
    return out


def parse_ide(path: Path) -> dict[int,IdeObj]:
    out={}
    for section,p in parse_sectioned_text(path):
        if section not in {"objs","tobj"} or len(p)<6:
            continue
        try:
            ident=int(p[0]); model=p[1]; txd=p[2]
            mesh_count=max(1,min(3,int(float(p[3]))))
            # reVC CFileLoader::LoadObject: 1..3 LOD distances followed by flags.
            distances=tuple(float(p[4+i]) for i in range(mesh_count))
            draw=distances[0]
            flags=int(float(p[4+mesh_count]))
            damaged=0
            if mesh_count==2 and not (distances[0]<distances[1]):
                damaged=1
            elif mesh_count==3:
                if distances[0]<distances[1]:
                    damaged=0 if distances[1]<distances[2] else 2
                else:
                    damaged=1
            out[ident]=IdeObj(
                ident,model,txd,draw,flags,
                num_atomics=mesh_count,lod_distances=distances,first_damaged=damaged
            )
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


def qrot_vc_ipl(q, p):
    """
    Match reVC CFileLoader::LoadObjectInstance exactly.

    VC IPL stores axis.xyz and cos(angle/2) in w. reVC computes
    angle = -2*acos(w), rotates around axis, then translates. The IPL scale
    values are parsed but deliberately not applied by the game.
    """
    x,y,z,w=q
    n=math.sqrt(x*x+y*y+z*z+w*w)
    if n>1.0e-12:
        x/=n;y/=n;z/=n;w/=n
    # Conjugating xyz is the normalized quaternion equivalent of reVC's
    # negative axis-angle rotation.
    return qrot((-x,-y,-z,w),p)


def transform_vc_instance_point(it, p):
    rx,ry,rz=qrot_vc_ipl(it.quat,p)
    return (it.pos[0]+rx,it.pos[1]+ry,it.pos[2]+rz)


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


def mat4_mul(a,b):
    """Row-major 4x4 matrix product for row-vector chaining: v * a * b."""
    return [
        sum(a[r*4+k]*b[k*4+col] for k in range(4))
        for r in range(4) for col in range(4)
    ]


def frame_local_mat(frame):
    r=frame.rotation_matrix
    p=frame.position
    return [
        r[0],r[1],r[2],0.0,
        r[3],r[4],r[5],0.0,
        r[6],r[7],r[8],0.0,
        p[0],p[1],p[2],1.0,
    ]


def _expanded_bin_indices(indices,flags):
    vals=list(indices)
    if flags!=1:
        return vals
    out=[]
    for i in range(len(vals)-2):
        a,b,c=vals[i],vals[i+1],vals[i+2]
        if a==b or b==c or a==c:
            continue
        out.extend((b,a,c) if (i&1) else (a,b,c))
    return out


def dff_generic_mesh_world_transforms(dff):
    """Mirror rwfury.to_generic_meshes() ordering, but accumulate frame parents."""
    frames=list(getattr(dff,"frames",[]) or [])
    world_cache={}

    def world_frame(i,stack=None):
        if i in world_cache:
            return world_cache[i]
        if i<0 or i>=len(frames):
            return [1.0,0.0,0.0,0.0,
                    0.0,1.0,0.0,0.0,
                    0.0,0.0,1.0,0.0,
                    0.0,0.0,0.0,1.0]
        if stack is None: stack=set()
        if i in stack:
            raise ValueError(f"DFF frame parent cycle at {i}")
        stack=set(stack);stack.add(i)
        local=frame_local_mat(frames[i])
        parent=int(getattr(frames[i],"parent",-1))
        # Row-vector convention: local point * child_local * parent_world.
        out=mat4_mul(local,world_frame(parent,stack)) if parent>=0 else local
        world_cache[i]=out
        return out

    result=[]
    atomics=list(getattr(dff,"atomics",[]) or [])
    geoms=list(getattr(dff,"geometries",[]) or [])
    for atomic in atomics:
        gi=int(getattr(atomic,"geometry_index",-1))
        fi=int(getattr(atomic,"frame_index",-1))
        if gi<0 or gi>=len(geoms):
            continue
        geom=geoms[gi]
        wm=world_frame(fi)
        bin_mesh=getattr(geom,"bin_mesh",None)
        splits=getattr(bin_mesh,"splits",None) if bin_mesh else None
        if splits:
            flags=int(getattr(bin_mesh,"flags",0))
            for split in splits:
                src=_expanded_bin_indices(getattr(split,"indices",[]) or [],flags)
                if src and all(0<=int(v)<len(geom.vertices) for v in src):
                    result.append(wm)
        else:
            for mat_idx in range(len(getattr(geom,"materials",[]) or [])):
                src=[
                    idx
                    for a,b,cc,tri_mat in (getattr(geom,"triangles",[]) or [])
                    if tri_mat==mat_idx
                    for idx in (a,b,cc)
                ]
                if src and all(0<=int(v)<len(geom.vertices) for v in src):
                    result.append(wm)
    return result


def dff_generic_mesh_atomic_indices(dff):
    """Mirror rwfury.to_generic_meshes() ordering and identify each mesh's atomic."""
    result=[]
    geoms=list(getattr(dff,"geometries",[]) or [])
    for ai,atomic in enumerate(list(getattr(dff,"atomics",[]) or [])):
        gi=int(getattr(atomic,"geometry_index",-1))
        if gi<0 or gi>=len(geoms):
            continue
        geom=geoms[gi]
        bin_mesh=getattr(geom,"bin_mesh",None)
        splits=getattr(bin_mesh,"splits",None) if bin_mesh else None
        if splits:
            flags=int(getattr(bin_mesh,"flags",0))
            for split in splits:
                src=_expanded_bin_indices(getattr(split,"indices",[]) or [],flags)
                if src and all(0<=int(v)<len(geom.vertices) for v in src):
                    result.append(ai)
        else:
            for mat_idx in range(len(getattr(geom,"materials",[]) or [])):
                src=[
                    idx
                    for a,b,c,tri_mat in (getattr(geom,"triangles",[]) or [])
                    if tri_mat==mat_idx
                    for idx in (a,b,c)
                ]
                if src and all(0<=int(v)<len(geom.vertices) for v in src):
                    result.append(ai)
    return result


def dff_atomic_lods(dff):
    """reVC GetNameAndLOD: trailing _lN selects SimpleModelInfo atomic slot N."""
    frames=list(getattr(dff,"frames",[]) or [])
    out=[]
    for atomic in list(getattr(dff,"atomics",[]) or []):
        fi=int(getattr(atomic,"frame_index",-1))
        name=""
        if 0<=fi<len(frames):
            name=(getattr(frames[fi],"name","") or "").strip().lower()
        m=re.search(r"_l(\d+)$",name)
        out.append(int(m.group(1)) if m else 0)
    return out


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


RASTER_PAL8=0x2000
RASTER_PAL4=0x4000

D3D8_DXT_FOURCC={
    1:0x31545844, # DXT1
    2:0x32545844, # DXT2
    3:0x33545844, # DXT3
    4:0x34545844, # DXT4
    5:0x35545844, # DXT5
}

def annotate_d3d8_txd_hints(raw,txd):
    """Recover the D3D8 platform-property byte discarded by rwfury.

    PC RenderWare D3D8 stores DXT type (0..5) in the final byte of the
    TextureNative raster header. DragonFF decodes compression from this byte;
    rwfury currently does not retain it, which can make compressed VC textures
    look like raw 16/32-bit pixel streams.
    """
    try:
        data=memoryview(raw)
        if len(data)<24:
            return
        outer_id,outer_size,_=struct.unpack_from("<III",data,0)
        if outer_id!=0x16:
            return
        pos=12
        # First child is the TXD struct (texture count/device id).
        if pos+12>len(data):
            return
        cid,csize,_=struct.unpack_from("<III",data,pos)
        if cid!=0x1:
            return
        pos+=12+csize
        tex_i=0
        while pos+12<=len(data) and tex_i<len(txd.textures):
            cid,csize,_=struct.unpack_from("<III",data,pos)
            child_end=pos+12+csize
            if child_end>len(data):
                break
            if cid==0x15 and pos+24<=child_end:
                sp=pos+12
                sid,ssize,_=struct.unpack_from("<III",data,sp)
                body=sp+12
                # Native raster struct is at least 88 bytes on D3D8/9 PC.
                if sid==0x1 and ssize>=88 and body+88<=child_end:
                    platform_id=struct.unpack_from("<I",data,body)[0]
                    platform_prop=int(data[body+87])
                    tex=txd.textures[tex_i]
                    tex._vc_platform_id=platform_id
                    tex._vc_platform_prop=platform_prop
                    if platform_id==8:
                        tex._vc_d3d8_dxt_type=platform_prop if platform_prop in D3D8_DXT_FOURCC else 0
                tex_i+=1
            pos=child_end
    except Exception as exc:
        print(f"[vc-import] WARN D3D8 TXD hint parse failed: {exc}",file=sys.stderr)


def decode_txd_texture_rgba(tex):
    """Decode TXD texture robustly, including D3D8 DXT and packed PAL4."""
    is_pal4=bool(int(getattr(tex,"raster_format",0)) & RASTER_PAL4)
    is_pal8=bool(int(getattr(tex,"raster_format",0)) & RASTER_PAL8)

    # Palettized textures use their palette representation, not DXT.
    if not is_pal4 and not is_pal8:
        dxt_type=int(getattr(tex,"_vc_d3d8_dxt_type",0) or 0)
        if dxt_type in D3D8_DXT_FOURCC:
            old_fmt=int(getattr(tex,"d3d_format",0))
            try:
                tex.d3d_format=D3D8_DXT_FOURCC[dxt_type]
                mips,has_alpha=tex.to_rgba()
            finally:
                tex.d3d_format=old_fmt
            return mips,has_alpha,f"D3D8-DXT{dxt_type}"

    if not is_pal4:
        mips,has_alpha=tex.to_rgba()
        fmt="PAL8" if is_pal8 else (getattr(tex,"compression_name","none") or "none")
        if fmt=="none":
            fmt=f"RAW{int(getattr(tex,'depth',0))}"
        return mips,has_alpha,fmt

    palette_raw=bytes(getattr(tex,"palette",b"") or b"")
    palette=[]
    for i in range(16):
        off=i*4
        if off+3<len(palette_raw):
            palette.append((
                palette_raw[off],palette_raw[off+1],
                palette_raw[off+2],palette_raw[off+3]
            ))
        else:
            palette.append((0,0,0,255))

    out=[]
    has_alpha=False
    w=max(1,int(getattr(tex,"width",1)))
    h=max(1,int(getattr(tex,"height",1)))
    storage_seen="unknown"

    for mip in list(getattr(tex,"mipmaps",[]) or []):
        raw=bytes(mip)
        count=w*h
        pixels=bytearray(count*4)

        if len(raw)>=count:
            indices=[raw[i]&0x0F for i in range(count)]
            storage_seen="expanded"
        else:
            indices=[]
            for byte in raw:
                indices.append(byte&0x0F)
                if len(indices)>=count: break
                indices.append((byte>>4)&0x0F)
                if len(indices)>=count: break
            if len(indices)<count:
                indices.extend([0]*(count-len(indices)))
            storage_seen="packed"

        for j,idx in enumerate(indices[:count]):
            r,g,b,a=palette[idx]
            di=j*4
            pixels[di]=r;pixels[di+1]=g;pixels[di+2]=b;pixels[di+3]=a
            if a<255:has_alpha=True

        out.append(bytes(pixels))
        w=max(1,w//2);h=max(1,h//2)

    return out,has_alpha,f"PAL4-{storage_seen}"


class TextureAtlas:
    def __init__(self,w=1024,h=1024,max_tex=96):
        self.w=w;self.h=h;self.max_tex=max_tex
        self.pixels=[0]*(w*h)
        self.x=1;self.y=1;self.row_h=0

    @staticmethod
    def _scale_rgba(src,sw,sh,dw,dh):
        """Area/box filter for texture minification.

        Stage7.9 used nearest-neighbour downsampling, which preserved high
        frequency GTA texture detail as hard aliases. On the 640x360 software
        renderer that became grain/moire on oblique facades. Average the source
        footprint of every destination texel instead.
        """
        out=bytearray(dw*dh*4)
        for y in range(dh):
            sy0=(y*sh)//dh
            sy1=max(sy0+1,((y+1)*sh+dh-1)//dh)
            sy1=min(sh,sy1)
            for x in range(dw):
                sx0=(x*sw)//dw
                sx1=max(sx0+1,((x+1)*sw+dw-1)//dw)
                sx1=min(sw,sx1)
                sr=sg=sb=sa=count=0
                for sy in range(sy0,sy1):
                    row=sy*sw
                    for sx in range(sx0,sx1):
                        si=(row+sx)*4
                        sr+=src[si];sg+=src[si+1];sb+=src[si+2];sa+=src[si+3]
                        count+=1
                di=(y*dw+x)*4
                if count:
                    out[di]=sr//count
                    out[di+1]=sg//count
                    out[di+2]=sb//count
                    out[di+3]=sa//count
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


def discover_map(game_root: Path, extracted_root: Path|None=None):
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
    txd_parents={}
    for p in ide_paths:
        ide.update(parse_ide(p))
        txd_parents.update(parse_txdp(p))

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

    model_paths=[]
    for rel in directives["MODELFILE"]:
        p=find_case(game_root,rel)
        if p and p not in model_paths:
            model_paths.append(p)

    hier_paths=[]
    for rel in directives["HIERFILE"]:
        p=find_case(game_root,rel)
        if p and p not in hier_paths:
            hier_paths.append(p)

    col_paths=[]
    for rel in directives["COLFILE"]:
        p=find_case(game_root,rel)
        if p and p not in col_paths:
            col_paths.append(p)

    # Vice City keeps important dictionaries such as vehicles.col under
    # models/coll even when they are not part of the streamed map COLFILE list.
    # Index them as well so the same parser can later supply the player's real
    # collision model instead of a hand-made approximation.
    coll_dir=find_case(game_root,"models/coll")
    if coll_dir and coll_dir.is_dir():
        try:
            for p in sorted(coll_dir.iterdir(),key=lambda q:q.name.lower()):
                if p.is_file() and p.suffix.lower()==".col" and p not in col_paths:
                    col_paths.append(p)
        except OSError:
            pass

    # A locally extracted gta3.img directory is the preferred source for the
    # full Vice City world. Stock map collision dictionaries live in the IMG
    # alongside DFF/TXD assets and are not all enumerated by gta_vc.dat.
    # Without these files the visual city can load while most road collision is
    # absent. Index every extracted COL before packing; GitHub never receives
    # these files.
    if extracted_root and extracted_root.exists() and extracted_root.is_dir():
        try:
            known={str(p.resolve()).lower() for p in col_paths}
            for p in sorted(extracted_root.rglob("*"),key=lambda q:str(q).lower()):
                if not p.is_file() or p.suffix.lower()!=".col":
                    continue
                rp=str(p.resolve()).lower()
                if rp not in known:
                    col_paths.append(p)
                    known.add(rp)
        except OSError:
            pass

    return {
        "dat_files":[str(x) for x in dats],
        "ide_files":[str(x) for x in ide_paths],
        "ipl_files":[str(x) for x in ipl_paths],
        "img_files":[str(x) for x in img_paths],
        "model_files":[str(x) for x in model_paths],
        "hier_files":[str(x) for x in hier_paths],
        "col_files":[str(x) for x in col_paths],
        "extracted_root":str(extracted_root) if extracted_root else "",
        "ide":ide,
        "txd_parents":txd_parents,
        "instances":inst,
    }


class ArchiveSet:
    def __init__(self, paths, extracted_root:Path|None=None):
        if Img is None:
            raise RuntimeError("rwfury is not installed; run: py -m pip install rwfury")
        self.loose={}
        self.extracted_root=extracted_root
        if extracted_root and extracted_root.exists() and extracted_root.is_dir():
            try:
                for p in sorted(extracted_root.rglob("*"),key=lambda q:str(q).lower()):
                    if not p.is_file():
                        continue
                    key=p.name.lower()
                    # The clean VC dump currently has unique archive member
                    # names. Keep the first entry deterministically if a modded
                    # install contains duplicates.
                    if key not in self.loose:
                        self.loose[key]=p
            except OSError as exc:
                print(f"[vc-import] WARN cannot index extracted assets {extracted_root}: {exc}",file=sys.stderr)
        self.archives=[]
        for p in paths:
            try:
                self.archives.append((p,Img.from_file(str(p))))
            except Exception as exc:
                print(f"[vc-import] WARN cannot open IMG {p}: {exc}",file=sys.stderr)

    def read(self, name):
        lname=name.lower()
        loose=self.loose.get(lname)
        if loose is not None:
            try:
                return loose.read_bytes(),str(loose)
            except OSError as exc:
                print(f"[vc-import] WARN cannot read extracted asset {loose}: {exc}",file=sys.stderr)
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


def col_name_key(name):
    n=(name or "").replace("\\","/").split("/")[-1].strip().lower()
    if n.endswith(".dff") or n.endswith(".col"):
        n=n.rsplit(".",1)[0]
    return n


def load_collision_models(paths):
    by_id={}
    by_name={}
    errors=[]
    if Col is None:
        return by_id,by_name,["rwfury.Col unavailable"]
    for p in paths:
        try:
            col=Col.from_file(str(p))
        except Exception as exc:
            errors.append(f"{p}: {exc}")
            continue
        for model in col.models:
            mid=int(getattr(model,"model_id",-1))
            name=col_name_key(getattr(model,"name",""))
            if mid>=0:
                by_id[mid]=model
            if name:
                by_name[name]=model
    return by_id,by_name,errors


def transform_col_vertex(it, v):
    gx,gy,gz=transform_vc_instance_point(it,v)
    # GTA Z-up -> Racer Y-up.
    return (gx,gz,gy)


# reVC SurfaceTable: 0=default, 1=tarmac, 5=pavement.
# Tarmac/pavement are semantic road surfaces. DEFAULT is only a last-resort
# fallback because props/signs frequently use material 0 as well.
VC_SPAWN_ROAD_MATERIALS={1,5}
VC_SPAWN_DEFAULT_MATERIALS={0}

def vc_spawn_material_priority(material):
    material=int(material)
    if material==1:
        return (0,"tarmac")
    if material==5:
        return (1,"pavement")
    if material in VC_SPAWN_DEFAULT_MATERIALS:
        return (2,"default-fallback")
    return None


VC_SPAWN_POSITIVE_NAME_TOKENS=(
    "road","street","bridge","freeway","highway","causeway","avenue","lane"
)
VC_SPAWN_NEGATIVE_NAME_TOKENS=(
    "rock","seabed","water","ocean","jump","sand","beach","grass","hedge",
    "tree","bush","plant","shadow","reef","coral","cliff","mount","riverbed",
    "ramp","stunt","jump","airport","runway","taxiway","hangar","terminal",
    "stadium","stad_","armybase","armybas",
    "billbd","billboard","advert","advertise","poster","sign","signage",
    "bboard","hoarding"
)

def vc_spawn_model_class(name, ide_flags=0):
    n=col_name_key(name)
    positive=any(tok in n for tok in VC_SPAWN_POSITIVE_NAME_TOKENS)
    negative=any(tok in n for tok in VC_SPAWN_NEGATIVE_NAME_TOKENS)
    # A clear road/street name wins even if another token such as "ocean"
    # appears in the same model name (e.g. oceanroadXX).
    if positive:
        return (0,"named-road")
    if negative:
        return None
    if int(ide_flags)&1:
        return (1,"ide-road-flag")
    return (2,"generic-surface")


def choose_dense_spawn(candidates, all_instances, ide, radius, interior, max_eval=600):
    if not candidates:
        return None

    # Many COL faces describe the same short stretch. Collapse them to coarse
    # 20 m cells/model so density scoring is spent on distinct places.
    unique=[]
    seen=set()
    # Candidate generation is distance-oriented. For choosing a useful demo
    # district, evaluate semantic roads first so a nearby generic slab cannot
    # crowd real streets out of the scoring budget.
    ordered=sorted(candidates,key=lambda q:(q[10],q[0],q[1],q[2]))
    for cand in ordered:
        _,dist2,neg_area,tx,ty,tz,up,material,model,label,model_rank,model_kind=cand
        key=(round(tx/20.0),round(tz/20.0),col_name_key(model),material)
        if key in seen:
            continue
        seen.add(key)
        unique.append(cand)
        if len(unique)>=max_eval:
            break

    best=None
    best_score=None
    for cand in unique:
        prio,dist2,neg_area,tx,ty,tz,up,material,model,label,model_rank,model_kind=cand
        neighborhood=choose_instances(
            all_instances,ide,(tx,tz),radius,interior
        )
        density=len(neighborhood)

        # Do not accept a technically valid surface in an empty outskirts cell.
        # A radius-150 playable city test should contain a useful number of IPL
        # objects around the car.
        if density<12:
            continue

        # Named roads first, then IDE-road flagged models, then generic
        # road/concrete surfaces. Within the class prefer actual street material
        # and the denser neighborhood.
        score=(model_rank,prio,-density,dist2,neg_area)
        if best_score is None or score<best_score:
            best_score=score
            best=(cand,density)

    return best


def collision_spawn_candidates(selected, col_by_id, col_by_name, center):
    cx,cz=center
    out=[]
    used_models=0
    face_count=0
    eligible_faces=0
    rejected_materials=defaultdict(int)
    for it,meta in selected:
        model=col_by_id.get(it.ident)
        if model is None:
            model=col_by_name.get(col_name_key(meta.model))
        if model is None or not getattr(model,"vertices",None) or not getattr(model,"faces",None):
            continue
        used_models+=1
        verts=[transform_col_vertex(it,v) for v in model.vertices]
        for face in model.faces:
            try:
                a=verts[int(face.a)]; b=verts[int(face.b)]; d=verts[int(face.c)]
            except (IndexError,ValueError):
                continue
            face_count+=1
            material=int(getattr(face,"material",0))
            priority=vc_spawn_material_priority(material)
            if priority is None:
                rejected_materials[material]+=1
                continue

            model_class=vc_spawn_model_class(meta.model,meta.flags)
            if model_class is None:
                rejected_materials[f"name:{col_name_key(meta.model)}"]+=1
                continue
            model_rank,model_kind=model_class

            ux,uy,uz=b[0]-a[0],b[1]-a[1],b[2]-a[2]
            vx,vy,vz=d[0]-a[0],d[1]-a[1],d[2]-a[2]
            nx=uy*vz-uz*vy
            ny=uz*vx-ux*vz
            nz=ux*vy-uy*vx
            area2=math.sqrt(nx*nx+ny*ny+nz*nz)
            if area2<1.0e-6:
                continue
            up=abs(ny)/area2
            if up<0.82:
                continue

            # Do not spawn the car on furniture-sized horizontal collision
            # faces. area2 is twice the triangle area in GTA world units:
            # tables, benches and small props are normally below this floor,
            # while real road/bridge triangles comfortably exceed it.
            horizontal_area2=abs(
                (b[0]-a[0])*(d[2]-a[2]) -
                (b[2]-a[2])*(d[0]-a[0])
            )
            if horizontal_area2<8.0:
                rejected_materials["small-horizontal"]+=1
                continue

            # Generic DEFAULT-surface candidates are dangerous: many props use
            # material 0. Only keep them when they are truly broad ground-like
            # slabs; named/flagged roads may still use smaller triangles.
            if material==0 and model_rank>=2 and horizontal_area2<80.0:
                rejected_materials["generic-default-small"]+=1
                continue

            eligible_faces+=1
            tx=(a[0]+b[0]+d[0])/3.0
            ty=(a[1]+b[1]+d[1])/3.0
            tz=(a[2]+b[2]+d[2])/3.0
            dist2=(tx-cx)*(tx-cx)+(tz-cz)*(tz-cz)
            prio,label=priority
            # Prefer broad road faces before tiny edge triangles. This avoids
            # choosing decorative roadside geometry near an otherwise valid
            # center point.
            out.append((prio,dist2,-horizontal_area2,tx,ty,tz,up,material,meta.model,label,model_rank,model_kind))
    out.sort()
    return out,used_models,face_count,eligible_faces,dict(rejected_materials)


def collision_match_stats(selected,col_by_id,col_by_name):
    matched_id=0
    matched_name=0
    mesh_models=0
    face_total=0
    box_models=0
    sphere_models=0
    unmatched=[]
    matched_no_faces=[]
    seen=set()

    for it,meta in selected:
        key=(it.ident,col_name_key(meta.model))
        if key in seen:
            continue
        seen.add(key)

        model=col_by_id.get(it.ident)
        source="id"
        if model is None:
            model=col_by_name.get(col_name_key(meta.model))
            source="name"
        if model is None:
            if len(unmatched)<12:
                unmatched.append(meta.model)
            continue

        if source=="id":
            matched_id+=1
        else:
            matched_name+=1

        faces=getattr(model,"faces",None) or []
        boxes=getattr(model,"boxes",None) or []
        spheres=getattr(model,"spheres",None) or []
        if faces:
            mesh_models+=1
            face_total+=len(faces)
        else:
            if len(matched_no_faces)<12:
                matched_no_faces.append(meta.model)
        if boxes: box_models+=1
        if spheres: sphere_models+=1

    return {
        "unique_selected":len(seen),
        "matched_id":matched_id,
        "matched_name":matched_name,
        "mesh_models":mesh_models,
        "face_total":face_total,
        "box_models":box_models,
        "sphere_models":sphere_models,
        "unmatched":unmatched,
        "matched_no_faces":matched_no_faces,
    }


def print_collision_stats(label,stats):
    print(
        "VC_COLLISION_MATCH",
        f"scope={label}",
        f"unique={stats['unique_selected']}",
        f"id={stats['matched_id']}",
        f"name={stats['matched_name']}",
        f"mesh={stats['mesh_models']}",
        f"faces={stats['face_total']}",
        f"boxes={stats['box_models']}",
        f"spheres={stats['sphere_models']}",
        f"unmatched={len(stats['unmatched'])}"
    )
    if stats["unmatched"]:
        print("VC_COLLISION_UNMATCHED",",".join(stats["unmatched"]))
    if stats["matched_no_faces"]:
        print("VC_COLLISION_NO_FACES",",".join(stats["matched_no_faces"]))


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


VC_REGION_PRESETS={
    # Small island-only diagnostic pack.
    "starfish":{
        "bounds":(-760.0,-80.0,-820.0,-80.0),  # minX,maxX,minY,maxY
        "spawn":(-346.818,-290.741),
        "label":"Starfish Island",
    },
    # Large central-city development pack. The measured source world is about
    # 3.93 x 3.68 km, so R1000 gives a useful ~2 km diameter around Starfish
    # without forcing the Hi3531 to carry the entire Vice City map at once.
    "starfish_large":{
        "center":(-346.818,-290.741),
        "radius":1000.0,
        "spawn":(-346.818,-290.741),
        "label":"Starfish Island + central city (R1000)",
    },
}


def choose_instances_box(instances, ide, bounds, interior):
    min_x,max_x,min_y,max_y=bounds
    out=[]
    for it in instances:
        if interior is not None and it.interior!=interior:
            continue
        x,y,_=it.pos
        if x<min_x or x>max_x or y<min_y or y>max_y:
            continue
        meta=ide.get(it.ident)
        if meta is None:
            meta=IdeObj(it.ident,it.model,it.model,300.0,0)
        out.append((it,meta))
    return out


def sector_key(x,z,size):
    return math.floor(x/size),math.floor(z/size)


def write_atlas_bmp(path:Path,atlas):
    """Write a 24-bit diagnostic BMP without external dependencies."""
    w,h=atlas.w,atlas.h
    row_stride=(w*3+3)&~3
    pixel_bytes=row_stride*h
    header_size=14+40
    out=bytearray(header_size+pixel_bytes)
    struct.pack_into("<2sIHHI",out,0,b"BM",len(out),0,0,header_size)
    struct.pack_into("<IIIHHIIIIII",out,14,40,w,h,1,24,0,pixel_bytes,2835,2835,0,0)
    for y in range(h):
        dst=header_size+(h-1-y)*row_stride
        for x in range(w):
            px=atlas.pixels[y*w+x]
            if not (px&0x8000):
                r=g=b=0
            else:
                r=((px>>10)&31)*255//31
                g=((px>>5)&31)*255//31
                b=(px&31)*255//31
            off=dst+x*3
            out[off]=b;out[off+1]=g;out[off+2]=r
    path.parent.mkdir(parents=True,exist_ok=True)
    path.write_bytes(out)


def _col_surface_pair(surface):
    """Vice City COL1 first two surface bytes map to reVC surface/piece."""
    if surface is None:
        return 0,0
    return (
        int(getattr(surface,"material",0))&0xff,
        int(getattr(surface,"flag",0))&0xff,
    )


def _box_collision_triangles(it,box):
    """Preserve a GTA COL box exactly as its six transformed faces."""
    mn=getattr(box,"min",(0,0,0));mx=getattr(box,"max",(0,0,0))
    local=[
        (mn[0],mn[1],mn[2]),(mx[0],mn[1],mn[2]),
        (mx[0],mx[1],mn[2]),(mn[0],mx[1],mn[2]),
        (mn[0],mn[1],mx[2]),(mx[0],mn[1],mx[2]),
        (mx[0],mx[1],mx[2]),(mn[0],mx[1],mx[2]),
    ]
    v=[transform_col_vertex(it,p) for p in local]
    faces=((0,1,2),(0,2,3),(4,6,5),(4,7,6),
           (0,4,5),(0,5,1),(1,5,6),(1,6,2),
           (2,6,7),(2,7,3),(3,7,4),(3,4,0))
    surface,piece=_col_surface_pair(getattr(box,"surface",None))
    return [(v[a],v[b],v[c],surface,piece) for a,b,c in faces]


def _sphere_collision_world(it,sphere):
    center=transform_col_vertex(it,getattr(sphere,"center",(0,0,0)))
    r=float(getattr(sphere,"radius",0.0))
    surface,piece=_col_surface_pair(getattr(sphere,"surface",None))
    return center,r,surface,piece


def pack_collision_sidecar(chosen,col_by_id,col_by_name,out_path:Path,sector_m:float,scale:float):
    """
    VCC2: direct Vice City collision primitives.

    Mesh faces retain their original GTA surface id. Boxes retain their original
    surface/piece and are represented by their exact six transformed faces.
    Spheres remain spheres. No ground/wall classification is invented here.
    """
    sectors=defaultdict(lambda:{"tris":[],"spheres":[]})
    mesh_faces=0
    box_faces=0
    sphere_count=0
    matched=0
    rejected_pathological=0
    surface_hist=defaultdict(int)

    def add_triangle(a,b,c,surface,piece):
        nonlocal rejected_pathological
        vals=(*a,*b,*c)
        if not all(math.isfinite(v) for v in vals):
            rejected_pathological+=1
            return
        edge=max(math.dist(a,b),math.dist(b,c),math.dist(c,a))
        if edge>500.0:
            rejected_pathological+=1
            return
        minx=min(a[0],b[0],c[0]);maxx=max(a[0],b[0],c[0])
        minz=min(a[2],b[2],c[2]);maxz=max(a[2],b[2],c[2])
        sx0=math.floor(minx/sector_m);sx1=math.floor(maxx/sector_m)
        sz0=math.floor(minz/sector_m);sz1=math.floor(maxz/sector_m)
        rec=(a,b,c,int(surface)&0xff,int(piece)&0xff)
        for sx in range(sx0,sx1+1):
            for sz in range(sz0,sz1+1):
                sectors[(sx,sz)]["tris"].append(rec)
        surface_hist[int(surface)&0xff]+=1

    def add_sphere(center,r,surface,piece):
        nonlocal rejected_pathological
        if r<=0.0 or not all(math.isfinite(v) for v in (*center,r)):
            rejected_pathological+=1
            return
        minx=center[0]-r;maxx=center[0]+r
        minz=center[2]-r;maxz=center[2]+r
        sx0=math.floor(minx/sector_m);sx1=math.floor(maxx/sector_m)
        sz0=math.floor(minz/sector_m);sz1=math.floor(maxz/sector_m)
        rec=(center,float(r),int(surface)&0xff,int(piece)&0xff)
        for sx in range(sx0,sx1+1):
            for sz in range(sz0,sz1+1):
                sectors[(sx,sz)]["spheres"].append(rec)
        surface_hist[int(surface)&0xff]+=1

    for it,meta in chosen:
        ml=(meta.model or "").strip().lower()
        if ml.startswith("lod") or ml.endswith("_lod") or "_lod_" in ml:
            continue
        model=col_by_id.get(it.ident)
        if model is None:
            model=col_by_name.get(col_name_key(meta.model))
        if model is None:
            continue
        matched+=1

        verts=[transform_col_vertex(it,v) for v in (getattr(model,"vertices",None) or [])]
        for face in (getattr(model,"faces",None) or []):
            try:
                a=verts[int(face.a)];b=verts[int(face.b)];c=verts[int(face.c)]
            except (IndexError,ValueError):
                continue
            surface=int(getattr(face,"material",0))&0xff
            add_triangle(a,b,c,surface,0)
            mesh_faces+=1

        for box in (getattr(model,"boxes",None) or []):
            for a,b,c,surface,piece in _box_collision_triangles(it,box):
                add_triangle(a,b,c,surface,piece)
                box_faces+=1

        for sphere in (getattr(model,"spheres",None) or []):
            center,r,surface,piece=_sphere_collision_world(it,sphere)
            add_sphere(center,r,surface,piece)
            sphere_count+=1

    flat_tris=[]
    flat_spheres=[]
    meta=[]
    for sx,sz in sorted(sectors):
        bucket=sectors[(sx,sz)]
        tb=len(flat_tris);sb=len(flat_spheres)
        flat_tris.extend(bucket["tris"])
        flat_spheres.extend(bucket["spheres"])
        meta.append((sx,sz,tb,len(bucket["tris"]),sb,len(bucket["spheres"])))

    out_path.parent.mkdir(parents=True,exist_ok=True)
    with out_path.open("wb") as fp:
        # VCC2 header: magic/version, world scale, sector metres,
        # triangle count, sphere count, sector count.
        fp.write(struct.pack(
            "<4sIffIII",b"VCC2",2,float(scale),float(sector_m),
            len(flat_tris),len(flat_spheres),len(meta)
        ))
        # Triangle record: 3 world-space points + original surface/piece.
        for a,b,c,surface,piece in flat_tris:
            fp.write(struct.pack(
                "<9fBBH",
                float(a[0]),float(a[1]),float(a[2]),
                float(b[0]),float(b[1]),float(b[2]),
                float(c[0]),float(c[1]),float(c[2]),
                surface&0xff,piece&0xff,0
            ))
        # Sphere record: center/radius + original surface/piece.
        for center,r,surface,piece in flat_spheres:
            fp.write(struct.pack(
                "<4fBBH",
                float(center[0]),float(center[1]),float(center[2]),float(r),
                surface&0xff,piece&0xff,0
            ))
        # Sector ranges into both primitive arrays.
        for sx,sz,tb,tc,sb,sc in meta:
            fp.write(struct.pack("<hhIIII",sx,sz,tb,tc,sb,sc))

    return {
        "format":"VCC2",
        "path":str(out_path),
        "bytes":out_path.stat().st_size,
        "matched_instances":matched,
        "triangles":len(flat_tris),
        "mesh_triangles":mesh_faces,
        "box_triangles":box_faces,
        "spheres":len(flat_spheres),
        "source_spheres":sphere_count,
        "surface_histogram":dict(sorted(surface_hist.items())),
        "rejected_pathological":rejected_pathological,
        "sectors":len(meta),
    }


def pack_city(selected, archives, txd_parents, col_by_id, col_by_name, col_errors, out_header:Path, out_bin:Path, out_report:Path, sector_m:float, scale:float, max_instances:int, center:tuple[float,float], region_name:str=""):
    sectors=defaultdict(lambda:{"verts":[],"tris":[]})
    cache={}
    model_stats={}
    missing={}
    used=0
    source_tri=0
    skipped_lod_meshes=0
    skipped_lod_triangles=0
    rejected_visual_pathological=0

    # 640x360 target: a 56px ceiling exhausted the 2048 atlas on the clean
    # R1000 city (thousands of materials fell back to flat colours). 40px keeps
    # useful road/facade detail while fitting the original VC district much more
    # reliably in the same 8 MiB A1R5G5B5 atlas.
    atlas=TextureAtlas(2048,2048,40)
    txd_cache={}
    materials=[]
    material_cache={}
    texture_stats={}
    texture_format_stats=defaultdict(int)
    texture_missing=defaultdict(int)
    texture_atlas_full=0
    texture_parent_hits=0
    texture_shared_hits=0
    texture_mask_hits=0
    search_txd_names=[]

    def add_solid_material(color):
        key=("solid",int(color)&0xffff)
        if key in material_cache:return material_cache[key]
        if len(materials)>=1536:return 0
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
            annotate_d3d8_txd_hints(raw,txd)
            table={t.name.lower():t for t in txd.textures}
            txd_cache[key]=(table,archive)
        except Exception as exc:
            print(f"[vc-import] WARN TXD parse failed {name}: {exc}",file=sys.stderr)
            txd_cache[key]=({},archive)
        return txd_cache[key]

    def resolve_texture(txd_name,tname):
        nonlocal texture_parent_hits,texture_shared_hits
        seen=set()
        cur=(txd_name or "").strip().lower()
        depth=0
        while cur and cur not in seen and depth<8:
            seen.add(cur)
            table,archive=load_txd(cur)
            tex=table.get(tname)
            if tex is not None:
                if depth>0:
                    texture_parent_hits+=1
                return tex,archive,cur
            cur=txd_parents.get(cur,"")
            depth+=1

        # Some Vice City world materials rely on textures that are effectively
        # global/common even when a usable TXDP relationship is absent in the
        # text IDE set. Search only TXDs referenced by this selected district,
        # keeping the fallback deterministic and local.
        for candidate in search_txd_names:
            if candidate in seen:
                continue
            table,archive=load_txd(candidate)
            tex=table.get(tname)
            if tex is not None:
                texture_shared_hits+=1
                return tex,archive,candidate
        return None,None,None

    def apply_mask_rgba(base_rgba,bw,bh,mask_tex):
        nonlocal texture_mask_hits
        if not base_rgba or mask_tex is None:
            return base_rgba,False,None
        try:
            mmips,mhas,mfmt=decode_txd_texture_rgba(mask_tex)
            if not mmips:
                return base_rgba,False,mfmt
            mw=max(1,int(mask_tex.width));mh=max(1,int(mask_tex.height))
            mask=mmips[0]
            out=bytearray(base_rgba)
            varied_alpha=False
            for y in range(bh):
                my=min(mh-1,int((y+0.5)*mh/bh))
                for x in range(bw):
                    mx=min(mw-1,int((x+0.5)*mw/bw))
                    mi=(my*mw+mx)*4
                    bi=(y*bw+x)*4
                    ma=mask[mi+3]
                    if not mhas:
                        # Separate RenderWare masks are often ordinary grayscale
                        # images rather than alpha-bearing textures.
                        ma=(int(mask[mi])+int(mask[mi+1])+int(mask[mi+2]))//3
                    if ma<250:
                        varied_alpha=True
                    if ma<out[bi+3]:
                        out[bi+3]=ma
            if varied_alpha:
                texture_mask_hits+=1
            return bytes(out),varied_alpha,mfmt
        except Exception as exc:
            print(f"[vc-import] WARN mask decode failed: {exc}",file=sys.stderr)
            return base_rgba,False,None


    def material_for(meta,texname,maskname,diffuse,model_name,mesh_no):
        nonlocal texture_atlas_full
        fallback=diffuse1555(diffuse,model_name,mesh_no)
        tname=(texname or "").strip().lower()
        if not tname:
            return add_solid_material(fallback)
        mname=(maskname or "").strip().lower()
        key=(meta.txd.lower(),tname,mname)
        if key in material_cache:return material_cache[key]
        if len(materials)>=1536:
            return add_solid_material(fallback)

        tex,archive,resolved_txd=resolve_texture(meta.txd,tname)
        if tex is None:
            texture_missing[f"{meta.txd}/{tname}"]+=1
            mid=add_solid_material(fallback)
            material_cache[key]=mid
            return mid

        try:
            mips,has_alpha,tex_format=decode_txd_texture_rgba(tex)
            texture_format_stats[tex_format]+=1
            rgba=mips[0] if mips else b""
            mask_resolved_txd=None
            mask_format=None
            mask_applied=False
            if mname and mname!=tname:
                mask_tex,_,mask_resolved_txd=resolve_texture(meta.txd,mname)
                if mask_tex is not None:
                    rgba,mask_applied,mask_format=apply_mask_rgba(
                        rgba,int(tex.width),int(tex.height),mask_tex
                    )
                    has_alpha=bool(has_alpha or mask_applied)
            slot=atlas.add_rgba(rgba,int(tex.width),int(tex.height))
        except Exception as exc:
            print(f"[vc-import] WARN texture decode failed {meta.txd}/{tname}: {exc}",file=sys.stderr)
            slot=None
            has_alpha=False
        if slot is None:
            texture_atlas_full+=1
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
            "resolved_txd":resolved_txd,
            "mask_name":(maskname or ""),
            "mask_resolved_txd":mask_resolved_txd,
            "mask_format":mask_format,
            "mask_applied":bool(mask_applied),
            "format":tex_format,
            "platform_id":int(getattr(tex,"_vc_platform_id",0) or 0),
            "platform_prop":int(getattr(tex,"_vc_platform_prop",0) or 0),
            "mip0_bytes":len(getattr(tex,"mipmaps",[b""])[0]) if getattr(tex,"mipmaps",None) else 0,
        }
        return mid

    chosen=selected[:max_instances if max_instances>0 else None]
    collision_sidecar=pack_collision_sidecar(
        chosen,col_by_id,col_by_name,out_bin.with_name("VCCOL.BIN"),sector_m,scale
    )

    # Deterministic local TXD search universe for shared/common texture fallback.
    txd_seen=set()
    for _,m in chosen:
        cur=(m.txd or "").strip().lower()
        depth=0
        while cur and cur not in txd_seen and depth<8:
            txd_seen.add(cur)
            search_txd_names.append(cur)
            cur=txd_parents.get(cur,"")
            depth+=1
    for common_txd in ("generic","particle","vehicle"):
        if common_txd not in txd_seen:
            table,_=load_txd(common_txd)
            if table:
                txd_seen.add(common_txd)
                search_txd_names.append(common_txd)

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
                mesh_atomic_indices=dff_generic_mesh_atomic_indices(dff)
                atomic_lods=dff_atomic_lods(dff)
                best_atomic_lod=min(atomic_lods) if atomic_lods else 0
                parsed=[]
                tv=tt=0
                if len(mesh_atomic_indices)!=len(meshes):
                    print(
                        f"[vc-import] WARN atomic split mismatch {meta.model}: "
                        f"meshes={len(meshes)} atomic_ids={len(mesh_atomic_indices)}",
                        file=sys.stderr
                    )
                for mi,mesh in enumerate(meshes):
                    verts=positions_iter(mesh.positions)
                    tris=indices_iter(mesh.indices)
                    atomic_index=mesh_atomic_indices[mi] if mi<len(mesh_atomic_indices) else 0
                    atomic_lod=(
                        atomic_lods[atomic_index]
                        if 0<=atomic_index<len(atomic_lods) else 0
                    )
                    # reVC LoadAtomicFile assigns each atomic to its _lN slot,
                    # removes it from the source clump and gives it a fresh
                    # identity frame. Select the highest-detail available LOD by
                    # suffix, not by arbitrary atomic index.
                    if atomic_lod!=best_atomic_lod:
                        skipped_lod_meshes+=1
                        skipped_lod_triangles+=len(tris)
                        continue
                    uvs=texcoords_iter(mesh)
                    if len(uvs)<len(verts):
                        uvs=uvs+[(0.0,0.0)]*(len(verts)-len(uvs))
                    # reVC resets streamed SimpleModelInfo atomics to an
                    # identity frame after extracting them from the DFF clump.
                    # Geometry vertices are already model-local. Applying the
                    # source atomic frame here created displaced/spike geometry.
                    verts=[(float(v[0]),float(v[1]),float(v[2])) for v in verts]
                    parsed.append((
                        verts,uvs,tris,
                        getattr(mesh,"texture_name","") or "",
                        getattr(mesh,"mask_name","") or "",
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
        for verts,uvs,tris,texname,maskname,diffuse,mi in parsed:
            mat=material_for(meta,texname,maskname,diffuse,meta.model,mi)
            world=[]
            for vi,(vx,vy,vz) in enumerate(verts):
                rx,ry,rz=qrot_vc_ipl(it.quat,(vx,vy,vz))
                gx=px+rx; gy=py+ry; gz=pz+rz
                u,v=uvs[vi] if vi<len(uvs) else (0.0,0.0)
                world.append((gx,gz,gy,float(u),float(v)))

            tri_flags=1 if (meta.flags & 1) else 0
            for a,b,ci in tris:
                if a>=len(world) or b>=len(world) or ci>=len(world):
                    continue
                va,vb,vc=world[a],world[b],world[ci]

                # The real VC near-world models do not contain city-spanning
                # triangles. A corrupt/remapped DFF index can otherwise connect
                # unrelated vertices and create a huge "sheet" that pops as its
                # centroid sector enters/leaves the render window. VCCOL already
                # applies the same 500 m sanity ceiling.
                xyz=(va[:3],vb[:3],vc[:3])
                if not all(math.isfinite(q) for p in xyz for q in p):
                    rejected_visual_pathological+=1
                    continue
                if max(
                    math.dist(xyz[0],xyz[1]),
                    math.dist(xyz[1],xyz[2]),
                    math.dist(xyz[2],xyz[0])
                )>500.0:
                    rejected_visual_pathological+=1
                    continue

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

    # Physics comes from Vice City's COL data, not visual DFF flags/materials.
    col_spawn,col_models_used,col_face_count,col_eligible_faces,col_rejected_materials=collision_spawn_candidates(
        chosen,col_by_id,col_by_name,(cx,cy)
    )
    if col_spawn:
        # Explicit centers should also prefer a semantic road over a nearby
        # generic slab/ramp. Candidate tuple carries model_rank at index 10.
        spawn_pick=min(col_spawn,key=lambda q:(q[10],q[0],q[1],q[2]))
        _,_,_,spawn_x,road_y,spawn_z,spawn_up,spawn_col_material,spawn_col_model,spawn_surface_kind,spawn_model_rank,spawn_model_kind=spawn_pick
        spawn_y=road_y+0.12
        spawn_source="col-triangle"
    else:
        spawn_x=float(cx);spawn_y=1.5;spawn_z=float(cy)
        spawn_up=0.0
        spawn_col_material=-1
        spawn_col_model=""
        spawn_surface_kind="none"
        spawn_model_rank=99
        spawn_model_kind="none"
        spawn_source="fallback-center"
    spawn_yaw=0.0

    if metas:
        min_sx=min(x[0] for x in metas);max_sx=max(x[0] for x in metas)
        min_sz=min(x[1] for x in metas);max_sz=max(x[1] for x in metas)
        map_min_x=min_sx*sector_m;map_max_x=(max_sx+1)*sector_m
        map_min_z=min_sz*sector_m;map_max_z=(max_sz+1)*sector_m
    else:
        map_min_x=map_max_x=spawn_x;map_min_z=map_max_z=spawn_z

    # VCM3 keeps the 72-byte header but widens triangle material ids to uint16.
    out_bin.parent.mkdir(parents=True,exist_ok=True)
    with out_bin.open("wb") as fp:
        fp.write(struct.pack(
            "<4sI10f6I",
            b"VCM3",3,
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
                raise SystemExit("VCMAP3 local triangle index exceeds uint16")
            if m>65535:
                raise SystemExit("VCMAP3 material index exceeds uint16")
            # a,b,c,material,flags,pad = 10 bytes.
            fp.write(struct.pack("<HHHHBB",a,b,ci,m,flags&0xff,0))
        for sx0,sz0,vb,vc,tb,tc in metas:
            fp.write(struct.pack("<hhIIII",sx0,sz0,vb,vc,tb,tc))
        for px in atlas.pixels:
            fp.write(struct.pack("<H",px&0xffff))

    atlas_bmp=out_bin.with_name("vc_atlas.bmp")
    write_atlas_bmp(atlas_bmp,atlas)

    # Keep the text header as a lightweight diagnostic only; runtime uses BIN.
    out_header.parent.mkdir(parents=True,exist_ok=True)
    out_header.write_text(
        "/* VCMAP3 diagnostic header; runtime data lives in VCMAP.BIN. */\n"
        f"#define VC_CITY_VERTEX_COUNT {len(allv)}u\n"
        f"#define VC_CITY_TRIANGLE_COUNT {len(allt)}u\n"
        f"#define VC_CITY_SECTOR_COUNT {len(metas)}u\n"
        f"#define VC_CITY_MATERIAL_COUNT {len(materials)}u\n"
        f"#define VC_CITY_ATLAS_W {atlas.w}u\n"
        f"#define VC_CITY_ATLAS_H {atlas.h}u\n",
        encoding="utf-8"
    )

    report={
        "format":"VCM3",
        "placement_transform":"reVC LoadObjectInstance: negative quaternion angle, IPL scale ignored",
        "instances_nonunit_scale_ignored":sum(
            1 for it,_ in selected
            if max(abs(it.scale[0]-1.0),abs(it.scale[1]-1.0),abs(it.scale[2]-1.0))>1.0e-5
        ),
        "instances_selected":len(selected),
        "instances_packed":used,
        "unique_models_loaded":len(model_stats),
        "missing_models":missing,
        "source_triangles":source_tri,
        "skipped_lod_meshes":skipped_lod_meshes,
        "skipped_lod_triangles":skipped_lod_triangles,
        "rejected_visual_pathological":rejected_visual_pathological,
        "packed_vertices":len(allv),
        "packed_triangles":len(allt),
        "sectors":len(metas),
        "materials":len(materials),
        "textures_packed":len(texture_stats),
        "texture_stats":texture_stats,
        "texture_format_stats":dict(texture_format_stats),
        "texture_missing":dict(sorted(texture_missing.items())),
        "texture_parent_hits":texture_parent_hits,
        "texture_shared_hits":texture_shared_hits,
        "texture_mask_hits":texture_mask_hits,
        "texture_atlas_full":texture_atlas_full,
        "txd_parent_count":len(txd_parents),
        "atlas":[atlas.w,atlas.h],
        "atlas_bytes":atlas.w*atlas.h*2,
        "atlas_bmp":str(atlas_bmp),
        "models":model_stats,
        "vcmap_bin":str(out_bin),
        "vcmap_bytes":out_bin.stat().st_size,
        "spawn_gta_xyz":[spawn_x,spawn_z,spawn_y],
        "spawn_racer_x_y_z_unscaled":[spawn_x,spawn_y,spawn_z],
        "spawn_source":spawn_source,
        "spawn_up_alignment":spawn_up,
        "spawn_col_material":spawn_col_material,
        "spawn_col_model":spawn_col_model,
        "spawn_surface_kind":spawn_surface_kind,
        "spawn_model_kind":spawn_model_kind,
        "collision_files":len(col_by_id) if col_by_id else 0,
        "collision_models_used":col_models_used,
        "collision_faces_considered":col_face_count,
        "collision_faces_spawn_eligible":col_eligible_faces,
        "collision_rejected_materials":col_rejected_materials,
        "collision_parse_errors":col_errors,
        "collision_sidecar":collision_sidecar,
        "map_bounds_unscaled":[map_min_x,map_max_x,map_min_z,map_max_z],
        "region":region_name,
    }
    out_report.parent.mkdir(parents=True,exist_ok=True)
    out_report.write_text(json.dumps(report,indent=2),encoding="utf-8")
    print(
        "VC_LOCAL_PACK_OK",
        f"format=VCM3",f"region={region_name or 'radius'}",f"selected={len(selected)}",f"packed={used}",
        f"models={len(model_stats)}",f"triangles={len(allt)}",
        f"vertices={len(allv)}",f"sectors={len(metas)}",
        f"lod_skipped={skipped_lod_meshes}/{skipped_lod_triangles}",
        f"visual_bad={rejected_visual_pathological}",
        f"textures={len(texture_stats)}",f"materials={len(materials)}",
        f"formats={dict(texture_format_stats)}",
        f"txdp={len(txd_parents)}",f"parent_hits={texture_parent_hits}",
        f"shared_hits={texture_shared_hits}",f"mask_hits={texture_mask_hits}",
        f"missing_textures={len(texture_missing)}",f"atlas_full={texture_atlas_full}",
        f"atlas={atlas.w}x{atlas.h}",f"atlas_bmp={atlas_bmp}",
        f"spawn={spawn_source}:{spawn_x:.2f},{spawn_y:.2f},{spawn_z:.2f}",
        f"surface={spawn_surface_kind}:mat{spawn_col_material}:{spawn_col_model}:{spawn_model_kind}",
        f"collision_tris={collision_sidecar['triangles']}",
        f"collision_sectors={collision_sidecar['sectors']}",
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
        "model_files":world.get("model_files",[]),
        "hier_files":world.get("hier_files",[]),
        "col_files":world.get("col_files",[]),
        "ide_objects":len(ide),
        "txd_parents":world.get("txd_parents",{}),
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
        f"models={len(models)}",f"imgs={len(world['img_files'])}",
        f"model_files={len(world.get('model_files',[]))}",
        f"hier_files={len(world.get('hier_files',[]))}",
        f"cols={len(world.get('col_files',[]))}",
        f"txdp={len(world.get('txd_parents',{}))}"
    )


def _placement_transform_selftest():
    # reVC: axis Z, w=cos(45deg), negative 90deg rotation.
    q=(0.0,0.0,math.sqrt(0.5),math.sqrt(0.5))
    p=qrot_vc_ipl(q,(1.0,0.0,0.0))
    if abs(p[0])>1.0e-5 or abs(p[1]+1.0)>1.0e-5 or abs(p[2])>1.0e-5:
        raise RuntimeError(f"VC IPL transform regression: {p}")


def main():
    _placement_transform_selftest()
    ap=argparse.ArgumentParser()
    ap.add_argument("--game-root",required=True,help="Your local GTA Vice City installation folder")
    ap.add_argument("--extracted-root",default="",
                    help="Optional extracted gta3.img directory; preferred for DFF/TXD/COL reads")
    ap.add_argument("--inventory-only",action="store_true")
    ap.add_argument("--center-x",type=float,default=0.0)
    ap.add_argument("--center-y",type=float,default=0.0,help="GTA world Y (horizontal), not height")
    ap.add_argument("--radius",type=float,default=350.0,help="Import radius in GTA world units")
    ap.add_argument("--region",choices=sorted(VC_REGION_PRESETS),default=None,
                    help="Named coherent Vice City region; overrides center/radius selection")
    ap.add_argument("--interior",type=int,default=0)
    ap.add_argument("--sector-m",type=float,default=24.0)
    ap.add_argument("--world-scale",type=float,default=240.0)
    ap.add_argument("--max-instances",type=int,default=0,help="0 = no artificial cap")
    ap.add_argument("--output-header",default="build/vc-local/vc_city_map.h")
    ap.add_argument("--output-bin",default="build/vc-local/VCMAP.BIN")
    ap.add_argument("--output-report",default="build/vc-local/vc_city_report.json")
    args=ap.parse_args()

    root=Path(args.game_root).resolve()
    if not root.exists():
        raise SystemExit(f"game root does not exist: {root}")

    if args.extracted_root:
        extracted_root=Path(args.extracted_root).resolve()
    else:
        auto_extracted=root/"models"/"gta3"
        extracted_root=auto_extracted.resolve() if auto_extracted.exists() else None
    if extracted_root and not extracted_root.exists():
        print(f"[vc-import] WARN extracted root not found; IMG fallback only: {extracted_root}",file=sys.stderr)
        extracted_root=None

    world=discover_map(root,extracted_root)
    if not world["instances"]:
        raise SystemExit("no IPL instances found; verify this is a PC Vice City installation")

    if args.inventory_only:
        inventory_only(world,Path(args.output_report))
        return

    if Img is None or Dff is None or Txd is None or Col is None:
        raise SystemExit("rwfury missing; install locally with: py -m pip install rwfury")

    col_by_id,col_by_name,col_errors=load_collision_models(
        [Path(x) for x in world.get("col_files",[])]
    )
    print(
        "VC_COLLISION_INDEX_OK",
        f"files={len(world.get('col_files',[]))}",
        f"ids={len(col_by_id)}",f"names={len(col_by_name)}",
        f"errors={len(col_errors)}"
    )
    for err in col_errors:
        print("VC_COLLISION_INDEX_ERROR",err)

    region_name=args.region or ""
    if args.region:
        preset=VC_REGION_PRESETS[args.region]
        center=tuple(preset["spawn"])
        if "radius" in preset:
            region_center=tuple(preset.get("center",center))
            selected=choose_instances(
                world["instances"],world["ide"],
                region_center,float(preset["radius"]),args.interior
            )
            extent=f"center={region_center[0]:.3f},{region_center[1]:.3f} radius={preset['radius']:.0f}"
        else:
            selected=choose_instances_box(
                world["instances"],world["ide"],preset["bounds"],args.interior
            )
            extent=(
                f"bounds={preset['bounds'][0]:.0f},{preset['bounds'][1]:.0f},"
                f"{preset['bounds'][2]:.0f},{preset['bounds'][3]:.0f}"
            )
        print(
            "VC_REGION_PRESET",
            f"name={args.region}",
            f"label={preset['label']}",
            extent,
            f"spawn_hint={center[0]:.3f},{center[1]:.3f}",
            f"instances={len(selected)}"
        )
    else:
        center=(args.center_x,args.center_y)
        selected=choose_instances(
            world["instances"],world["ide"],
            center,args.radius,args.interior
        )
    if not selected:
        raise SystemExit("no instances in requested region/radius/interior")

    local_stats=collision_match_stats(selected,col_by_id,col_by_name)
    scope=(f"region:{args.region}" if args.region
           else f"r{args.radius:g}@{center[0]:.1f},{center[1]:.1f}")
    print_collision_stats(scope,local_stats)
    local_spawn,_,_,local_eligible,local_rejected=collision_spawn_candidates(
        selected,col_by_id,col_by_name,center
    )
    print(
        "VC_SPAWN_SURFACES",
        f"scope=local",f"eligible={local_eligible}",
        f"rejected={local_rejected}"
    )

    if args.region and local_spawn:
        # Named development regions must start on a real semantic road. Re-center
        # the spawn hint on the best candidate so pack_city cannot fall back to
        # the geometric centre under a building/terrain slab.
        best=min(local_spawn,key=lambda q:(q[10],q[0],q[1],q[2]))
        center=(best[3],best[5])
        print(
            "VC_REGION_SPAWN_OK",
            f"name={args.region}",
            f"center={center[0]:.2f},{center[1]:.2f}",
            f"surface_y={best[4]:.2f}",
            f"material={best[7]}",
            f"model={best[8]}",
            f"kind={best[9]}"
        )
    elif args.region and not local_spawn:
        raise SystemExit(
            f"region {args.region!r} has no semantic road collision spawn; "
            "refusing to write a fallback spawn under the city"
        )

    # Default (0,0) is often water / between islands in Vice City. If the
    # requested neighborhood has no horizontal COL face, progressively widen
    # the search and re-center the actual import on the nearest valid collision
    # surface. User-specified non-zero centers remain authoritative.
    if (not args.region and not local_spawn and
        abs(args.center_x)<1.0e-6 and abs(args.center_y)<1.0e-6):
        best_global=None
        best_global_key=None

        for probe_radius in (300.0,600.0,1200.0,2400.0,5000.0):
            probe=choose_instances(
                world["instances"],world["ide"],
                (0.0,0.0),probe_radius,args.interior
            )
            stats=collision_match_stats(probe,col_by_id,col_by_name)
            print_collision_stats(f"probe{probe_radius:g}",stats)

            candidates,_,_,eligible,rejected=collision_spawn_candidates(
                probe,col_by_id,col_by_name,(0.0,0.0)
            )
            print(
                "VC_SPAWN_SURFACES",
                f"scope=probe{probe_radius:g}",
                f"eligible={eligible}",f"rejected={rejected}"
            )

            dense=choose_dense_spawn(
                candidates,world["instances"],world["ide"],
                args.radius,args.interior
            )
            if not dense:
                continue

            candidate,density=dense
            (
                prio,dist2,neg_area,
                auto_x,auto_y,auto_z,auto_up,
                auto_mat,auto_model,auto_kind,
                auto_model_rank,auto_model_kind
            )=candidate

            # Do not stop on the first generic surface. Search all probe radii
            # and choose the strongest semantic road candidate globally.
            key=(
                auto_model_rank,  # named-road -> IDE road flag -> generic
                prio,             # street/road material before concrete
                -density,         # denser city neighborhood is better
                dist2,
                neg_area,
                probe_radius
            )

            print(
                "VC_AUTO_CENTER_CANDIDATE",
                f"probe_radius={probe_radius:.0f}",
                f"center={auto_x:.2f},{auto_z:.2f}",
                f"surface_y={auto_y:.2f}",
                f"model={auto_model}",
                f"material={auto_mat}",
                f"kind={auto_kind}",
                f"model_kind={auto_model_kind}",
                f"density={density}",
                f"rank={auto_model_rank}"
            )

            if best_global_key is None or key<best_global_key:
                best_global_key=key
                best_global=(candidate,density,probe_radius)

            # Keep probing the whole map: a later named road can be much denser
            # and visually more representative than the first valid street.

        if best_global:
            best,density,probe_radius=best_global
            (
                _,_,_,
                auto_x,auto_y,auto_z,auto_up,
                auto_mat,auto_model,auto_kind,
                auto_model_rank,auto_model_kind
            )=best
            center=(auto_x,auto_z)
            print(
                "VC_AUTO_CENTER_OK",
                f"probe_radius={probe_radius:.0f}",
                f"center={center[0]:.2f},{center[1]:.2f}",
                f"surface_y={auto_y:.2f}",
                f"model={auto_model}",
                f"material={auto_mat}",
                f"kind={auto_kind}",
                f"model_kind={auto_model_kind}",
                f"density={density}",
                f"up={auto_up:.3f}"
            )
            selected=choose_instances(
                world["instances"],world["ide"],
                center,args.radius,args.interior
            )

    final_stats=collision_match_stats(selected,col_by_id,col_by_name)
    print_collision_stats(f"final@{center[0]:.1f},{center[1]:.1f}",final_stats)

    archives=ArchiveSet([Path(x) for x in world["img_files"]],extracted_root)
    pack_city(
        selected,archives,world.get("txd_parents",{}),col_by_id,col_by_name,col_errors,
        Path(args.output_header),Path(args.output_bin),Path(args.output_report),
        args.sector_m,args.world_scale,args.max_instances,
        center,region_name
    )


if __name__=="__main__":
    main()
