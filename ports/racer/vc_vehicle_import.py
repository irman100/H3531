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
import re
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
    wheel_id: int = -1
    wheel_scale: float = 1.0


@dataclass
class ClumpDef:
    ident: int
    model: str
    txd: str


def parse_clump_defs(paths: list[Path]) -> dict[int, ClumpDef]:
    """Parse reVC LoadClumpObject/HIER entries, including wheel_lightmod."""
    out={}
    for path in paths:
        for section,p in base.parse_sectioned_text(path):
            if section!="hier" or len(p)<3:
                continue
            try:
                item=ClumpDef(int(p[0]),p[1].strip(),p[2].strip())
            except (ValueError,IndexError):
                continue
            out[item.ident]=item
    return out


def parse_vehicle_defs(paths: list[Path]) -> dict[str, VehicleDef]:
    out: dict[str, VehicleDef] = {}
    for path in paths:
        for section, p in base.parse_sectioned_text(path):
            if section != "cars" or len(p) < 5:
                continue
            try:
                vtype=p[3].strip()
                wheel_id=-1
                wheel_scale=1.0
                # reVC LoadVehicleObject reads VC cars as:
                # id model txd type handling game anim class freq level
                # compRules misc wheelScale. For cars, misc is wheel model id.
                if vtype.lower()=="car" and len(p)>=13:
                    wheel_id=int(float(p[11]))
                    wheel_scale=float(p[12])
                item = VehicleDef(
                    ident=int(p[0]),
                    model=p[1].strip(),
                    txd=p[2].strip(),
                    vehicle_type=vtype,
                    handling=p[4].strip(),
                    wheel_id=wheel_id,
                    wheel_scale=wheel_scale,
                )
            except (ValueError, IndexError):
                continue
            out[item.model.lower()] = item
    return out


def model_frame_name_lod(name: str) -> tuple[str,int]:
    """
    Match reVC GetNameAndLOD for DAT MODELFILE atomics.

    Stock Vice City WHEELS.DFF uses names such as wheel_classic_l0.
    reVC strips the trailing _lN and stores the atomic at LOD slot N.
    """
    n=(name or "").strip().lower()
    m=re.search(r"_l(\d+)$",n)
    if m:
        return n[:m.start()],int(m.group(1))
    for suffix in ("_vlo","_dam","_hi","_lo"):
        if n.endswith(suffix):
            n=n[:-len(suffix)]
            break
    return n,0


def model_frame_key(name: str) -> str:
    return model_frame_name_lod(name)[0]


def loose_model_meshes(world, target_model: str):
    """
    Resolve an IDE model from DAT MODELFILE/HIERFILE assets.

    Vice City stock wheels live as named atomics inside MODELS/GENERIC/WHEELS.DFF,
    not as wheel_sport.dff/wheel_saloon.dff files. Modded installs commonly add
    wheel_lightmod through a loose nowheel.DFF MODELFILE. reVC LoadModelFile
    matches those atomics to model info by the frame/node name.
    """
    target=model_frame_key(target_model)
    paths=list(world.get("model_files",[]))+list(world.get("hier_files",[]))
    for raw_path in paths:
        p=Path(raw_path)
        try:
            dff=base.Dff.from_bytes(p.read_bytes())
            meshes=dff.to_generic_meshes()
            names=dff_generic_mesh_frame_names(dff)
            transforms=base.dff_generic_mesh_world_transforms(dff)
        except Exception as exc:
            print(f"[vc-vehicle] WARN loose MODELFILE parse failed {p}: {type(exc).__name__}: {exc}")
            continue
        if len(names)!=len(meshes):
            continue
        selected=[
            i for i,n in enumerate(names)
            if model_frame_key(n)==target
        ]
        if selected:
            # reVC stores MODELFILE atomics by _lN slot. Keep only the
            # highest-detail available slot instead of stacking L0/L1/etc.
            best_lod=min(model_frame_name_lod(names[i])[1] for i in selected)
            selected=[
                i for i in selected
                if model_frame_name_lod(names[i])[1]==best_lod
            ]
            print(
                f"[vc-vehicle] MODELFILE_MATCH model={target_model} "
                f"source={p} lod={best_lod} "
                f"frames={[names[i] for i in selected]}"
            )
            return (
                [meshes[i] for i in selected],
                [transforms[i] if i<len(transforms) else getattr(meshes[i],"transform",None)
                 for i in selected],
                [names[i] for i in selected],
                str(p),
            )
    return None,None,None,None


def dff_generic_mesh_frame_names(dff) -> list[str]:
    """Mirror rwfury generic-mesh split ordering and retain the source frame name."""
    result=[]
    frames=list(getattr(dff,"frames",[]) or [])
    geoms=list(getattr(dff,"geometries",[]) or [])
    for atomic in list(getattr(dff,"atomics",[]) or []):
        gi=int(getattr(atomic,"geometry_index",-1))
        fi=int(getattr(atomic,"frame_index",-1))
        if gi<0 or gi>=len(geoms):
            continue
        frame_name=""
        if 0<=fi<len(frames):
            frame_name=(getattr(frames[fi],"name","") or "")
        geom=geoms[gi]
        bin_mesh=getattr(geom,"bin_mesh",None)
        splits=getattr(bin_mesh,"splits",None) if bin_mesh else None
        if splits:
            flags=int(getattr(bin_mesh,"flags",0))
            for split in splits:
                src=base._expanded_bin_indices(getattr(split,"indices",[]) or [],flags)
                if src and all(0<=int(v)<len(geom.vertices) for v in src):
                    result.append(frame_name)
        else:
            for mat_idx in range(len(getattr(geom,"materials",[]) or [])):
                src=[
                    idx
                    for a,b,c,tri_mat in (getattr(geom,"triangles",[]) or [])
                    if tri_mat==mat_idx
                    for idx in (a,b,c)
                ]
                if src and all(0<=int(v)<len(geom.vertices) for v in src):
                    result.append(frame_name)
    return result


