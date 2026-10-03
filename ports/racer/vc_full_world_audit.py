#!/usr/bin/env python3
from __future__ import annotations
import argparse,csv,json,math,sys
from collections import Counter,defaultdict
from pathlib import Path
import vc_local_import as vc
try:
    from rwfury import Dff,Txd,Col
except Exception:
    Dff=Txd=Col=None

VERSION=1
LOD_DISTANCE=300.0

def safe_rel(p,root):
    if not p:return None
    p=Path(p)
    try:return p.resolve().relative_to(root.resolve()).as_posix()
    except Exception:return p.name

class ExtractedIndex:
    def __init__(self,root):
        self.root=root
        self.by_name={}
        self.counts=Counter()
        self.bytes=Counter()
        self.duplicates=defaultdict(list)
        if not root or not root.exists():return
        for p in root.rglob("*"):
            if not p.is_file():continue
            ext=p.suffix.lower()
            self.counts[ext]+=1
            try:self.bytes[ext]+=p.stat().st_size
            except OSError:pass
            k=p.name.lower()
            if k in self.by_name:self.duplicates[k].append(p.relative_to(root).as_posix())
            else:self.by_name[k]=p
    def find(self,name):return self.by_name.get(name.lower())
    def report(self):
        return {
            "present":bool(self.root and self.root.exists()),
            "root":str(self.root) if self.root else None,
            "files":sum(self.counts.values()),
            "counts_by_extension":dict(sorted(self.counts.items())),
            "bytes_by_extension":dict(sorted(self.bytes.items())),
            "duplicate_names":dict(sorted(self.duplicates.items())),
        }

class Resolver:
    def __init__(self,game_root,extracted_root,world):
        self.game_root=game_root
        self.extracted=ExtractedIndex(extracted_root)
        self.archives=None
        if vc.Img is not None and world.get("img_files"):
            try:self.archives=vc.ArchiveSet([Path(x) for x in world["img_files"]])
            except Exception as e:print("[vc-full-audit] WARN IMG fallback:",e,file=sys.stderr)
    def read(self,name):
        p=self.extracted.find(name)
        if p:
            try:return p.read_bytes(),f"extracted:{p.name}","extracted"
            except OSError:pass
        for base in (self.game_root/"models",self.game_root):
            p=base/name
            if p.exists() and p.is_file():
                try:return p.read_bytes(),f"loose:{safe_rel(p,self.game_root)}","loose"
                except OSError:pass
        if self.archives:
            raw,archive=self.archives.read(name)
            if raw is not None:return raw,f"img:{Path(archive).name if archive else 'unknown'}","img"
        return None,None,"missing"

def inspect_dff(raw):
    if Dff is None:return {"parsed":False,"error":"rwfury.Dff unavailable"}
    try:
        d=Dff.from_bytes(raw)
        meshes=d.to_generic_meshes()
        atomic_lods=vc.dff_atomic_lods(d)
        atomic_ids=vc.dff_generic_mesh_atomic_indices(d)
        verts=tris=0
        tex=set();masks=set();lod_meshes=Counter()
        for i,m in enumerate(meshes):
            v=vc.positions_iter(m.positions);t=vc.indices_iter(m.indices)
            verts+=len(v);tris+=len(t)
            tn=(getattr(m,"texture_name","") or "").strip().lower()
            mn=(getattr(m,"mask_name","") or "").strip().lower()
            if tn:tex.add(tn)
            if mn:masks.add(mn)
            ai=atomic_ids[i] if i<len(atomic_ids) else 0
            lod=atomic_lods[ai] if 0<=ai<len(atomic_lods) else 0
            lod_meshes[str(lod)]+=1
        return {
            "parsed":True,"atomics":len(getattr(d,"atomics",[]) or []),
            "geometries":len(getattr(d,"geometries",[]) or []),
            "meshes":len(meshes),"atomic_lods":atomic_lods,
            "lod_meshes":dict(lod_meshes),"vertices":verts,"triangles":tris,
            "texture_names":sorted(tex),"mask_names":sorted(masks),
        }
    except Exception as e:return {"parsed":False,"error":str(e)[:500]}

