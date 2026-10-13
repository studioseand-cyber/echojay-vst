import json,sys,os,glob,re,collections
r=sys.argv[1]
# --expect (13 Oct): the profile classes this baseline may explain. 'not_written' (d26db00f): the new profile's neutral is the old one
# minus EXACTLY the entries its not_written lists (by index), and nothing else differs. "differ" counts the UNEXPLAINED only.
EXPECT=set(x for x in sys.argv[sys.argv.index('--expect')+1].split(',') if x) if '--expect' in sys.argv else set()
def explained(sub,a,b):
    if sub!='profiles' or 'not_written' not in EXPECT or 'not_written' not in b or 'not_written' in a: return None
    drop={x.get('index') for x in b['not_written']}
    a2=dict(a, neutral=[x for x in a.get('neutral',[]) if x.get('index') not in drop]); b2={k:v for k,v in b.items() if k!='not_written'}
    if len(a2['neutral'])!=len(a.get('neutral',[]))-len(drop): return None
    return 'not_written (d26db00f)' if a2==b2 else None
o=json.load(open(r+'/old/outcomes.json')); n=json.load(open(r+'/new/outcomes.json'))
om={x['product']:x for x in o}; nm={x['product']:x for x in n}
d=[p for p in om if p in nm and (om[p].get('state'),om[p].get('reason'))!=(nm[p].get('state'),nm[p].get('reason'))]
VOL=re.compile(r'"(measuredAt|at|writtenAt|readAt|resampledAt|probe|cdhash|date|tool)"\s*:\s*("[^"]*"|[-0-9.]+)')
def norm(t): return VOL.sub(r'"\1":"~"', t)
print('  rows', len(o), len(n), 'state/reason diffs', len(d), d[:6])
def keydiff(a,b,path=''):
    out=[]
    if isinstance(a,dict) and isinstance(b,dict):
        for k in set(a)|set(b):
            if k not in a: out.append(('+',path+'/'+k))
            elif k not in b: out.append(('-',path+'/'+k))
            elif a[k]!=b[k]: out.extend(keydiff(a[k],b[k],path+'/'+k))
    elif isinstance(a,list) and isinstance(b,list) and len(a)==len(b):
        for i,(x,y) in enumerate(zip(a,b)):
            if x!=y: out.extend(keydiff(x,y,path+'['+str(i)+']'))
    else: out.append(('~',path))
    return out
for sub in ('fixtures','profiles','controls'):
    fo=sorted(os.path.basename(f) for f in glob.glob(r+'/old/'+sub+'/*.json')); fn=sorted(os.path.basename(f) for f in glob.glob(r+'/new/'+sub+'/*.json'))
    diff=[f for f in fo if f in fn and norm(open(r+'/old/'+sub+'/'+f).read())!=norm(open(r+'/new/'+sub+'/'+f).read())]
    why=collections.Counter(); ex=collections.Counter(); un=[]
    for f in diff:
        a=json.loads(norm(open(r+'/old/'+sub+'/'+f).read())); b=json.loads(norm(open(r+'/new/'+sub+'/'+f).read()))
        e=explained(sub,a,b)
        if e: ex[e]+=1; continue
        un.append(f)
        for t,p in keydiff(a,b): why[t+re.sub(r'\[\d+\]','[]',p)]+=1
    print('  %s: %d old / %d new, only-old %s, only-new %s, differ %d' % (sub,len(fo),len(fn),sorted(set(fo)-set(fn))[:3],sorted(set(fn)-set(fo))[:3],len(un)))
    for k,v in ex.most_common(): print('      explained',v,k)
    for k,v in why.most_common(12): print('     ',v,k)