def dff_generic_mesh_wheel_parts(dff) -> list[int]:
    """
    Return runtime wheel part ids aligned with to_generic_meshes().

    DMagic/custom vehicles often put the visible rim/tyre atomic below a
    wheel_*_dummy parent, so looking only at the atomic frame name loses the
    wheel identity. Walk the frame ancestry exactly for that reason.
    """
    result=[]
    frames=list(getattr(dff,"frames",[]) or [])
    geoms=list(getattr(dff,"geometries",[]) or [])

    def part_for_frame(fi: int) -> int:
        seen=set()
        while 0<=fi<len(frames) and fi not in seen:
            seen.add(fi)
            frame=frames[fi]
            part=wheel_part_from_frame(getattr(frame,"name","") or "")
            if part:
                return part
            fi=int(getattr(frame,"parent",-1))
        return 0

    for atomic in list(getattr(dff,"atomics",[]) or []):
        gi=int(getattr(atomic,"geometry_index",-1))
        fi=int(getattr(atomic,"frame_index",-1))
        if gi<0 or gi>=len(geoms):
            continue
        part=part_for_frame(fi)
        geom=geoms[gi]
        bin_mesh=getattr(geom,"bin_mesh",None)
        splits=getattr(bin_mesh,"splits",None) if bin_mesh else None
        if splits:
            flags=int(getattr(bin_mesh,"flags",0))
            for split in splits:
                src=base._expanded_bin_indices(getattr(split,"indices",[]) or [],flags)
                if src and all(0<=int(v)<len(geom.vertices) for v in src):
                    result.append(part)
        else:
            for mat_idx in range(len(getattr(geom,"materials",[]) or [])):
                src=[
                    idx
                    for a,b,c,tri_mat in (getattr(geom,"triangles",[]) or [])
                    if tri_mat==mat_idx
                    for idx in (a,b,c)
                ]
                if src and all(0<=int(v)<len(geom.vertices) for v in src):
                    result.append(part)
    return result


def vehicle_frame_lod(name: str) -> str:
    n=(name or "").strip().lower()
    if "_dam" in n:
        return "damaged"
    if re.search(r"(^|_)vlo($|_)",n):
        return "verylow"
    if re.search(r"(^|_)lo($|_)",n):
        return "low"
    if re.search(r"(^|_)hi($|_)",n):
        return "high"
    return "base"


def keep_vehicle_render_frame(name: str, mode: str="high") -> bool:
    """
    Choose one intact DFF render tier.

    reVC's normal-car renderer treats _hi as normal detail, destroys _lo, and
    retains _vlo for RenderVehicleReallyLowDetailCB. Older importer revisions
    accidentally discarded _vlo and preferred _lo, the opposite of the game's
    own hierarchy.
    """
    n=(name or "").strip().lower()
    lod=vehicle_frame_lod(n)
    if lod=="damaged" or n.startswith("extra"):
        return False
    if mode=="verylow":
        return lod=="verylow"
    if mode=="low":
        return lod=="low" or wheel_part_from_frame(n)!=0
    return lod not in {"low","verylow"}


def wheel_part_from_frame(name: str) -> int:
    """Runtime part ids: 0 body, 1 LF, 2 RF, 3 LR/LB, 4 RR/RB."""
    n=(name or "").strip().lower()
    if "wheel_lf" in n:
        return 1
    if "wheel_rf" in n:
        return 2
    if "wheel_lb" in n or "wheel_lr" in n:
        return 3
    if "wheel_rb" in n or "wheel_rr" in n:
        return 4
    return 0


def dff_frame_world_matrices(dff) -> list[list[float]]:
    frames=list(getattr(dff,"frames",[]) or [])
    cache={}

    def world(i,stack=None):
        if i in cache:
            return cache[i]
        if i<0 or i>=len(frames):
            return [
                1.0,0.0,0.0,0.0,
                0.0,1.0,0.0,0.0,
                0.0,0.0,1.0,0.0,
                0.0,0.0,0.0,1.0,
            ]
        if stack is None:
            stack=set()
        if i in stack:
            raise ValueError(f"DFF frame parent cycle at {i}")
        stack=set(stack);stack.add(i)
        local=base.frame_local_mat(frames[i])
        parent=int(getattr(frames[i],"parent",-1))
        out=base.mat4_mul(local,world(parent,stack)) if parent>=0 else local
        cache[i]=out
        return out

    return [world(i) for i in range(len(frames))]


