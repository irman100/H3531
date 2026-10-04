#!/usr/bin/env python3
from __future__ import annotations
import argparse,json,math,struct,time
from collections import defaultdict
from pathlib import Path
import vc_local_import as vc

MAGIC=b"VFW1"
VERSION=1
HEADER_FMT="<4sIff4f4iII"
ENTRY_FMT="<iiIIIIIIII"

def page_key(x,y,page_m):
    return math.floor(x/page_m),math.floor(y/page_m)

def page_dir_name(px,py):
    return f"P_{px}_{py}"

def is_named_lod(meta):
    ml=(meta.model or "").strip().lower()
    return ml.startswith("lod") or ml.endswith("_lod") or "_lod_" in ml

def is_stream_base(meta):
    """
    Approximate reVC's persistent/background world layer using the same source
    metadata GTA uses:
      - IDE flag bit 0: wet-road/road geometry;
      - IDE flag 0x100: ignore draw distance;
      - draw distance > 300: SetupBigBuilding() threshold in reVC;
      - named LOD helpers.
    Everything else becomes a streamed detail model.
    """
    largest=max(meta.lod_distances) if getattr(meta,"lod_distances",()) else float(meta.draw_distance)
    return bool((meta.flags & 1) or (meta.flags & 0x100) or largest>300.0 or is_named_lod(meta))

def write_world_index(path,page_m,sector_m,bounds,entries):
    minpx=min((e["page_x"] for e in entries),default=0)
    maxpx=max((e["page_x"] for e in entries),default=0)
    minpy=min((e["page_y"] for e in entries),default=0)
    maxpy=max((e["page_y"] for e in entries),default=0)
    path.parent.mkdir(parents=True,exist_ok=True)
    with path.open("wb") as fp:
        fp.write(struct.pack(
            HEADER_FMT,MAGIC,VERSION,float(page_m),float(sector_m),
            float(bounds[0]),float(bounds[1]),float(bounds[2]),float(bounds[3]),
            int(minpx),int(maxpx),int(minpy),int(maxpy),len(entries),0
        ))
        for e in entries:
            fp.write(struct.pack(
                ENTRY_FMT,
                int(e["page_x"]),int(e["page_y"]),
                int(e["instances"]),int(e["vcmap_bytes"]),int(e["vccol_bytes"]),
                int(e["vertices"]),int(e["triangles"]),int(e["materials"]),
                int(e["atlas_w"]),int(e["atlas_h"])
            ))

def load_existing_report(page_dir):
    p=page_dir/"page_report.json"
    if not p.exists() or not (page_dir/"VCMAP.BIN").exists() or not (page_dir/"VCCOL.BIN").exists():
        return None
    try:
        report=json.loads(p.read_text(encoding="utf-8"))
    except Exception:
        return None
    # Old VFW1 pages baked roads/buildings together. Force a rebuild once so
    # --resume cannot silently keep the pre-GTA-streaming layout.
    if report.get("streaming_layout")!="gta-base-detail-v1":
        return None
    base=report.get("stream_base",{})
    if base.get("present") and not (page_dir/"VCBASE.BIN").exists():
        return None
    return report

