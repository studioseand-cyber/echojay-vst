import json,glob,os,collections,sys
R=sys.argv[1]
def load(side): return {os.path.relpath(f,f'{R}/{side}'):json.load(open(f)) for f in glob.glob(f'{R}/{side}/phaseb/*/drafts/*.json')}
o,n=load('old'),load('new')
print('drafts old',len(o),'new',len(n),'only-old',len(set(o)-set(n)),'only-new',len(set(n)-set(o)))
def norm_status(x):
    return x.replace(' v0.1 (a proposal)',' v0.2 (a proposal)').replace('PROPOSAL v0.1 - not for publication','PROPOSAL v0.2 - not for publication') if isinstance(x,str) else x
def fix(d, path=()):
    # EXPECTED 1 (50a39e1b): every status string v0.1 -> v0.2 (top level and the timing block's own)
    if isinstance(d,dict): return {k:(norm_status(v) if k=='status' else fix(v,path+(k,))) for k,v in d.items()}
    if isinstance(d,list): return [fix(v,path) for v in d]
    return d
NOTE_OLD='(limiters are not run through the compressor certification)'
NOTE_NEW="(the limiter_comp step has not run on it, or its certification did not export - phaseb/limitercomp/outcome/ says which)"
c=collections.Counter(); unexplained=[]
for k in sorted(set(o)&set(n)):
    a,b=o[k],n[k]
    if a==b: c['identical']+=1; continue
    cat=k.split('/')[1]; reasons=[]
    a2=fix(a)
    if a2!=a: reasons.append('status v0.2 (50a39e1b)')
    if cat=='limiter':
        # EXPECTED 2 (5185311f, worded by 7ee67720): vs 00f2ae79 a limiter draft gains compressor_profile null + exactly one appended
        # note naming why it is the block alone (Sean's folder: no limiter has a compressor profile yet)
        nb=b.get('notes') or []; na=a2.get('notes') or []
        if 'compressor_profile' in b and b['compressor_profile'] is None and nb[:-1]==na and NOTE_NEW in nb[-1] and 'no compressor profile exported for' in nb[-1]:
            a2=dict(a2, compressor_profile=None, notes=nb); reasons.append('limiter: compressor_profile null + the one-profile note (5185311f / 7ee67720)')
    if a2==b: c[' + '.join(reasons)]+=1
    else: unexplained.append((k,sorted(x for x in set(a2)|set(b) if a2.get(x)!=b.get(x))))
for k,v in c.most_common(): print(f'  {v:5d}  {k}')
print('UNEXPLAINED:',len(unexplained)); [print('   ',u) for u in unexplained[:12]]
