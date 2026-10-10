#!/usr/bin/env python3
"""
THE MORNING SCRIPT (Kathy, 10 Oct). One command:

    tools/ejmap/scripts/morning.py --zip ~/Downloads/<Sean's --since zip> [--cut]

1. MERGE   a fresh scratch copy: ~/Downloads/cert 2, then cert 3 over it, then the zip's cert/ over that. Sean's folders are only read.
2. REPORT  run-all steps; the follow-up re-checks (every tone check by build, and CL 1B, Zip, bx_opto + the five v2 units at g 2/4/5/6
           with build); fixups; EQ progress; the zip's rows by outcome; rows the next build would file silent_output / probe_crashed.
3. CUT     (--cut only) build HEAD (ejmap + EchoJayProbe, -j4), sign the probe, package to ~/Desktop/ejmap-dist-<date>-<hash>, preflight.
4. EQUALITY vs 00f2ae79 (the packaged cut, or the build tree without --cut): the four compressor sets derive-only, and --phaseb-drafts
           over the merged folder classified by classify_drafts.py. ANY unexplained difference -> STOP (exit 3).
5. RESUME  --run-all --dry-run of the next night's line on the merged folder with the new binary: the plan it would run.

Nothing is sent; nothing touches ~/Library/ejmap or Sean's folders; no plugin is loaded (derive-only and dry-run only).
"""
import argparse, collections, glob, gzip, json, os, re, shutil, subprocess, sys, time, zipfile

WT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
HOME = os.path.expanduser('~')
SCRATCH_DEFAULT = '/private/tmp/claude-503/-Users-SeanD-Documents-ECHOJAY-FILES-WORKTREES-vst-reasoning/9231c63a-2a37-48b6-9847-1c1f92c8caf3/scratchpad'
OLD_BIN = HOME + '/Desktop/ejmap-dist-10oct-b/ejmap.app/Contents/MacOS/ejmap'   # 00f2ae79, Sean's build tonight
NAMED = ['Tube-Tech CL 1B', 'Unfiltered Audio Zip', 'bx_opto', 'Mixland Vac Attack', 'dbx-160 (s)', 'DPR-402 (s)', 'SPL IRON', 'Auto-Tune Vocal Compressor']
NEXT_NIGHT = 'preflight,followup,fixups,reverb_delay,transient_gate,limiter_comp,drafts'
SIGN = 'Developer ID Application: Sean Donoghue (8BT5F9B887)'

def say(s=''): print(s, flush=True)
def sh(cmd, **kw): return subprocess.run(cmd, shell=isinstance(cmd, str), capture_output=True, text=True, **kw)

# ---------------------------------------------------------------- 1. merge
def merge(zip_path, cert2, cert3, out):
    if os.path.exists(out): sys.exit(f'MORNING: {out} exists - give a fresh --work folder')
    os.makedirs(out)
    dst = os.path.join(out, 'cert')
    shutil.copytree(cert2, dst, symlinks=True)
    shutil.copytree(cert3, dst, dirs_exist_ok=True, symlinks=True)
    z = zipfile.ZipFile(zip_path)
    names = [n for n in z.namelist() if n.startswith('cert/') and not n.endswith('/')]
    for n in names:
        target = os.path.join(out, n); os.makedirs(os.path.dirname(target), exist_ok=True)
        with z.open(n) as src, open(target, 'wb') as f: shutil.copyfileobj(src, f)
    say(f'MERGE: cert 2 + cert 3 + {os.path.basename(zip_path)} ({len(names)} file(s)) -> {dst}')
    return dst, set(n[len('cert/'):] for n in names)

# ---------------------------------------------------------------- 2. report
def tone_rows(cert):
    out = {}
    for f in glob.glob(os.path.join(cert, 'profiles', '*.tonecheck.json')):
        try: d = json.load(open(f))
        except Exception: continue
        out[d.get('product', os.path.basename(f))] = (d, os.path.basename(f))
    return out

