# Validates trust_levels.json + hold_states.json against B's description (FOR_KATHY_2026-10-17.md, 22:30/22:35). Loads nothing.
import json, sys, collections
D = sys.argv[1]; t = json.load(open(D + '/trust_levels.json')); h = json.load(open(D + '/hold_states.json')); err = []
def need(c, m): (None if c else err.append(m))
need(isinstance(t, list), 'trust_levels.json is not an array'); need(isinstance(h, list), 'hold_states.json is not an array')
fps = collections.Counter(r.get('fp') for r in t); need(all(n == 1 for n in fps.values()), 'duplicate fp rows')
for r in t:
    k = r.get('fp') or f"{r.get('name')}|{r.get('version')}"
    need(bool(r.get('fp')) or (r.get('name') and r.get('version')), f'{k}: no fp and no name+version')
    need(r.get('level') in ('certified', 'mapped', 'unsure'), f'{k}: level {r.get("level")}')
    need(isinstance(r.get('why'), str) and r['why'], f'{k}: no why')
    need(isinstance(r.get('roles'), list), f'{k}: roles not a list')
    for x in r.get('roles', []): need(isinstance(x.get('control'), str) and x['control'].strip(), f'{k}: a role with no control')
    if r['level'] == 'mapped':
        need(r.get('roles'), f'{k}: mapped with no roles'); need(isinstance(r.get('limits'), dict), f'{k}: mapped with no limits')
        for x in r['roles']: need(x.get('semantic'), f'{k}: role {x["control"]} with no semantic'); need((x.get('readback') or {}).get('pass') is True, f'{k}: role {x["control"]} without a passing read-back')
    else: need('limits' not in r, f'{k}: limits on a {r["level"]} row')
    if 'held' in r: need(r['level'] == 'unsure' and isinstance(r['held'], dict) and r['held'].get('reason'), f'{k}: bad held')
    for c in r.get('controls', []):
        rb = c.get('readback') or {}
        need(c.get('name'), f'{k}: a control with no name')
        need(isinstance(rb.get('checked'), int) and isinstance(rb.get('failed'), int) and isinstance(rb.get('pass'), bool) and 'worst' in rb and isinstance(rb.get('source'), str),
             f'{k}: {c.get("name")} readback not {{checked, failed, pass, worst, source}}')
        need(rb.get('pass') == (rb.get('checked', 0) >= 2 and rb.get('failed') == 0), f'{k}: {c.get("name")} pass disagrees with checked/failed')
held_t = {r['fp'] for r in t if 'held' in r}; held_h = {x.get('fp') for x in h}
for x in h: need((x.get('fp') or x.get('name')) and isinstance(x.get('held'), dict) and x['held'].get('reason'), f'hold row {x.get("name")}: no fp/name or held')
need(held_t == held_h, f'held rows differ: {len(held_t ^ held_h)}')
c = collections.Counter(('held' if 'held' in r else r['level'], r['format']) for r in t)
tab = {f: [c[(l, f)] for l in ('certified', 'mapped', 'unsure', 'held')] for f in ('AudioUnit', 'VST3')}
for f, v in tab.items(): print(f'{f}: certified {v[0]} / mapped {v[1]} / unsure {v[2]} of {sum(v[:3])} testable; held {v[3]}')
tot = [sum(tab[f][i] for f in tab) for i in range(4)]; print(f'all: {tot[0]} / {tot[1]} / {tot[2]} of {sum(tot[:3])}; held {tot[3]}')
print('rows', len(t), 'holds', len(h), 'controls with readback', sum(len(r.get('controls', [])) for r in t), '| errors', len(err)); [print(' ', e) for e in err[:20]]
sys.exit(1 if err else 0)
