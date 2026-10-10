# every difference between dc77d0a5 (old/) and the new build (new/) on one equality set, each with its reason; UNEXPLAINED stops the cut
import json,os,sys,glob,collections,re
root=sys.argv[1]; old=root+'/old'; new=root+'/new'
def load(p):
    try: return json.load(open(p))
    except Exception: return None
def flat(v,p=''):
    out={}
    if isinstance(v,dict):
        for k,x in v.items(): out.update(flat(x,p+'.'+k if p else k))
    elif isinstance(v,list):
        if v and all(isinstance(x,dict) and 'control' in x for x in v):
            for x in v: out.update(flat(x, p+'['+str(x.get('control'))+']'))
        else:
            for i,x in enumerate(v): out.update(flat(x,p+'['+str(i)+']'))
    else: out[p]=v
    return out
meter=re.compile(r'gain reduction|(^|[^a-z0-9])gr([^a-z0-9]|$)', re.I)
def linked_twins(rec):
    rd=(rec or {}).get('ruleDecided') or {}
    return set(rd.get('twin') or []) if rd.get('rule')=='linked_pair' else set()
def pair_twin(rec):
    rd=(rec or {}).get('ruleDecided') or {}
    pw=rd.get('pair_with') or {}
    return pw.get('name')
results=collections.Counter(); unexplained=[]; detail=collections.defaultdict(list)
# records (fixtures)
for f in sorted(glob.glob(new+'/fixtures/*.json')):
    b=os.path.basename(f); o=load(old+'/fixtures/'+b); n=load(f)
    if o is None or n is None: continue
    fo,fn=flat(o),flat(n)
    for k in sorted(set(fo)|set(fn)):
        if fo.get(k)==fn.get(k): continue
        if 'instantiate_seen' in k: results['record: instantiate_seen added (VBC instantiate fix)']+=1; continue
        if '.preconditions' in k and pair_twin(n): results['record: pair twin out of preconditions (MAGNUM-K pair fix)']+=1; detail['pair'].append(b+' '+k); continue
        unexplained.append(('record',b,k,fo.get(k),fn.get(k)))
# profiles
for f in sorted(glob.glob(new+'/profiles/*.json')):
    b=os.path.basename(f); o=load(old+'/profiles/'+b); n=load(f)
    if o is None or n is None:
        if (o is None)!=(n is None): unexplained.append(('profile presence',b,'',o is None,n is None))
        continue
    prod=n.get('plugin',{}).get('name') or b
    rec=None
    for r in glob.glob(new+'/fixtures/*.json'):
        x=load(r)
        if x and x.get('product')==prod and 'thresholdCandidates' in x or (x and x.get('product')==prod and 'thresholdSweep' in json.dumps(x)[:20000]): rec=x; break
    tw=linked_twins(rec); on={e['control']:e for e in o.get('neutral',[])}; nn={e['control']:e for e in n.get('neutral',[])}
    for c in sorted(set(on)|set(nn)):
        a,z=on.get(c),nn.get(c)
        if a==z: continue
        if a and not z and c in tw: results['profile: linked twin no longer a neutral (linked-twin fix)']+=1; detail['twin'].append(prod+': '+c); continue
        if a and not z and meter.search(c) and not re.search(r'limit|amount|range|threshold|(^|[^a-z])on([^a-z]|$)', c, re.I): results['profile: output-only meter no longer a neutral (meter rule)']+=1; detail['meter'].append(prod+': '+c); continue
        if a and z and str(z.get('source','')).startswith('instantiate (as the sweep'): results["profile: neutral at the sweep's instantiate value (VBC instantiate fix)"]+=1; detail['inst'].append(prod+': '+c+' '+str(a.get('set'))+'@'+str(a.get('norm'))+' -> '+str(z.get('set'))+'@'+str(z.get('norm'))); continue
        unexplained.append(('profile neutral',prod,c,a,z))
    fo={k:v for k,v in flat(o).items() if not k.startswith('neutral')}; fn={k:v for k,v in flat(n).items() if not k.startswith('neutral')}
    for k in sorted(set(fo)|set(fn)):
        if fo.get(k)!=fn.get(k): unexplained.append(('profile',prod,k,fo.get(k),fn.get(k)))
for k,v in results.most_common(): print(f'  {v:4d} x {k}')
for k,v in detail.items(): print(f'     {k}: '+'; '.join(sorted(set(v))[:12]) + (' ...' if len(set(v))>12 else ''))
print(f'  UNEXPLAINED: {len(unexplained)}')
for u in unexplained[:25]: print('    ', str(u)[:240])
