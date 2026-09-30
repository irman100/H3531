#!/usr/bin/env python3
"""
Local-only GTA Vice City vehicle -> Stayplaytion VCVEH.BIN packer.

No Rockstar/GTA asset is embedded in this source file or uploaded to GitHub.
The tool reads a vehicle DFF/TXD and handling.cfg from the user's own local
Vice City installation and emits a compact runtime sidecar for Racer.

Default reference car: sentinel (balanced four-door baseline).
"""
from __future__ import annotations

import argparse
import json
import math
import struct
from dataclasses import dataclass
from pathlib import Path

import vc_local_import as base


@dataclass
class VehicleDef:
    ident: int
    model: str
    txd: str
    vehicle_type: str
    handling: str


def parse_vehicle_defs(paths: list[Path]) -> dict[str, VehicleDef]:
    out: dict[str, VehicleDef] = {}
    for path in paths:
        for section, p in base.parse_sectioned_text(path):
            if section != "cars" or len(p) < 5:
                continue
            try:
                item = VehicleDef(
                    ident=int(p[0]),
                    model=p[1].strip(),
                    txd=p[2].strip(),
                    vehicle_type=p[3].strip(),
                    handling=p[4].strip(),
                )
            except (ValueError, IndexError):
                continue
            out[item.model.lower()] = item
    return out


def parse_handling(path: Path, name: str) -> dict[str, float | str]:
    if not path.exists():
        raise SystemExit(f"handling.cfg not found: {path}")
    target=name.strip().upper()
    for raw in path.read_text(encoding="latin-1",errors="ignore").splitlines():
        line=raw.strip()
        if not line or line.startswith((";", "#", "!", "$", "%", "^")):
            continue
        fields=line.split()
        if not fields or fields[0].upper()!=target:
            continue
        if len(fields)<31:
            raise SystemExit(f"handling row {target} is too short ({len(fields)} fields)")
        try:
            return {
                "name":fields[0],
                "mass":float(fields[1]),
                "dim_x":float(fields[2]),
                "dim_y":float(fields[3]),
                "dim_z":float(fields[4]),
                "com_x":float(fields[5]),
                "com_y":float(fields[6]),
                "com_z":float(fields[7]),
                "traction_mult":float(fields[9]),
                "traction_loss":float(fields[10]),
                "traction_bias":float(fields[11]),
                "gears":int(float(fields[12])),
                "max_velocity_kmh":float(fields[13]),
                "engine_accel_raw":float(fields[14]),
                "drive_type":fields[15],
                "engine_type":fields[16],
                "brake_decel_raw":float(fields[17]),
                "brake_bias":float(fields[18]),
                "steering_lock_deg":float(fields[20]),
            }
        except (ValueError,IndexError) as exc:
            raise SystemExit(f"cannot parse handling row {target}: {exc}")
    raise SystemExit(f"handling id {target!r} not found in {path}")


def load_txd_from_archives(archives: base.ArchiveSet, name: str):
    raw, archive=archives.read(name+".txd")
    if raw is None:
        return None, archive
    txd=base.Txd.from_bytes(raw)
    base.annotate_d3d8_txd_hints(raw,txd)
    return txd, archive


