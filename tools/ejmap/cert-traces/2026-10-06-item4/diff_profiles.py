import json,sys,os,glob,collections
old,new=sys.argv[1],sys.argv[2]
LOST={'Solid Bus Comp','kHs Compressor','CLA-76 (m)','CLA-76 (s)','Unfiltered Audio Zip','bx_opto','elysia alpha master','elysia alpha mix','CLA-3A (m)','CLA-3A (s)','CLA-2A (m)','CLA-2A (s)','DPR-402 (m)','DPR-402 (s)','C1 comp-sc (m)','C1 comp-sc (s)','C1 comp-gate (m)','C1 comp-gate (s)','API-2500 (m)','API-2500 (s)','Ozone 12 Dynamics','Abbey Road RS124 (m)','Abbey Road RS124 (s)','Pro Audio DSP DSM V3','SSL Native X-Comp v6','SPL IRON','Kiive XTComp','Acme Opticom XLA-3','AMEK Mastering Compressor','VBC FG-Red','VBC FG-MU','VBC Rack','VBC FG-Grey','MixWave DW Fearn VT-7','Maag MAGNUM-K','Millennia TCL-2','Lindell 7X-500','Lindell 254E','Lindell 354E','Bettermaker Bus Compressor DSP','Lindell MBC','Lindell SBC','SSL LMC+','Shadow Hills Class A Mastering Comp','Shadow Hills Mastering Compressor','SSL G3 MultiBusComp','Vertigo VSC-2','Auto-Tune Vocal Compressor','SSL Native Bus Compressor 2'}
def writes(p):
    w={}
    for e in p.get('neutral',[]) or []: w[e['control']]=(round(e['norm'],4), e.get('set'))
    for e in p.get('engage',[]) or []: w['ENGAGE:'+e.get('control','')]=(round(e['norm'],4), e.get('set'))
    r=(p.get('ratio') or {}); c=(r.get('curve') or [{}])[0]
    if r.get('control'): w['RATIO:'+r['control']]=(round(c.get('norm',-1),4), c.get('set'))
    return w
def curve(p): return [(round(c['norm'],4), c.get('in_at_gr_dbfs',{}).get('1'), c.get('in_at_gr_dbfs',{}).get('2')) for c in p['amount']['curve']]
def load(d):
    out={}
    for f in glob.glob(d+'/profiles/*.json'):
        if f.endswith('.tonecheck.json'): continue
        p=json.load(open(f)); out[p['plugin']['name']]=p
    return out
oldP,newP=load(old),load(new)
print('4 Oct profiles', len(oldP), '| fixed-build derive-only profiles', len(newP), '| in both', len(set(oldP)&set(newP)), '| 4 Oct only', sorted(set(oldP)-set(newP)))
print('new only (no 4 Oct profile to compare: products first exported by the 5 Oct follow-up):', len(set(newP)-set(oldP)))
cats=collections.Counter(); details=collections.defaultdict(list); lostBoth=[]
for n in sorted(set(oldP)&set(newP)):
    o,x=oldP[n],newP[n]
    if n in LOST: lostBoth.append(n)
    wo,wx=writes(o),writes(x)
    if wo!=wx: d=[(k,wo.get(k),wx.get(k)) for k in set(wo)|set(wx) if wo.get(k)!=wx.get(k)]; cats['writes differ']+=1; details['writes differ'].append((n,d[:4]))
    if o['amount']['control']!=x['amount']['control']: cats['amount control differs']+=1; details['amount control differs'].append((n,o['amount']['control'],x['amount']['control']))
    co,cx=curve(o),curve(x)
    if [c[0] for c in co]!=[c[0] for c in cx]: cats['curve norms differ']+=1; details['curve norms differ'].append((n,len(co),len(cx)))
    elif co!=cx: cats['curve values differ']+=1; details['curve values differ'].append((n,[ (a,b) for a,b in zip(co,cx) if a!=b][:2]))
    for k in ('detector_f','static_gain_db','topology','map_fp'):
        vo=o.get(k) if k!='map_fp' else o['plugin'].get('map_fp'); vx=x.get(k) if k!='map_fp' else x['plugin'].get('map_fp')
        if vo!=vx: cats[k+' differs']+=1; details[k+' differs'].append((n,vo,vx))
    ko,kx=set(o.keys()),set(x.keys())
    if ko!=kx: cats['top-level keys differ']+=1; details['top-level keys differ'].append((n,sorted(kx-ko),sorted(ko-kx)))
    if (o['amount'].get('stepped'))!=(x['amount'].get('stepped')): cats['stepped differs']+=1; details['stepped differs'].append((n,o['amount'].get('stepped'),x['amount'].get('stepped')))
    mo,mx=set(o.get('measured',{}).keys()),set(x.get('measured',{}).keys())
    if mo!=mx: cats['measured keys differ']+=1; details['measured keys differ'].append((n,sorted(mx-mo),sorted(mo-mx)))
    pn=[k for k in x.get('notes',{}) if k not in o.get('notes',{})] if isinstance(x.get('notes'),dict) else []
    if pn: cats['new note keys']+=1; details['new note keys'].append((n,pn[:4]))
print('of the 49 that lost their writes, in both profile sets:', len(lostBoth), '-> writes identical for', sum(1 for n in lostBoth if writes(oldP[n])==writes(newP[n])), 'of', len(lostBoth))
print(dict(cats))
for k,v in details.items():
    print('==',k, len(v)); [print('   ',str(i)[:200]) for i in v[:6]]
