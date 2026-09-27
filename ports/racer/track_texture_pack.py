#!/usr/bin/env python3
from pathlib import Path
from PIL import Image
import sys

if len(sys.argv)!=3:
    raise SystemExit("usage: track_texture_pack.py asphalt_image out_header")

src=Path(sys.argv[1])
out=Path(sys.argv[2])
resampling=getattr(getattr(Image,"Resampling",Image),"LANCZOS")
source=Image.open(src).convert("RGB")

# Road material should read as one surface at 640x360, not as high-frequency
# photographic grain. Prefilter aggressively before the final runtime atlas.
# The two-stage reduction acts as a cheap low-pass filter on old Pillow too.
prefilter=source.resize((64,64),resampling)
im=prefilter.resize((128,128),resampling)

# Compress local contrast and bias very slightly toward neutral asphalt.
pixels=[]
for r,g,b in im.getdata():
    y=(r*30+g*59+b*11)//100
    r=(r*35+y*65)//100
    g=(g*35+y*65)//100
    b=(b*35+y*65)//100
    # Gentle contrast compression around mid-grey.
    r=max(0,min(255,128+(r-128)*70//100))
    g=max(0,min(255,128+(g-128)*70//100))
    b=max(0,min(255,128+(b-128)*70//100))
    pixels.append((r,g,b))

tex=[]
for r,g,b in pixels:
    tex.append(0x8000|((r>>3)<<10)|((g>>3)<<5)|(b>>3))

with out.open("w",encoding="utf-8") as f:
    f.write("#ifndef STAYPLAYTION_TRACK_TEXTURE_H\n#define STAYPLAYTION_TRACK_TEXTURE_H\n")
    f.write("#define TRACK_ASPHALT_W 128\n#define TRACK_ASPHALT_H 128\n")
    f.write(f"static const uint16_t track_asphalt[{len(tex)}]={{\n")
    for i in range(0,len(tex),16):
        f.write(" "+",".join(f"0x{x:04x}" for x in tex[i:i+16])+",\n")
    f.write("};\n#endif\n")
print("TRACK_TEXTURE_OK",src,"source",source.size,"prefilter",(64,64),"packed",(128,128),"texels",len(tex),"contrast=70pct")
