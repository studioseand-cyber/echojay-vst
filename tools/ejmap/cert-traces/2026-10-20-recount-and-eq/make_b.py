# B item 4 (18 Oct; 20 Oct: + TONIGHT2_HELD): trust_levels.json + hold_states.json in B's format, from the latest trust run (r4a). Loads nothing.
import json, sys, os, re, collections, datetime
T, OUT = sys.argv[1], sys.argv[2]
H = os.path.expanduser('~/Desktop/EJMAP_TRUST_17oct/held/wizard_15oct_manifest.json')
OC = '/Users/SeanD/Downloads/cert 5/outcomes.json'
rows = json.load(open(T + '/trust_levels.json'))
oc = {x['product']: x for x in json.load(open(OC))}
man = json.load(open(H)); mrows = {}
for k in ('skipped', 'crashed', 'held', 'out_of_scope'):
    for x in man.get(k) or []:
        if isinstance(x, dict) and x.get('product'): mrows.setdefault(x['product'], x)
silent = {s['product'] for s in man['skipped'] if 'digital silence' in s.get('why', '')}
if os.environ.get('TONIGHT_HELD'): silent |= {x['product'] for x in json.load(open(os.environ['TONIGHT_HELD'])) if 'digital silence' in (x.get('why') or '')}   # 19 Oct: the 18 Oct night run
if os.environ.get('TONIGHT2_HELD'): silent |= {x['product'] for x in json.load(open(os.environ['TONIGHT2_HELD'])) if 'digital silence' in (x.get('why') or '')}   # 20 Oct: the 19 Oct run
# the count rule (recount.sh, 17 Oct) - the same function, so the totals and this file cannot disagree
def held_class(x):
    if not (x['level'] == 'unsure' and x['why'].startswith('held')): return None
    w = x['why'][6:].strip(); p = x['product']
    if p.startswith('UAD'): return 'UAD'
    if p.startswith('APB') or w.startswith('needs_hardware') or (w == 'held' and 'hardware' in (oc.get(p, {}).get('reason') or '')): return 'APB / hardware'
    if p in silent: return 'silent'
    if w.startswith('needs_licence') or (w.startswith('refused') and 'licens' in (oc.get(p, {}).get('reason') or '')): return 'licence'
    if w.startswith('demo'): return 'demo'
    return None
# EjmapTrust.h roleTable, name words only (first match wins) - the role a dialable control was found for
RT = [("predelay","pre-delay|predelay|pre delay|pre dly|predly"),("ceiling","ceiling"),("threshold","threshold|thresh|thrsh"),("ratio"," ratio"),
      ("attack","attack|atk"),("release","release|recovery"),("makeup","makeup|make-up|make up"),("output","output|out gain|out level|master out|master gain"),
      ("decay","decay|rt60|reverb time| rt|rt "),("delay_time","delay time|time|delay|note|sync"),("freq","freq|frequency|frq"),("q","bandwidth| q |^q$"),
      ("mix","mix|dry/wet|dry-wet|dry wet|wet"),("drive","drive|saturation|saturate"),("gain","gain")]
def semantic(name):
    l = name.lower().replace('_', ' '); p = ' ' + l + ' '
    for r, ws in RT:
        for w in ws.split('|'):
            if (w == '^q$' and l == 'q') or (w != '^q$' and w in p): return r
    return None
# TRUST_LEVELS_v0 sec 3, cautious limits (proposed defaults) - mapped only
def limits(cat):
    L = {'max_mix_pct': 50}
    if cat in ('compressor', 'limiter', 'multiband', 'deesser', 'gate'): L['max_gr_db'] = 6
    if cat == 'eq': L['max_gain_db'] = 3
    if cat == 'saturation': L['max_drive_fraction'] = round(1/3, 3)
    return L
def rb(c):
    r = c.get('readback')
    if not r: return None
    return {k: r.get(k) for k in ('checked', 'failed', 'pass', 'worst', 'source') if k in r} | ({'worst': None} if 'worst' not in r else {})
