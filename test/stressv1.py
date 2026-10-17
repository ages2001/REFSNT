import os,random,subprocess,sys,tempfile
T=os.environ.get('RFSTOOL', os.path.join(os.environ.get('OUT', '/tmp/refsbuild'), 'rfstool')); img=sys.argv[1]; rounds=int(sys.argv[2]); seed=int(sys.argv[3]) if len(sys.argv)>3 else 1
rnd=random.Random(seed); model={'/':None}; D=tempfile.mkdtemp(); ctr=0
def run(args,ok=True):
    r=subprocess.run([T,img]+args,capture_output=True,text=True)
    if ok and r.returncode: print('FAIL',args,r.stdout[-400:],r.stderr[-200:]); sys.exit(1)
    return r
def dirs(): return [p for p,v in model.items() if v is None]
def files(): return [p for p,v in model.items() if v is not None]
for rnd_i in range(rounds):
    ops=[]
    for _ in range(rnd.randint(5,15)):
        a=rnd.random()
        if a<0.3:
            d=rnd.choice(dirs()); n='d%d'%rnd.randint(0,9999); p=(d.rstrip('/')+'/'+n)
            if p not in model: ops.append('mkdir:'+p); model[p]=None
        elif a<0.7:
            d=rnd.choice(dirs()); n='f%d'%rnd.randint(0,9999); p=(d.rstrip('/')+'/'+n)
            if p not in model:
                sz=rnd.choice([0,10,500,5000,70000,300000]); data=os.urandom(sz)
                ctr+=1; tf='%s/t%d'%(D,ctr); open(tf,'wb').write(data)
                ops.append('put:%s:%s'%(tf,p)); model[p]=data
        else:
            fs=files()
            if fs: p=rnd.choice(fs); ops.append('rm:'+p); del model[p]
    run(['do']+ops+['commit'])
    for p in list(files()):
        r=run(['get',p,os.path.join(D, 'g.out')],ok=False)
        if r.returncode: print('GETFAIL round',rnd_i,p); sys.exit(1)
        got=open(os.path.join(D, 'g.out'),'rb').read()
        if got!=model[p]: print('MISMATCH round',rnd_i,p,'got',len(got),'exp',len(model[p])); sys.exit(1)
    for _ in range(ctr): pass
print('OK',rounds,'rounds;',len(files()),'files',len(dirs()),'dirs')
