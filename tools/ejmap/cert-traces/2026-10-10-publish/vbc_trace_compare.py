import sys,os,re,glob
C2,C3=sys.argv[1],sys.argv[2]
units=[('VBC FG-Grey','645b6178','VBC_FG-Grey'),('VBC FG-MU','645b6172','VBC_FG-MU'),('VBC Rack','645b6174','VBC_Rack'),('VBC FG-Red','645b616d','VBC_FG-Red')]
def rd(f): return open(f,errors='replace').read() if os.path.exists(f) else ''
def params(t): return {l.split('\t')[1]:(l.split('\t')[2], l.split('\t')[3] if len(l.split('\t'))>3 else '', '\t'.join(l.split('\t')[4:])) for l in t.splitlines() if l.startswith('param\t')}
def lines(t,pfx): return [l for l in t.splitlines() if l.startswith(pfx)]
for prod,uid,stem in units:
    sw=sorted(glob.glob(f'{C2}/raw/AudioUnit_{uid}_1.3.5.sweep.c*.pos00.1.txt'))
    tc=f'{C3}/raw/{stem}_1.3.5.tonecheck.1.txt'
    print('=====',prod,'| sweep trace:',os.path.basename(sw[0]) if sw else None,'| tone trace exists:',os.path.exists(tc))
    a,b=rd(sw[0]) if sw else '',rd(tc)
    for pf in ('probe:','policy','sidechain','bus\t','config'):
        la,lb=lines(a,pf),lines(b,pf)
        if la!=lb: print('  DIFF',pf,'\n     4 Oct:',la[:3],'\n    10 Oct:',lb[:3])
    pa,pb=params(a),params(b); d=[(k,pa.get(k),pb.get(k)) for k in sorted(set(pa)|set(pb),key=lambda x:int(x)) if pa.get(k)!=pb.get(k)]
    print('  params: 4 Oct',len(pa),'| 10 Oct',len(pb),'| differ:',len(d))
    for k,x,y in d[:14]: print(f'     [{k}] {x[2] if x else "-"}: {x[0] if x else "-"} \'{x[1] if x else ""}\' -> {y[0] if y else "-"} \'{y[1] if y else ""}\'')
    da=rd(f'{C2}/raw/AudioUnit_{uid}_1.3.5.defaults.text-at.1.txt'); db=rd(f'{C3}/raw/AudioUnit_{uid}_1.3.5.defaults.text-at.1.txt')
    print('  defaults re-read 10 Oct:', 'present' if db else 'absent', '| identical to 4 Oct' if da and db and da.split('\n',1)[1:]==db.split('\n',1)[1:] else '')
