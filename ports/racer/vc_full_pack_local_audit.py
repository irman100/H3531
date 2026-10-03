#!/usr/bin/env python3
from __future__ import annotations
import argparse,json
from collections import Counter,defaultdict
from pathlib import Path

def main():
    ap=argparse.ArgumentParser(description="Audit an already-built local VFW1 pack without reading GTA assets")
    ap.add_argument("--pack-dir",default="build/vc-full-pack")
    args=ap.parse_args()
    root=Path(args.pack_dir).resolve()
    pages=root/"pages"
    if not pages.is_dir():
        raise SystemExit(f"pages directory not found: {pages}")

    missing=Counter()
    missing_pages=defaultdict(list)
    models=Counter()
    atlas_full=0
    rows=[]
    for p in sorted(pages.glob("P_*_*")):
        rp=p/"page_report.json"
        if not rp.exists(): continue
        r=json.loads(rp.read_text(encoding="utf-8"))
        tex=r.get("texture_missing",{}) or {}
        for name,count in tex.items():
            c=int(count)
            key=str(name).strip().lower()
            missing[key]+=c
            missing_pages[key].append(p.name)
        for name,count in (r.get("missing_models",{}) or {}).items():
            models[name]+=int(count)
        atlas_full+=int(r.get("texture_atlas_full",0) or 0)
        rows.append({
            "page":p.name,
            "vertices":int(r.get("packed_vertices",0) or 0),
            "triangles":int(r.get("packed_triangles",0) or 0),
            "materials":int(r.get("materials",0) or 0),
            "missing_texture_names":len(tex),
            "missing_texture_uses":sum(int(x) for x in tex.values()),
            "atlas_full":int(r.get("texture_atlas_full",0) or 0),
        })
    if not rows: raise SystemExit("no page_report.json files found")

    top=missing.most_common()
    report={
        "format":"VFW1_LOCAL_AUDIT",
        "pack_dir":str(root),
        "pages":len(rows),
        "atlas_full":atlas_full,
        "unique_missing_textures":len(missing),
        "missing_texture_uses":sum(missing.values()),
        "missing_textures":[
            {"name":name,"uses":count,"pages":missing_pages[name]}
            for name,count in top
        ],
        "missing_models":dict(models),
        "heaviest_by_vertices":sorted(rows,key=lambda x:x["vertices"],reverse=True)[:20],
        "heaviest_by_triangles":sorted(rows,key=lambda x:x["triangles"],reverse=True)[:20],
    }
    (root/"vc_full_pack_local_audit.json").write_text(json.dumps(report,indent=2),encoding="utf-8")
    lines=[
        "VFW1 LOCAL PACK AUDIT",
        "=====================",
        f"Pages                  : {len(rows)}",
        f"Atlas-full fallbacks   : {atlas_full}",
        f"Unique missing textures: {len(missing)}",
        f"Missing texture uses   : {sum(missing.values())}",
        f"Missing models         : {len(models)}",
        "",
        "TOP MISSING TEXTURES",
    ]
    for name,count in top[:100]:
        lines.append(f"{count:6d}  {name}  pages={len(missing_pages[name])}")
    if models:
        lines += ["","MISSING MODELS"]
        for name,count in models.most_common():
            lines.append(f"{count:6d}  {name}")
    (root/"vc_full_pack_local_audit.txt").write_text("\n".join(lines)+"\n",encoding="utf-8")
    print("VC_FULL_PACK_LOCAL_AUDIT_OK",
          f"pages={len(rows)}",f"unique_missing={len(missing)}",
          f"uses={sum(missing.values())}",f"atlas_full={atlas_full}")

if __name__=="__main__": main()