def inspect_txd(raw):
    if Txd is None:return {"parsed":False,"error":"rwfury.Txd unavailable"}
    try:
        t=Txd.from_bytes(raw)
        rows=[]
        for x in list(getattr(t,"textures",[]) or []):
            rows.append({
                "name":(getattr(x,"name","") or "").strip(),
                "width":int(getattr(x,"width",0) or 0),
                "height":int(getattr(x,"height",0) or 0),
                "depth":int(getattr(x,"depth",0) or 0),
                "mips":len(getattr(x,"mipmaps",[]) or []),
            })
        return {
            "parsed":True,"textures":len(rows),
            "max_dimension":max((max(x["width"],x["height"]) for x in rows),default=0),
            "texture_meta":rows,
        }
    except Exception as e:return {"parsed":False,"error":str(e)[:500]}

def relation_key(name):
    s=(name or "").lower()
    return s[3:] if len(s)>3 else s

def page_key(x,y,page_m):return math.floor(x/page_m),math.floor(y/page_m)

def section_counts(path):
    c=Counter()
    try:
        for sec,_ in vc.parse_sectioned_text(path):
            if sec:c[sec]+=1
    except Exception:c["__parse_error__"]+=1
    return c

def supporting(root):
    names={
        "water":["data/water.dat","data/waterpro.dat"],
        "timecycle":["data/timecyc.dat"],
        "surface":["data/surface.dat"],
        "object":["data/object.dat"],
        "handling":["data/handling.cfg"],
    }
    out={}
    for k,rels in names.items():
        a=[]
        for rel in rels:
            p=vc.find_case(root,rel)
            if p and p.exists():
                try:n=p.stat().st_size
                except OSError:n=None
                a.append({"path":safe_rel(p,root),"bytes":n})
        out[k]=a
    return out

def write_csv(path,fields,rows):
    with path.open("w",newline="",encoding="utf-8-sig") as f:
        w=csv.DictWriter(f,fieldnames=fields,extrasaction="ignore");w.writeheader();w.writerows(rows)

