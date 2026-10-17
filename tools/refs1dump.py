#!/usr/bin/env python3
"""refs1dump.py - ReFS 1.x (1.1 / 1.2) explorer, used to analyse the format (see doc/REFS1.md).

  refs1dump.py IMAGE            volume header, superblocks, checkpoints, roots, allocators
  refs1dump.py IMAGE OID...     every row of those objects' tables (embedded tables expanded)

IMAGE is the volume itself (a raw image; convert a VHD first: qemu-img convert -O raw).
"""
import struct, sys
B=16384
def u16(b,o): return struct.unpack_from('<H',b,o)[0]
def u32(b,o): return struct.unpack_from('<I',b,o)[0]
def u64(b,o): return struct.unpack_from('<Q',b,o)[0]
P=0x9A6C9329AC4BC9B5; M=(1<<64)-1
T=[]
for i in range(256):
    c=i
    for _ in range(8): c=(c>>1)^P if c&1 else c>>1
    T.append(c)
def crc64(d,c=M):
    for b in d: c=T[(c^b)&0xff]^(c>>8)
    return c^M
def hd(b,base=0,n=None):
    n=len(b) if n is None else min(n,len(b)); out=[]
    for i in range(0,n,16):
        c=b[i:i+16]
        out.append('%06x: %-48s %s'%(base+i,' '.join('%02x'%x for x in c),''.join(chr(x) if 32<=x<127 else '.' for x in c)))
    return '\n'.join(out)
class Vol:
    def __init__(s,path):
        s.f=open(path,'rb')
        sb=s.blk(30); co=u32(sb,0x50)
        cps=[u64(sb,co+8*i) for i in range(u32(sb,0x54))]
        best=max(cps,key=lambda c:u64(s.blk(c),0x40))
        s.chkp_blk=best; s.chkp=s.blk(best)
    def blk(s,n): s.f.seek(n*B); return s.f.read(B)
    def raw(s,off,n): s.f.seek(off); return s.f.read(n)
    def ref(s,b,o):
        return dict(blk=u64(b,o),flags=u16(b,o+8),cktype=b[o+10],ckoff=b[o+11],cklen=u16(b,o+12),ck=b[o+16:o+16+u16(b,o+12)])
    def check(s,r,page):
        if r['cktype']==2: return crc64(page)==int.from_bytes(r['ck'],'little')
        return None

def node_at(buf, desc):
    """desc: offset of the index root (descriptor) in buf. returns dict"""
    hoff=u32(buf,desc)
    d=dict(desc=desc, desclen=hoff, w4=u16(buf,desc+4), kind=u16(buf,desc+6), w8=u32(buf,desc+8),
           schema=u32(buf,desc+0xc), schema2=u32(buf,desc+0x10), w14=u32(buf,desc+0x14),
           q18=u64(buf,desc+0x18), q20=u64(buf,desc+0x20), hdrdata=buf[desc+0x28:desc+hoff])
    return d, hoff
def nodehdr(buf, h):
    return dict(h=h, dstart=u32(buf,h), dend=u32(buf,h+4), free=u32(buf,h+8), level=buf[h+0xc], flags=buf[h+0xd],
                w0e=u16(buf,h+0xe), astart=u32(buf,h+0x10), count=u32(buf,h+0x14), aend=u32(buf,h+0x18), w1c=u32(buf,h+0x1c))
def rows(buf, h):
    n=nodehdr(buf,h); out=[]
    for i in range(n['count']):
        e=u32(buf,h+n['astart']+4*i)
        ro=h+(e&0xffff) if (e>>16)==0xffff else h+e
        rs,ko,kl,fl,vo,vl=struct.unpack_from('<IHHHHH',buf,ro)
        out.append(dict(off=ro,size=rs,koff=ko,klen=kl,flags=fl,voff=vo,vlen=vl,w0e=u16(buf,ro+0xe),
                        key=buf[ro+ko:ro+ko+kl], val=buf[ro+vo:ro+vo+vl], rawe=e))
    return n,out
def page_node(V, blk):
    p=V.blk(blk)
    # root pages carry a descriptor at 0x30; non-root pages? detect: if u32 at 0x30 looks like small descriptor size
    return p

def leaf_rows(V, blk):
    """descend a tree from block blk, returning only level-0 (leaf) rows.
    In ReFS 1.x every tree page (root and internal) carries the index
    descriptor at 0x30, with the node header at 0x30+desclen."""
    p=V.blk(blk)
    d,ho=node_at(p,0x30); h=0x30+ho
    n,rs=rows(p,h)
    if n['level']==0:
        return [x for x in rs if x['klen']>0]
    out=[]
    for r in rs:
        if r['vlen']<16: continue            # skip +inf sentinel / empty child ptr
        cr=V.ref(r['val'],0)
        out+=leaf_rows(V,cr['blk'])
    return out

