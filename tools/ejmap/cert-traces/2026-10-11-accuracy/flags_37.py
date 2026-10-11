import json,glob,sys,re,os
C=sys.argv[1]; L=sys.argv[2]
pub=[]
for l in open(L):
    p=l.split(' | ')
    if len(p)>5 and re.match(r'\d',p[1]): pub.append((p[0].strip(),p[1].strip()))
def load(cat):
    d={}
    for f in glob.glob(f"{C}/phaseb/{cat}/{cat}/*.json"):
        j=json.load(open(f)); d.setdefault((j.get('product'),j.get('version')),[]).append(j)
    return d
P={c:load(c) for c in ('combined','frequency','material','samplerate')}
rows=[]
for prod,ver in pub:
    k=(prod,ver); f=[]; have=[]
    fr=P['frequency'].get(k); ma=P['material'].get(k); co=P['combined'].get(k); sr=P['samplerate'].get(k)
    have=''.join(x for x,y in (('F',fr),('M',ma),('C',co),('S',sr)) if y)
    det={}
    if fr:
        j=fr[-1]; det['freq']=f"max {j['max_abs_delta_db']:.2f} by_hz {j['gr_by_hz']}"
        if j.get('frequency_dependent'): f.append(f"frequency_dependent (max move {j['max_abs_delta_db']:.2f} dB: {j['delta_vs_997_db']})")
    if ma:
        j=ma[-1]; det['mat']={m['kind']:m.get('median_miss_db') for m in j['materials']}
        for m in j['materials']:
            if m.get('median_miss_db') is not None and abs(m['median_miss_db'])>1: f.append(f"material {m['kind']} median miss {m['median_miss_db']:+.2f} dB (median GR {m['median_gr_db']} vs predicted {m['median_predicted_db']})")
    if co:
        j=co[-1]; det['comb']=(j.get('gr_miss_db'),j.get('out_miss_db'),j.get('makeup_target_db'),j.get('makeup_gives_db'),j.get('attack'),j.get('release'))
        if j.get('gr_miss_db') is not None and abs(j['gr_miss_db'])>0.5: f.append(f"combined GR miss {j['gr_miss_db']:+.2f} dB (g {j['g_db']}, attack '{j.get('attack')}' {j.get('attack_gr_shift_db')}, release '{j.get('release')}' {j.get('release_gr_shift_db')})")
        mm=(j.get('makeup_target_db') or 0)-(j.get('makeup_gives_db') or 0)
        res=(j['out_miss_db']+(j.get('gr_miss_db') or 0)) if j.get('out_miss_db') is not None else 0
        if j.get('out_miss_db') is None: f.append('combined: no output measured (' + '; '.join(j.get('notes',[]))[:120] + ')')
        if abs(mm)>0.5: f.append(f"make-up clamped: target {j.get('makeup_target_db')} gives {j.get('makeup_gives_db')}")
        if abs(res)>0.5: f.append(f"output miss net of GR {res:+.2f} dB (out miss {j['out_miss_db']:+.2f}, make-up {j.get('makeup_target_db')} -> {j.get('makeup_gives_db')})")
    if sr:
        j=sr[-1]
        if j.get('rate_dependent'): f.append(f"rate_dependent {j['delta_vs_48k_db']}")
    rows.append((prod,ver,have,f,det))
print(len(pub),'published')
for prod,ver,have,f,det in rows:
    print(f"{prod} [{have}]: " + ('; '.join(f) if f else 'no flag'))
json.dump([dict(product=a,version=b,passes=c,flags=d,detail=e) for a,b,c,d,e in rows],open(os.path.dirname(L)+'/../acc37.json','w'),indent=1,default=str)
