#!/usr/bin/env python3
"""
Generic static GLB -> compact flat-shaded C mesh for Stayplaytion Racer.

The Hi3531 runtime deliberately has no GLTF/PBR loader.  This build-time tool:
- parses GLB 2.0 POSITION/TEXCOORD_0/indices;
- bakes node hierarchy transforms;
- samples material base colour (and, when available, the embedded base-colour
  texture at each triangle centroid) into an RGB1555 material palette;
- recenters X/Z, places the lowest Y at zero, and normalizes to --target-height;
- emits one compact v3f_t/tri3d_t mesh plus a uint16_t palette.

This preserves the authored low-poly silhouette and a useful amount of its
colour design while keeping runtime drawing to flat shaded triangles.
"""
from __future__ import annotations

import argparse
import io
import json
import math
import struct
from pathlib import Path

from PIL import Image

COMP = {
    5120: ("b",1), 5121: ("B",1), 5122: ("h",2),
    5123: ("H",2), 5125: ("I",4), 5126: ("f",4),
}
NCOMP = {"SCALAR":1,"VEC2":2,"VEC3":3,"VEC4":4,"MAT4":16}


def ident():
    return [[1.0,0.0,0.0,0.0],[0.0,1.0,0.0,0.0],[0.0,0.0,1.0,0.0],[0.0,0.0,0.0,1.0]]


def mmul(a,b):
    return [[sum(a[r][k]*b[k][c] for k in range(4)) for c in range(4)] for r in range(4)]


def mpoint(m,p):
    x,y,z=p
    return (
        m[0][0]*x+m[0][1]*y+m[0][2]*z+m[0][3],
        m[1][0]*x+m[1][1]*y+m[1][2]*z+m[1][3],
        m[2][0]*x+m[2][1]*y+m[2][2]*z+m[2][3],
    )


def node_matrix(n):
    if "matrix" in n:
        a=n["matrix"]
        return [[float(a[c*4+r]) for c in range(4)] for r in range(4)]
    t=n.get("translation",[0,0,0])
    s=n.get("scale",[1,1,1])
    x,y,z,w=n.get("rotation",[0,0,0,1])
    xx,yy,zz=x*x,y*y,z*z
    xy,xz,yz=x*y,x*z,y*z
    wx,wy,wz=w*x,w*y,w*z
    r=[
        [1-2*(yy+zz),2*(xy-wz),2*(xz+wy),0],
        [2*(xy+wz),1-2*(xx+zz),2*(yz-wx),0],
        [2*(xz-wy),2*(yz+wx),1-2*(xx+yy),0],
        [0,0,0,1],
    ]
    sm=ident();sm[0][0]=s[0];sm[1][1]=s[1];sm[2][2]=s[2]
    tm=ident();tm[0][3]=t[0];tm[1][3]=t[1];tm[2][3]=t[2]
    return mmul(tm,mmul(r,sm))


def load_glb(path):
    data=Path(path).read_bytes()
    if len(data)<20 or data[:4]!=b"glTF":
        raise SystemExit("not a GLB file")
    _magic,version,total=struct.unpack_from("<III",data,0)
    if version!=2 or total>len(data):
        raise SystemExit("unsupported GLB")
    off=12
    doc=None
    bin_chunk=b""
    while off+8<=total:
        length,typ=struct.unpack_from("<II",data,off);off+=8
        chunk=data[off:off+length];off+=length
        if typ==0x4E4F534A:
            doc=json.loads(chunk.rstrip(b"\x00 \t\r\n").decode("utf-8"))
        elif typ==0x004E4942:
            bin_chunk=chunk
    if doc is None or not bin_chunk:
        raise SystemExit("GLB missing JSON/BIN")
    return doc,bin_chunk


