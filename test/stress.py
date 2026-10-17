#!/usr/bin/env python3
"""Random operations on a ReFS image through rfstool, checked against a model."""
import os, random, subprocess, sys, hashlib, tempfile
T=os.environ.get('RFSTOOL', os.path.join(os.environ.get('OUT', '/tmp/refsbuild'), 'rfstool'))
img=sys.argv[1]; rounds=int(sys.argv[2]); seed=int(sys.argv[3]) if len(sys.argv)>3 else 1
rnd=random.Random(seed)
TMP=tempfile.mkdtemp()
model={'/':None}          # path -> bytes (files) or None (dirs)
base=set()                # paths that existed before (not tracked by content)
def run(*args, ok=True):
    r=subprocess.run([T,img]+list(args),capture_output=True,text=True)
    if ok and r.returncode!=0:
        print('FAIL',args,r.stdout[-2000:],r.stderr[-2000:]); sys.exit(1)
    return r
def dirs(): return [p for p,v in model.items() if v is None]
def files(): return [p for p,v in model.items() if v is not None]
def children(d):
    pre=d.rstrip('/')+'/'
    return [p for p in model if p!=d and p.startswith(pre) and '/' not in p[len(pre):]]
def newname(d):
    while True:
        n=''.join(rnd.choice('abcdefghijklmnopqrstuvwxyz0123456789_') for _ in range(rnd.randint(1,24)))
        if rnd.random()<0.3: n+='.'+rnd.choice(['txt','bin','dat','log'])
        p=(d.rstrip('/')+'/'+n)
        if not any(q.lower()==p.lower() for q in model): return p
def data(n):
    k=rnd.random()
    if k<0.3: return os.urandom(n)
    return bytes([rnd.randint(0,255)])*n
fid=0
def tmpfile(b):
    global fid; fid+=1
    p='%s/f%d'%(TMP,fid); open(p,'wb').write(b); return p
# start inside a fresh test dir
def leak():
    for l in run('check').stdout.splitlines():
        if l.startswith('allocated but not referenced:'): return int(l.split(':')[1])
L0=leak()
top='/stress%d'%seed
run('do','mkdir:'+top); model[top]=None
for rd in range(rounds):
    ops=[]
    for i in range(rnd.randint(5,40)):
        k=rnd.random()
        ds=[d for d in dirs() if d.startswith(top)]
        fs=[f for f in files()]
        if k<0.12 and len(ds)<60:
            p=newname(rnd.choice(ds)); ops.append('mkdir:'+p); model[p]=None
        elif k<0.40:
            p=newname(rnd.choice(ds)); n=rnd.choice([0,1,100,4095,4096,4097,20000,rnd.randint(0,300000),rnd.randint(0,3000000)])
            b=data(n); ops.append('put:%s:%s'%(tmpfile(b),p)); model[p]=b
        elif k<0.52 and fs:
            p=rnd.choice(fs); old=model[p]; off=rnd.randint(0,len(old)+50000); n=rnd.randint(1,200000)
            b=data(n); ops.append('write:%s:%d:%s'%(p,off,tmpfile(b)))
            nb=bytearray(old)
            if off>len(nb): nb+=b'\0'*(off-len(nb))
            nb[off:off+n]=b; model[p]=bytes(nb)
        elif k<0.62 and fs:
            p=rnd.choice(fs); old=model[p]; n=rnd.choice([0,1,rnd.randint(0,len(old)+1),len(old)+rnd.randint(0,100000)])
            ops.append('trunc:%s:%d'%(p,n)); model[p]=(old[:n]+b'\0'*max(0,n-len(old)))
        elif k<0.75 and fs:
            p=rnd.choice(fs); ops.append('rm:'+p); del model[p]
        elif k<0.80:
            e=[d for d in ds if d!=top and not children(d)]
            if e: p=rnd.choice(e); ops.append('rm:'+p); del model[p]
        elif k<0.92 and fs:
            p=rnd.choice(fs); q=newname(rnd.choice(ds)); ops.append('mv:%s:%s'%(p,q)); model[q]=model.pop(p)
        elif k<0.97:
            e=[d for d in ds if d!=top]
            if e:
                p=rnd.choice(e); tgt=[d for d in ds if not (d==p or d.startswith(p+'/'))]
                q=newname(rnd.choice(tgt)); ops.append('mv:%s:%s'%(p,q))
                for x in sorted([x for x in model if x==p or x.startswith(p+'/')]):
                    model[q+x[len(p):]]=model.pop(x)
        else:
            ops.append('commit')
    run('do',*ops)
    # verify listing + content
    r=run('files'); got={}
    for line in r.stdout.splitlines():
        if line.startswith('D '): got[line[2:]]=None
        elif line.startswith('F ') and not line.endswith(' reparse') and ' ERROR ' not in line:
            parts=line[2:].rsplit(' ',2); got[parts[0]]=(int(parts[1]),parts[2])
    def fnv(b):
        h=1469598103934665603
        for x in b: h=((h^x)*1099511628211)&0xffffffffffffffff
        return '%016x'%h
    bad=0
    mine={p:v for p,v in model.items() if p.startswith(top)}
    theirs={p:v for p,v in got.items() if p.startswith(top)}
    for p,v in mine.items():
        if p not in theirs: print('MISSING',p); bad+=1
        elif v is None and theirs[p] is not None: print('NOTDIR',p); bad+=1
        elif v is not None and (theirs[p] is None or theirs[p][0]!=len(v) or theirs[p][1]!=fnv(v)): print('CONTENT',p,len(v),theirs[p]); bad+=1
    for p in theirs:
        if p not in mine: print('EXTRA',p); bad+=1
    c=run('check').stdout.strip().splitlines()[-1]
    o=run('order').stdout.strip().splitlines()[-1]
    alloc=[l for l in run('check').stdout.splitlines() if 'DIFFERS' in l or 'bad rows' in l and not l.endswith(' 0 bad rows')]
    print('round',rd,'ops',len(ops),'files',len(files()),'bad',bad,'|',c,'|',o, alloc)
    lk=leak()
    if lk!=L0: print('LEAK',L0,lk); bad+=1
    if bad or ' 0 not allocated' not in c or ' 0 order errors, 0 other' not in o or alloc: sys.exit(1)
    for f in os.listdir(TMP): os.unlink(os.path.join(TMP,f))
print('OK')
