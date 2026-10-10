import json,gzip,os,sys,re,glob,collections
# compare.py <passA> <passB>: per unit, every JSON figure and every trace number, timing fields excluded
SP=os.path.dirname(os.path.abspath(__file__)); A,B=sys.argv[1],sys.argv[2]
TIMEKEY=re.compile(r'(^|_)(ms|seconds|s)$|_ms$|^ms$|At$|^at$|date|elapsed|slept|stamp|time_s$|wall|writtenAt|measuredAt|startedAt|updated|started|t_s$|^t0$|render_blocks|reads$|confirm|text_ms|landed_by|attempt|^runs$|cdhash|^probe$')
def norm_str(s): return s.replace('/par/'+A+'/','/par/X/').replace('/par/'+B+'/','/par/X/')
def walk(a,b,path,out):
    if isinstance(a,dict) and isinstance(b,dict):
        for k in set(a)|set(b):
            if TIMEKEY.search(k): continue
            if k not in a or k not in b: out['keys'].append(path+'/'+k); continue
            walk(a[k],b[k],path+'/'+k,out)
    elif isinstance(a,list) and isinstance(b,list):
        if len(a)!=len(b): out['lens'].append((path,len(a),len(b))); return
        for i,(x,y) in enumerate(zip(a,b)): walk(x,y,path+'[]',out)
    elif isinstance(a,(int,float)) and isinstance(b,(int,float)) and not isinstance(a,bool):
        d=abs(a-b)
        if d>0: out['num'].append((d,path,a,b))
    elif isinstance(a,str) and isinstance(b,str):
        if norm_str(a)!=norm_str(b): out['str'].append((path,a[:80],b[:80]))
    elif a!=b: out['other'].append((path,str(a)[:40],str(b)[:40]))
def files(root,pat): return {os.path.relpath(f,root):f for f in glob.glob(root+'/**/'+pat,recursive=True) if '/.tmp-' not in f}
def tracelines(f):
    t=gzip.open(f,'rt',errors='replace').read() if f.endswith('.gz') else open(f,errors='replace').read()
    return [l.split('\t') for l in t.splitlines() if l and not l.startswith(('probe:','JUCE','stage','policy','bus','config'))]
def isnum(s):
    try: float(s); return True
    except: return False
units=[l.split('|')[0] for l in open(SP+'/units.txt') if l.strip()]
for u in units:
    ra,rb=f'{SP}/{A}/{u}/cert',f'{SP}/{B}/{u}/cert'
    ja,jb=files(ra,'*.json'),files(rb,'*.json')
    out=collections.defaultdict(list)
    skip=('progress.json','summary.json','run.jsonl')
    for k in sorted(set(ja)|set(jb)):
        if k.endswith(skip): continue
        if k not in ja or k not in jb: out['only'].append(k); continue
        try: walk(json.load(open(ja[k])),json.load(open(jb[k])),k,out)
        except Exception as e: out['err'].append((k,str(e)[:60]))
    ta={k:v for k,v in files(ra,'*.txt*').items() if '/raw/' in k or k.startswith('raw/') or 'fixtures/raw' in k}
    tb={k:v for k,v in files(rb,'*.txt*').items() if k in ta}
    tnum=[]; tstr=0; tlen=0; nfiles=0
    for k in sorted(set(ta)&set(tb)):
        la,lb=tracelines(ta[k]),tracelines(tb[k]); nfiles+=1
        if len(la)!=len(lb): tlen+=1; continue
        for x,y in zip(la,lb):
            if len(x)!=len(y): tstr+=1; continue
            for i,(p,q) in enumerate(zip(x,y)):
                name=x[i-1] if i>0 else ''
                if TIMEKEY.search(name): continue
                if isnum(p) and isnum(q):
                    d=abs(float(p)-float(q))
                    if d>0: tnum.append((d,x[0],name))
                elif p!=q and not (',' in p and all(isnum(z) for z in p.split(',')+q.split(','))): tstr+=1
                elif ',' in p and p!=q:
                    for z,w in zip(p.split(','),q.split(',')):
                        if isnum(z) and isnum(w) and abs(float(z)-float(w))>0: tnum.append((abs(float(z)-float(w)),x[0],name+'[]'))
    num=sorted(out['num'],reverse=True)
    lvl=[d for d,p,a,b in num if re.search(r'db|gr|dbfs|level|ratio|norm|knee|offset|hz|in_at|rt60|time|ms\b',p,re.I)]
    print(f"== {u}: json figures differing {len(num)} (max |d| {num[0][0]:.4g} at {num[0][1]})" if num else f"== {u}: json figures differing 0", end='')
    print(f"; strings {len(out['str'])}; keys {len(out['keys'])}; list-lens {len(out['lens'])}; files only-one {len(out['only'])}")
    tn=sorted(tnum,reverse=True)
    print(f"   traces compared {nfiles}: numbers differing {len(tn)} (max |d| {tn[0][0]:.4g} in '{tn[0][1]}' {tn[0][2]})" if tn else f"   traces compared {nfiles}: numbers differing 0", f"; line-count diffs {tlen}; token diffs {tstr}")
    for x in out['str'][:3]: print('   str', x)
    for x in out['only'][:3]: print('   only', x)
    for x in out['lens'][:2]: print('   len', x)
    for x in num[:3]: print('   num', x)