def gcells(d):
    cells = {2.0: (d.get('gr_measured_db'), d.get('pass_within_0_5_db'))}
    for x in d.get('deep_levels', []) or []:
        cells[float(x.get('g_db', 0))] = (x.get('gr_measured_db'), x.get('pass_within_0_5_db') if x.get('ran') else None)
    def c(g):
        v = cells.get(g)
        if not v or v[0] is None: return f'g{g:g} -'
        return f'g{g:g} {v[0]:.2f} ' + ('PASS' if v[1] else 'FAIL' if v[1] is False else 'not run')
    return '  '.join(c(g) for g in (2.0, 4.0, 5.0, 6.0))

TIMEKEY = None
def silent_or_crashed(traces):
    """The next build's two filings, as Python ports of phaseb::outputSilence and phaseb::traceEnded (the census rules)."""
    judged = silent = 0; cut = []
    for name, t in traces:
        ended = ('\nstage\tdone' in t or t.startswith('stage\tdone') or (t.strip().splitlines() or [''])[-1].lstrip().startswith('refused'))
        if not ended: cut.append(name)
        if '\ntwin\t' in t or t.startswith('twin\t'):
            inp = live = False
            for line in t.splitlines():
                if not line.startswith('twin\t'): continue
                x = line.split('\t')
                try: i, o = float(x[x.index('in_db') + 1]), float(x[x.index('out_db') + 1])
                except Exception: continue
                if i > -100: inp = True
                if o > -300: live = True; break
            if inp or live: judged += 1; silent += (not live)
            continue
        for line in t.splitlines():
            x = line.split('\t')
            if not x or x[0] not in ('hold', 'rtotal'): continue
            ok = 'level_db' if x[0] == 'hold' else 'out_rms_db'
            if ok not in x or 'in_rms_db' not in x: continue
            try: i, o = float(x[x.index('in_rms_db') + 1]), float(x[x.index(ok) + 1])
            except Exception: continue
            if i <= -100: continue
            judged += 1; silent += (o <= -300)
    return (judged >= 3 and silent == judged), cut

def report(cert, from_zip):
    say(); say('================ THE MORNING REPORT ================')
    st = json.load(open(os.path.join(cert, 'run_all.json'))) if os.path.exists(os.path.join(cert, 'run_all.json')) else {'steps': {}}
    say('RUN-ALL STEPS (state, build that finished it):')
    for k, v in sorted(st.get('steps', {}).items()):
        say(f"   {k:16} {v.get('state',''):8} build {v.get('build','(none: before the build stamp)'):22} runs {v.get('runs','')}  finished {v.get('finished_at','')}")
    tr = tone_rows(cert)
    by = collections.Counter(d.get('build', '(no build stamp)') for d, _ in tr.values())
    new = [p for p, (d, f) in tr.items() if ('profiles/' + f) in from_zip]
    say(); say(f'FOLLOW-UP RE-CHECKS: {len(tr)} tone check(s) in the folder; by build: ' + ', '.join(f'{k} {v}' for k, v in by.most_common()))
    say(f'   in this zip: {len(new)} re-checked; passing g 2: {sum(1 for p in new if tr[p][0].get("pass_within_0_5_db"))}, failing: {sum(1 for p in new if tr[p][0].get("pass_within_0_5_db") is False)}')
    for name in NAMED:
        hit = [p for p in tr if p == name or p.startswith(name + ' ')] or [p for p in tr if name.lower() in p.lower()]
        if not hit: say(f'   {name:30} no tone check in the folder'); continue
        for p in hit:
            d, f = tr[p]
            say(f"   {p:30} {gcells(d)}   build {d.get('build','(none)')}  {d.get('measuredAt','')[:15]}  {'NEW in zip' if ('profiles/' + f) in from_zip else 'not in zip'}")
    rows = []
    for f in glob.glob(os.path.join(cert, 'phaseb', '*', '*.phaseb.json')):
        rel = os.path.relpath(f, cert)
        try: r = json.load(open(f))
        except Exception: continue
        rows.append((rel, r, rel in from_zip))
    say(); say('ROWS IN THIS ZIP, by category and outcome:')
    zc = collections.defaultdict(collections.Counter)
    for rel, r, z in rows:
        if z: zc[rel.split('/')[1]][r.get('outcome', '?')] += 1
    for cat in sorted(zc): say(f'   {cat:12} ' + ', '.join(f'{k} {v}' for k, v in zc[cat].most_common()))
    if not zc: say('   (none)')
    fx = st.get('steps', {}).get('fixups', {})
    say(); say(f"FIXUPS: step {fx.get('state','not run')} (build {fx.get('build','-')}); multiband rows in zip {sum(zc.get('multiband', {}).values())}, gain-all rows in zip {sum(zc.get('gainall', {}).values())}")
    eq = [r for rel, r, z in rows if rel.split('/')[1] == 'eq']
    last = ''
    lg = os.path.join(cert, 'run_all', 'eq.log')
    if os.path.exists(lg):
        for line in open(lg, errors='replace'):
            if line.startswith('[eq '): last = line.strip()
    say(f"EQ: {len(eq)} row(s) in the folder ({', '.join(f'{k} {v}' for k, v in collections.Counter(r.get('outcome') for r in eq).most_common())}); step {st.get('steps',{}).get('eq',{}).get('state','-')}; last progress: {last[:110] or '-'}")
    say(); say('NEW SILENT / CRASHED (what the next build would file, judged on the zip rows\' traces):')
    flagged = 0
    for rel, r, z in rows:
        if not z or r.get('outcome') not in ('ok', 'failed', 'slept'): continue
        cat = rel.split('/')[1]; stem = os.path.basename(rel).replace('.phaseb.json', '')
        traces = []
        for g in glob.glob(os.path.join(cert, 'phaseb', cat, 'raw', stem + '.*.txt.gz')):
            n = os.path.basename(g)
            if '.list-params.' in n or '.text-at' in n: continue
            try: traces.append((n, gzip.open(g, 'rt', errors='replace').read()))
            except Exception: pass
        sil, cut = silent_or_crashed(traces)
        if sil: say(f'   silent_output  {cat:10} {r.get("product")}'); flagged += 1
        if cut: say(f'   probe_crashed  {cat:10} {r.get("product")}  ({len(cut)} trace(s): {", ".join(cut[:3])})'); flagged += 1
    if not flagged: say('   none')

