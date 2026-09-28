#!/usr/bin/env python3
"""
Build a compact playable city directly from OSM XML + SRTM HGT.

Why this exists:
Stage7.4 used OSM2World node accessor AABBs as building footprints. That was
fast, but rotated/complex buildings expanded into intersecting boxes. Stage7.5
uses the actual closed OSM building ways for render geometry and a DEM for Y.

Runtime stays intentionally tiny:
- no XML/GLB/DEM parser on Hi3531;
- roads, terrain and buildings are baked to v3f_t/tri3d_t arrays;
- geometry is split into sectors;
- building collision uses tight footprint AABBs only for the first prototype.
"""
from __future__ import annotations

import argparse
import gzip
import math
import re
import struct
import xml.etree.ElementTree as ET
from collections import defaultdict
from pathlib import Path

EARTH_M_PER_DEG = 111320.0


def pack1555(r,g,b):
    r=max(0,min(255,int(r))); g=max(0,min(255,int(g))); b=max(0,min(255,int(b)))
    return 0x8000|((r>>3)<<10)|((g>>3)<<5)|(b>>3)


# 0..3 roads; 4..11 wall light/dark pairs; 12..15 roofs;
# 16..19 ground/terrain.
PALETTE=[
    pack1555(48,51,55), pack1555(61,64,68), pack1555(78,80,83), pack1555(104,105,105),
    pack1555(206,190,166), pack1555(154,139,120),
    pack1555(184,194,197), pack1555(132,143,148),
    pack1555(205,177,142), pack1555(151,126,99),
    pack1555(199,166,157), pack1555(145,116,110),
    pack1555(111,93,78), pack1555(77,83,91), pack1555(137,121,104), pack1555(96,101,105),
    pack1555(137,135,124), pack1555(126,128,118), pack1555(102,126,93), pack1555(118,113,101),
]


