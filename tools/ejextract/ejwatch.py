#!/usr/bin/env python3
"""
ejwatch - live progress viewer for ejextract runs.

Tails an ejextract run log (--au-registry, --extract-list, --bootstrap) and
renders progress, per-outcome counts, rate, ETA, the slowest component, and a
stall warning. READ-ONLY: it never touches the extractor, its binary, or its
ledgers, so it is safe to run against a live campaign. Works on a live run
(follows the file) or replays a finished log (renders the final state).

  tools/ejextract/ejwatch.py <run.log> [--interval SEC] [--replay]

Each completed component is one log line:
  [131/2102] timeout  Eios / aufx,AEq5,Eios  [120.003s]
  [160/2102] ok       Waves / aufx,ASPS,ksWV  [1.356s]
Every other line (plugin noise, objc warnings) is ignored.

Note on "current component": the per-component line is written when a
component FINISHES, so the one in flight has no line yet. We report it by
index and by how long it has been running (time since the last completion the
viewer observed), which is also what the 30s stall warning keys on. Its name
is not known until it completes.

Dependencies: Python 3 standard library only.
"""
import sys, re, time, os

# [idx/total] status  vendor / id-tail  [elapsed s]  (optional reason)
# The status is space-padded, and every non-ok line carries a trailing
# "(reason)" - anchoring the end right after [Xs] would silently drop every
# timeout/no-types/crashed line (and understate the slowest component).
LINE = re.compile(r'^\[(\d+)/(\d+)\]\s+(\S+)\s+(.+?)\s+\[([\d.]+)s\](?:\s+\((.*)\))?\s*$')
DONE = re.compile(r'(au-registry done|extract-list done|RUN COMPLETE|bootstrap: run complete)', re.I)

# Display order for the outcome tally; unknown statuses are appended as-is.
ORDER = ['ok', 'no-types', 'no-data', 'license-refused', 'load-failed',
         'all_flat', 'failed', 'timeout', 'crashed']
COLOR = {
    'ok': '\033[32m', 'timeout': '\033[31m', 'crashed': '\033[97;41m',
    'license-refused': '\033[33m', 'load-failed': '\033[33m',
    'no-types': '\033[90m', 'no-data': '\033[90m', 'all_flat': '\033[90m',
    'failed': '\033[31m',
}
R, BOLD, DIM = '\033[0m', '\033[1m', '\033[2m'
GRN, INV = '\033[32m', '\033[97;41m'
STALL_S = 30.0


def parse(path):
    rows, done = [], False
    try:
        with open(path, 'r', errors='replace') as f:
            for ln in f:
                m = LINE.match(ln)
                if m:
                    rows.append((int(m.group(1)), int(m.group(2)), m.group(3),
                                 m.group(4).strip(), float(m.group(5)),
                                 (m.group(6) or '').strip()))
                elif DONE.search(ln):
                    done = True
    except FileNotFoundError:
        pass
    return rows, done


def fmt_dur(s):
    s = int(round(s))
    if s < 60:
        return f"{s}s"
    if s < 3600:
        return f"{s // 60}m{s % 60:02d}s"
    return f"{s // 3600}h{(s % 3600) // 60:02d}m"


def bar(frac, width=44):
    fill = max(0, min(width, int(frac * width)))
    return '█' * fill + '░' * (width - fill)


def render(rows, done, last_growth_ts, now, logname):
    if not rows:
        return f"{BOLD}ejextract run{R}  {logname}\n\n  waiting for the first component...\n"

    n = len(rows)
    total = rows[-1][1] or n
    counts = {}
    for r in rows:
        counts[r[2]] = counts.get(r[2], 0) + 1
    elapseds = [r[4] for r in rows]
    tot_el = sum(elapseds)
    avg = tot_el / n if n else 0.0
    remaining = max(0, total - n)
    eta = remaining * avg
    frac = n / total if total else 0.0
    slow = max(rows, key=lambda r: r[4])
    finished = done or n >= total
    running_for = now - last_growth_ts

    out = []
    out.append(f"{BOLD}ejextract run{R}  {DIM}{logname}{R}")
    out.append("")
    out.append(f"  [{bar(frac)}] {BOLD}{frac * 100:5.1f}%{R}   {BOLD}{n}{R}/{total}")
    out.append("")

    parts = []
    for k in ORDER:
        if k in counts:
            parts.append(f"{COLOR.get(k, '')}{k} {counts[k]}{R}")
    for k in counts:
        if k not in ORDER:
            parts.append(f"{k} {counts[k]}")
    out.append("  " + "    ".join(parts))
    out.append("")

    out.append(f"  rate    {avg:5.2f}s / component        elapsed {fmt_dur(tot_el)}")
    if finished:
        out.append(f"  {BOLD}{GRN}run complete{R} - {n} components, "
                   f"{counts.get('ok', 0)} ok")
    else:
        out.append(f"  ETA     {fmt_dur(eta)}   ({remaining} remaining)")
        cur = f"  now     #{n + 1} in flight, running {running_for:4.0f}s"
        if running_for >= STALL_S:
            cur += f"   {INV} STALL > {int(STALL_S)}s {R}"
        out.append(cur)
    out.append("")
    slow_reason = f"  {DIM}({slow[5]}){R}" if slow[5] else ""
    out.append(f"  {DIM}slowest{R}  {slow[3]}  {BOLD}[{slow[4]:.1f}s]{R}  "
               f"{COLOR.get(slow[2], '')}{slow[2]}{R}{slow_reason}")
    out.append("")

    out.append(f"  {DIM}recent{R}")
    for r in rows[-7:]:
        c = COLOR.get(r[2], '')
        out.append(f"    [{r[0]}/{r[1]}] {c}{r[2]:<15}{R} {r[3]}  {DIM}[{r[4]:.2f}s]{R}")
    return "\n".join(out) + "\n"


def main():
    args = sys.argv[1:]
    if not args or args[0] in ('-h', '--help'):
        print(__doc__)
        sys.exit(0 if args else 1)
    path = args[0]
    replay = '--replay' in args
    interval = 1.0
    if '--interval' in args:
        try:
            interval = float(args[args.index('--interval') + 1])
        except (IndexError, ValueError):
            print("--interval needs a number of seconds")
            sys.exit(1)
    logname = os.path.basename(path)

    last_n, last_growth = 0, time.time()
    hide_cursor, show_cursor = '\033[?25l', '\033[?25h'
    sys.stdout.write(hide_cursor)
    try:
        while True:
            rows, done = parse(path)
            now = time.time()
            if len(rows) != last_n:
                last_growth, last_n = now, len(rows)
            frame = render(rows, done, last_growth, now, logname)
            sys.stdout.write('\033[H\033[J' + frame)
            sys.stdout.flush()
            finished = done or (rows and len(rows) >= rows[-1][1])
            if replay or finished:
                break
            time.sleep(interval)
    except KeyboardInterrupt:
        pass
    finally:
        sys.stdout.write(show_cursor)
        sys.stdout.flush()


if __name__ == '__main__':
    main()
