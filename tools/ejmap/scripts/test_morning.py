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