# B, 17 Oct: a profile the server REJECTED is no profile - never certified from it. RVox m/s stay held until a re-measure passes the gate
# (RVOX_POINT_ERROR.md); TCL-2 / DSM V3 / bx_opto are certified again by v3b (copy it before this file is read)
SERVER_REJECTED = {}   # 18 Oct (v3c): RVox m/s re-judged (point_error 0.03 dB) - certified once v3c replaces the rejected files, like the v3b three
out, holds = [], []
for x in rows:
    if x['level'] == 'certified' and (x['product'], x['format']) in SERVER_REJECTED:
        x = dict(x, level='unsure', why='not dialled: ' + SERVER_REJECTED[(x['product'], x['format'])], dialable=[], server_rejected=True)
    mp = json.load(open(f"{T}/maps/{x['fp']}.json")); ident = mp.get('identity', {})
    ctl = mp.get('controls', {}); byname = {c['name']: c for c in ctl.values()}
    hc = held_class(x); hw = x['why'][6:].strip() if x['why'].startswith('held') else None
    src = oc.get(x['product']) or mrows.get(x['product']) or {}
    held = None
    if hw is not None:
        held = {'reason': hw, 'count_rule': hc,   # the count rule's class: UAD, demo, silent, APB / hardware, licence
                'stage': ('cert 5' if x['product'] in oc else 'wizard 15 Oct' if x['product'] in mrows else 'scan / quarantine'),
                'at': src.get('at') or (man['created'] if x['product'] in mrows else None),
                'why': src.get('reason') or src.get('why')}
        if hc: holds.append({'fp': x['fp'], 'name': x['product'], 'version': x['version'], 'format': x['format'], 'held': held})
    # a params-format map (CLA-76, PuigChild...) keeps its roles in params, keyed by kind ("input_db"): role = kind less its unit
    pby = {q.get('name'): q for q in (mp.get('params') or {}).values() if isinstance(q, dict)}
    def role(n):
        c, q = byname.get(n), pby.get(n)
        if n in runroles: sem, how = runroles[n], 'role rule'   # the trust run's own (need, control) pair (WZ-TRUST-18)
        else: sem, how = (re.sub(r'_(db|ms|hz|pct|s)$', '', q['kind']), 'params') if q and q.get('kind') else (semantic(n), 'name') if semantic(n) else (None, 'measured')
        return {'semantic': sem, 'semantic_from': how, 'control': n, 'index': (c or q or {}).get('index'), 'readback': rb(c or q or {})}
    runroles = {r['control']: r['semantic'] for r in x.get('roles') or []}
    roles = [role(n) for n in x['dialable']]
    controls = [{'name': c['name'], 'index': c.get('index'), 'readback': rb(c)} for c in ctl.values() if c.get('readback')]
    controls += [{'name': n, 'index': q.get('index'), 'readback': rb(q)} for n, q in pby.items() if q.get('readback') and n not in byname]   # a params-format map's credited role controls
    r = {'fp': x['fp'], 'name': x['product'], 'vendor': ident.get('vendor'), 'version': x['version'], 'format': x['format'], 'category': x['category'],
         'level': x['level'], 'why': x['why'], 'roles': roles, 'controls': controls}
    # held only for the count rule's holds ("can't test on this Mac"); the other holds (multiband, needs_review, quarantined, refused) stay
    # unsure - never dialled either way - so the server's totals and TRUST_LEVELS agree. Keys absent when they do not apply
    if hc: r['held'] = held
    elif held: r['not_dialled'] = {k: held[k] for k in ('reason', 'stage', 'at', 'why')}
    if x['level'] == 'mapped': r['limits'] = limits(x['category'])
    if x.get('server_rejected'): r['not_dialled'] = {'reason': 'server_rejected', 'stage': 'B, 17 Oct', 'at': '2026-10-17T22:15', 'why': SERVER_REJECTED[(x['product'], x['format'])]}
    for k in ('bridged_from', 'bridge_refused'):
        if k in x: r[k] = x[k]
    out.append(r)
meta = {'schema': 'ej_trust_levels/0', 'created': datetime.datetime.now().astimezone().isoformat(timespec='seconds'),
        'source': 'EJ Map --trust-levels (18 Oct build: roles per control + params credit), the 13.6 recount inputs + count rule; cert 6 compressors recounted separately (README)',
        'note': 'roles = the controls a mapped plugin may be dialled on; limits apply to mapped only; certified dials its profile'}
json.dump(out, open(OUT + '/trust_levels.json', 'w'), indent=1)    # B reads an array, {plugins:[...]} or a map by fp: a plain array
json.dump(holds, open(OUT + '/hold_states.json', 'w'), indent=1)
json.dump(meta, open(OUT + '/trust_levels.meta.json', 'w'), indent=1)
print(len(out), 'rows;', collections.Counter(r['level'] for r in out), '; holds', len(holds), collections.Counter(h['held']['count_rule'] for h in holds))
