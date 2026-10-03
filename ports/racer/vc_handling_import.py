#!/usr/bin/env python3
from __future__ import annotations
import argparse,json,struct
from pathlib import Path
from vc_vehicle_import import parse_handling

FMT="<4sI16s21f4I"
VERSION=1

def main():
    ap=argparse.ArgumentParser(description="Build local Racer handling profile from GTA Vice City handling.cfg")
    ap.add_argument("--game-root",required=True)
    ap.add_argument("--handling",default="CHEETAH")
    ap.add_argument("--output-bin",required=True)
    ap.add_argument("--output-report",required=True)
    args=ap.parse_args()

    root=Path(args.game_root).resolve()
    cfg=root/"data"/"handling.cfg"
    if not cfg.exists():
        cfg=root/"DATA"/"HANDLING.CFG"
    if not cfg.exists():
        raise SystemExit(f"handling.cfg not found under {root}")

    h=parse_handling(cfg,args.handling)
    floats=[
        float(h["mass"]),
        float(h["dim_x"]),float(h["dim_y"]),float(h["dim_z"]),
        float(h["com_x"]),float(h["com_y"]),float(h["com_z"]),
        float(h["traction_mult"]),float(h["traction_loss"]),float(h["traction_bias"]),
        float(h["max_velocity_kmh"]),float(h["engine_accel_raw"]),
        float(h["brake_decel_raw"]),float(h["brake_bias"]),
        float(h["steering_lock_deg"]),
        float(h["suspension_force"]),float(h["suspension_damping"]),
        float(h["suspension_upper"]),float(h["suspension_lower"]),
        float(h["suspension_bias"]),float(h["suspension_antidive"]),
    ]
    ints=[
        int(h["gears"]),
        ord(str(h["drive_type"])[0]),
        ord(str(h["engine_type"])[0]),
        int(h.get("abs",0)),
    ]
    name=str(h["name"]).encode("ascii","ignore")[:15]
    name=name+b"\0"*(16-len(name))

    out=Path(args.output_bin)
    out.parent.mkdir(parents=True,exist_ok=True)
    out.write_bytes(struct.pack(FMT,b"VCH1",VERSION,name,*floats,*ints))

    report={
        "format":"VCH1","version":VERSION,
        "source":"local Vice City handling.cfg",
        "handling":h,
        "binary_bytes":out.stat().st_size,
        "privacy":{"contains_original_game_file":False,"contains_single_numeric_handling_profile":True},
    }
    rp=Path(args.output_report)
    rp.parent.mkdir(parents=True,exist_ok=True)
    rp.write_text(json.dumps(report,indent=2),encoding="utf-8")
    print(
        "VC_HANDLING_PACK_OK",
        f"name={h['name']}",f"mass={h['mass']}",
        f"drive={h['drive_type']}",f"gears={h['gears']}",
        f"traction={h['traction_mult']}/{h['traction_loss']}/{h['traction_bias']}",
        f"suspension={h['suspension_force']}/{h['suspension_damping']}/"
        f"{h['suspension_upper']}/{h['suspension_lower']}/{h['suspension_bias']}",
        f"output={out}"
    )

if __name__=="__main__":
    main()