def audit(args):
    root=Path(args.game_root).resolve()
    ext=Path(args.extracted_root).resolve() if args.extracted_root else None
    if not root.exists():raise SystemExit(f"game root does not exist: {root}")
    if ext and not ext.exists():
        print(f"[vc-full-audit] WARN extracted root not found, using IMG fallback: {ext}",file=sys.stderr)
    world=vc.discover_map(root,ext if ext and ext.exists() else None)
    ide=world["ide"];inst=world["instances"]
    if not inst:raise SystemExit("no IPL instances found")
    resolver=Resolver(root,ext,world)

    total_sections=Counter();per_file={}
    for s in world.get("ipl_files",[]):
        c=section_counts(Path(s));total_sections.update(c);per_file[safe_rel(s,root)]=dict(c)

    model_use=Counter((x.model or "").lower() for x in inst)
    interior_use=Counter(int(x.interior) for x in inst)
    xs=[x.pos[0] for x in inst];ys=[x.pos[1] for x in inst];zs=[x.pos[2] for x in inst]
    lookup=Counter();model_rows=[];model_stats={};dff_errors={};txd_needed=Counter()

    print("VC_FULL_AUDIT_MODELS",f"ide={len(ide)}",f"instances={len(inst)}",f"unique={len(model_use)}")
    for i,(ident,m) in enumerate(sorted(ide.items()),1):
        model=m.model.lower();txd=m.txd.lower();used=model_use.get(model,0)
        if used:txd_needed[txd]+=used
        raw,src,kind=resolver.read(m.model+".dff");lookup["dff_"+kind]+=1
        info={"parsed":False,"vertices":0,"triangles":0,"atomics":0,"atomic_lods":[]}
        if raw is not None and not args.fast:
            info=inspect_dff(raw)
            if not info.get("parsed"):dff_errors[model]=info.get("error","unknown")
        model_stats[model]=info
        lod=list(m.lod_distances or ())
        model_rows.append({
            "id":ident,"model":m.model,"txd":m.txd,"instances":used,
            "dff_source":src or "","dff_present":int(raw is not None),
            "dff_parsed":int(bool(info.get("parsed"))),
            "vertices":int(info.get("vertices",0) or 0),"triangles":int(info.get("triangles",0) or 0),
            "atomics":int(info.get("atomics",0) or 0),
            "mesh_lods":"|".join(map(str,info.get("atomic_lods",[]))),
            "ide_num_atomics":m.num_atomics,
            "lod0":lod[0] if len(lod)>0 else "","lod1":lod[1] if len(lod)>1 else "",
            "lod2":lod[2] if len(lod)>2 else "","flags":m.flags,
            "big_building_candidate":int(bool(lod and lod[0]>LOD_DISTANCE)),
            "relation_key":relation_key(m.model),
        })
        if i%250==0 or i==len(ide):print(f"VC_FULL_AUDIT_PROGRESS models={i}/{len(ide)}")

    txd_names=set(txd_needed)|set(world.get("txd_parents",{}))|set(world.get("txd_parents",{}).values())
    txd_names.update(("generic","particle","vehicle","wheels"))
    txd_rows=[];txd_stats={};txd_errors={}
    for name in sorted(x for x in txd_names if x):
        raw,src,kind=resolver.read(name+".txd");lookup["txd_"+kind]+=1
        info={"parsed":False,"textures":0,"max_dimension":0}
        if raw is not None and not args.fast:
            info=inspect_txd(raw)
            if not info.get("parsed"):txd_errors[name]=info.get("error","unknown")
        txd_stats[name]=info
        txd_rows.append({
            "txd":name,"instance_references":txd_needed.get(name,0),"source":src or "",
            "present":int(raw is not None),"parsed":int(bool(info.get("parsed"))),
            "textures":int(info.get("textures",0) or 0),"max_dimension":int(info.get("max_dimension",0) or 0),
            "parent":world.get("txd_parents",{}).get(name,""),
        })

    col_by_id={};col_by_name={};col_errors=[]
    collision_match={}
    if Col is not None and not args.fast:
        col_by_id,col_by_name,col_errors=vc.load_collision_models([Path(x) for x in world.get("col_files",[])])
        selected_for_collision=[
            (x,ide[x.ident]) for x in inst
            if x.ident in ide and (args.interior is None or x.interior==args.interior)
        ]
        collision_match=vc.collision_match_stats(selected_for_collision,col_by_id,col_by_name)

    groups=defaultdict(list)
    for ident,m in ide.items():
        groups[relation_key(m.model)].append({
            "id":ident,"model":m.model,"lod_distances":list(m.lod_distances or ()),
            "instances":model_use.get(m.model.lower(),0),
        })
    relations=[{"key":k,"models":v} for k,v in sorted(groups.items()) if len(v)>1 and any(x["lod_distances"] and x["lod_distances"][0]>LOD_DISTANCE for x in v)]

    missing={r["model"].lower() for r in model_rows if not r["dff_present"]}
    pages=defaultdict(lambda:{"instances":0,"models":Counter(),"txds":Counter(),"est_vertices":0,"est_triangles":0,"missing":0,"big":0})
    for x in inst:
        if args.interior is not None and x.interior!=args.interior:continue
        m=ide.get(x.ident)
        if not m:continue
        p=pages[page_key(x.pos[0],x.pos[1],args.page_m)]
        model=m.model.lower();p["instances"]+=1;p["models"][model]+=1;p["txds"][m.txd.lower()]+=1
        st=model_stats.get(model,{})
        p["est_vertices"]+=int(st.get("vertices",0) or 0);p["est_triangles"]+=int(st.get("triangles",0) or 0)
        if model in missing:p["missing"]+=1
        if m.lod_distances and m.lod_distances[0]>LOD_DISTANCE:p["big"]+=1

    page_rows=[]
    for (px,py),p in sorted(pages.items()):
        page_rows.append({
            "page_x":px,"page_y":py,"min_x":px*args.page_m,"max_x":(px+1)*args.page_m,
            "min_y":py*args.page_m,"max_y":(py+1)*args.page_m,
            "instances":p["instances"],"unique_models":len(p["models"]),"unique_txds":len(p["txds"]),
            "est_vertices":p["est_vertices"],"est_triangles":p["est_triangles"],
            "missing_dff_instances":p["missing"],"big_building_instances":p["big"],
            "top_models":";".join(f"{m}:{n}" for m,n in p["models"].most_common(8)),
            "txds":";".join(sorted(p["txds"])[:24]),
        })

    bounds={"min":[min(xs),min(ys),min(zs)],"max":[max(xs),max(ys),max(zs)],
            "size":[max(xs)-min(xs),max(ys)-min(ys),max(zs)-min(zs)]}
    return {
        "format":"VC_FULL_WORLD_AUDIT","version":VERSION,
        "privacy":{"contains_gta_asset_bytes":False,"contains_only_metadata":True},
        "inputs":{"game_root":str(root),"extracted_root":str(ext) if ext else None,
                  "preferred_asset_source":"extracted-first","page_m":args.page_m,
                  "interior_filter":args.interior,"fast":bool(args.fast)},
        "source_files":{
            "dat":[safe_rel(x,root) for x in world.get("dat_files",[])],
            "ide":[safe_rel(x,root) for x in world.get("ide_files",[])],
            "ipl":[safe_rel(x,root) for x in world.get("ipl_files",[])],
            "img":[safe_rel(x,root) for x in world.get("img_files",[])],
            "col":[safe_rel(x,root) for x in world.get("col_files",[])],
            "supporting":supporting(root),
        },
        "extracted_inventory":resolver.extracted.report(),
        "world":{"ide_objects":len(ide),"instances":len(inst),"unique_instance_models":len(model_use),
                 "interiors":dict(sorted(interior_use.items())),"bounds":bounds,
                 "ipl_section_counts":dict(total_sections),"ipl_section_counts_by_file":per_file,
                 "txd_parent_count":len(world.get("txd_parents",{})),
                 "txd_parents":dict(sorted(world.get("txd_parents",{}).items()))},
        "assets":{"lookup_summary":dict(sorted(lookup.items())),"missing_dff_models":sorted(missing),
                  "dff_parse_errors":dff_errors,"txd_parse_errors":txd_errors,"txds":txd_stats,
                  "collision_source_files":len(world.get("col_files",[])),
                  "collision_ids":len(col_by_id),"collision_names":len(col_by_name),
                  "collision_match":collision_match,
                  "collision_parse_errors":col_errors},
        "lod":{"revc_relation_rule":"candidate key = model name without first 3 chars",
               "big_building_threshold":LOD_DISTANCE,"related_groups":relations},
        "streaming_plan":{"page_m":args.page_m,"current_racer_sector_m":24.0,
                          "sectors_per_page_axis":args.page_m/24.0,"pages":len(page_rows),
                          "page_stats":{
                              "max_instances":max((r["instances"] for r in page_rows),default=0),
                              "max_unique_models":max((r["unique_models"] for r in page_rows),default=0),
                              "max_unique_txds":max((r["unique_txds"] for r in page_rows),default=0),
                              "max_est_vertices":max((r["est_vertices"] for r in page_rows),default=0),
                              "max_est_triangles":max((r["est_triangles"] for r in page_rows),default=0)}},
        "models":model_rows,"txd_rows":txd_rows,"pages":page_rows,
    }

