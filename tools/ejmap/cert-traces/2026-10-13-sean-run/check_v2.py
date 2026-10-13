import json,glob,os,sys
C=os.path.expanduser(sys.argv[1]); BUILD=sys.argv[2]
WANT=['Mixland Vac Attack','dbx-160 (s)','DPR-402 (s)','Auto-Tune Vocal Compressor','VBC FG-Red','VBC FG-Grey','VBC FG-MU','VBC Rack']
def load(cat):
    d={}
    for f in glob.glob(f"{C}/phaseb/{cat}/{cat}/*.json"):
        j=json.load(open(f)); d.setdefault((j.get('product'),j.get('version')),[]).append(j)
    return d
P={c:load(c) for c in ('combined','frequency','material','samplerate')}
out=[]
for want in WANT:
    tcs=[f for f in glob.glob(C+'/profiles/*.tonecheck.json') if json.load(open(f)).get('product')==want]
    if len(tcs)!=1: out.append(dict(product=want,clean=False,why=[f'{len(tcs)} tone checks found'])); continue
    tc=json.load(open(tcs[0])); prof=tcs[0].replace('.tonecheck.json','.json'); p=json.load(open(prof))
    ver=(p.get('plugin') or {}).get('plugin_id','').split('|')[-1]
    why=[]; cells={}
    if tc.get('build')!=BUILD: why.append(f"tone check made by {tc.get('build')}, not {BUILD}")
    cells[2.0]=(tc.get('gr_measured_db'),tc.get('pass_within_0_5_db'))
    if not tc.get('pass_within_0_5_db'): why.append(f"g 2 fails ({tc.get('gr_measured_db')})")
    deep=tc.get('deep_levels') or []
    for x in deep:
        g=x.get('g_db'); cells[g]=(x.get('gr_measured_db'),x.get('pass_within_0_5_db') if x.get('ran') else 'not run')
        if not x.get('ran'): why.append(f"g {g:g} not run ({x.get('why_not_run','')})")
        elif not x.get('pass_within_0_5_db'): why.append(f"g {g:g} fails ({x.get('gr_measured_db')} vs {(x.get('pick') or {}).get('expected_gr_db')})")
    for g in (4.0,5.0,6.0):
        if g not in cells: why.append(f"g {g:g} missing")
    if tc.get('deep_levels_nulled'): why.append(f"deep levels nulled {tc['deep_levels_nulled']}")
    k=(want,ver); acc=[]
    fr=P['frequency'].get(k); ma=P['material'].get(k); co=P['combined'].get(k); sr=P['samplerate'].get(k)
    have=''.join(x for x,y in (('F',fr),('M',ma),('C',co),('S',sr)) if y)
    for need,lab in (('F','frequency'),('M','material'),('C','combined')):
        if need not in have: why.append(f"no {lab} record (accuracy pass not run on it)")
    if fr and fr[-1].get('frequency_dependent'): acc.append(f"frequency_dependent (max move {fr[-1]['max_abs_delta_db']:.2f} dB)")
    if ma:
        for m in ma[-1]['materials']:
            if m.get('median_miss_db') is not None and abs(m['median_miss_db'])>1: acc.append(f"material {m['kind']} median miss {m['median_miss_db']:+.2f} dB")
    if co:
        j=co[-1]
        if j.get('gr_miss_db') is not None and abs(j['gr_miss_db'])>0.5: acc.append(f"combined GR miss {j['gr_miss_db']:+.2f} dB")
        mm=(j.get('makeup_target_db') or 0)-(j.get('makeup_gives_db') or 0)
        res=(j['out_miss_db']+(j.get('gr_miss_db') or 0)) if j.get('out_miss_db') is not None else 0
        if abs(mm)>0.5: acc.append(f"make-up clamped: target {j.get('makeup_target_db')} gives {j.get('makeup_gives_db')}")
        if abs(res)>0.5: acc.append(f"make-up: output miss net of GR {res:+.2f} dB")
    if sr and sr[-1].get('rate_dependent'): acc.append('rate_dependent')
    why+=acc
    out.append(dict(product=want,version=ver,profile=os.path.basename(prof),tonecheck=os.path.basename(tcs[0]),build=tc.get('build'),measuredAt=tc.get('measuredAt'),
                    cells={f'{g:g}':v for g,v in sorted(cells.items())},passes=have,clean=not why,why=why,
                    acc_at={c:(P[c].get(k) or [{}])[-1].get('at') or (P[c].get(k) or [{}])[-1].get('measuredAt') for c in ('frequency','material','combined')}))
json.dump(out,open(sys.argv[3],'w'),indent=1,default=str)
for o in out:
    print(('CLEAN ' if o['clean'] else 'HELD  ')+o['product'], o.get('version',''), o.get('passes',''), '|', '; '.join(o['why']) or '-')
    print('       ', ' '.join(f"g{g} {v[0]} {v[1]}" for g,v in o.get('cells',{}).items()))