# ---------------------------------------------------------------- 3. cut
def cut(desk_name):
    h = sh(['git', '-C', WT, 'rev-parse', '--short=8', 'HEAD']).stdout.strip()
    if sh(['git', '-C', WT, 'status', '--short']).stdout.strip(): sys.exit('MORNING: the worktree is not clean - commit before the cut')
    dist = os.path.join(HOME, 'Desktop', f'{desk_name}-{h}')
    if os.path.exists(dist): sys.exit(f'MORNING: {dist} exists - the cut is already there')
    say(f'CUT: HEAD {h}: building ejmap + EchoJayProbe (-j4)')
    b = sh(f'cd "{WT}" && cmake --build build-ejmap --target ejmap EchoJayProbe -j4 2>&1 | grep -E " error" ; true')
    if b.stdout.strip(): sys.exit('MORNING: build errors\n' + b.stdout)
    probe = os.path.join(WT, 'build-ejmap/EchoJayProbe_artefacts/RelWithDebInfo/EchoJayProbe')
    s = sh(['codesign', '-f', '-s', SIGN, '--timestamp', '--options', 'runtime', '--entitlements', os.path.join(WT, 'tools/au_instantiate_probe/EchoJayProbe.entitlements'), probe])
    if s.returncode: sys.exit('MORNING: probe signing failed\n' + s.stderr)
    p = sh([os.path.join(WT, 'tools/ejmap/packaging/package_app.sh'), os.path.join(WT, 'build-ejmap/tools/ejmap/ejmap_artefacts/RelWithDebInfo/ejmap.app'), probe, dist, SIGN])
    say('\n'.join(l for l in (p.stdout + p.stderr).splitlines() if 'PACKAGED' in l or 'TeamIdentifier' in l))
    pk = os.path.join(dist, 'ejmap.app/Contents/MacOS/ejmap')
    pf = sh([pk, '--cert-preflight']); say(f'CUT: preflight exit {pf.returncode}')
    if pf.returncode: sys.exit('MORNING: the packaged preflight failed - STOP\n' + pf.stdout[-1500:])
    return pk, h

