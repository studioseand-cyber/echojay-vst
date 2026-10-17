#!/usr/bin/env python3
"""
EXTRACT THIS MORNING'S REAL SESSIONS INTO REPLAY FIXTURES (17 Oct 2026, Sean's method).

His instruction for this round: "Replay this morning's real sessions from your logs (the exact start
bodies and frames between 08:05 and 08:30) into the runner, and make each one RED from the real data
before fixing. Rows from synthetic setups don't count for these."

WHAT IS EXACT HERE, AND WHAT IS NOT - said plainly, because the distinction decides what a row proves:

  * EXACT, from the plugin's own log: the frame SEQUENCE per round, each frame's kind, tool-call id,
    tool name, approval mode, plan heading, the done summary, every state transition, every press
    (kind=1 Apply, kind=4 tap) with its step id, every typed answer, the working target, the goal and
    the checkpoint. That is what the client saw and what it did.
  * NOT exact: the raw bytes of /api/agent/start (the log records only `bytes=N`) and, for sessions
    before this morning's build, the TEXT of `message` frames - the agent parser skipped them without
    recording their words. Both are now logged (EJAgentClient's unknown-frame line carries the text),
    so the next replay is byte-closer; for THIS round the prose comes from Sean's own quotation of
    what he read, which is the same sentence from the only other witness there was.

A fixture is a JSON file: {id, goal, target, rounds: [[frame, ...], ...], actions: [...]}, where the
frames are served verbatim by the mock in order, one list per round, and the actions are what the rig
must do (press apply, tap a label, type a text) at which round.
"""
import json, re, sys, os

LOG   = sys.argv[1] if len(sys.argv) > 1 else "morning.log"
OUT   = sys.argv[2] if len(sys.argv) > 2 else "tools/host_rig/replay"
START = "08:05:00"
END   = "08:31:00"

ts  = re.compile(r'^(\d{4}-\d\d-\d\d) (\d\d:\d\d:\d\d)\s+\[\d+\] (.*)$')
pats = {
 'target':  re.compile(r'EJAgent: working target -> (.*)$'),
 'start':   re.compile(r'EJAgent: START goal="(.*)" checkpoint=(\S+)'),
 'post':    re.compile(r'EJAgent: POST /api/agent/(start|step|stop)(?: bytes=(\d+))?(?: session=(\S+))?'),
 'round':   re.compile(r'EJAgent: frame round session=(\S+) round=(\d+)'),
 'tool':    re.compile(r'EJAgent: frame tool_call id=(\S+) name=(\S+) approval=(\S+) mode=(\S+)(.*)$'),
 'plan':    re.compile(r'EJAgent: frame plan round=(\d+) items=(\d+) heading="(.*)"'),
 'await':   re.compile(r'EJAgent: frame await ids=(\S+)'),
 'unknown': re.compile(r'EJAgent: frame unknown event="(\w+)" skipped(?: text="(.*)")?'),
 'done':    re.compile(r'EJAgent: frame done reason=(\S+) rounds=(\d+) costUsd=(\S+) summary="(.*)$', re.S),
 'ask':     re.compile(r'EJAgent: ask card: "(.*?)" choices=(.*?)\s+freeText=(\w)', re.S),
 'state':   re.compile(r'EJAgent: state (\S+) -> (\S+)'),
 'press':   re.compile(r'EJAgentRow: press kind=(\d+) step=(\S+)'),
 'tapped':  re.compile(r'EJAgent: ask answered \(tapped\): "(.*)"'),
 'typedans':re.compile(r'EJAgent: ask answered \(typed\): "(.*)"'),
 'typednew':re.compile(r'EJAgent: typed text "(.*?)" (?:matches none|is a new instruction)'),
 'failed':  re.compile(r'EJAgent: step (\S+) FAILED code=(\S+) "(.*)$', re.S),
 'stepdone':re.compile(r'EJAgent: step (\S+) DONE(?: undo=(\S+))?(?: "(.*)")?'),
 'chat':    re.compile(r'EJChat: channel (\S+) -> chat (\d+) \(latest of (\d+)\)'),
 'loadfail':re.compile(r'EJLoad: "(.*?)" FAILED to load - (.*)$'),
}

def records(path):
    """One logical record per timestamped line, with its continuation lines joined.

    The log is not line-per-fact: an ask card's question carries newlines ("placeholder\nWhich CL 1B
    do you have loaded...") and a refused batch's detail is two or three lines. The first cut of this
    parser skipped every line that did not start with a timestamp, so it lost exactly the two things
    this round is about - the ask's CHOICES and the failure's words. A parser that drops what it does
    not recognise is how a replay comes out shorter than the session it replays.
    """
    cur = None
    for line in open(path, encoding='utf-8', errors='replace'):
        line = line.rstrip("\n")
        m = ts.match(line)
        if m:
            if cur: yield cur
            cur = [m.group(1), m.group(2), m.group(3)]
        elif cur is not None:
            cur[2] += "\n" + line
    if cur: yield cur

