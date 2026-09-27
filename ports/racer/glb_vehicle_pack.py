#!/usr/bin/env python3
"""
Kenney GLB vehicle -> compact C geometry for Stayplaytion Racer.

No runtime GLTF loader is needed on Hi3531. This converter:
- parses GLB using Python stdlib only;
- finds body / wheel-front-left / wheel-front-right / wheel-back-left /
  wheel-back-right nodes;
- bakes GLTF hierarchy transforms;
- recenters the car between wheel pivots;
- flips Z so Racer's +Z is vehicle forward;
- scales the longest horizontal vehicle extent to --target-size;
- emits separate body and four wheel meshes + wheel pivots.

The source model is CC0. Generated geometry contains only positions, indices and
coarse material classes.
"""
from __future__ import annotations
import argparse, json, math, struct
from pathlib import Path

TARGETS = {
    "body": ("body",),
    "wheel_fl": ("wheel-front-left","wheel_front_left","wheelfrontleft"),
    "wheel_fr": ("wheel-front-right","wheel_front_right","wheelfrontright"),
    "wheel_rl": ("wheel-back-left","wheel-rear-left","wheel_back_left","wheelrearleft"),
    "wheel_rr": ("wheel-back-right","wheel-rear-right","wheel_back_right","wheelrearright"),
}

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
    magic,version,total=struct.unpack_from("<III",data,0)
    if version!=2 or total>len(data):
        raise SystemExit("unsupported GLB")
    off=12;j=None;bin_chunk=b""
    while off+8<=total:
        length,typ=struct.unpack_from("<II",data,off);off+=8
        chunk=data[off:off+length];off+=length
        if typ==0x4E4F534A:
            j=json.loads(chunk.rstrip(b"\x00 \t\r\n").decode("utf-8"))
        elif typ==0x004E4942:
            bin_chunk=chunk
    if j is None or not bin_chunk:
        raise SystemExit("GLB missing JSON/BIN")
    return j,bin_chunk

def read_accessor(g,blob,idx):
    a=g["accessors"][idx]
    bv=g["bufferViews"][a["bufferView"]]
    fmt,size=COMP[a["componentType"]]
    n=NCOMP[a["type"]]
    stride=bv.get("byteStride",size*n)
    start=bv.get("byteOffset",0)+a.get("byteOffset",0)
    out=[]
    unpack="<"+fmt*n
    for i in range(a["count"]):
        out.append(struct.unpack_from(unpack,blob,start+i*stride))
    return out

def norm_name(s):
    return "".join(ch for ch in (s or "").lower() if ch.isalnum())

