import json,os,sys,collections
SP=sys.argv[1]; HIS=SP+'/profiles_before'; NEW=SP+'/merged/cert/profiles'
o=json.load(open(SP+'/outcomes_before.json')); rows=o if isinstance(o,list) else o.get('rows',[])
exp=[r for r in rows if r.get('state')=='exported']
def flat(v,p=''):
    out={}
    if isinstance(v,dict):
        for k,x in v.items(): out.update(flat(x,p+'.'+k if p else k))
    elif isinstance(v,list):
        out[p]=json.dumps(v,sort_keys=True)
    else: out[p]=v
    return out
diffkeys=collections.Counter(); per={}
for r in exp:
    name=os.path.basename(r['profile']); h=json.load(open(HIS+'/'+name)); n=json.load(open(NEW+'/'+name)) if os.path.exists(NEW+'/'+name) else None
    if n is None: per[r['product']]=['NO RE-DERIVED PROFILE']; continue
    fh={k:v for k,v in flat(h).items() if not k.startswith('tone_check')}; fn={k:v for k,v in flat(n).items() if not k.startswith('tone_check')}
    d=[]
    for k in sorted(set(fh)|set(fn)):
        if fh.get(k)!=fn.get(k): d.append(k); diffkeys[k]+=1
    per[r['product']]=d
print('exported', len(exp)); print('field differences (his vs current re-derive, tone_check aside):'); [print(f'  {n:3d} x {k}') for k,n in diffkeys.most_common(40)]
same=[p for p,d in per.items() if not d]; print('identical apart from the tone check:', len(same))
json.dump(per, open(SP+'/publish_diff.json','w'), indent=1)