def pack_vehicle(game_root: Path, model_name: str, out_bin: Path, out_report: Path,
                 world_scale: float=240.0, atlas_w: int=512, atlas_h: int=512):
    if base.Img is None or base.Dff is None or base.Txd is None:
        raise SystemExit("rwfury missing; install locally with: py -m pip install rwfury")

    world=base.discover_map(game_root)
    defs=parse_vehicle_defs([Path(p) for p in world["ide_files"]])
    meta=defs.get(model_name.lower())
    if meta is None:
        names=", ".join(sorted(defs)[:40])
        raise SystemExit(f"vehicle model {model_name!r} not found in IDE cars sections; examples: {names}")

    handling_path=base.find_case(game_root,"data/handling.cfg")
    if handling_path is None:
        raise SystemExit("data/handling.cfg not found")
    handling=parse_handling(handling_path,meta.handling)

    archives=base.ArchiveSet([Path(x) for x in world["img_files"]])
    raw_dff,dff_archive=archives.read(meta.model+".dff")
    if raw_dff is None:
        raise SystemExit(f"{meta.model}.dff not found in IMG archives")
    dff=base.Dff.from_bytes(raw_dff)
    meshes=dff.to_generic_meshes()
    transforms=base.dff_generic_mesh_world_transforms(dff)

    atlas=base.TextureAtlas(atlas_w,atlas_h,128)
    materials=[]
    material_cache={}
    txd_cache={}
    texture_report={}

    def txd_table(name):
        key=(name or "").strip().lower()
        if key in txd_cache:
            return txd_cache[key]
        txd,archive=load_txd_from_archives(archives,key)
        if txd is None:
            txd_cache[key]=({},archive)
        else:
            txd_cache[key]=({t.name.lower():t for t in txd.textures},archive)
        return txd_cache[key]

    # Vehicle-specific textures usually fall back to the common vehicle TXD.
    search_txd=[]
    for n in (meta.txd,"vehicle","generic","particle"):
        n=(n or "").strip().lower()
        if n and n not in search_txd:
            search_txd.append(n)

    def resolve_texture(name):
        key=(name or "").strip().lower()
        for txd_name in search_txd:
            table,archive=txd_table(txd_name)
            tex=table.get(key)
            if tex is not None:
                return tex,archive,txd_name
        return None,None,None

    def solid_material(color):
        key=("solid",int(color)&0xffff)
        if key in material_cache:
            return material_cache[key]
        # Give every solid material a real 1x1 atlas texel so the runtime can
        # use one affine textured path for textured and fallback car surfaces.
        r=((color>>10)&31)*255//31
        g=((color>>5)&31)*255//31
        b=(color&31)*255//31
        slot=atlas.add_rgba(bytes((r,g,b,255)),1,1)
        if slot is None:
            raise SystemExit("vehicle atlas full while adding solid material")
        x,y,w,h,_=slot
        idx=len(materials)
        materials.append((x,y,w,h,color,1,0))
        material_cache[key]=idx
        return idx

    def mesh_material(mesh, mesh_no):
        texname=(getattr(mesh,"texture_name","") or "").strip().lower()
        maskname=(getattr(mesh,"mask_name","") or "").strip().lower()
        fallback=base.diffuse1555(getattr(mesh,"diffuse_color",None),meta.model,mesh_no)
        if not texname:
            return solid_material(fallback)

        key=(texname,maskname)
        if key in material_cache:
            return material_cache[key]
        tex,archive,resolved=resolve_texture(texname)
        if tex is None:
            return solid_material(fallback)
        mips,has_alpha,fmt=base.decode_txd_texture_rgba(tex)
        rgba=mips[0] if mips else b""

        # Vehicle masks are treated as 1-bit alpha when a separate mask texture
        # is available.  This is enough for windows/grilles at 640x360.
        if maskname and maskname!=texname:
            mask_tex,_,_=resolve_texture(maskname)
            if mask_tex is not None:
                mmips,mhas,_=base.decode_txd_texture_rgba(mask_tex)
                if mmips:
                    src=bytearray(rgba)
                    mask=mmips[0]
                    mw=max(1,int(mask_tex.width));mh=max(1,int(mask_tex.height))
                    bw=max(1,int(tex.width));bh=max(1,int(tex.height))
                    for yy in range(bh):
                        my=min(mh-1,int((yy+0.5)*mh/bh))
                        for xx in range(bw):
                            mx=min(mw-1,int((xx+0.5)*mw/bw))
                            si=(yy*bw+xx)*4
                            mi=(my*mw+mx)*4
                            ma=mask[mi+3] if mhas else (mask[mi]+mask[mi+1]+mask[mi+2])//3
                            if ma<src[si+3]:
                                src[si+3]=ma
                    rgba=bytes(src)
                    has_alpha=True

        slot=atlas.add_rgba(rgba,int(tex.width),int(tex.height))
        if slot is None:
            return solid_material(fallback)
        x,y,w,h,seen_alpha=slot
        idx=len(materials)
        materials.append((x,y,w,h,fallback,1|(2 if (has_alpha or seen_alpha) else 0),0))
        material_cache[key]=idx
        texture_report[texname]={
            "resolved_txd":resolved,
            "archive":archive,
            "format":fmt,
            "source":[int(tex.width),int(tex.height)],
            "atlas":[x,y,w,h],
            "alpha":bool(has_alpha or seen_alpha),
        }
        return idx

    verts=[]
    tris=[]
    mesh_report=[]
    for mi,mesh in enumerate(meshes):
        mverts=base.positions_iter(mesh.positions)
        muvs=base.texcoords_iter(mesh)
        mtris=base.indices_iter(mesh.indices)
        if len(muvs)<len(mverts):
            muvs=muvs+[(0.0,0.0)]*(len(mverts)-len(muvs))
        transform=transforms[mi] if mi<len(transforms) else getattr(mesh,"transform",None)
        mat=mesh_material(mesh,mi)
        vb=len(verts)

        for vi,p in enumerate(mverts):
            x,y,z=base.apply_mat4_row_major(transform,p)
            # RenderWare/GTA: X right, Y forward, Z up.
            # Racer: X right, Y up, Z forward.
            u,v=muvs[vi]
            verts.append((x*world_scale,z*world_scale,y*world_scale,float(u),float(v)))

        for a,b,c in mtris:
            if max(a,b,c)>=len(mverts):
                continue
            aa=vb+a;bb=vb+b;cc=vb+c
            if max(aa,bb,cc)>65535:
                raise SystemExit("vehicle vertex index exceeds uint16")
            tris.append((aa,bb,cc,mat,0))

        mesh_report.append({
            "mesh":mi,
            "vertices":len(mverts),
            "triangles":len(mtris),
            "texture":getattr(mesh,"texture_name","") or "",
            "mask":getattr(mesh,"mask_name","") or "",
            "material":mat,
        })

    if not verts or not tris:
        raise SystemExit(f"{meta.model}.dff produced no renderable geometry")
    if len(materials)>255:
        raise SystemExit("vehicle material count exceeds uint8")

    # VCV1: fixed header followed by vc_material_t[], vc_vertex_t[], vc_tri_t[], atlas.
    # Header = 4s + version + 5 counts + 16 floats = 92 bytes.
    hf=[
        float(world_scale),
        float(handling["mass"]),
        float(handling["traction_mult"]),
        float(handling["traction_loss"]),
        float(handling["traction_bias"]),
        float(handling["max_velocity_kmh"]),
        float(handling["engine_accel_raw"]),
        float(handling["brake_decel_raw"]),
        float(handling["brake_bias"]),
        float(handling["steering_lock_deg"]),
        float(handling["com_x"]),float(handling["com_y"]),float(handling["com_z"]),
        float(handling["dim_x"]),float(handling["dim_y"]),float(handling["dim_z"]),
    ]

    out_bin.parent.mkdir(parents=True,exist_ok=True)
    with out_bin.open("wb") as fp:
        fp.write(struct.pack(
            "<4sI5I16f",b"VCV1",1,
            len(verts),len(tris),len(materials),atlas.w,atlas.h,*hf
        ))
        for m in materials:
            fp.write(struct.pack("<HHHHHBB",*m))
        for v in verts:
            fp.write(struct.pack("<5f",*v))
        for t in tris:
            fp.write(struct.pack("<HHHBB",*t))
        for px in atlas.pixels:
            fp.write(struct.pack("<H",px&0xffff))

    atlas_bmp=out_bin.with_name("vc_vehicle_atlas.bmp")
    base.write_atlas_bmp(atlas_bmp,atlas)

    report={
        "format":"VCV1",
        "model":meta.model,
        "id":meta.ident,
        "txd":meta.txd,
        "vehicle_type":meta.vehicle_type,
        "handling_id":meta.handling,
        "handling":handling,
        "dff_archive":dff_archive,
        "vertices":len(verts),
        "triangles":len(tris),
        "materials":len(materials),
        "atlas":[atlas.w,atlas.h],
        "atlas_bmp":str(atlas_bmp),
        "textures":texture_report,
        "meshes":mesh_report,
        "output":str(out_bin),
        "bytes":out_bin.stat().st_size,
    }
    out_report.parent.mkdir(parents=True,exist_ok=True)
    out_report.write_text(json.dumps(report,indent=2),encoding="utf-8")

    print(
        "VC_VEHICLE_PACK_OK",
        f"model={meta.model}",
        f"handling={meta.handling}",
        f"vertices={len(verts)}",
        f"triangles={len(tris)}",
        f"materials={len(materials)}",
        f"atlas={atlas.w}x{atlas.h}",
        f"bytes={out_bin.stat().st_size}",
    )


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--game-root",required=True)
    ap.add_argument("--model",default="sentinel")
    ap.add_argument("--world-scale",type=float,default=240.0)
    ap.add_argument("--atlas-w",type=int,default=512)
    ap.add_argument("--atlas-h",type=int,default=512)
    ap.add_argument("--output-bin",default="build/vc-local/VCVEH.BIN")
    ap.add_argument("--output-report",default="build/vc-local/vc_vehicle_report.json")
    args=ap.parse_args()

    root=Path(args.game_root).resolve()
    if not root.exists():
        raise SystemExit(f"game root does not exist: {root}")
    pack_vehicle(
        root,args.model,Path(args.output_bin),Path(args.output_report),
        args.world_scale,args.atlas_w,args.atlas_h
    )


if __name__=="__main__":
    main()
