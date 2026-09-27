#!/usr/bin/env python3
from pathlib import Path
from PIL import Image
import sys

if len(sys.argv)!=3:
    raise SystemExit("usage: track_texture_pack.py asphalt_image out_header")

src=Path(sys.argv[1])
out=Path(sys.argv[2])
resampling=getattr(getattr(Image,"Resampling",Image),"LANCZOS")
im=Image.open(src).convert("RGB").resize((256,256),resampling)

tex=[]
for r,g,b in im.getdata():
    tex.append(0x8000|((r>>3)<<10)|((g>>3)<<5)|(b>>3))

with out.open("w",encoding="utf-8") as f:
    f.write("#ifndef STAYPLAYTION_TRACK_TEXTURE_H\n#define STAYPLAYTION_TRACK_TEXTURE_H\n")
    f.write("#define TRACK_ASPHALT_W 256\n#define TRACK_ASPHALT_H 256\n")
    f.write(f"static const uint16_t track_asphalt[{len(tex)}]={{\n")
    for i in range(0,len(tex),16):
        f.write(" "+",".join(f"0x{x:04x}" for x in tex[i:i+16])+",\n")
    f.write("};\n#endif\n")
print("TRACK_TEXTURE_OK",src,"source",Image.open(src).size,"packed",(256,256),"texels",len(tex))
