# MORNING-NEXT pin: an unfinished step is never dropped from the next night's plan.  python3 tools/ejmap/scripts/test_morning.py
import os, sys; sys.path.insert(0, os.path.dirname(__file__)); import morning as m
ra = {'steps': {'eq': {'state': 'done'}, 'saturation': {'state': 'started'}, 'uad': {'state': 'device_absent'}, 'tuners': {'state': 'done'}}}
got = m.next_steps(ra)
want = 'preflight,followup,fixups,saturation,uad,reverb_delay,transient_gate,drafts'
assert got == want, got
assert m.next_steps({'steps': {}}) == m.NEXT_NIGHT
assert m.next_steps({'steps': {'reverb_delay': {'state': 'started'}}}) == m.NEXT_NIGHT   # already listed: not twice
print('MORNING-NEXT: 3 checks, 0 failures')

# MORNING-BASE (Kathy, 13 Oct): the baseline is b287aef3; its cache is its own (never equality19, 00f2ae79's); a binary is the baseline
# only if it carries the hash; the classifier explains a v0.2 status change only when the baseline expects it (b287aef3 expects none)
import json, subprocess, tempfile
T = tempfile.mkdtemp(prefix='morning_pin_')
assert m.BASELINE['hash'] == 'b287aef3' and m.BASELINE['expect'] == '' and 'ejmap-dist-11oct-b287aef3' in m.OLD_BIN
assert m.baseline_cache('/s') == '/s/equality_b287aef3' and 'equality19' not in m.baseline_cache('/s')
open(os.path.join(T, 'yes'), 'wb').write(b'\0\0b287aef3\0'); open(os.path.join(T, 'no'), 'wb').write(b'\0\000f2ae79\0')
assert m.binary_is(os.path.join(T, 'yes'), 'b287aef3') and not m.binary_is(os.path.join(T, 'no'), 'b287aef3') and not m.binary_is(os.path.join(T, 'absent'), 'b287aef3')
R = os.path.join(T, 'R')
for side, st in (('old', 'v0.1 (a proposal)'), ('new', 'v0.2 (a proposal)')):
    os.makedirs(os.path.join(R, side, 'phaseb', 'eq', 'drafts')); json.dump({'status': 'PROPOSAL ' + st}, open(os.path.join(R, side, 'phaseb', 'eq', 'drafts', 'x.json'), 'w'))
cls = os.path.join(os.path.dirname(__file__), '..', 'cert-traces', '2026-10-10-fixes', 'classify_drafts.py')
run = lambda *a: subprocess.run(['python3', cls, R] + list(a), capture_output=True, text=True).stdout
assert 'UNEXPLAINED: 1' in run('--expect', ''), run('--expect', '')
assert 'UNEXPLAINED: 0' in run() and 'UNEXPLAINED: 0' in run('--expect', 'status')
# MORNING-FOLDER: a full folder's "this run" = rows at or after the first step the baseline build started + the tone checks it stamped
C = os.path.join(T, 'cert'); os.makedirs(os.path.join(C, 'phaseb', 'eq')); os.makedirs(os.path.join(C, 'profiles'))
json.dump({'steps': {'eq': {'started_at': '20261010T010000'}, 'followup': {'build': 'b287aef3', 'started_at': '20261012T195541'},
                     'drafts': {'build': 'b287aef3', 'started_at': '20261013T085739'}}}, open(os.path.join(C, 'run_all.json'), 'w'))
for n, at in (('old', '20261011T120000'), ('edge', '20261012T195541'), ('new', '20261012T220000')): json.dump({'at': at}, open(os.path.join(C, 'phaseb', 'eq', n + '.phaseb.json'), 'w'))
for n, b in (('a', 'b287aef3'), ('b', '5ab8352b')): json.dump({'build': b}, open(os.path.join(C, 'profiles', n + '.tonecheck.json'), 'w'))
got, since = m.this_run(C, 'b287aef3')
assert since == '20261012T195541' and got == {'phaseb/eq/edge.phaseb.json', 'phaseb/eq/new.phaseb.json', 'profiles/a.tonecheck.json'}, (since, got)
assert m.this_run(C, 'ffffffff')[0] == set()
print('MORNING-BASE / MORNING-FOLDER: 13 checks, 0 failures')

# MORNING-SETS (13 Oct, d26db00f): a profile that drops EXACTLY its not_written controls is explained, only when the baseline expects
# it; one that also changes anything else, or drops a control it does not list, stays a difference (STOP)
assert m.BASELINE['expect_sets'] == 'not_written'
cmp = os.path.join(os.path.dirname(__file__), '..', 'cert-traces', '2026-10-07-equality', 'compare.py')
def mkset(root, old, new):
    for side, profs in (('old', old), ('new', new)):
        for d in ('profiles', 'fixtures', 'controls'): os.makedirs(os.path.join(root, side, d), exist_ok=True)
        json.dump([{'product': 'x', 'state': 'exported', 'reason': ''}], open(os.path.join(root, side, 'outcomes.json'), 'w'))
        for n, p in profs.items(): json.dump(p, open(os.path.join(root, side, 'profiles', n), 'w'))
base = {'neutral': [{'index': 1, 'control': 'Mix'}, {'index': 57, 'control': 'M'}], 'amount': {'control': 'T'}}
good = {'neutral': [{'index': 1, 'control': 'Mix'}], 'amount': {'control': 'T'}, 'not_written': [{'index': 57, 'control': 'M'}]}
bad1 = dict(good, amount={'control': 'U'})                                          # something else changed too
bad2 = dict(good, not_written=[{'index': 57, 'control': 'M'}, {'index': 99, 'control': ''}])   # lists a control the old profile never wrote
S = os.path.join(T, 'sets'); mkset(S, {'a.json': base, 'b.json': base, 'c.json': base}, {'a.json': good, 'b.json': bad1, 'c.json': bad2})
out = lambda *a: subprocess.run(['python3', cmp, S] + list(a), capture_output=True, text=True).stdout
w = out('--expect', 'not_written'); n0 = out()
assert 'profiles: 3 old / 3 new' in w and 'differ 2' in w and 'explained 1 not_written' in w, w
assert 'differ 3' in n0 and 'explained' not in n0, n0
print('MORNING-SETS: 3 checks, 0 failures')