def vehicle_wheel_dummy_matrices(dff) -> dict[int,list[float]]:
    out={}
    mats=dff_frame_world_matrices(dff)
    for i,frame in enumerate(list(getattr(dff,"frames",[]) or [])):
        name=(getattr(frame,"name","") or "").strip().lower()
        part=wheel_part_from_frame(name)
        if part and "dummy" in name and part not in out:
            out[part]=mats[i]
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
                "suspension_force":float(fields[21]),
                "suspension_damping":float(fields[22]),
                "suspension_upper":float(fields[26]),
                "suspension_lower":float(fields[27]),
                "suspension_bias":float(fields[28]),
                "suspension_antidive":float(fields[29]),
            }
        except (ValueError,IndexError) as exc:
            raise SystemExit(f"cannot parse handling row {target}: {exc}")
    raise SystemExit(f"handling id {target!r} not found in {path}")


def load_txd_anywhere(game_root: Path, archives: base.ArchiveSet, name: str):
    """Load a vehicle TXD from every plausible VC location, tolerating bad candidates."""
    key=(name or "").strip()
    candidates=[]

    # Vice City's shared dictionaries are commonly standalone under models/
    # rather than the archive entry with the same basename. Prefer those first.
    prefer_standalone=key.lower() in {"vehicle","generic","particle","wheels"}

    def add_file(rel):
        p=base.find_case(game_root,rel)
        if p is not None:
            try:
                candidates.append((p.read_bytes(),str(p)))
            except OSError as exc:
                print(f"[vc-vehicle] WARN TXD read failed {p}: {exc}")

    def add_archive():
        raw,archive=archives.read(key+".txd")
        if raw is not None:
            candidates.append((raw,archive or f"<IMG>/{key}.txd"))

    if prefer_standalone:
        add_file(f"models/generic/{key}.txd")
        add_file(f"models/{key}.txd")
        add_archive()
    else:
        add_archive()
        add_file(f"models/{key}.txd")
        add_file(f"models/generic/{key}.txd")

    if not candidates:
        return None,None

    seen=set()
    for raw,source in candidates:
        fingerprint=(len(raw),raw[:32])
        if fingerprint in seen:
            continue
        seen.add(fingerprint)
        try:
            txd=base.Txd.from_bytes(raw)
            base.annotate_d3d8_txd_hints(raw,txd)
            return txd,source
        except Exception as exc:
            print(
                f"[vc-vehicle] WARN TXD parse failed {key} source={source}: "
                f"{type(exc).__name__}: {exc}"
            )

    return None,candidates[-1][1]


def find_vehicle_collision_model(world, model_name: str, model_id: int):
    """
    Find the original Vice City COL model for this vehicle.

    reVC attaches a named CColModel to the vehicle model info and then uses
    model A's collision spheres (plus generated suspension lines) against the
    world. Prefer an exact name match, with model id only as a fallback.
    """
    wanted=base.col_name_key(model_name)
    id_fallback=None
    errors=[]
    if base.Col is None:
        return None,None,["rwfury.Col unavailable"]

    for raw_path in world.get("col_files",[]):
        p=Path(raw_path)
        try:
            col=base.Col.from_file(str(p))
        except Exception as exc:
            errors.append(f"{p}: {type(exc).__name__}: {exc}")
            continue
        for model in col.models:
            name=base.col_name_key(getattr(model,"name",""))
            mid=int(getattr(model,"model_id",-1))
            if name==wanted:
                return model,str(p),errors
            if id_fallback is None and mid==int(model_id):
                id_fallback=(model,str(p))
    if id_fallback is not None:
        return id_fallback[0],id_fallback[1],errors
    return None,None,errors


def build_vehicle_suspension_lines(
    wheel_dummies: dict[int,list[float]],
    handling: dict[str,float | str],
    wheel_scale: float,
    world_scale: float,
):
    """
    Recreate reVC SetupSuspensionLines geometry from the vehicle's wheel dummies.

    Line p0 is the upper suspension position. Line p1 is the lower suspension
    position minus half the tyre diameter. Coordinates are stored in Racer
    local axes and world-scaled units.
    """
    upper=float(handling.get("suspension_upper",0.0))
    lower=float(handling.get("suspension_lower",0.0))
    tyre_half=float(wheel_scale)*0.5
    out=[]
    for part in (1,2,3,4):
        mat=wheel_dummies.get(part)
        if mat is None:
            continue
        x,y,z=base.apply_mat4_row_major(mat,(0.0,0.0,0.0))
        p0=(x,y,z+upper)
        p1=(x,y,z+lower-tyre_half)
        # GTA X/right,Y/forward,Z/up -> Racer X/right,Y/up,Z/forward.
        out.append((
            p0[0]*world_scale,p0[2]*world_scale,p0[1]*world_scale,
            p1[0]*world_scale,p1[2]*world_scale,p1[1]*world_scale,
            part
        ))
    return out


def vehicle_rest_height_world(
    suspension_lines,
    handling: dict[str,float | str],
    wheel_scale: float,
    world_scale: float,
) -> float:
    """
    reVC CAutomobile::SetupSuspensionLines normal road height:
      springLen*(1 - 1/(4*suspensionForce)) - line.p0.z + wheelScale/2
    converted to Racer's world units.
    """
    if not suspension_lines:
        return 21.0
    force=max(0.05,float(handling.get("suspension_force",1.0)))
    upper=float(handling.get("suspension_upper",0.0))
    lower=float(handling.get("suspension_lower",0.0))
    spring_len=upper-lower
    # Stored line p0y is GTA local Z already converted to Racer Y/world units.
    p0z=float(suspension_lines[0][1])/float(world_scale)
    h=(
        spring_len*(1.0-1.0/(4.0*force))
        - p0z
        + float(wheel_scale)*0.5
    )
    return h*float(world_scale)


