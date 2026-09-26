#!/usr/bin/env python3
from pathlib import Path
import struct, zlib, binascii, stat, sys

SRC = Path(sys.argv[1] if len(sys.argv) > 1 else 'H3531-MEM256.IMG')
DST = Path(sys.argv[2] if len(sys.argv) > 2 else 'H3531-MEM256K.IMG')

PROFILE = b'''PATH=/usr/bin:/usr/sbin:/bin:/sbin\nLD_LIBRARY_PATH=/usr/local/lib:/usr/lib\nexport PATH LD_LIBRARY_PATH\numask 022\necho "Welcome to Monitor Tech."\n[ -e /var/sp ] || {\n touch /var/sp\n (trap '' 2;U=/mnt/usb;S=$U/H3531/SYSTEM;mkdir -p $U\n for i in 1 2 3 4 5 6 7 8 9 10;do\n  [ -x $S/STAGE66A-DESKTOP.APP ]&&break\n  [ ! -b /dev/sda1 ]||mount -t vfat /dev/sda1 $U 2>/dev/null\n  sleep 1\n done\n [ ! -x $S/STAGE66A-DESKTOP.APP ]||exec $S/STAGE66A-DESKTOP.APP\n ) </dev/null >/var/sp.log 2>&1 &\n}\n'''

def inode(buf, off):
    w0,w1,w2=struct.unpack_from('<3I',buf,off)
    return {'off':off,'mode':w0&0xffff,'size':w1&0xffffff,'gid':w1>>24,
            'namelen':(w2&0x3f)*4,'data_off':(w2>>6)*4}

def directory(buf, ino, path=''):
    pos,end=ino['data_off'],ino['data_off']+ino['size']
    while pos<end:
        e=inode(buf,pos)
        name=bytes(buf[pos+12:pos+12+e['namelen']]).split(b'\\0',1)[0].decode()
        e['path']=(path.rstrip('/')+'/'+name) if path else '/'+name
        yield e
        pos+=12+e['namelen']

def tree(buf):
    out={}
    def walk(ino,path=''):
        if stat.S_ISDIR(ino['mode']) and ino['size']:
            for e in directory(buf,ino,path):
                out[e['path']]=e
                if stat.S_ISDIR(e['mode']): walk(e,e['path'])
    walk(inode(buf,64))
    return out

def read_file(buf,e):
    n=(e['size']+4095)//4096
    ptrs=struct.unpack_from('<'+'I'*n,buf,e['data_off'])
    cur=e['data_off']+4*n
    out=bytearray()
    for p in ptrs:
        end=p & ~0xC0000000
        if p & 0xC0000000: raise RuntimeError('extended cramfs block flags not expected')
        out.extend(zlib.decompress(bytes(buf[cur:end])))
        cur=end
    return bytes(out[:e['size']])

raw=SRC.read_bytes()
if len(raw)<64: raise RuntimeError('input too small')
h=raw[:64]; data=bytearray(raw[64:])
magic,hcrc,ts,sz,load,ep,dcrc,osid,arch,typ,comp,name=struct.unpack('>7I4B32s',h)
if magic!=0x27051956 or sz!=len(data): raise RuntimeError('not expected uImage')
if binascii.crc32(data)&0xffffffff != dcrc: raise RuntimeError('source data CRC bad')
entries=tree(data)
p=entries['/etc/profile']
blocks=(p['size']+4095)//4096
if blocks!=1: raise RuntimeError('unexpected profile block count')
start=p['data_off']+4
old_end=struct.unpack_from('<I',data,p['data_off'])[0]
enc=zlib.compress(PROFILE,9)
if len(enc)>old_end-start: raise RuntimeError('new profile does not fit old cramfs extent')
oldw1=struct.unpack_from('<I',data,p['off']+4)[0]
struct.pack_into('<I',data,p['off']+4,((oldw1>>24)<<24)|len(PROFILE))
new_end=start+len(enc)
struct.pack_into('<I',data,p['data_off'],new_end)
data[start:new_end]=enc
data[new_end:old_end]=b'\\0'*(old_end-new_end)
data[48:64]=b'Stayplay MEM256K'
struct.pack_into('<I',data,32,0)
struct.pack_into('<I',data,32,binascii.crc32(data)&0xffffffff)
entries=tree(data)
loader=read_file(data,entries['/bin/load3531-mem256'])
for s in (b'if [ "$HVRMEM_OS" = "256" ]; then',b'anonymous,0,0xC0000000,128M:ddr1,0,0xC8000000,128M'):
    if s not in loader: raise RuntimeError('MEM256 MMZ invariant missing')
dcrc=binascii.crc32(data)&0xffffffff
nm=b'Stayplaytion MEM256K'.ljust(32,b'\\0')
h=struct.pack('>7I4B32s',0x27051956,0,0,len(data),0,0,dcrc,5,2,3,0,nm)
hcrc=binascii.crc32(h)&0xffffffff
h=struct.pack('>7I4B32s',0x27051956,hcrc,0,len(data),0,0,dcrc,5,2,3,0,nm)
DST.write_bytes(h+data)
print(DST, DST.stat().st_size, hex(hcrc), hex(dcrc))