class HGT:
    def __init__(self,path:Path):
        raw=gzip.open(path,"rb").read() if path.suffix==".gz" else path.read_bytes()
        n=int(round(math.sqrt(len(raw)//2)))
        if n*n*2!=len(raw):
            raise SystemExit(f"bad HGT byte count {len(raw)}")
        self.n=n
        self.raw=raw

    def sample(self,lat,lon):
        # N37E023 tile: caller's map is constrained to this degree square.
        south=math.floor(lat)
        west=math.floor(lon)
        # For this prototype the whole bbox is inside one tile; floor remains stable.
        u=(lon-west)*(self.n-1)
        v=((south+1.0)-lat)*(self.n-1)
        x0=max(0,min(self.n-1,int(math.floor(u))))
        y0=max(0,min(self.n-1,int(math.floor(v))))
        x1=min(self.n-1,x0+1); y1=min(self.n-1,y0+1)
        tx=u-x0; ty=v-y0
        def at(x,y):
            val=struct.unpack_from(">h",self.raw,2*(y*self.n+x))[0]
            return 0.0 if val<=-32768 else float(val)
        a=at(x0,y0); b=at(x1,y0); c=at(x0,y1); d=at(x1,y1)
        return (a*(1-tx)+b*tx)*(1-ty)+(c*(1-tx)+d*tx)*ty


def parse_tags(el):
    return {t.attrib.get("k",""):t.attrib.get("v","") for t in el.findall("tag")}


def number(text,default=None):
    if text is None: return default
    m=re.search(r"[-+]?[0-9]*\.?[0-9]+",text.replace(",","."))
    return float(m.group(0)) if m else default


def polygon_area(poly):
    return 0.5*sum(poly[i][0]*poly[(i+1)%len(poly)][1]-poly[(i+1)%len(poly)][0]*poly[i][1] for i in range(len(poly)))


def point_in_tri(p,a,b,c):
    def cross(u,v,w):
        return (v[0]-u[0])*(w[1]-u[1])-(v[1]-u[1])*(w[0]-u[0])
    c1=cross(a,b,p); c2=cross(b,c,p); c3=cross(c,a,p)
    eps=1e-7
    return (c1>=-eps and c2>=-eps and c3>=-eps) or (c1<=eps and c2<=eps and c3<=eps)


def clean_poly(poly):
    out=[]
    for p in poly:
        if not out or (p[0]-out[-1][0])**2+(p[1]-out[-1][1])**2>0.01:
            out.append(p)
    if len(out)>2 and (out[0][0]-out[-1][0])**2+(out[0][1]-out[-1][1])**2<0.01:
        out.pop()
    changed=True
    while changed and len(out)>3:
        changed=False
        for i in range(len(out)):
            a=out[i-1]; b=out[i]; c=out[(i+1)%len(out)]
            cross=abs((b[0]-a[0])*(c[1]-b[1])-(b[1]-a[1])*(c[0]-b[0]))
            if cross<0.03:
                out.pop(i); changed=True; break
    return out


def ear_clip(poly):
    poly=clean_poly(poly)
    if len(poly)<3:return []
    ccw=polygon_area(poly)>0
    idx=list(range(len(poly)))
    out=[]
    guard=0
    while len(idx)>3 and guard<4096:
        guard+=1
        found=False
        for ii in range(len(idx)):
            ia=idx[ii-1]; ib=idx[ii]; ic=idx[(ii+1)%len(idx)]
            a=poly[ia]; b=poly[ib]; c=poly[ic]
            cross=(b[0]-a[0])*(c[1]-b[1])-(b[1]-a[1])*(c[0]-b[0])
            if (cross>1e-7) != ccw: continue
            if any(point_in_tri(poly[j],a,b,c) for j in idx if j not in (ia,ib,ic)):
                continue
            out.append((ia,ib,ic) if ccw else (ia,ic,ib))
            idx.pop(ii); found=True; break
        if not found: break
    if len(idx)==3:
        a,b,c=idx
        out.append((a,b,c) if ccw else (a,c,b))
    return poly,out


def road_width(tags):
    if "width" in tags:
        w=number(tags.get("width"))
        if w and 2.2<=w<=24:return w
    lanes=max(1.0,number(tags.get("lanes"),1.0) or 1.0)
    hw=tags.get("highway","")
    defaults={
        "motorway":11.5,"trunk":10.5,"primary":9.0,"secondary":8.0,
        "tertiary":7.2,"residential":6.2,"unclassified":6.0,
        "living_street":5.4,"service":4.4,
    }
    if hw not in defaults:return None
    return max(defaults[hw],lanes*3.0)


def building_height(tags):
    h=number(tags.get("height"))
    if h and 2.5<=h<=120:return h
    levels=number(tags.get("building:levels"))
    if levels and 1<=levels<=40:return levels*3.05+0.5
    typ=tags.get("building","")
    if typ in ("apartments","hotel","office"):return 15.0
    if typ in ("church","cathedral"):return 16.0
    if typ in ("house","detached","residential"):return 8.2
    if typ in ("commercial","retail"):return 11.5
    return 9.5


def hash32(text):
    h=2166136261
    for ch in text.encode("utf-8",errors="ignore"):
        h=((h^ch)*16777619)&0xffffffff
    return h


def sector_key(x,z,size):
    return (math.floor(x/size),math.floor(z/size))


def add_tri(sectors,verts,tri,mat,sector_m):
    pts=[verts[tri[0]],verts[tri[1]],verts[tri[2]]]
    cx=sum(p[0] for p in pts)/3.0; cz=sum(p[2] for p in pts)/3.0
    b=sectors[sector_key(cx,cz,sector_m)]
    base=len(b["verts"])
    b["verts"].extend(pts)
    b["tris"].append((base,base+1,base+2,mat))


def make_road_way(sectors,points,width,sector_m,terrain_fn):
    if len(points)<2:return 0
    # Mitered polyline strip. Coordinates are x,z metres.
    left=[]; right=[]
    half=width*0.5
    for i,p in enumerate(points):
        if i==0:
            dx=points[1][0]-p[0]; dz=points[1][1]-p[1]
            ln=max(1e-6,math.hypot(dx,dz)); nx=-dz/ln; nz=dx/ln
            ox=nx*half; oz=nz*half
        elif i==len(points)-1:
            dx=p[0]-points[i-1][0]; dz=p[1]-points[i-1][1]
            ln=max(1e-6,math.hypot(dx,dz)); nx=-dz/ln; nz=dx/ln
            ox=nx*half; oz=nz*half
        else:
            d0=(p[0]-points[i-1][0],p[1]-points[i-1][1])
            d1=(points[i+1][0]-p[0],points[i+1][1]-p[1])
            l0=max(1e-6,math.hypot(*d0)); l1=max(1e-6,math.hypot(*d1))
            n0=(-d0[1]/l0,d0[0]/l0); n1=(-d1[1]/l1,d1[0]/l1)
            mx=n0[0]+n1[0]; mz=n0[1]+n1[1]
            ml=max(1e-6,math.hypot(mx,mz)); mx/=ml; mz/=ml
            denom=max(0.35,abs(mx*n1[0]+mz*n1[1]))
            m=min(half/denom,half*2.25)
            ox=mx*m; oz=mz*m
        left.append((p[0]+ox,p[1]+oz)); right.append((p[0]-ox,p[1]-oz))

    ntri=0
    for i in range(len(points)-1):
        l0=left[i]; r0=right[i]; l1=left[i+1]; r1=right[i+1]
        y0=terrain_fn(points[i][0],points[i][1])+0.055
        y1=terrain_fn(points[i+1][0],points[i+1][1])+0.055
        v=[(l0[0],y0,l0[1]),(r0[0],y0,r0[1]),(r1[0],y1,r1[1]),(l1[0],y1,l1[1])]
        add_tri(sectors,v,(0,1,2),0,sector_m); add_tri(sectors,v,(0,2,3),0,sector_m); ntri+=2
    return ntri


def make_building(sectors,poly,height,sector_m,terrain_fn,name,collision):
    poly=clean_poly(poly)
    if len(poly)<3:return 0
    poly,roof_tris=ear_clip(poly)
    if len(poly)<3:return 0
    cx=sum(p[0] for p in poly)/len(poly); cz=sum(p[1] for p in poly)/len(poly)
    base=terrain_fn(cx,cz)+0.03
    top=base+height
    h=hash32(name)
    pair=(h>>1)%4
    wall_light=4+pair*2; wall_dark=wall_light+1
    roof=12+((h>>5)%4)
    ntri=0

    # Walls preserve the real footprint. Face brightness depends on direction.
    for i,a in enumerate(poly):
        b=poly[(i+1)%len(poly)]
        dx=b[0]-a[0]; dz=b[1]-a[1]
        ln=max(1e-6,math.hypot(dx,dz))
        nx=-dz/ln; nz=dx/ln
        mat=wall_light if (0.65*nx+0.76*nz)>-0.15 else wall_dark
        v=[(a[0],base,a[1]),(b[0],base,b[1]),(b[0],top,b[1]),(a[0],top,a[1])]
        add_tri(sectors,v,(0,1,2),mat,sector_m)
        add_tri(sectors,v,(0,2,3),mat,sector_m)
        ntri+=2

    rv=[(x,top,z) for x,z in poly]
    for tri in roof_tris:
        add_tri(sectors,rv,tri,roof,sector_m); ntri+=1

    minx=min(p[0] for p in poly); maxx=max(p[0] for p in poly)
    minz=min(p[1] for p in poly); maxz=max(p[1] for p in poly)
    collision.append((minx,maxx,minz,maxz))
    return ntri


def emit_header(out_path,sectors,collision,palette,scale,sector_m,bounds,spawn,height_xs,height_zs,height_values):
    all_v=[]; all_t=[]; meta=[]
    for (sx,sz) in sorted(sectors):
        b=sectors[(sx,sz)]
        if not b["tris"]:continue
        # Vertices are intentionally duplicated in the source buckets because
        # generation is simpler. Compact exact duplicates inside each sector.
        used={}; compact=[]; tris=[]
        for a,bv,c,m in b["tris"]:
            ids=[]
            for old in (a,bv,c):
                p=b["verts"][old]
                key=(round(p[0],4),round(p[1],4),round(p[2],4))
                if key not in used:
                    used[key]=len(compact); compact.append(p)
                ids.append(used[key])
            tris.append((ids[0],ids[1],ids[2],m))
        if len(compact)>4096 or len(tris)>8192:
            raise SystemExit(f"sector {(sx,sz)} exceeds scratch {len(compact)}v {len(tris)}t")
        vb=len(all_v); tb=len(all_t)
        all_v.extend(compact); all_t.extend(tris)
        meta.append((sx,sz,vb,len(compact),tb,len(tris)))

    if len(all_v)>=65535:raise SystemExit(f"too many packed vertices {len(all_v)}")
    minx,maxx,minz,maxz=bounds
    sx,sy,sz,yaw=spawn
    L=[
        "/* Auto-generated by osm_city_pack.py (OSM footprints + SRTM terrain). */",
        "#ifndef OSM_CITY_MAP_H","#define OSM_CITY_MAP_H","",
        f"#define OSM_CITY_WORLD_SCALE {scale:.6f}f",
        f"#define OSM_CITY_SECTOR_METERS {sector_m:.6f}f",
        f"#define OSM_CITY_SECTOR_WORLD {sector_m*scale:.6f}f",
        f"#define OSM_CITY_MIN_X {minx*scale:.6f}f",
        f"#define OSM_CITY_MAX_X {maxx*scale:.6f}f",
        f"#define OSM_CITY_MIN_Z {minz*scale:.6f}f",
        f"#define OSM_CITY_MAX_Z {maxz*scale:.6f}f",
        f"#define OSM_CITY_SPAWN_X {sx*scale:.6f}f",
        f"#define OSM_CITY_SPAWN_Y {sy*scale:.6f}f",
        f"#define OSM_CITY_SPAWN_Z {sz*scale:.6f}f",
        f"#define OSM_CITY_SPAWN_YAW {yaw:.9f}f",
        f"#define OSM_CITY_VERTEX_COUNT {len(all_v)}",
        f"#define OSM_CITY_TRIANGLE_COUNT {len(all_t)}",
        f"#define OSM_CITY_SECTOR_COUNT {len(meta)}",
        f"#define OSM_CITY_BUILDING_COUNT {len(collision)}",
        f"#define OSM_CITY_MATERIAL_COUNT {len(palette)}",
        f"#define OSM_CITY_HEIGHT_NX {len(height_xs)}",
        f"#define OSM_CITY_HEIGHT_NZ {len(height_zs)}",
        f"#define OSM_CITY_HEIGHT_MIN_X {height_xs[0]*scale:.6f}f",
        f"#define OSM_CITY_HEIGHT_MIN_Z {height_zs[0]*scale:.6f}f",
        f"#define OSM_CITY_HEIGHT_STEP_X {(height_xs[1]-height_xs[0])*scale:.6f}f",
        f"#define OSM_CITY_HEIGHT_STEP_Z {(height_zs[1]-height_zs[0])*scale:.6f}f","",
        "typedef struct { int16_t sx,sz; uint16_t vertex_base,vertex_count; uint32_t tri_base,tri_count; } osm_city_sector_t;",
        "typedef struct { float minx,maxx,minz,maxz; } osm_city_building_t;","",
        "static const uint16_t osm_city_mat[OSM_CITY_MATERIAL_COUNT]={",
    ]
    L += [f"    0x{c:04x}{',' if i+1<len(palette) else ''}" for i,c in enumerate(palette)]
    L += ["};","","static const v3f_t osm_city_v[OSM_CITY_VERTEX_COUNT]={"]
    L += [f"    {{{x*scale:.4f}f,{y*scale:.4f}f,{z*scale:.4f}f}}," for x,y,z in all_v]
    L += ["};","","static const tri3d_t osm_city_t[OSM_CITY_TRIANGLE_COUNT]={"]
    L += [f"    {{{a},{b},{c},{m}}}," for a,b,c,m in all_t]
    L += ["};","","static const osm_city_sector_t osm_city_sector[OSM_CITY_SECTOR_COUNT]={"]
    L += [f"    {{{sx},{sz},{vb},{vc},{tb}u,{tc}u}}," for sx,sz,vb,vc,tb,tc in meta]
    L += ["};","","static const osm_city_building_t osm_city_building[OSM_CITY_BUILDING_COUNT]={"]
    L += [f"    {{{a*scale:.4f}f,{b*scale:.4f}f,{c*scale:.4f}f,{d*scale:.4f}f}}," for a,b,c,d in collision]
    L += ["};","","static const float osm_city_height[OSM_CITY_HEIGHT_NX*OSM_CITY_HEIGHT_NZ]={"]
    for row in height_values:
        L.append("    "+",".join(f"{v*scale:.4f}f" for v in row)+",")
    L += ["};","","#endif"]
    out_path.write_text("\n".join(L)+"\n",encoding="utf-8")
    return len(all_v),len(all_t),len(meta)


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("input_osm")
    ap.add_argument("input_hgt")
    ap.add_argument("output_header")
    ap.add_argument("--scale",type=float,default=240.0)
    ap.add_argument("--sector-m",type=float,default=48.0)
    ap.add_argument("--terrain-step-m",type=float,default=24.0)
    args=ap.parse_args()

    root=ET.parse(args.input_osm).getroot()
    bounds_el=root.find("bounds")
    if bounds_el is None:raise SystemExit("OSM bounds missing")
    south=float(bounds_el.attrib["minlat"]); north=float(bounds_el.attrib["maxlat"])
    west=float(bounds_el.attrib["minlon"]); east=float(bounds_el.attrib["maxlon"])
    lat0=(south+north)*0.5; lon0=(west+east)*0.5
    coslat=math.cos(math.radians(lat0))
    def to_local(lat,lon):
        return ((lon-lon0)*EARTH_M_PER_DEG*coslat,(lat-lat0)*EARTH_M_PER_DEG)
    def to_geo(x,z):
        return (lat0+z/EARTH_M_PER_DEG,lon0+x/(EARTH_M_PER_DEG*coslat))

    nodes={}
    for n in root.findall("node"):
        nodes[n.attrib["id"]]=(float(n.attrib["lat"]),float(n.attrib["lon"]))

    hgt=HGT(Path(args.input_hgt))
    # Flatten DEM only by a common datum, never by slope. Relative terrain is real.
    center_ele=hgt.sample(lat0,lon0)
    def terrain_fn(x,z):
        lat,lon=to_geo(x,z)
        return hgt.sample(lat,lon)-center_ele

    minx,minz=to_local(south,west); maxx,maxz=to_local(north,east)
    if minx>maxx:minx,maxx=maxx,minx
    if minz>maxz:minz,maxz=maxz,minz

    sectors=defaultdict(lambda:{"verts":[],"tris":[]})
    collision=[]
    road_count=road_tri=building_count=building_tri=0

    # Terrain grid first.
    step=args.terrain_step_m
    nx=max(2,int(math.ceil((maxx-minx)/step))+1)
    nz=max(2,int(math.ceil((maxz-minz)/step))+1)
    xs=[minx+(maxx-minx)*i/(nx-1) for i in range(nx)]
    zs=[minz+(maxz-minz)*j/(nz-1) for j in range(nz)]
    terrain_tri=0
    for j in range(nz-1):
        for i in range(nx-1):
            p00=(xs[i],terrain_fn(xs[i],zs[j]),zs[j])
            p10=(xs[i+1],terrain_fn(xs[i+1],zs[j]),zs[j])
            p11=(xs[i+1],terrain_fn(xs[i+1],zs[j+1]),zs[j+1])
            p01=(xs[i],terrain_fn(xs[i],zs[j+1]),zs[j+1])
            avg=(p00[1]+p10[1]+p11[1]+p01[1])*0.25
            mat=18 if avg>2.5 else (16 if ((i+j)&1)==0 else 17)
            v=[p00,p10,p11,p01]
            add_tri(sectors,v,(0,2,1),mat,args.sector_m)
            add_tri(sectors,v,(0,3,2),mat,args.sector_m)
            terrain_tri+=2

    spawn_candidates=[]

    for way in root.findall("way"):
        tags=parse_tags(way)
        refs=[nd.attrib.get("ref") for nd in way.findall("nd")]
        coords=[nodes[r] for r in refs if r in nodes]
        if len(coords)<2:continue

        w=road_width(tags)
        if w is not None:
            pts=[to_local(lat,lon) for lat,lon in coords]
            road_tri+=make_road_way(sectors,pts,w,args.sector_m,terrain_fn)
            road_count+=1
            # Prefer a long, ordinary drivable segment close to map centre.
            rank={"residential":0,"tertiary":1,"secondary":2,"unclassified":3,"primary":4,"service":5,"living_street":6}.get(tags.get("highway",""),9)
            for a,b in zip(pts,pts[1:]):
                dx=b[0]-a[0]; dz=b[1]-a[1]; ln=math.hypot(dx,dz)
                if ln<12:continue
                mx=(a[0]+b[0])*0.5; mz=(a[1]+b[1])*0.5
                dist=math.hypot(mx,mz)
                spawn_candidates.append((rank,dist,-ln,mx,mz,math.atan2(dx,dz)))

        if tags.get("building") and tags.get("building")!="no" and refs and refs[0]==refs[-1] and len(coords)>=4:
            poly=[to_local(lat,lon) for lat,lon in coords[:-1]]
            if abs(polygon_area(clean_poly(poly)))<8.0:continue
            building_tri+=make_building(
                sectors,poly,building_height(tags),args.sector_m,terrain_fn,
                way.attrib.get("id","building"),collision
            )
            building_count+=1

    if not spawn_candidates:raise SystemExit("no spawnable road")
    spawn_candidates.sort()
    _,_,_,sx,sz,yaw=spawn_candidates[0]
    sy=terrain_fn(sx,sz)+0.09

    height_values=[[terrain_fn(x,z) for x in xs] for z in zs]

    vc,tc,sc=emit_header(
        Path(args.output_header),sectors,collision,PALETTE,args.scale,args.sector_m,
        (minx,maxx,minz,maxz),(sx,sy,sz,yaw),xs,zs,height_values
    )
    print(
        "OSM_CITY_PACK_OK",
        f"roads={road_count}",f"road_tri={road_tri}",
        f"buildings={building_count}",f"building_tri={building_tri}",
        f"terrain_tri={terrain_tri}",f"vertices={vc}",f"triangles={tc}",
        f"sectors={sc}",
        f"terrain_range={min(terrain_fn(x,z) for x in (minx,maxx) for z in (minz,maxz)):.2f}:"
        f"{max(terrain_fn(x,z) for x in (minx,maxx) for z in (minz,maxz)):.2f}",
        f"spawn=({sx:.2f},{sy:.2f},{sz:.2f},{yaw:.4f})"
    )


if __name__=="__main__":
    main()
