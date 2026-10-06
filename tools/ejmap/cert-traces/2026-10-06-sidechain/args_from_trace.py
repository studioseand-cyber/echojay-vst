import sys
# mirror of sidechaincheck::argsFor over a single-position sweep trace: prints the probe's args after the identity
t=open(sys.argv[1]).read().splitlines(); levels=sys.argv[2] if len(sys.argv)>2 else None
thr=None; spec={}; sets=[]; norm=None; holds=[]; ident=None; name=None
for line in t:
    f=line.split('\t')
    if line.startswith('probe: '):
        name=line.split('"')[1]; ident=line.split('|')[1].strip(); uid=line.split('uid=')[1].split()[0]
    if f[0]=='sweep':
        thr=f[f.index('thr')+1]
    elif f[0]=='spec':
        for i in range(1,len(f)-1,2): spec[f[i]]=f[i+1]
    elif f[0]=='set': sets.append(f[1]+':'+f[2])
    elif f[0]=='pos': norm=f[f.index('norm')+1]
    elif f[0] in ('hold','rerender'): holds.append(f[2])
lv=levels or ','.join(sorted(set(holds), key=float))
args=['--sweep','thr='+thr,'norms='+norm,'levels='+lv,'hz=997','hold='+spec['hold_s'],'discard='+spec['discard_s'],'win='+spec['win_s'],'ref=0','moving_db='+spec['moving_db'],'reset='+('1' if spec.get('reset_per_hold')=='1' else '0')]
if sets: args.append('set='+','.join(sets))
print(name); print(ident); print(uid); print('\n'.join(args))
