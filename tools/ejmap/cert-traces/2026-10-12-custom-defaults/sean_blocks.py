import os,re,json,sys,gzip,glob
roots=[sys.argv[1]+'/raw']+glob.glob(sys.argv[1]+'/phaseb/*/raw')
out={}   # (uid,version) -> {name, fid, source, params{idx:[norm,name,text]}}
pri=lambda f: 0 if '.categorise.resp30.' in f else 1
files=[]
for r in roots:
    for f in os.listdir(r): files.append(os.path.join(r,f))
files.sort(key=pri)
stemre=re.compile(r'AudioUnit_([0-9a-f]+)_(\d+(?:\.\d+)*)\.(?=[a-zA-Z])')
n=0
for f in files:
    m=stemre.search(os.path.basename(f))
    if not m: continue
    key=(m.group(1).lower(), m.group(2))
    if key in out and out[key]['source_rank']==0: continue
    if key in out and pri(f)>=out[key]['source_rank']: continue
    op=gzip.open if f.endswith('.gz') else open
    try:
        name=fid=None; params={}; dirty=False
        with op(f,'rt',errors='replace') as fh:
            for i,l in enumerate(fh):
                if i>4000: break
                if l.startswith('probe: '):
                    p=l.split(' | '); name=p[0][len('probe: '):].strip().strip('"'); fid=p[1].strip() if len(p)>1 else None
                elif l.startswith(('set\t','write\t','pos\t','hold\t')) and not params: dirty=True; break
                elif l.startswith('param\t'):
                    t=l.rstrip('\n').split('\t')
                    if len(t)>=5:
                        try: params[int(t[1])]=[float(t[2]),t[3],t[4]]
                        except: pass
                elif params and not l.startswith('param\t'): break
        if dirty or not params or not name: continue
        out[key]={'name':name,'fid':fid,'source':os.path.basename(f),'source_rank':pri(f),'params':params}; n+=1
    except Exception as e: pass
json.dump({f'{k[0]}|{k[1]}':v for k,v in out.items()}, open(sys.argv[2],'w'))
print('clean instantiate blocks from Sean\'s traces:',len(out),'(categorise resp30:',sum(1 for v in out.values() if v['source_rank']==0),')')
