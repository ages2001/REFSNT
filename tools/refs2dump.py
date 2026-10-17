import struct,sys
def u16(b,o): return struct.unpack_from('<H',b,o)[0]
def u32(b,o): return struct.unpack_from('<I',b,o)[0]
def u64(b,o): return struct.unpack_from('<Q',b,o)[0]
class R2:
    def __init__(s,path,cl):
        s.f=open(path,'rb'); s.cl=cl
        s.f.seek(0); v=s.f.read(512); s.tot=(u64(v,0x18)*u32(v,0x20))//cl
        s.f.seek(30*cl); sb=s.f.read(cl)
        off=u32(sb,0x70); s.cp0=u64(sb,off); s.cp1=u64(sb,off+8)
        s.cont=None  # cluster->phys map set later
    def blk(s,vlcn):
        p = s.phys(vlcn)
        s.f.seek(p*s.cl); return s.f.read(s.cl)
    def phys(s,vlcn):
        if s.cont is None: return vlcn
        c=vlcn//s.cpc; off=vlcn%s.cpc
        return s.cont.get(c,vlcn if c==0 else (1<<60))+off if c in s.cont else vlcn
    def roots(s):
        s.f.seek(s.cp0*s.cl); cp=s.f.read(s.cl)
        n=u32(cp,0x90); base=0x94; rs=u32(cp,0x5c); out=[]
        for i in range(n):
            o=u32(cp,base+4*i); ref=cp[o:o+rs]; out.append(u64(ref,0))
        return out
    def node_rows(s,page):
        # root page: descriptor+header at 0x50
        At=0x50; d=u32(page,At); hdr=At+d; h=page[hdr:]
        lvl=page[hdr+0x0c]; fl=page[hdr+0x0d]
        ks=u32(page,hdr+0x10); cnt=u32(page,hdr+0x14)
        rows=[]
        for i in range(cnt):
            e=u32(page,hdr+ks+4*i); ro=hdr+(e&0xffff)
            rs=u32(page,ro); ko=u16(page,ro+4); kl=u16(page,ro+6); flg=u16(page,ro+8); vo=u16(page,ro+0x0a); vl=u16(page,ro+0x0c)
            rows.append(dict(key=page[ro+ko:ro+ko+kl],val=page[ro+vo:ro+vo+vl],flags=flg))
        return lvl,fl,cnt,rows,d
    def walk_leaves(s,vlcn,depth=0):
        """yield leaf rows, descending child pointers (value is a ref whose LCN@0)"""
        page=s.blk(vlcn)
        if page[:4]!=b'MSB+': return
        lvl,fl,cnt,rows,d=s.node_rows(page)
        if lvl==0:
            for r in rows: yield r
        else:
            for r in rows:
                if len(r['val'])>=8:
                    child=u64(r['val'],0)
                    yield from s.walk_leaves(child,depth+1)