def read_accessor(g,blob,idx):
    a=g["accessors"][idx]
    if "bufferView" not in a:
        raise SystemExit("sparse/accessor without bufferView is unsupported")
    bv=g["bufferViews"][a["bufferView"]]
    fmt,size=COMP[a["componentType"]]
    n=NCOMP[a["type"]]
    stride=bv.get("byteStride",size*n)
    start=bv.get("byteOffset",0)+a.get("byteOffset",0)
    unpack="<"+fmt*n
    return [struct.unpack_from(unpack,blob,start+i*stride) for i in range(a["count"])]


def pack1555(rgb):
    r,g,b=rgb
    r=max(0,min(255,int(round(r))))
    g=max(0,min(255,int(round(g))))
    b=max(0,min(255,int(round(b))))
    return 0x8000|((r>>3)<<10)|((g>>3)<<5)|(b>>3)


def load_images(g,blob):
    out=[]
    for im in g.get("images",[]):
        if "bufferView" in im:
            bv=g["bufferViews"][im["bufferView"]]
            start=bv.get("byteOffset",0)
            raw=blob[start:start+bv["byteLength"]]
            try:
                out.append(Image.open(io.BytesIO(raw)).convert("RGBA"))
            except Exception:
                out.append(None)
        else:
            out.append(None)
    return out


def tex_image_for_material(g,images,mat_index):
    if mat_index is None:
        return None
    mats=g.get("materials",[])
    if not (0<=mat_index<len(mats)):
        return None
    pbr=mats[mat_index].get("pbrMetallicRoughness",{})
    bt=pbr.get("baseColorTexture")
    if not bt:
        return None
    tidx=bt.get("index")
    textures=g.get("textures",[])
    if tidx is None or not (0<=tidx<len(textures)):
        return None
    src=textures[tidx].get("source")
    if src is None or not (0<=src<len(images)):
        return None
    return images[src]


def factor_for_material(g,mat_index):
    mats=g.get("materials",[])
    if mat_index is None or not (0<=mat_index<len(mats)):
        return (0.72,0.76,0.74,1.0)
    return tuple(mats[mat_index].get("pbrMetallicRoughness",{}).get(
        "baseColorFactor",[1.0,1.0,1.0,1.0]))


