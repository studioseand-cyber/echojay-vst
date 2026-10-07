import json,sys,os,glob,re,collections
r=sys.argv[1]; o=json.load(open(r+'/old/outcomes.json')); n=json.load(open(r+'/new/outcomes.json'))
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
    why=collections.Counter()
    for f in diff:
        a=json.loads(norm(open(r+'/old/'+sub+'/'+f).read())); b=json.loads(norm(open(r+'/new/'+sub+'/'+f).read()))
        for t,p in keydiff(a,b): why[t+re.sub(r'\[\d+\]','[]',p)]+=1
    print('  %s: %d old / %d new, only-old %s, only-new %s, differ %d' % (sub,len(fo),len(fn),sorted(set(fo)-set(fn))[:3],sorted(set(fn)-set(fo))[:3],len(diff)))
    for k,v in why.most_common(12): print('     ',v,k)