def outputs(r,out):
    out.mkdir(parents=True,exist_ok=True)
    (out/"vc_full_world_audit.json").write_text(json.dumps(r,indent=2),encoding="utf-8")
    write_csv(out/"vc_full_models.csv",["id","model","txd","instances","dff_source","dff_present","dff_parsed","vertices","triangles","atomics","mesh_lods","ide_num_atomics","lod0","lod1","lod2","flags","big_building_candidate","relation_key"],r["models"])
    write_csv(out/"vc_full_txds.csv",["txd","instance_references","source","present","parsed","textures","max_dimension","parent"],r["txd_rows"])
    write_csv(out/"vc_full_pages.csv",["page_x","page_y","min_x","max_x","min_y","max_y","instances","unique_models","unique_txds","est_vertices","est_triangles","missing_dff_instances","big_building_instances","top_models","txds"],r["pages"])
    w=r["world"];a=r["assets"];s=r["streaming_plan"]
    lines=[
        "VICE CITY FULL WORLD AUDIT","==========================","",
        "No GTA asset bytes are included in these reports.","",
        f"IDE objects              : {w['ide_objects']}",
        f"IPL instances            : {w['instances']}",
        f"Unique instance models   : {w['unique_instance_models']}",
        f"World bounds min         : {w['bounds']['min']}",
        f"World bounds max         : {w['bounds']['max']}",
        f"World size               : {w['bounds']['size']}",
        f"TXD parent relations     : {w['txd_parent_count']}","",
        f"Extracted source present : {r['extracted_inventory']['present']}",
        f"Extracted source files   : {r['extracted_inventory']['files']}",
        f"Asset lookup             : {a['lookup_summary']}",
        f"Missing DFF models       : {len(a['missing_dff_models'])}",
        f"DFF parse errors         : {len(a['dff_parse_errors'])}",
        f"TXD parse errors         : {len(a['txd_parse_errors'])}",
        f"COL source files         : {a['collision_source_files']}",
        f"COL model ids/names      : {a['collision_ids']} / {a['collision_names']}",
        f"COL matched id/name      : {a.get('collision_match',{}).get('matched_id',0)} / {a.get('collision_match',{}).get('matched_name',0)}",
        f"COL mesh models/faces    : {a.get('collision_match',{}).get('mesh_models',0)} / {a.get('collision_match',{}).get('face_total',0)}",
        f"COL parse errors         : {len(a['collision_parse_errors'])}","",
        f"Page size (GTA units)    : {s['page_m']}",
        f"Current sectors/page axis: {s['sectors_per_page_axis']}",
        f"Planned pages            : {s['pages']}",
        f"Max page instances       : {s['page_stats']['max_instances']}",
        f"Max page unique models   : {s['page_stats']['max_unique_models']}",
        f"Max page unique TXDs     : {s['page_stats']['max_unique_txds']}",
        f"Max est page vertices    : {s['page_stats']['max_est_vertices']}",
        f"Max est page triangles   : {s['page_stats']['max_est_triangles']}","",
        f"reVC LOD related groups  : {len(r['lod']['related_groups'])}","",
        "Send back: vc_full_world_summary.txt, vc_full_world_audit.json, vc_full_models.csv, vc_full_txds.csv, vc_full_pages.csv",
    ]
    (out/"vc_full_world_summary.txt").write_text("\n".join(lines)+"\n",encoding="utf-8")
    print("VC_FULL_AUDIT_OK",f"report={out/'vc_full_world_audit.json'}",f"pages={len(r['pages'])}")

def main():
    p=argparse.ArgumentParser()
    p.add_argument("--game-root",required=True)
    p.add_argument("--extracted-root")
    p.add_argument("--page-m",type=float,default=192.0)
    p.add_argument("--interior",type=int,default=0)
    p.add_argument("--output-dir",default="build/vc-full-audit")
    p.add_argument("--fast",action="store_true")
    a=p.parse_args()
    if a.page_m<=0:raise SystemExit("--page-m must be > 0")
    outputs(audit(a),Path(a.output_dir))

if __name__=="__main__":main()