def material_class(g,mat_index):
    if mat_index is None: return 0
    mats=g.get("materials",[])
    if not (0<=mat_index<len(mats)): return 0
    name=(mats[mat_index].get("name") or "").lower()
    if any(k in name for k in ("glass","window","windshield")): return 3
    if any(k in name for k in ("tire","tyre","rubber","black")): return 2
    if any(k in name for k in ("rim","metal","chrome","light")): return 4
    return 0

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--target-size",type=float,default=900.0)
    args=ap.parse_args()

    g,blob=load_glb(args.input)
    nodes=g.get("nodes",[])
    parents=[None]*len(nodes)
    for i,n in enumerate(nodes):
        for ch in n.get("children",[]): parents[ch]=i

    world=[None]*len(nodes)
    def world_m(i):
        if world[i] is not None:return world[i]
        local=node_matrix(nodes[i])
        p=parents[i]
        world[i]=local if p is None else mmul(world_m(p),local)
        return world[i]

    normalized=[norm_name(n.get("name","")) for n in nodes]
    found={}
    for key,names in TARGETS.items():
        nn=[norm_name(x) for x in names]
        cand=[i for i,s in enumerate(normalized) if s in nn]
        if not cand:
            cand=[i for i,s in enumerate(normalized) if any(x in s for x in nn)]
        if cand: found[key]=cand[0]

    print("GLB_NODES",[(i,nodes[i].get("name","")) for i in range(len(nodes))])
    print("GLB_TARGETS",{k:(v,nodes[v].get("name","")) for k,v in found.items()})
    missing=[k for k in TARGETS if k not in found]
    if missing:
        raise SystemExit("missing expected nodes: "+",".join(missing))

    def descendants(root):
        out=[]
        def rec(i):
            out.append(i)
            for ch in nodes[i].get("children",[]):rec(ch)
        rec(root)
        return out

    def gather(root_idx):
        verts=[];tris=[]
        for ni in descendants(root_idx):
            n=nodes[ni]
            if "mesh" not in n:continue
            mesh=g["meshes"][n["mesh"]]
            wm=world_m(ni)
            for prim in mesh.get("primitives",[]):
                if prim.get("mode",4)!=4:continue
                if "POSITION" not in prim.get("attributes",{}):continue
                pos=read_accessor(g,blob,prim["attributes"]["POSITION"])
                base=len(verts)
                verts.extend(mpoint(wm,p[:3]) for p in pos)
                if "indices" in prim:
                    inds=[int(x[0]) for x in read_accessor(g,blob,prim["indices"])]
                else:
                    inds=list(range(len(pos)))
                if len(inds)%3: inds=inds[:len(inds)//3*3]
                mat=material_class(g,prim.get("material"))
                for q in range(0,len(inds),3):
                    tris.append((base+inds[q],base+inds[q+1],base+inds[q+2],mat))
        return verts,tris

    parts={k:gather(v) for k,v in found.items()}
    pivots={k:mpoint(world_m(v),(0,0,0)) for k,v in found.items() if k!="body"}

    for k,(v,t) in parts.items():
        print(f"GLB_PART {k} vertices={len(v)} triangles={len(t)} pivot={pivots.get(k)}")
        if not v or not t: raise SystemExit(f"part {k} has no geometry")

    wheel_p=list(pivots.values())
    origin=(
        sum(p[0] for p in wheel_p)/4.0,
        sum(p[1] for p in wheel_p)/4.0,
        sum(p[2] for p in wheel_p)/4.0,
    )

    allv=[p for vv,_ in parts.values() for p in vv]
    minx,maxx=min(p[0] for p in allv),max(p[0] for p in allv)
    minz,maxz=min(p[2] for p in allv),max(p[2] for p in allv)
    extent=max(maxx-minx,maxz-minz)
    if extent<=1e-9: raise SystemExit("degenerate vehicle")
    scale=args.target_size/extent

    def cv(p,center):
        # GLTF/Godot vehicle forward is -Z; Racer uses +Z.
        return ((p[0]-center[0])*scale,(p[1]-center[1])*scale,-(p[2]-center[2])*scale)

    # Body is relative to vehicle origin. Wheel meshes are relative to their pivots.
    body_v=[cv(p,origin) for p in parts["body"][0]]
    wheel_data={}
    pivot_out={}
    for k in ("wheel_fl","wheel_fr","wheel_rl","wheel_rr"):
        pivot=pivots[k]
        wheel_data[k]=([cv(p,pivot) for p in parts[k][0]],parts[k][1])
        pivot_out[k]=cv(pivot,origin)

    def emit_part(out,name,verts,tris):
        macro=name.upper()
        out.write(f"#define {macro}_VERTEX_COUNT {len(verts)}\n")
        out.write(f"#define {macro}_TRIANGLE_COUNT {len(tris)}\n")
        out.write(f"static const v3f_t {name}_v[{len(verts)}] = {{\n")
        for x,y,z in verts:
            out.write(f"  {{{x:.3f}f,{y:.3f}f,{z:.3f}f}},\n")
        out.write("};\n")
        out.write(f"static const tri3d_t {name}_t[{len(tris)}] = {{\n")
        for a,b,c,m in tris:
            # Flip winding because Z is mirrored.
            out.write(f"  {{{a},{c},{b},{m}}},\n")
        out.write("};\n\n")

    outp=Path(args.output)
    with outp.open("w",encoding="utf-8") as out:
        out.write("#ifndef STAYPLAYTION_KENNEY_VEHICLE_H\n#define STAYPLAYTION_KENNEY_VEHICLE_H\n\n")
        out.write("/* Generated from Kenney Starter Kit Racing CC0 vehicle GLB. */\n")
        emit_part(out,"kenney_body",body_v,parts["body"][1])
        for k in ("wheel_fl","wheel_fr","wheel_rl","wheel_rr"):
            emit_part(out,"kenney_"+k,*wheel_data[k])
            x,y,z=pivot_out[k]
            out.write(f"static const v3f_t kenney_{k}_pivot = {{{x:.3f}f,{y:.3f}f,{z:.3f}f}};\n\n")
        out.write("#endif\n")

    print("GLB_PACK_OK scale",scale,"origin",origin)
    for k,p in pivot_out.items(): print("GLB_PIVOT",k,p)

if __name__=="__main__":
    main()
