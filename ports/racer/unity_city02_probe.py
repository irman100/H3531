#!/usr/bin/env python3
"""
Probe a Unity text prefab and extract a coarse object-placement inventory.

Used for the CC0 Community Core City 02 map. This is intentionally a probe,
not a runtime loader: CI reads the authored Unity prefab, resolves prefab GUIDs
through .meta files and reports asset placements/bounds. A later pack step can
turn the verified layout into Stayplaytion sector tables.
"""
from __future__ import annotations
import argparse, json, re
from pathlib import Path
from collections import Counter

GUID_RE=re.compile(r"^guid:\s*([0-9a-fA-F]{32})\s*$",re.M)
SRC_RE=re.compile(r"m_SourcePrefab:\s*\{fileID:\s*[-0-9]+,\s*guid:\s*([0-9a-fA-F]{32}),\s*type:\s*3\}")
MOD_RE=re.compile(
    r"propertyPath:\s*(m_Local(?:Position|Scale|Rotation)\.[xyzw])\s*\n\s*value:\s*([^\n]+)",
    re.M
)

def guid_map(root: Path):
    out={}
    for p in root.rglob("*.meta"):
        try:
            txt=p.read_text(encoding="utf-8",errors="ignore")
        except OSError:
            continue
        m=GUID_RE.search(txt)
        if m:
            # asset path is the .meta filename without the suffix
            out[m.group(1).lower()]=str(p.with_suffix("")).replace("\\","/")
    return out

def classify(path: str):
    s=path.lower()
    if "city kit (roads)" in s or "/roads/" in s or "road-" in s:
        return "road"
    if "building" in s or "commercial" in s or "industrial" in s or "suburban" in s:
        return "building"
    if "tree" in s or "nature" in s or "vegetation" in s:
        return "vegetation"
    if "car" in s or "vehicle" in s:
        return "vehicle"
    if "light" in s or "sign" in s or "prop" in s or "detail" in s:
        return "prop"
    return "other"

def parse_prefab(prefab: Path, gm):
    text=prefab.read_text(encoding="utf-8",errors="ignore")
    blocks=re.split(r"(?=^--- !u!)",text,flags=re.M)
    placements=[]
    for block in blocks:
        sm=SRC_RE.search(block)
        if not sm:
            continue
        guid=sm.group(1).lower()
        vals={}
        for k,v in MOD_RE.findall(block):
            try: vals[k]=float(v.strip())
            except ValueError: pass
        path=gm.get(guid,"<unresolved:"+guid+">")
        pos=[
            vals.get("m_LocalPosition.x",0.0),
            vals.get("m_LocalPosition.y",0.0),
            vals.get("m_LocalPosition.z",0.0),
        ]
        scale=[
            vals.get("m_LocalScale.x",1.0),
            vals.get("m_LocalScale.y",1.0),
            vals.get("m_LocalScale.z",1.0),
        ]
        rot=[
            vals.get("m_LocalRotation.x",0.0),
            vals.get("m_LocalRotation.y",0.0),
            vals.get("m_LocalRotation.z",0.0),
            vals.get("m_LocalRotation.w",1.0),
        ]
        placements.append({
            "guid":guid,"asset":path,"kind":classify(path),
            "position":pos,"scale":scale,"rotation":rot
        })
    return placements

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("repo_root")
    ap.add_argument("prefab")
    ap.add_argument("output_json")
    args=ap.parse_args()
    root=Path(args.repo_root)
    prefab=Path(args.prefab)
    gm=guid_map(root)
    placements=parse_prefab(prefab,gm)
    counts=Counter(p["kind"] for p in placements)
    asset_counts=Counter(p["asset"] for p in placements)
    resolved=[p for p in placements if not p["asset"].startswith("<unresolved:")]
    pts=[p["position"] for p in resolved]
    bounds=None
    if pts:
        bounds={
            "min":[min(p[i] for p in pts) for i in range(3)],
            "max":[max(p[i] for p in pts) for i in range(3)],
        }
    report={
        "source":"Community Core City 02",
        "prefab":str(prefab),
        "meta_guids":len(gm),
        "prefab_instances":len(placements),
        "resolved_instances":len(resolved),
        "counts_by_kind":dict(counts),
        "bounds_local_positions":bounds,
        "top_assets":[{"asset":a,"count":n} for a,n in asset_counts.most_common(40)],
        "placements":placements,
    }
    Path(args.output_json).write_text(json.dumps(report,indent=2),encoding="utf-8")
    print("UNITY_CITY02_PROBE_OK",
          "instances",len(placements),
          "resolved",len(resolved),
          "kinds",dict(counts),
          "bounds",bounds)

if __name__=="__main__":
    main()