def objects(V):
    cp=V.chkp
    o=u32(cp,0x5c); r=V.ref(cp,o)
    return {u64(x['key'],8):x['val'] for x in leaf_rows(V,r['blk'])}
def walk_page(V, blk, depth=0, out=None, verbose=True):
    """walk a tree whose root page is blk; yields (level, node, rows) """
    p=V.blk(blk)
    d,ho=node_at(p,0x30)
    return walk_node(V,p,0x30+ho,d,depth)
def walk_node(V,p,h,d,depth):
    n,rs=rows(p,h)
    res=[(depth,n,rs,d)]
    if n['level']>0:
        for r in rs:
            if r['vlen']<16: continue
            cr=V.ref(r['val'],0); cp=V.blk(cr['blk'])
            ok=V.check(cr,cp)
            res.append(('child',cr['blk'],ok,cp[:0x60]))
            cd,cho=node_at(cp,0x30)              # child also carries descriptor at 0x30
            res+=walk_node(V,cp,0x30+cho,cd,depth+1)
    return res

def table_rows(V, oid):
    objs=objects(V); r=V.ref(objs[oid],0); out=[]
    for item in walk_page(V,r['blk']):
        if item[0]=='child': continue
        out+=item[2]
    return out
def embedded(val):
    d,ho=node_at(val,0); n,rs=rows(val,ho)
    return d,n,rs

def show_emb(val,ind='      '):
    d,n,rs=embedded(val)
    print(ind+'desc len %x w4 %x kind %x w8 %x schema %x/%x w14 %x q18 %x q20 %x'%(d['desclen'],d['w4'],d['kind'],d['w8'],d['schema'],d['schema2'],d['w14'],d['q18'],d['q20']))
    print(ind+'hdrdata:'); print('\n'.join(ind+'  '+l for l in hd(d['hdrdata'],0x28).split('\n')))
    print(ind+'node',{k:hex(v) for k,v in n.items()})
    for x in rs:
        print(ind+' row size %x fl %x k(%x,%x) v(%x,%x) w0e %x'%(x['size'],x['flags'],x['koff'],x['klen'],x['voff'],x['vlen'],x['w0e']))
        print(ind+'  key '+x['key'].hex())
        if x['flags']&8: show_emb(x['val'],ind+'    ')
        else: print('\n'.join(ind+'   '+l for l in hd(x['val']).split('\n')))

def info(V):
    v=V.raw(0,512)
    print('ReFS %d.%d, %d-byte sectors, %d sectors a cluster, %d sectors, serial %016x, flags %x'%(v[0x28],v[0x29],u32(v,0x20),u32(v,0x24),u64(v,0x18),u64(v,0x38),u32(v,0x2c)))
    sb=V.blk(30); co=u32(sb,0x50)
    for c in [u64(sb,co+8*i) for i in range(u32(sb,0x54))]:
        p=V.blk(c); so=u32(p,0x38); r=V.ref(p,so); z=bytearray(p); z[so:so+24]=bytes(24)
        print('checkpoint block %x: sequence %d, checksum %s'%(c,u64(p,0x40),'ok' if crc64(z)==int.from_bytes(r['ck'],'little') else 'BAD'))
    cp=V.chkp
    for i in range(u32(cp,0x58)):
        r=V.ref(cp,u32(cp,0x5c+4*i)); p=V.blk(r['blk']); d,ho=node_at(p,0x30)
        print('root %d: block %x table %x schema %x rows %d checksum %s'%(i,r['blk'],u64(p,0x18),d['schema'],d['q20'],'ok' if V.check(r,p) else 'BAD'))
    for oid,val in sorted(objects(V).items()):
        r=V.ref(val,0); p=V.blk(r['blk'])
        print('object %x: block %x checksum %s, next file id %x'%(oid,r['blk'],'ok' if V.check(r,p) else 'BAD',u64(val,0x28)))
if __name__=='__main__':
    V=Vol(sys.argv[1])
    if len(sys.argv)==2: info(V)
    for a in sys.argv[2:]:
        oid=int(a,16)
        print('######## OID %x'%oid)
        for x in table_rows(V,oid):
            print('  ROW fl %x key %s vlen %d'%(x['flags'],x['key'].hex(),x['vlen']))
            if x['flags']&8: show_emb(x['val'])
            else: print(hd(x['val']))