def pack_vehicle_collision_extension(
    fp, model, world_scale: float, suspension_lines=None,
    rest_height_world: float=21.0, handling=None
):
    """
    Append a compact native CColModel trailer after the legacy VCV1 payload.

    reVC's ProcessColModels uses model A's spheres for body collision, so VCL1
    stores those exact GTA spheres plus the original CColModel bounds. Box and
    triangle counts are retained for audit/future expansion, but are not turned
    into invented body probes.
    """
    suspension_lines=list(suspension_lines or [])
    handling=dict(handling or {})
    if model is None:
        return {
            "extension":None,
            "spheres":0,
            "boxes":0,
            "triangles":0,
            "lines":len(suspension_lines),
        }

    spheres=list(getattr(model,"spheres",None) or [])
    boxes=list(getattr(model,"boxes",None) or [])
    faces=list(getattr(model,"faces",None) or [])
    bounds=getattr(model,"bounds",None)

    if len(spheres)>128:
        raise SystemExit(f"vehicle collision sphere count exceeds reVC bound: {len(spheres)}")

    if bounds is not None:
        cx,cy,cz=getattr(bounds,"center",(0.0,0.0,0.0))
        radius=float(getattr(bounds,"radius",0.0))
        bmin=getattr(bounds,"min",(0.0,0.0,0.0))
        bmax=getattr(bounds,"max",(0.0,0.0,0.0))
    else:
        cx=cy=cz=radius=0.0
        bmin=(0.0,0.0,0.0)
        bmax=(0.0,0.0,0.0)

    # GTA local: X right, Y forward, Z up.
    # Racer local: X right, Y up, Z forward.
    bound_values=[
        float(cx)*world_scale,float(cz)*world_scale,float(cy)*world_scale,
        float(radius)*world_scale,
        float(bmin[0])*world_scale,float(bmin[2])*world_scale,float(bmin[1])*world_scale,
        float(bmax[0])*world_scale,float(bmax[2])*world_scale,float(bmax[1])*world_scale,
    ]

    # VCL3 extends VCL2 with the six native Vice City suspension parameters.
    # Keeping them beside the CColModel data makes the runtime suspension use
    # the selected car's own handling.cfg rather than Racer-wide constants.
    suspension_values=[
        float(handling.get("suspension_force",1.0)),
        float(handling.get("suspension_damping",0.10)),
        float(handling.get("suspension_upper",0.30)),
        float(handling.get("suspension_lower",-0.10)),
        float(handling.get("suspension_bias",0.50)),
        float(handling.get("suspension_antidive",0.0)),
    ]
    fp.write(struct.pack(
        "<4sI4I17f",
        b"VCL3",3,
        len(spheres),len(boxes),len(faces),len(suspension_lines),
        *bound_values,float(rest_height_world),*suspension_values
    ))
    for sphere in spheres:
        x,y,z=getattr(sphere,"center",(0.0,0.0,0.0))
        radius=float(getattr(sphere,"radius",0.0))
        surface=getattr(sphere,"surface",None)
        surf=int(getattr(surface,"material",0))&0xff
        piece=int(getattr(surface,"flag",0))&0xff
        fp.write(struct.pack(
            "<4fBBH",
            float(x)*world_scale,float(z)*world_scale,float(y)*world_scale,
            radius*world_scale,
            surf,piece,0
        ))
    for p0x,p0y,p0z,p1x,p1y,p1z,part in suspension_lines:
        fp.write(struct.pack(
            "<6fBBH",
            float(p0x),float(p0y),float(p0z),
            float(p1x),float(p1y),float(p1z),
            int(part)&0xff,0,0
        ))

    return {
        "extension":"VCL3",
        "rest_height_world":float(rest_height_world),
        "suspension":{
            "force":suspension_values[0],
            "damping":suspension_values[1],
            "upper":suspension_values[2],
            "lower":suspension_values[3],
            "bias":suspension_values[4],
            "antidive":suspension_values[5],
        },
        "spheres":len(spheres),
        "boxes":len(boxes),
        "triangles":len(faces),
        "lines":len(suspension_lines),
        "suspension_lines":[
            {
                "part":int(line[6]),
                "p0":[float(line[0]),float(line[1]),float(line[2])],
                "p1":[float(line[3]),float(line[4]),float(line[5])],
            }
            for line in suspension_lines
        ],
        "bounds":{
            "center":[bound_values[0],bound_values[1],bound_values[2]],
            "radius":bound_values[3],
            "min":[bound_values[4],bound_values[5],bound_values[6]],
            "max":[bound_values[7],bound_values[8],bound_values[9]],
        },
        "sphere_surfaces":[
            int(getattr(getattr(sp,"surface",None),"material",0))&0xff
            for sp in spheres
        ],
    }


