#!/usr/bin/env python3
from __future__ import annotations
import argparse,re,struct,json
from pathlib import Path

N=6
FMT="<4sI36f"

def parse_surface(path:Path):
    rows=[]
    for raw in path.read_text(encoding="latin-1",errors="ignore").splitlines():
        line=raw.strip()
        if not line or line.startswith(";"):
            continue
        toks=[x for x in re.split(r"[\s,]+",line) if x]
        if len(toks)<2:
            continue
        vals=[]
        for t in toks[1:]:
            if t=="-":
                vals.append(0.0)
            else:
                try: vals.append(float(t))
                except ValueError: break
        if vals:
            rows.append((toks[0],vals))
        if len(rows)>=N:
            break
    if len(rows)!=N:
        raise SystemExit(f"expected {N} adhesive-group rows in {path}, got {len(rows)}")
    m=[[0.0]*N for _ in range(N)]
    names=[]
    for i,(name,vals) in enumerate(rows):
        names.append(name)
        if len(vals)<i+1:
            raise SystemExit(f"surface row {name} has {len(vals)} values, expected at least {i+1}")
        for j in range(i+1):
            m[i][j]=m[j][i]=float(vals[j])
    return names,m

def main():
    ap=argparse.ArgumentParser(description="Pack Vice City surface.dat adhesive matrix for Racer")
    ap.add_argument("--game-root",required=True)
    ap.add_argument("--output-bin",required=True)
    ap.add_argument("--output-report",required=True)
    args=ap.parse_args()
    root=Path(args.game_root).resolve()
    candidates=[root/"data"/"surface.dat",root/"DATA"/"SURFACE.DAT"]
    path=next((p for p in candidates if p.exists()),None)
    if path is None:
        raise SystemExit(f"surface.dat not found under {root}")
    names,m=parse_surface(path)
    flat=[m[i][j] for i in range(N) for j in range(N)]
    out=Path(args.output_bin)
    out.parent.mkdir(parents=True,exist_ok=True)
    out.write_bytes(struct.pack(FMT,b"VCS1",1,*flat))
    report={
        "format":"VCS1","source":str(path),"groups":names,"adhesive_matrix":m,
        "bytes":out.stat().st_size
    }
    rp=Path(args.output_report);rp.parent.mkdir(parents=True,exist_ok=True)
    rp.write_text(json.dumps(report,indent=2),encoding="utf-8")
    print("VC_SURFACE_PACK_OK",f"source={path}",f"groups={','.join(names)}",f"output={out}")

if __name__=="__main__":
    main()