def main():
    ap=argparse.ArgumentParser(description="Local-only Vice City -> paged Stayplaytion full-world pack")
    ap.add_argument("--game-root",required=True)
    ap.add_argument("--extracted-root",default="")
    ap.add_argument("--output-dir",default="build/vc-full-pack")
    ap.add_argument("--page-m",type=float,default=192.0)
    ap.add_argument("--sector-m",type=float,default=24.0)
    ap.add_argument("--world-scale",type=float,default=240.0)
    ap.add_argument("--interior",type=int,default=0)
    ap.add_argument("--atlas-size",type=int,default=1024)
    ap.add_argument("--texture-max",type=int,default=40)
    ap.add_argument("--resume",action="store_true")
    ap.add_argument(
        "--repair-missing-textures",action="store_true",
        help="Reuse clean pages but rebuild pages whose page_report.json still lists texture_missing"
    )
    ap.add_argument("--max-pages",type=int,default=0,help="0 = all pages; useful for local smoke tests")
    ap.add_argument("--only-page",default="",help="Optional 'x,y' page coordinate")
    ap.add_argument("--allow-atlas-full",action="store_true")
    args=ap.parse_args()

    root=Path(args.game_root).resolve()
    if not root.exists():
        raise SystemExit(f"game root does not exist: {root}")
    if args.page_m<=0 or args.sector_m<=0:
        raise SystemExit("page/sector size must be positive")
    if args.atlas_size<64 or args.atlas_size>2048:
        raise SystemExit("--atlas-size must be between 64 and 2048")
    if args.texture_max<8 or args.texture_max>128:
        raise SystemExit("--texture-max must be between 8 and 128")

    if args.extracted_root:
        extracted=Path(args.extracted_root).resolve()
    else:
        p=root/"models"/"gta3"
        extracted=p.resolve() if p.exists() else None
    if not extracted or not extracted.exists():
        raise SystemExit("full-world pack requires extracted gta3 assets; pass --extracted-root")

    if vc.Img is None or vc.Dff is None or vc.Txd is None or vc.Col is None:
        raise SystemExit("rwfury missing; install locally with: py -3 -m pip install rwfury")

    out=Path(args.output_dir).resolve()
    pages_root=out/"pages"
    pages_root.mkdir(parents=True,exist_ok=True)

    print("VC_FULL_PACK_DISCOVER",f"root={root}",f"extracted={extracted}")
    world=vc.discover_map(root,extracted)
    ide=world["ide"]
    selected=[]
    for it in world["instances"]:
        if it.interior!=args.interior:
            continue
        meta=ide.get(it.ident)
        if meta is None:
            continue
        selected.append((it,meta))
    if not selected:
        raise SystemExit("no exterior world instances found")

    col_by_id,col_by_name,col_errors=vc.load_collision_models(
        [Path(x) for x in world.get("col_files",[])]
    )
    match=vc.collision_match_stats(selected,col_by_id,col_by_name)
    if match["unmatched"]:
        raise SystemExit(f"collision coverage incomplete: {len(match['unmatched'])} sample unmatched={match['unmatched']}")

    groups=defaultdict(list)
    xs=[];ys=[]
    for it,meta in selected:
        x,y,_=it.pos
        groups[page_key(x,y,args.page_m)].append((it,meta))
        xs.append(x);ys.append(y)

    coords=sorted(groups)
    if args.only_page:
        try:
            sx,sy=args.only_page.split(",",1)
            want=(int(sx.strip()),int(sy.strip()))
        except Exception:
            raise SystemExit("--only-page must be x,y")
        coords=[want] if want in groups else []
    if args.max_pages>0:
        coords=coords[:args.max_pages]
    if not coords:
        raise SystemExit("no pages selected")

    archives=vc.ArchiveSet(
        [Path(x) for x in world["img_files"]],
        extracted,
        [Path(x) for x in world.get("loose_asset_roots",[])]
    )
    asset_cache={}
    entries=[]
    page_reports=[]
    total_atlas_full=0
    total_missing_textures=0
    total_map_bytes=0
    total_base_bytes=0
    total_col_bytes=0
    started=time.time()

    print(
        "VC_FULL_PACK_BEGIN",
        f"pages={len(coords)}",f"world_pages={len(groups)}",
        f"instances={len(selected)}",f"collision_names={len(col_by_name)}"
    )

    for n,(px,py) in enumerate(coords,1):
        pdir=pages_root/page_dir_name(px,py)
        pdir.mkdir(parents=True,exist_ok=True)
        report=None
        if args.resume or args.repair_missing_textures:
            report=load_existing_report(pdir)
            if (args.repair_missing_textures and report and
                (report.get("texture_missing") or report.get("texture_atlas_full",0))):
                print(
                    "VC_FULL_PACK_REPAIR_PAGE",
                    f"page={px},{py}",
                    f"missing={len(report.get('texture_missing',{}))}",
                    f"atlas_full={int(report.get('texture_atlas_full',0) or 0)}"
                )
                report=None
        if report is None:
            center=((px+0.5)*args.page_m,(py+0.5)*args.page_m)
            report_path=pdir/"page_report.json"
            base_report_path=pdir/"page_base_report.json"
            full_group=groups[(px,py)]
            base_group=[pair for pair in full_group if is_stream_base(pair[1])]
            detail_group=[pair for pair in full_group if not is_stream_base(pair[1])]

            # Collision remains complete and independent of visual streaming.
            vc.pack_collision_sidecar(
                full_group,col_by_id,col_by_name,pdir/"VCCOL.BIN",
                args.sector_m,args.world_scale
            )

            # Near/detail layer: ordinary buildings/props. It is the legacy
            # VCMAP filename so old runtimes still open something useful.
            vc.pack_city(
                detail_group,archives,world.get("txd_parents",{}),
                col_by_id,col_by_name,col_errors,
                pdir/"vc_city_map.h",pdir/"VCMAP.BIN",report_path,
                args.sector_m,args.world_scale,0,center,
                region_name=f"full-page-detail:{px},{py}",
                asset_cache=asset_cache,
                atlas_w=args.atlas_size,atlas_h=args.atlas_size,
                texture_max_px=args.texture_max,
                trim_atlas=True,write_debug_artifacts=False,
                write_collision=False,include_named_lods=False
            )
            report=json.loads(report_path.read_text(encoding="utf-8"))

            # Persistent/background layer: roads, GTA big buildings and named
            # LOD helpers. Lower texture resolution keeps it cheap on H3531.
            base_path=pdir/"VCBASE.BIN"
            if base_group:
                vc.pack_city(
                    base_group,archives,world.get("txd_parents",{}),
                    col_by_id,col_by_name,col_errors,
                    pdir/"vc_base_map.h",base_path,base_report_path,
                    args.sector_m,args.world_scale,0,center,
                    region_name=f"full-page-base:{px},{py}",
                    asset_cache=asset_cache,
                    atlas_w=args.atlas_size,atlas_h=args.atlas_size,
                    texture_max_px=max(16,args.texture_max//2),
                    trim_atlas=True,write_debug_artifacts=False,
                    write_collision=False,include_named_lods=True
                )
                base_report=json.loads(base_report_path.read_text(encoding="utf-8"))
            else:
                if base_path.exists():
                    base_path.unlink()
                base_report={"packed_vertices":0,"packed_triangles":0,"materials":0,
                             "atlas":[0,0],"texture_atlas_full":0,"texture_missing":{}}

            report["streaming_layout"]="gta-base-detail-v1"
            report["instances_full"]=len(full_group)
            report["instances_detail"]=len(detail_group)
            report["instances_base"]=len(base_group)
            report["stream_base"]={
                "present":bool(base_group),
                "vcbase_bytes":base_path.stat().st_size if base_path.exists() else 0,
                "packed_vertices":int(base_report.get("packed_vertices",0)),
                "packed_triangles":int(base_report.get("packed_triangles",0)),
                "materials":int(base_report.get("materials",0)),
                "atlas":base_report.get("atlas",[0,0]),
                "texture_atlas_full":int(base_report.get("texture_atlas_full",0)),
                "missing_textures":len(base_report.get("texture_missing",{})),
            }
            report_path.write_text(json.dumps(report,indent=2),encoding="utf-8")

        vcm=pdir/"VCMAP.BIN";vcc=pdir/"VCCOL.BIN";vcbase=pdir/"VCBASE.BIN"
        vcm_bytes=vcm.stat().st_size
        vcbase_bytes=vcbase.stat().st_size if vcbase.exists() else 0
        vcc_bytes=vcc.stat().st_size
        atlas=report.get("atlas",[0,0])
        entry={
            "page_x":px,"page_y":py,
            "instances":len(groups[(px,py)]),
            "vcmap_bytes":vcm_bytes,"vccol_bytes":vcc_bytes,
            "vertices":int(report.get("packed_vertices",0)),
            "triangles":int(report.get("packed_triangles",0)),
            "materials":int(report.get("materials",0)),
            "atlas_w":int(atlas[0] if len(atlas)>0 else 0),
            "atlas_h":int(atlas[1] if len(atlas)>1 else 0),
            "atlas_full":int(report.get("texture_atlas_full",0))+
                         int(report.get("stream_base",{}).get("texture_atlas_full",0)),
            "missing_textures":len(report.get("texture_missing",{}))+
                               int(report.get("stream_base",{}).get("missing_textures",0)),
            "base_bytes":vcbase_bytes,
            "base_triangles":int(report.get("stream_base",{}).get("packed_triangles",0)),
            "path":f"pages/{page_dir_name(px,py)}",
        }
        entries.append(entry);page_reports.append(entry)
        total_map_bytes+=vcm_bytes;total_base_bytes+=vcbase_bytes;total_col_bytes+=vcc_bytes
        total_atlas_full+=entry["atlas_full"]
        total_missing_textures+=entry["missing_textures"]
        print(
            "VC_FULL_PACK_PAGE",
            f"{n}/{len(coords)}",f"page={px},{py}",f"inst={entry['instances']}",
            f"v={entry['vertices']}",f"t={entry['triangles']}",
            f"mat={entry['materials']}",f"atlas={entry['atlas_w']}x{entry['atlas_h']}",
            f"detail={vcm_bytes}",f"base={vcbase_bytes}",f"base_t={entry['base_triangles']}",
            f"col={vcc_bytes}",f"atlas_full={entry['atlas_full']}"
        )

    bounds=(min(xs),min(ys),max(xs),max(ys))
    index_path=out/"VCWORLD.BIN"
    write_world_index(index_path,args.page_m,args.sector_m,bounds,entries)

    report={
        "format":"VFW1","version":VERSION,
        "privacy":{"contains_original_gta_asset_bytes":False,
                   "note":"Generated runtime packs are derived from the user's local game and must remain local."},
        "inputs":{"game_root":str(root),"extracted_root":str(extracted),
                  "page_m":args.page_m,"sector_m":args.sector_m,
                  "world_scale":args.world_scale,"interior":args.interior,
                  "atlas_size":args.atlas_size,"texture_max":args.texture_max},
        "world":{"instances":len(selected),"pages_total":len(groups),
                 "pages_built":len(entries),"bounds_xy":list(bounds)},
        "collision":{"source_files":len(world.get("col_files",[])),
                     "models_by_name":len(col_by_name),"models_by_id":len(col_by_id),
                     "match":match,"parse_errors":col_errors},
        "totals":{"vcmap_detail_bytes":total_map_bytes,"vcbase_bytes":total_base_bytes,
                  "vccol_bytes":total_col_bytes,
                  "world_index_bytes":index_path.stat().st_size,
                  "atlas_full":total_atlas_full,
                  "pages_with_missing_textures":sum(1 for e in entries if e["missing_textures"]),
                  "missing_texture_entries":total_missing_textures,
                  "elapsed_seconds":round(time.time()-started,3)},
        "pages":page_reports,
        "runtime_contract":{
            "layout":"gta-base-detail-v1",
            "active_pages":"3x3 window; runtime loads edge pages incrementally instead of synchronously replacing all three",
            "detail_map":"VCMAP.BIN contains ordinary streamed buildings/props",
            "base_map":"VCBASE.BIN contains IDE wet-road geometry, ignore-draw-distance objects, >300m big buildings and named LOD helpers",
            "collision_format":"VCCOL.BIN remains complete VCC2 and independent from visual LOD",
            "fade":"new detail pages start invisible and fade in after load; base layer is immediately available",
            "source_semantics":"300m big-building threshold and model fade/stream behaviour mirror reVC Renderer/SimpleModelInfo concepts"
        }
    }
    (out/"vc_full_pack_report.json").write_text(json.dumps(report,indent=2),encoding="utf-8")
    (out/"vc_full_pack_summary.txt").write_text(
        "\n".join([
            "VICE CITY PAGED FULL-WORLD PACK",
            "==============================",
            f"Format               : VFW1",
            f"World instances      : {len(selected)}",
            f"Pages available      : {len(groups)}",
            f"Pages built          : {len(entries)}",
            f"Page size             : {args.page_m}",
            f"Sector size           : {args.sector_m}",
            f"VCMAP detail bytes    : {total_map_bytes}",
            f"VCBASE bytes          : {total_base_bytes}",
            f"VCCOL bytes           : {total_col_bytes}",
            f"Index bytes           : {index_path.stat().st_size}",
            f"Atlas-full fallbacks  : {total_atlas_full}",
            f"Pages missing texture : {sum(1 for e in entries if e['missing_textures'])}",
            f"Elapsed seconds       : {round(time.time()-started,3)}",
            "",
            "Generated map/collision packs are derived from the user's local Vice City copy.",
            "Do not upload VCMAP/VCCOL/VCWORLD page outputs to GitHub."
        ])+"\n",encoding="utf-8"
    )

    if total_atlas_full and not args.allow_atlas_full:
        raise SystemExit(
            f"full pack completed but {total_atlas_full} texture entries overflowed page atlases; "
            "rerun with a larger --atlas-size or smaller --texture-max"
        )
    print(
        "VC_FULL_PACK_OK",f"pages={len(entries)}",f"index={index_path}",
        f"detail_bytes={total_map_bytes}",f"base_bytes={total_base_bytes}",
        f"col_bytes={total_col_bytes}",
        f"atlas_full={total_atlas_full}"
    )

if __name__=="__main__":
    main()
