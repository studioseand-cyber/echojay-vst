# MORNING-NEXT pin: an unfinished step is never dropped from the next night's plan.  python3 tools/ejmap/scripts/test_morning.py
import os, sys; sys.path.insert(0, os.path.dirname(__file__)); import morning as m
ra = {'steps': {'eq': {'state': 'done'}, 'saturation': {'state': 'started'}, 'uad': {'state': 'device_absent'}, 'tuners': {'state': 'done'}}}
got = m.next_steps(ra)
want = 'preflight,followup,fixups,saturation,uad,reverb_delay,transient_gate,drafts'
assert got == want, got
assert m.next_steps({'steps': {}}) == m.NEXT_NIGHT
assert m.next_steps({'steps': {'reverb_delay': {'state': 'started'}}}) == m.NEXT_NIGHT   # already listed: not twice
print('MORNING-NEXT: 3 checks, 0 failures')
