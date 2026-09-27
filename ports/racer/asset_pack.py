#!/usr/bin/env python3
from PIL import Image
import sys
from pathlib import Path

def pack1555(r,g,b):
    return 0x8000 | ((r>>3)<<10) | ((g>>3)<<5) | (b>>3)

def emit(name, img, out):
    img = img.convert("RGB")
    vals=[]
    for r,g,b in img.getdata():
        vals.append(pack1555(r,g,b))
    out.write(f"#define {name.upper()}_W {img.width}\n")
    out.write(f"#define {name.upper()}_H {img.height}\n")
    out.write(f"static const unsigned short {name}[{len(vals)}] = {{\n")
    for i in range(0,len(vals),16):
        out.write("  "+",".join(f"0x{v:04x}" for v in vals[i:i+16])+",\n")
    out.write("};\n\n")

def main():
    if len(sys.argv)!=4:
        raise SystemExit("usage: asset_pack.py SKY BILLBOARD OUT")
    resample = getattr(getattr(Image, "Resampling", Image), "LANCZOS")
    sky=Image.open(sys.argv[1]).resize((320,90),resample)
    board=Image.open(sys.argv[2]).resize((160,90),resample)
    with open(sys.argv[3],"w",encoding="utf-8") as out:
        out.write("#ifndef STAYPLAYTION_RACER_ASSETS_H\n#define STAYPLAYTION_RACER_ASSETS_H\n\n")
        emit("racer_sky",sky,out)
        emit("racer_billboard",board,out)
        out.write("#endif\n")

if __name__=="__main__":
    main()
