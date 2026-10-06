import sys,subprocess,re,os
# run a tone-check trace's process under the four policies, optionally overriding set norms ("5:0.484375,6:0.5")
P="/Users/SeanD/Documents/ECHOJAY FILES/ECHOJAY VST/ejmap-cert-wt/build-ejmap/EchoJayProbe_artefacts/RelWithDebInfo/EchoJayProbe"
SC=os.path.dirname(os.path.abspath(__file__))
trace,tag=sys.argv[1],sys.argv[2]; levels=sys.argv[3]; override=dict(x.split(':') for x in sys.argv[4].split(',')) if len(sys.argv)>4 and sys.argv[4] else {}
t=open(trace).read().splitlines(); spec={}; sets=[]; norm=None
for line in t:
    f=line.split('\t')
    if line.startswith('probe: '): name=line.split('"')[1]; ident=line.split('|')[1].strip(); uid=line.split('uid=')[1].split()[0]
    elif f[0]=='sweep': thr=f[f.index('thr')+1]
    elif f[0]=='spec':
        for i in range(1,len(f)-1,2): spec[f[i]]=f[i+1]
    elif f[0]=='set': sets.append([f[1],f[2]])
    elif f[0]=='pos': norm=f[f.index('norm')+1]
seen=set()
for s in sets:
    if s[0] in override: s[1]=override[s[0]]; seen.add(s[0])
for k,v in override.items():
    if k not in seen: sets.append([k,v])
args=[P,name,ident,uid,'--sweep','thr='+thr,'norms='+norm,'levels='+levels,'hz=997','hold='+spec['hold_s'],'discard='+spec['discard_s'],'win='+spec['win_s'],'ref=0','moving_db='+spec['moving_db'],'reset='+('1' if spec.get('reset_per_hold')=='1' else '0')]
if sets: args.append('set='+','.join(a+':'+b for a,b in sets))
print(f"== {name}  thr {thr} norm {norm}  overrides {override}")
for pol in ('silent','unconnected','self','echojay'):
    out=subprocess.run(args+['sidechain='+pol],capture_output=True,text=True,timeout=300).stdout
    open(f"{SC}/{tag}.{pol}.txt","w").write(out)
    pl=re.search(r'^policy\tsidechain\t(\S+)',out,re.M); buses=len(re.findall(r'^bus\trender\tin\t[1-9]',out,re.M))
    sc=re.findall(r'^sidechain\t(\d+)\t([^\t]*)\t(\d+)\t\S+\t(\S+)',out,re.M)
    holds=re.findall(r'^hold\t\d+\t(\S+)\tlevel_db\t(\S+)\tin_rms_db\t(\S+).*?\tch\t(\S+)',out,re.M)
    done='stage\tdone' in out
    print(f"  {pol:12} policy={pl.group(1) if pl else '?':14} extra_in={buses} [{' '.join(b+':'+n+'/'+c+'/'+st for b,n,c,st in sc)}]  " + '  '.join(f"L{l}: GR {float(i)-float(o):.2f} ch {ch}" for l,o,i,ch in holds) + ('' if done else '  NOT DONE: '+(re.search(r'refused.*|crash.*',out) or [''])[0][:60]))