def pack_vehicle(game_root: Path, model_name: str, out_bin: Path, out_report: Path,
                 world_scale: float=240.0, atlas_w: int=512, atlas_h: int=512,
                 detail_budget: int=4500):
    if base.Img is None or base.Dff is None or base.Txd is None:
        raise SystemExit("rwfury missing; install locally with: py -m pip install rwfury")

    world=base.discover_map(game_root)
    ide_paths=[Path(p) for p in world["ide_files"]]
    defs=parse_vehicle_defs(ide_paths)
    clumps=parse_clump_defs(ide_paths)
    archives=base.ArchiveSet([Path(x) for x in world["img_files"]])

    requested_model=model_name.strip().lower()
    if requested_model=="auto":
        preferred=[
            "admiral","washington","greenwoo","oceanic","glendale",
            "idaho","manana","virgo","blistac","sentinel"
        ]
        excluded_special={
            "rhino","firetruk","barracks","bus","coach","packer","trash",
            "flatbed","securica","ambulan","fbicar","police","enforcer",
            "hunter","seaspar","sparrow","maverick","vcnmav","dodo",
            "skimmer","predator","speeder","reefer","squalo","tropic",
        }
        ordered=[]
        for name in preferred:
            if name in defs:
                ordered.append(defs[name])

        best=None
        best_key=None
        budget=max(512,min(int(detail_budget),16000))
        passenger_cap=min(14000,max(12000,budget*3))
        target=min(7500,max(3500,int(passenger_cap*0.55)))
        candidate_debug=[]
        for cand in ordered:
            if cand.vehicle_type.lower()!="car" or cand.wheel_id<0:
                continue
            wheel_def=clumps.get(cand.wheel_id) or world["ide"].get(cand.wheel_id)
            if wheel_def is None:
                continue
            raw,_=archives.read(cand.model+".dff")
            if raw is None:
                continue
            try:
                cdff=base.Dff.from_bytes(raw)
                cmeshes=cdff.to_generic_meshes()
                cnames=dff_generic_mesh_frame_names(cdff)
                if len(vehicle_wheel_dummy_matrices(cdff))<4:
                    continue
                counts=[len(base.indices_iter(m.indices)) for m in cmeshes]
                cparts=dff_generic_mesh_wheel_parts(cdff)
                high=sum(counts[i] for i,n in enumerate(cnames)
                         if keep_vehicle_render_frame(n,"high"))
                low=sum(counts[i] for i,n in enumerate(cnames)
                        if keep_vehicle_render_frame(n,"low"))
                low_body=sum(
                    counts[i] for i,n in enumerate(cnames)
                    if vehicle_frame_lod(n)=="low" and
                       (cparts[i] if i<len(cparts) else wheel_part_from_frame(n))==0
                )
                candidate_debug.append((cand.model,high,low,low_body))
                if 500<=high<=passenger_cap:
                    tier=0;tris=high
                elif 300<=low_body and low<=passenger_cap:
                    tier=1;tris=low
                else:
                    continue
                pref_rank=preferred.index(cand.model.lower())
                key=(tier,abs(tris-target),pref_rank,-tris)
                if best_key is None or key<best_key:
                    best_key=key
                    best=(cand,tris,"high" if tier==0 else "low",wheel_def)
            except Exception as exc:
                print(f"[vc-vehicle] AUTO skip {cand.model}: {type(exc).__name__}: {exc}")
        if best is not None:
            meta,auto_tris,auto_tier,auto_wheel=best
            print(
                f"[vc-vehicle] AUTO_MODEL selected={meta.model} tier={auto_tier} "
                f"triangles={auto_tris} wheel={auto_wheel.model} budget={budget} "
                f"class={'passenger' if meta.model.lower() in preferred else 'fallback-car'}"
            )
        else:
            details=", ".join(
                f"{name}:hi={hi}/lo={lo}/lobody={lob}"
                for name,hi,lo,lob in candidate_debug
            )
            raise SystemExit(
                "auto passenger selection found no normal car within "
                f"{passenger_cap} triangles; candidates: {details}"
            )
    else:
        meta=defs.get(requested_model)

    if meta is None:
        names=", ".join(sorted(defs)[:40])
        raise SystemExit(f"vehicle model {model_name!r} not found in IDE cars sections; examples: {names}")

    handling_path=base.find_case(game_root,"data/handling.cfg")
    if handling_path is None:
        raise SystemExit("data/handling.cfg not found")
    handling=parse_handling(handling_path,meta.handling)

    col_model,col_source,col_errors=find_vehicle_collision_model(
        world,meta.model,meta.ident
    )
    if col_model is not None:
        print(
            f"[vc-vehicle] COL model={getattr(col_model,'name',meta.model)} "
            f"source={col_source} spheres={len(getattr(col_model,'spheres',None) or [])} "
            f"boxes={len(getattr(col_model,'boxes',None) or [])} "
            f"triangles={len(getattr(col_model,'faces',None) or [])}"
        )
    else:
        print(f"[vc-vehicle] WARN native COL model not found for {meta.model}")

    raw_dff,dff_archive=archives.read(meta.model+".dff")
    if raw_dff is None:
        raise SystemExit(f"{meta.model}.dff not found in IMG archives")
    dff=base.Dff.from_bytes(raw_dff)
    meshes=dff.to_generic_meshes()
    transforms=base.dff_generic_mesh_world_transforms(dff)
    frame_names=dff_generic_mesh_frame_names(dff)
    frame_wheel_parts=dff_generic_mesh_wheel_parts(dff)
    wheel_dummies=vehicle_wheel_dummy_matrices(dff)
    suspension_lines=build_vehicle_suspension_lines(
        wheel_dummies,handling,meta.wheel_scale,float(world_scale)
    )
    rest_height_world=vehicle_rest_height_world(
        suspension_lines,handling,meta.wheel_scale,float(world_scale)
    )
    wheel_meta=clumps.get(meta.wheel_id) if meta.wheel_id>=0 else None
    if wheel_meta is None and meta.wheel_id>=0:
        # Stock Vice City wheel IDs are OBJS entries fed by WHEELS.DFF via
        # MODELFILE; modded installs may add wheel_lightmod the same way.
        wheel_meta=world["ide"].get(meta.wheel_id)
    print(
        f"[vc-vehicle] SUSPENSION lines={len(suspension_lines)} "
        f"upper={handling['suspension_upper']:.3f} "
        f"lower={handling['suspension_lower']:.3f} "
        f"force={handling['suspension_force']:.3f} "
        f"wheelScale={meta.wheel_scale:.3f} "
        f"restHeight={rest_height_world:.1f}"
    )
    if len(frame_names)!=len(meshes):
        print(
            f"[vc-vehicle] WARN frame split mismatch meshes={len(meshes)} "
            f"frame_names={len(frame_names)}"
        )

    atlas=base.TextureAtlas(atlas_w,atlas_h,128)
    materials=[]
    material_cache={}
    txd_cache={}
    texture_report={}

    def txd_table(name):
        key=(name or "").strip().lower()
        if key in txd_cache:
            return txd_cache[key]
        txd,archive=load_txd_anywhere(game_root,archives,key)
        if txd is None:
            txd_cache[key]=({},archive)
        else:
            txd_cache[key]=({t.name.lower():t for t in txd.textures},archive)
        return txd_cache[key]

    # Vehicle-specific textures usually fall back to the common vehicle TXD.
    search_txd=[]
    wheel_txd=wheel_meta.txd if wheel_meta is not None else ""
    # Stock WHEELS.DFF uses WHEELS.TXD in clean Vice City. Keep this explicit
    # because OBJS wheel definitions do not always preserve the active DAT
    # TEXDICTION relationship in our simplified importer.
    for n in (meta.txd,wheel_txd,"wheels","vehicle","generic","particle"):
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
            print(f"[vc-vehicle] WARN texture unresolved {texname}; using solid fallback")
            return solid_material(fallback)
        try:
            mips,has_alpha,fmt=base.decode_txd_texture_rgba(tex)
            rgba=mips[0] if mips else b""
        except Exception as exc:
            print(
                f"[vc-vehicle] WARN texture decode failed {texname} "
                f"from {resolved}: {type(exc).__name__}: {exc}; using solid fallback"
            )
            return solid_material(fallback)

        # Vehicle masks are treated as 1-bit alpha when a separate mask texture
        # is available.  This is enough for windows/grilles at 640x360.
        if maskname and maskname!=texname:
            mask_tex,_,_=resolve_texture(maskname)
            if mask_tex is not None:
                try:
                    mmips,mhas,_=base.decode_txd_texture_rgba(mask_tex)
                except Exception as exc:
                    print(
                        f"[vc-vehicle] WARN mask decode failed {maskname}: "
                        f"{type(exc).__name__}: {exc}; ignoring mask"
                    )
                    mmips=[];mhas=False
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

    # Pick a hardware-appropriate LOD before material packing. Modded VC
    # installs can replace a classic ~1k-triangle car with a 50k+ model.
    mesh_tri_counts=[len(base.indices_iter(m.indices)) for m in meshes]
    high_triangles=sum(
        mesh_tri_counts[i] for i,n in enumerate(frame_names)
        if keep_vehicle_render_frame(n,"high")
    )
    low_triangles=sum(
        mesh_tri_counts[i] for i,n in enumerate(frame_names)
        if keep_vehicle_render_frame(n,"low")
    )
    low_body_triangles=sum(
        mesh_tri_counts[i] for i,n in enumerate(frame_names)
        if vehicle_frame_lod(n)=="low" and wheel_part_from_frame(n)==0
    )
    verylow_triangles=sum(
        mesh_tri_counts[i] for i,n in enumerate(frame_names)
        if keep_vehicle_render_frame(n,"verylow")
    )
    lod_mode="high"
    budget=max(256,min(int(detail_budget),16000))
    # Prefer a real GTA near/low-detail body when it fits our software-raster
    # budget. The former hard 16k threshold jumped straight from a 40k modded
    # body to the 118-triangle _vlo silhouette and made the player car much too
    # crude. A usable _lo tier in the 0.5k-4.5k range is a much better H3531
    # compromise; _vlo remains the emergency fallback.
    if high_triangles>budget:
        if 0<low_body_triangles and low_triangles<=budget:
            lod_mode="low"
        elif 0<verylow_triangles<high_triangles:
            lod_mode="verylow"
        elif 0<low_body_triangles and low_triangles<high_triangles:
            lod_mode="low"
    print(
        f"[vc-vehicle] AUTO_LOD high_triangles={high_triangles} "
        f"vlo_triangles={verylow_triangles} "
        f"low_triangles={low_triangles} low_body={low_body_triangles} "
        f"budget={budget} selected={lod_mode}"
    )

    verts=[]
    tris=[]
    mesh_report=[]
    skipped_meshes=[]
    source_vertices=0
    source_triangles=0
    for mi,mesh in enumerate(meshes):
        mverts=base.positions_iter(mesh.positions)
        muvs=base.texcoords_iter(mesh)
        mtris=base.indices_iter(mesh.indices)
        if len(muvs)<len(mverts):
            muvs=muvs+[(0.0,0.0)]*(len(mverts)-len(muvs))
        transform=transforms[mi] if mi<len(transforms) else getattr(mesh,"transform",None)
        frame_name=frame_names[mi] if mi<len(frame_names) else ""
        source_vertices+=len(mverts)
        source_triangles+=len(mtris)
        if not keep_vehicle_render_frame(frame_name,lod_mode):
            skipped_meshes.append({
                "mesh":mi,"frame":frame_name,
                "vertices":len(mverts),"triangles":len(mtris)
            })
            continue
        part=(frame_wheel_parts[mi] if mi<len(frame_wheel_parts)
              else wheel_part_from_frame(frame_name))
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
            tris.append((aa,bb,cc,mat,part))

        mesh_report.append({
            "mesh":mi,
            "frame":frame_name,
            "part":part,
            "vertices":len(mverts),
            "triangles":len(mtris),
            "texture":getattr(mesh,"texture_name","") or "",
            "mask":getattr(mesh,"mask_name","") or "",
            "material":mat,
        })

    wheel_report={
        "wheel_id":meta.wheel_id,
        "wheel_scale":meta.wheel_scale,
        "dummy_parts":sorted(wheel_dummies),
        "model":wheel_meta.model if wheel_meta is not None else None,
        "txd":wheel_meta.txd if wheel_meta is not None else None,
        "vertices_added":0,
        "triangles_added":0,
    }

    body_wheel_parts={t[4] for t in tris if t[4] in (1,2,3,4)}
    missing_parts=[p for p in (1,2,3,4) if p in wheel_dummies and p not in body_wheel_parts]
    wheel_lightmod=(
        wheel_meta is not None and
        (getattr(wheel_meta,"model","") or "").strip().lower()=="wheel_lightmod"
    )
    wheel_report["embedded_parts"]=sorted(body_wheel_parts)
    wheel_report["dmwheel_invisible_placeholder"]=bool(wheel_lightmod)

    # DMagic wheel_lightmod/nowheel.DFF is intentionally invisible. Modded
    # cars using ID 249 are expected to carry their visible wheels inside the
    # vehicle DFF under wheel_*_dummy branches. Never fabricate geometry from
    # nowheel.DFF in that case.
    if wheel_lightmod:
        if missing_parts:
            raise SystemExit(
                f"{meta.model}: DMagic wheel_lightmod expects embedded visible wheels, "
                f"but DFF hierarchy is missing runtime wheel parts {missing_parts}; "
                f"embedded_parts={sorted(body_wheel_parts)}"
            )
        print(
            f"[vc-vehicle] DMAGIC_EMBEDDED_WHEELS model={meta.model} "
            f"parts={sorted(body_wheel_parts)} external=none"
        )
    elif wheel_meta is not None and missing_parts:
        raw_wheel,wheel_archive=archives.read(wheel_meta.model+".dff")
        wmeshes=None
        wtrans=None
        wheel_names=None
        if raw_wheel is not None:
            try:
                wdff=base.Dff.from_bytes(raw_wheel)
                wmeshes=wdff.to_generic_meshes()
                wtrans=base.dff_generic_mesh_world_transforms(wdff)
                wheel_names=dff_generic_mesh_frame_names(wdff)
            except Exception as exc:
                print(f"[vc-vehicle] WARN direct wheel DFF parse failed: {type(exc).__name__}: {exc}")
                wmeshes=None
        if wmeshes is None:
            wmeshes,wtrans,wheel_names,wheel_archive=loose_model_meshes(
                world,wheel_meta.model
            )
            if wmeshes is not None:
                print(
                    f"[vc-vehicle] WHEEL_MODELFILE model={wheel_meta.model} "
                    f"source={wheel_archive} meshes={len(wmeshes)}"
                )
        wheel_report["archive"]=wheel_archive
        wheel_report["frames"]=wheel_names or []
        if wmeshes is None:
            print(
                f"[vc-vehicle] WARN wheel geometry not found id={meta.wheel_id} "
                f"model={wheel_meta.model} in IMG or DAT MODELFILE/HIERFILE"
            )
        else:
            try:
                for part in missing_parts:
                    dummy=wheel_dummies[part]
                    for wi,wmesh in enumerate(wmeshes):
                        wverts=base.positions_iter(wmesh.positions)
                        wuvs=base.texcoords_iter(wmesh)
                        wtris=base.indices_iter(wmesh.indices)
                        if len(wtris)>512:
                            print(
                                f"[vc-vehicle] WARN wheel mesh {wi} has {len(wtris)} tris; "
                                "using first 512 for Hi3531"
                            )
                            wtris=wtris[:512]
                        if len(wuvs)<len(wverts):
                            wuvs=wuvs+[(0.0,0.0)]*(len(wverts)-len(wuvs))
                        mat=mesh_material(wmesh,10000+wi)
                        vb=len(verts)

                        # reVC MODELFILE/streaming detaches the wheel atomic and
                        # assigns it a fresh identity frame. The vehicle wheel
                        # node contributes POSITION only; SetRotate then supplies
                        # spin/steer/orientation. Applying WHEELS.DFF's frame and
                        # the full dummy matrix here double-transformed stock
                        # wheels, moving fronts inward and rears outside.
                        dx,dy,dz=base.apply_mat4_row_major(dummy,(0.0,0.0,0.0))
                        for vi,p in enumerate(wverts):
                            wx,wy,wz=(float(p[0]),float(p[1]),float(p[2]))
                            wx*=meta.wheel_scale;wy*=meta.wheel_scale;wz*=meta.wheel_scale

                            # reVC gives left wheels a PI rotation around GTA Z
                            # at rest. Bake that static handedness; runtime still
                            # applies spin and front steering dynamically.
                            if part in (1,3):
                                wx=-wx;wy=-wy

                            x=dx+wx;y=dy+wy;z=dz+wz
                            u,v=wuvs[vi]
                            verts.append((x*world_scale,z*world_scale,y*world_scale,float(u),float(v)))
                        for a,b,c in wtris:
                            if max(a,b,c)>=len(wverts):
                                continue
                            aa=vb+a;bb=vb+b;cc=vb+c
                            if max(aa,bb,cc)>65535:
                                raise SystemExit("vehicle wheel vertex index exceeds uint16")
                            tris.append((aa,bb,cc,mat,part))
                        wheel_report["vertices_added"]+=len(wverts)
                        wheel_report["triangles_added"]+=len(wtris)
                print(
                    f"[vc-vehicle] WHEELS model={wheel_meta.model} "
                    f"parts={missing_parts} vertices={wheel_report['vertices_added']} "
                    f"triangles={wheel_report['triangles_added']}"
                )
            except Exception as exc:
                print(f"[vc-vehicle] WARN wheel import failed: {type(exc).__name__}: {exc}")

    if not verts or not tris:
        raise SystemExit(f"{meta.model}.dff produced no renderable geometry")

    final_wheel_parts={t[4] for t in tris if t[4] in (1,2,3,4)}
    missing_final=[p for p in (1,2,3,4) if p not in final_wheel_parts]
    if missing_final:
        raise SystemExit(
            f"{meta.model}: wheel import incomplete, missing parts {missing_final}; "
            f"wheel_id={meta.wheel_id} wheel_model={getattr(wheel_meta,'model',None)!r}. "
            "Refusing to build a wheel-less player vehicle."
        )
    if requested_model=="auto" and meta.model.lower() in preferred and        not wheel_lightmod and wheel_report.get("triangles_added",0)<=0:
        raise SystemExit(
            f"{meta.model}: passenger AUTO selection has no visible wheel geometry "
            f"(wheel_model={getattr(wheel_meta,'model',None)!r})."
        )
    if len(verts)>12000 or len(tris)>16000:
        print(
            f"[vc-vehicle] WARN selected {lod_mode} LOD still exceeds Hi3531 "
            f"runtime budget vertices={len(verts)}/12000 triangles={len(tris)}/16000"
        )
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
        collision_report=pack_vehicle_collision_extension(
            fp,col_model,float(world_scale),suspension_lines,rest_height_world,handling
        )

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
        "collision_source":col_source,
        "collision_errors":col_errors,
        "collision":collision_report,
        "lod_mode":lod_mode,
        "high_triangles":high_triangles,
        "low_triangles":low_triangles,
        "low_body_triangles":low_body_triangles,
        "verylow_triangles":verylow_triangles,
        "source_vertices":source_vertices,
        "source_triangles":source_triangles,
        "vertices":len(verts),
        "triangles":len(tris),
        "skipped_meshes":skipped_meshes,
        "materials":len(materials),
        "atlas":[atlas.w,atlas.h],
        "atlas_bmp":str(atlas_bmp),
        "textures":texture_report,
        "meshes":mesh_report,
        "wheel_frames":{
            str(part):sorted({m["frame"] for m in mesh_report if m["part"]==part})
            for part in (1,2,3,4)
        },
        "wheel_model":wheel_report,
        "output":str(out_bin),
        "bytes":out_bin.stat().st_size,
    }
    out_report.parent.mkdir(parents=True,exist_ok=True)
    out_report.write_text(json.dumps(report,indent=2),encoding="utf-8")

    print(
        "VC_VEHICLE_PACK_OK",
        f"model={meta.model}",
        f"handling={meta.handling}",
        f"lod={lod_mode}",
        f"source_vertices={source_vertices}",
        f"source_triangles={source_triangles}",
        f"vertices={len(verts)}",
        f"triangles={len(tris)}",
        f"skipped_meshes={len(skipped_meshes)}",
        f"wheel_model={wheel_report.get('model')}",
        f"wheel_tris={wheel_report.get('triangles_added',0)}",
        f"col_spheres={collision_report.get('spheres',0)}",
        f"col_boxes={collision_report.get('boxes',0)}",
        f"col_triangles={collision_report.get('triangles',0)}",
        f"suspension_lines={collision_report.get('lines',0)}",
        f"materials={len(materials)}",
        f"atlas={atlas.w}x{atlas.h}",
        f"bytes={out_bin.stat().st_size}",
    )


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--game-root",required=True)
    ap.add_argument("--model",default="auto")
    ap.add_argument(
        "--detail-budget",type=int,default=4500,
        help="Preferred body triangle budget before falling back to GTA _vlo"
    )
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
        args.world_scale,args.atlas_w,args.atlas_h,args.detail_budget
    )


if __name__=="__main__":
    main()
