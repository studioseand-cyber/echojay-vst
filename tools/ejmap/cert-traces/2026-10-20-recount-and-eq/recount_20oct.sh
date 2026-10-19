#!/bin/bash
# recount_20oct.sh <ejmap binary> <out dir> - recount_19oct.sh with the 19 Oct run (finished 14:05) added as a second night run, and EXTRA_CERT
# for the 19 Oct evening EQ re-measure (H fixes). Loads nothing, sends nothing.
# TONIGHT2 = the 19 Oct backed-up engine folder; TONIGHT2_HELD = its AU hold rows (held_1405_au.json, the same rule as held_tonight_au.json)
# H17 = ~/Desktop/EJMAP_TRUST_17oct (its inputs, unchanged); TONIGHT = the backed-up engine folder; TONIGHT_HELD = its AU hold rows
# (held_tonight_au.json: the trust run keys holds by product NAME, so the VST3 lane's holds would also hold their AU twins - left out).
# Maps: Kathy's store (read only) with the 4 re-mapped products swapped for remapped_maps_17oct/, rebuilt in <out dir>/maps_merged.
# Cert data: Sean's cert 5, the C4 smoke cert, the overnight cert, the wizard's 15 Oct folder; Kathy's answers; review list EQ-first.
# 17 Oct (the three rulings): CERT_OVERNIGHT="$H/overnight_cert_rederived" recount.sh <ejmap at the rulings commit> <out> -> 190 / 185 / 370.
H=~/Desktop/EJMAP_TRUST_17oct; B="$1"; OUT="$2"
: "${TONIGHT:?set TONIGHT}"; : "${TONIGHT_HELD:?set TONIGHT_HELD}"; : "${TONIGHT2:?set TONIGHT2}"; : "${TONIGHT2_HELD:?set TONIGHT2_HELD}"
[ -x "$B" ] && [ -n "$OUT" ] || { echo "usage: recount.sh <ejmap binary> <out dir>"; exit 2; }
mkdir -p "$OUT"; M="$OUT/maps_merged"; rm -rf "$M"; mkdir -p "$M"; cp ~/Library/ejmap/maps/*.json "$M/"
python3 - "$M" "$H/remapped_maps_17oct" <<'EOF'
import json, glob, os, shutil, sys
M, R = sys.argv[1], sys.argv[2]
names = {json.load(open(f))['identity']['name'] for f in glob.glob(R + '/*.json')}
for f in glob.glob(M + '/*.json'):
    if json.load(open(f))['identity']['name'] in names: os.remove(f)
for f in glob.glob(R + '/*.json'): shutil.copy(f, M)
EOF
# 17 Oct (NEXT): REPAIRED_MAPS=<dir> - the anchor-repaired copies (ejmap --anchor-repair <out>/maps) replace their maps by file name
[ -n "${REPAIRED_MAPS:-}" ] && cp "$REPAIRED_MAPS"/*.json "$M/"
W=~/Desktop/WIZARD_RESULTS_15oct_night/"EchoJay Wizard Test (work folder, copied 16 Oct)"
rm -rf "$OUT/trust"
# EXTRA_CERT: one or more extra cert folders, separated by ":" (17 Oct)
XC=(); IFS=: read -ra XCS <<< "${EXTRA_CERT:-}"; for c in "${XCS[@]}"; do [ -n "$c" ] && XC+=(--cert "$c"); done
"$B" --trust-levels --maps "$M" --cert ~/Downloads/"cert 5" --cert "$H/smoke_cert_c4" --cert "${CERT_OVERNIGHT:-$H/overnight_cert}" --wizard "$W/engine" \
     --certified ~/Desktop/EJMAP_PUBLISH_COMPRESSORS_v3_LIST.txt ${CERTIFIED_PROFILES:+--certified-profiles "$CERTIFIED_PROFILES"} "${XC[@]}" --held ~/Downloads/"cert 5"/outcomes.json --held "$H/held/wizard_15oct_manifest.json" --wizard "$TONIGHT" --held "$TONIGHT_HELD" --wizard "$TONIGHT2" --held "$TONIGHT2_HELD" \
     --quarantine ~/Library/ejmap/quarantine.json --human ~/Desktop/EJMAP_REVIEW/human_checks.json --review-first eq --out "$OUT/trust" | grep -E "read-back credited|human check|certified for no format|format bridge"
python3 - "$OUT/trust" "$H/held/wizard_15oct_manifest.json" "$TONIGHT_HELD" "$TONIGHT2_HELD" <<'EOF'
import json, collections, sys
t = json.load(open(sys.argv[1] + '/trust_levels.json'))
o = {x['product']: x for x in json.load(open('/Users/SeanD/Downloads/cert 5/outcomes.json'))}
m = json.load(open(sys.argv[2])); silent = {s['product'] for s in m['skipped'] if 'digital silence' in s['why']}
silent |= {x['product'] for x in json.load(open(sys.argv[3])) if 'digital silence' in (x.get('why') or '')}   # 18 Oct night run
silent |= {x['product'] for x in json.load(open(sys.argv[4])) if 'digital silence' in (x.get('why') or '')}   # 19 Oct run
def held(x):
    if not (x['level'] == 'unsure' and x['why'].startswith('held')): return None
    w = x['why'][6:].strip(); p = x['product']
    if p.startswith('UAD'): return 'UAD'
    if p.startswith('APB') or w.startswith('needs_hardware') or (w == 'held' and 'hardware' in (o.get(p, {}).get('reason') or '')): return 'APB / hardware'
    if p in silent: return 'silent'
    if w.startswith('needs_licence') or (w.startswith('refused') and 'licens' in (o.get(p, {}).get('reason') or '')): return 'licence'
    if w.startswith('demo'): return 'demo'
    return None
c = collections.defaultdict(collections.Counter); h = collections.Counter()
for x in t:
    r = held(x)
    if r: h[r] += 1; c[x['category']]['held'] += 1
    else: c[x['category']][x['level']] += 1
tot = collections.Counter(); print('category | certified | mapped | unsure | testable | held')
for k in sorted(c, key=lambda k: -(c[k]['certified'] + c[k]['mapped'] + c[k]['unsure'])):
    d = c[k]; tot.update(d); print(f"{k} | {d['certified']} | {d['mapped']} | {d['unsure']} | {d['certified'] + d['mapped'] + d['unsure']} | {d['held']}")
print('TOTAL', tot['certified'], tot['mapped'], tot['unsure'], tot['certified'] + tot['mapped'] + tot['unsure'], 'held', tot['held'])
print('held:', ', '.join(f"{r} {h[r]}" for r in ('UAD', 'demo', 'silent', 'APB / hardware', 'licence')))
# 17 Oct (NEXT 1): the same rule, split by format (AU is what Logic uses; VST3 the other DAWs)
for fmt in ('AudioUnit', 'VST3'):
    c2 = collections.Counter(); h2 = collections.Counter()
    for x in t:
        if x.get('format') != fmt: continue
        r = held(x)
        if r: h2[r] += 1
        else: c2[x['level']] += 1
    print(f"{fmt}: {c2['certified']} / {c2['mapped']} / {c2['unsure']} of {c2['certified'] + c2['mapped'] + c2['unsure']} testable; held {sum(h2.values())} (" + ', '.join(f"{r} {h2[r]}" for r in ('UAD', 'demo', 'silent', 'APB / hardware', 'licence') if h2[r]) + ")")
EOF