sessions, cur, chat = [], None, None
for _d, t, body in records(LOG):
    if not (START <= t <= END): continue
    if (g := pats['chat'].search(body)):
        chat = {'channel': g.group(1), 'chatId': g.group(2), 'ofN': int(g.group(3)), 'at': t}
    if (g := pats['target'].search(body)):
        pend_target = g.group(1)
    if (g := pats['start'].search(body)):
        cur = {'id': 'rp_' + g.group(2), 'goal': g.group(1), 'checkpoint': g.group(2),
               'target': locals().get('pend_target', '?'), 'at': t,
               'chat': dict(chat) if chat else None, 'rounds': [], 'actions': [], 'states': [],
               'startBytes': None}
        sessions.append(cur)
        continue
    if cur is None: continue
    if (g := pats['post'].search(body)):
        if g.group(1) == 'start' and cur['startBytes'] is None: cur['startBytes'] = int(g.group(2) or 0)
        if g.group(1) == 'stop': cur['actions'].append({'at': t, 'what': 'server_stop'})
    if (g := pats['round'].search(body)):
        cur['rounds'].append({'round': int(g.group(2)), 'session': g.group(1), 'frames':
                              [{'event':'round','sessionId':g.group(1),'round':int(g.group(2)),'ofSoftCap':20}]})
        continue
    if not cur['rounds']:
        cur['rounds'].append({'round': 1, 'session': '', 'frames': []})
    fr = cur['rounds'][-1]['frames']
    if (g := pats['tool'].search(body)):
        tail = g.group(5) or ''
        summ = re.search(r'"(.*)"\s*$', tail)
        fr.append({'event':'tool_call','id':g.group(1),'name':g.group(2),'approval':g.group(3),
                   'mode':g.group(4),'summary': summ.group(1) if summ else '',
                   'autoApplied': 'AUTO-APPLIED' in tail})
    elif (g := pats['plan'].search(body)):
        fr.append({'event':'plan','round':int(g.group(1)),'items':int(g.group(2)),'heading':g.group(3)})
    elif (g := pats['await'].search(body)):
        fr.append({'event':'await','ids':[g.group(1)]})
    elif (g := pats['unknown'].search(body)):
        fr.append({'event':g.group(1),'text':(g.group(2) or '')})
    elif (g := pats['done'].search(body)):
        fr.append({'event':'done','reason':g.group(1),'rounds':int(g.group(2)),
                   'costUsd':float(g.group(3)),'summary':g.group(4).rstrip('"')})
    elif (g := pats['ask'].search(body)):
        fr.append({'_ask':{'question':g.group(1),'choices':[c.strip() for c in g.group(2).split('|') if c.strip()],
                           'allowFreeText': g.group(3) == 'y'}})
    elif (g := pats['press'].search(body)):
        cur['actions'].append({'at':t,'what':'press','kind':int(g.group(1)),'step':g.group(2)})
    elif (g := pats['tapped'].search(body)):
        cur['actions'].append({'at':t,'what':'tap','label':g.group(1)})
    elif (g := pats['typedans'].search(body)):
        cur['actions'].append({'at':t,'what':'typed_answered_ask','text':g.group(1)})
    elif (g := pats['typednew'].search(body)):
        cur['actions'].append({'at':t,'what':'typed_became_new_run','text':g.group(1)})
    elif (g := pats['failed'].search(body)):
        cur['actions'].append({'at':t,'what':'step_failed','step':g.group(1),'code':g.group(2),
                               'detail':g.group(3).rstrip('"')})
    elif (g := pats['loadfail'].search(body)):
        cur['actions'].append({'at':t,'what':'load_failed','plugin':g.group(1),'why':g.group(2)})
    elif (g := pats['state'].search(body)):
        cur['states'].append({'at':t,'from':g.group(1),'to':g.group(2)})

os.makedirs(OUT, exist_ok=True)
index = []
for s in sessions:
    path = os.path.join(OUT, s['id'] + ".json")
    json.dump(s, open(path, 'w'), indent=1)
    index.append({'id': s['id'], 'at': s['at'], 'goal': s['goal'], 'target': s['target'],
                  'rounds': len(s['rounds']), 'actions': len(s['actions'])})
json.dump(index, open(os.path.join(OUT, "index.json"), 'w'), indent=1)
for i in index:
    print("%-8s %s  target=%-28s rounds=%-2d actions=%-2d  %s" %
          (i['id'], i['at'], i['target'][:28], i['rounds'], i['actions'], i['goal'][:52]))