def sample_material(g,images,mat_index,uv):
    fac=factor_for_material(g,mat_index)
    img=tex_image_for_material(g,images,mat_index)
    if img is None or uv is None:
        return tuple(255.0*fac[i] for i in range(3))
    u,v=uv
    # Repeat wrapping; nearest sample is enough because this is a build-time
    # colour bake for a flat-shaded low-poly triangle.
    u=u-math.floor(u)
    v=v-math.floor(v)
    x=max(0,min(img.width-1,int(u*(img.width-1)+0.5)))
    y=max(0,min(img.height-1,int((1.0-v)*(img.height-1)+0.5)))
    r,gc,b,a=img.getpixel((x,y))
    if a<16:
        return tuple(255.0*fac[i] for i in range(3))
    return (r*fac[0],gc*fac[1],b*fac[2])


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--symbol",required=True)
    ap.add_argument("--target-height",type=float,required=True)
    ap.add_argument("--max-materials",type=int,default=96)
    ap.add_argument("--cluster-step",type=float,default=0.0,
                    help="optional world-unit vertex clustering for same-model LOD generation")
    args=ap.parse_args()

    g,blob=load_glb(args.input)
    images=load_images(g,blob)
    nodes=g.get("nodes",[])

    parents=[None]*len(nodes)
    for i,n in enumerate(nodes):
        for ch in n.get("children",[]):
            parents[ch]=i

    world=[None]*len(nodes)
    def world_m(i):
        if world[i] is not None:
            return world[i]
        local=node_matrix(nodes[i])
        p=parents[i]
        world[i]=local if p is None else mmul(world_m(p),local)
        return world[i]

    if g.get("scenes"):
        scene_idx=g.get("scene",0)
        roots=g["scenes"][scene_idx].get("nodes",[])
    else:
        roots=[i for i,p in enumerate(parents) if p is None]

    ordered=[]
    seen=set()
    def walk(i):
        if i in seen:return
        seen.add(i);ordered.append(i)
        for ch in nodes[i].get("children",[]):walk(ch)
    for r in roots:walk(r)
    if not ordered:
        ordered=list(range(len(nodes)))

    verts=[]
    tris_raw=[]
    for ni in ordered:
        n=nodes[ni]
        if "mesh" not in n:
            continue
        wm=world_m(ni)
        mesh=g["meshes"][n["mesh"]]
        for prim in mesh.get("primitives",[]):
            if prim.get("mode",4)!=4:
                continue
            attrs=prim.get("attributes",{})
            if "POSITION" not in attrs:
                continue
            pos=read_accessor(g,blob,attrs["POSITION"])
            uvs=read_accessor(g,blob,attrs["TEXCOORD_0"]) if "TEXCOORD_0" in attrs else None
            base=len(verts)
            verts.extend(mpoint(wm,p[:3]) for p in pos)
            if "indices" in prim:
                inds=[int(x[0]) for x in read_accessor(g,blob,prim["indices"])]
            else:
                inds=list(range(len(pos)))
            inds=inds[:len(inds)//3*3]
            mi=prim.get("material")
            for q in range(0,len(inds),3):
                ia,ib,ic=inds[q:q+3]
                uv=None
                if uvs is not None:
                    uv=(
                        (float(uvs[ia][0])+float(uvs[ib][0])+float(uvs[ic][0]))/3.0,
                        (float(uvs[ia][1])+float(uvs[ib][1])+float(uvs[ic][1]))/3.0,
                    )
                rgb=sample_material(g,images,mi,uv)
                tris_raw.append((base+ia,base+ib,base+ic,pack1555(rgb)))

    if not verts or not tris_raw:
        raise SystemExit("GLB contains no triangle geometry")
    if len(verts)>65535:
        raise SystemExit("mesh has too many vertices for uint16 indices")

    minx=min(p[0] for p in verts);maxx=max(p[0] for p in verts)
    miny=min(p[1] for p in verts);maxy=max(p[1] for p in verts)
    minz=min(p[2] for p in verts);maxz=max(p[2] for p in verts)
    height=maxy-miny
    if height<=1.0e-9:
        raise SystemExit("degenerate model height")
    scale=args.target_height/height
    cx=(minx+maxx)*0.5
    cz=(minz+maxz)*0.5
    verts=[((x-cx)*scale,(y-miny)*scale,(z-cz)*scale) for x,y,z in verts]

    # Compact identical vertices produced by separate GLB primitives only when
    # the position matches exactly after transform. Triangle material is stored
    # separately, so this is safe and reduces the static header.
    remap={}
    compact=[]
    idxmap=[]
    for p in verts:
        key=(round(p[0],5),round(p[1],5),round(p[2],5))
        if key not in remap:
            remap[key]=len(compact)
            compact.append(p)
        idxmap.append(remap[key])

    # Palette is exact RGB1555 colours sampled from the authored GLB. If a model
    # produces too many colours, quantize progressively by clearing low 5-bit
    # channel bits until it fits the tiny runtime material index.
    colors=[t[3] for t in tris_raw]
    def quant(c,bits):
        r=(c>>10)&31;gg=(c>>5)&31;b=c&31
        mask=(~((1<<bits)-1))&31 if bits else 31
        return 0x8000|((r&mask)<<10)|((gg&mask)<<5)|(b&mask)
    chosen=colors
    for bits in range(0,5):
        trial=[quant(c,bits) for c in colors]
        if len(set(trial))<=args.max_materials:
            chosen=trial
            break
    palette=[]
    pmap={}
    tri=[]
    for (a,b,c,_),col in zip(tris_raw,chosen):
        if col not in pmap:
            pmap[col]=len(palette)
            palette.append(col)
        tri.append((idxmap[a],idxmap[b],idxmap[c],pmap[col]))

    # Optional same-model LOD generation by vertex clustering. This deliberately
    # preserves the source building silhouette/material identity instead of
    # swapping to a different mesh at distance. Vertices sharing a 3D grid cell
    # are averaged; collapsed/near-zero triangles are removed.
    if args.cluster_step>0.0:
        step=float(args.cluster_step)
        buckets={}
        for i,(x,y,z) in enumerate(compact):
            key=(int(round(x/step)),int(round(y/step)),int(round(z/step)))
            b=buckets.setdefault(key,{"sum":[0.0,0.0,0.0],"n":0,"members":[]})
            b["sum"][0]+=x;b["sum"][1]+=y;b["sum"][2]+=z
            b["n"]+=1;b["members"].append(i)

        lod=[]
        old_to_new=[0]*len(compact)
        for key,b in buckets.items():
            ni=len(lod)
            n=float(b["n"])
            lod.append((b["sum"][0]/n,b["sum"][1]/n,b["sum"][2]/n))
            for oi in b["members"]:
                old_to_new[oi]=ni

        lod_tri=[]
        seen=set()
        for a,b,c,m in tri:
            aa=old_to_new[a];bb=old_to_new[b];cc=old_to_new[c]
            if aa==bb or bb==cc or aa==cc:
                continue
            pa,pb,pc=lod[aa],lod[bb],lod[cc]
            ux,uy,uz=pb[0]-pa[0],pb[1]-pa[1],pb[2]-pa[2]
            vx,vy,vz=pc[0]-pa[0],pc[1]-pa[1],pc[2]-pa[2]
            nx=uy*vz-uz*vy;ny=uz*vx-ux*vz;nz=ux*vy-uy*vx
            if nx*nx+ny*ny+nz*nz < 1.0e-4:
                continue
            # Same geometric face/material after clustering only needs one copy.
            skey=tuple(sorted((aa,bb,cc)))+(m,)
            if skey in seen:
                continue
            seen.add(skey)
            lod_tri.append((aa,bb,cc,m))

        compact=lod
        tri=lod_tri
        if not tri:
            raise SystemExit("cluster-step collapsed the entire mesh")

    sym=args.symbol
    guard=("STAYPLAYTION_STATIC_"+sym+"_H").upper().replace("-","_")
    with Path(args.output).open("w",encoding="utf-8") as out:
        out.write(f"#ifndef {guard}\n#define {guard}\n\n")
        out.write(f"/* Generated from {Path(args.input).name}; source asset CC0. */\n")
        out.write(f"#define {sym.upper()}_VERTEX_COUNT {len(compact)}\n")
        out.write(f"#define {sym.upper()}_TRIANGLE_COUNT {len(tri)}\n")
        out.write(f"#define {sym.upper()}_MATERIAL_COUNT {len(palette)}\n")
        out.write(f"static const v3f_t {sym}_v[{len(compact)}] = {{\n")
        for x,y,z in compact:
            out.write(f"  {{{x:.3f}f,{y:.3f}f,{z:.3f}f}},\n")
        out.write("};\n")
        out.write(f"static const tri3d_t {sym}_t[{len(tri)}] = {{\n")
        for a,b,c,m in tri:
            out.write(f"  {{{a},{b},{c},{m}}},\n")
        out.write("};\n")
        out.write(f"static const uint16_t {sym}_mat[{len(palette)}] = {{\n  ")
        out.write(",".join(f"0x{x:04x}" for x in palette))
        out.write("\n};\n")
        out.write(f"#define {sym.upper()}_WIDTH {((maxx-minx)*scale):.3f}f\n")
        out.write(f"#define {sym.upper()}_HEIGHT {args.target_height:.3f}f\n")
        out.write(f"#define {sym.upper()}_DEPTH {((maxz-minz)*scale):.3f}f\n")
        out.write("\n#endif\n")

    print("STATIC_GLTF_PACK_OK",
          "symbol",sym,
          "vertices",len(compact),
          "triangles",len(tri),
          "materials",len(palette),
          "target_height",args.target_height,
          "cluster_step",args.cluster_step,
          "size",((maxx-minx)*scale,args.target_height,(maxz-minz)*scale))


if __name__=="__main__":
    main()