# ---------------------------------------------------------------- 4. equality
def equality(new_bin, cert, scratch, work):
    say(); say('================ EQUALITY vs 00f2ae79 ================')
    src = os.path.join(scratch, 'equality', 'src'); old = os.path.join(scratch, 'equality19')
    R = os.path.join(work, 'equality'); os.makedirs(R)
    stop = []
    for s in ('current', 'seanzip', 'cert_sc', 'cert_tc35'):
        os.makedirs(os.path.join(R, s)); os.symlink(os.path.join(old, 'pk_' + s, 'new'), os.path.join(R, s, 'old'))
        shutil.copytree(os.path.join(src, s), os.path.join(R, s, 'new'), symlinks=True)
        sh([new_bin, '--cert-tonecheck-all', '--derive-only', '--out', os.path.join(R, s, 'new')])
        c = sh(['python3', os.path.join(WT, 'tools/ejmap/cert-traces/2026-10-07-equality/compare.py'), os.path.join(R, s)]).stdout
        lines = [l for l in c.splitlines() if 'rows' in l or 'differ' in l]
        say(f'== compressor set {s}'); [say('  ' + l.strip()) for l in lines]
        if any(re.search(r'differ [1-9]|diffs [1-9]', l) for l in lines): stop.append(f'compressor set {s} differs')
    D = os.path.join(R, 'drafts'); os.makedirs(D)
    shutil.copytree(cert, os.path.join(D, 'old'), symlinks=True); shutil.copytree(cert, os.path.join(D, 'new'), symlinks=True)
    sh([OLD_BIN, '--phaseb-drafts', '--out', os.path.join(D, 'old')]); sh([new_bin, '--phaseb-drafts', '--out', os.path.join(D, 'new')])
    c = sh(['python3', os.path.join(WT, 'tools/ejmap/cert-traces/2026-10-10-fixes/classify_drafts.py'), D]).stdout
    say('== drafts over the merged folder (classified):'); [say('  ' + l) for l in c.splitlines()]
    m = re.search(r'UNEXPLAINED: (\d+)', c)
    if not m or int(m.group(1)) > 0: stop.append('drafts: unexplained differences')
    return stop

# ---------------------------------------------------------------- 5. resume
def resume(new_bin, cert):
    say(); say('================ RESUME: the next night on the merged folder (dry-run, nothing loaded) ================')
    r = sh([new_bin, '--run-all', '--jobs', '2', '--steps', NEXT_NIGHT, '--until', '07:00', '--out', cert, '--dry-run'])
    say('\n'.join(l.replace(cert, '<cert>')[:170] for l in r.stdout.splitlines()))

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--zip', required=True, help="Sean's --zip --since zip")
    ap.add_argument('--cert2', default=HOME + '/Downloads/cert 2'); ap.add_argument('--cert3', default=HOME + '/Downloads/cert 3')
    ap.add_argument('--scratch', default=SCRATCH_DEFAULT, help='holds equality/src (the four sets) and equality19 (00f2ae79 outputs)')
    ap.add_argument('--work', default=None, help='a fresh folder for this morning (default: <scratch>/morning-<time>)')
    ap.add_argument('--cut', action='store_true', help='build, sign, package and preflight HEAD; equality on the packaged binary')
    ap.add_argument('--dist-name', default='ejmap-dist-' + time.strftime('%d%b').lower())
    a = ap.parse_args()
    work = a.work or os.path.join(a.scratch, 'morning-' + time.strftime('%Y%m%d-%H%M%S'))
    cert, from_zip = merge(a.zip, a.cert2, a.cert3, work)
    report(cert, from_zip)
    if a.cut: new_bin, h = cut(a.dist_name)
    else:
        new_bin = os.path.join(WT, 'build-ejmap/tools/ejmap/ejmap_artefacts/RelWithDebInfo/ejmap.app/Contents/MacOS/ejmap'); h = 'build tree'
        say(); say('CUT: skipped (no --cut): the equality runs on the build-tree binary')
    stop = equality(new_bin, cert, a.scratch, work)
    if stop:
        say(); say('STOP: ' + '; '.join(stop)); sys.exit(3)
    say(); say(f'EQUALITY: every difference named ({h}); nothing unexplained')
    resume(new_bin, cert)
    say(); say(f'MORNING: done - {work}')

if __name__ == '__main__': main()
