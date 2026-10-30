#!/usr/bin/env python3
"""The compiling half of the push guard. See syntax_changed.sh for why it exists.

One job: for each committed file, find CMake's own command for it, replace the input with the
committed copy and the output with -fsyntax-only, and run it. The verdict is the compiler's.
"""
import io, json, os, shlex, subprocess, sys, tempfile

# ---- MARKERS, NOT A SPACE-JOINED STRING (Kathy, 30 Oct 2026) -------------------------------
#
# This took the databases as ONE argument and split it on spaces. Kathy's checkout is under
# "ECHOJAY FILES", so every path came apart, no database could be read, all thirteen of her files
# were "NOT CHECKED" and the push went through as "unverified" - a guard waving a push past because
# it could not read its own inputs. Each path is now its own argv entry between markers, so there
# is no quoting rule to get wrong.
def _args(argv):
    tip, dbs, files, mode = "", [], [], None
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--tip":     tip = argv[i + 1]; i += 2; mode = None; continue
        if a == "--dbs":     mode = "dbs";      i += 1; continue
        if a == "--files":   mode = "files";    i += 1; continue
        (dbs if mode == "dbs" else files).append(a)
        i += 1
    return tip, dbs, files

tip, cc_list, files = _args(sys.argv[1:])
if not tip or not cc_list:
    sys.exit("push-guard: CANNOT COMPILE - called without --tip and --dbs. Nothing was checked, "
             "and a guard that cannot read its inputs must not pass a push.")
root = os.getcwd()

# EVERY database, in the order given: the first one that knows a file wins, so the plugin's own
# build describes the plugin's files and a harness is still checked if only build-release has it.
by_file = {}
dbs_read = 0
for one in cc_list:
    try:
        db = json.load(io.open(one, encoding="utf-8"))
    except Exception as ex:                      # a half-written database is not a verdict
        print(f"push-guard: could not read {one} ({ex}) - the others are still used")
        continue
    dbs_read += 1
    for e in db:
        by_file.setdefault(os.path.realpath(e["file"]), e)

fails, checked, unknown = [], 0, []
with tempfile.TemporaryDirectory() as tmp:
    # ---- THE WHOLE COMMIT, NOT ONE FILE OUT OF IT -----------------------------------------
    #
    # The second rewrite's fault, and it is the same family as the one the guard exists for: a
    # committed .cpp compiled against the WORKING TREE's headers is a verdict on neither. It
    # reported HEAD as broken because a header rename sat uncommitted beside it - a false refusal,
    # which is worse than no guard at all.
    #
    # So Source/ and tools/ are materialised AS COMMITTED and every include path that pointed into
    # the working tree is redirected there. Build artefacts (JuceLibraryCode, the JUCE modules) are
    # not in the commit and stay where they are: they are the toolchain, not the change.
    tree = os.path.join(tmp, "tree")
    os.makedirs(tree)
    tar = subprocess.run(["git", "archive", tip, "Source", "tools"], capture_output=True)
    if tar.returncode != 0:
        print(f"push-guard: could not materialise {tip[:8]} - {tar.stderr.decode()[:200]}")
        sys.exit(1)
    subprocess.run(["tar", "-x", "-C", tree], input=tar.stdout, check=True)

    def redirect(tok):
        """A path into this checkout's Source/ or tools/ becomes the same path in the commit's."""
        for sub in ("Source", "tools"):
            here = os.path.join(root, sub)
            if tok == here or tok.startswith(here + os.sep):
                return os.path.join(tree, os.path.relpath(tok, root))
        return tok

    for f in files:
        full = os.path.realpath(os.path.join(root, f))
        e = by_file.get(full)
        if e is None:
            unknown.append(f)
            continue
        committed = os.path.join(tree, f)
        if not os.path.isfile(committed):
            print(f"push-guard: {f} is not in {tip[:8]} (deleted or renamed) - skipped")
            continue
        argv, skip = [], False
        for tok in shlex.split(e["command"]):
            if skip:
                skip = False
                continue
            if tok in ("-o", "-MF", "-MT", "-MQ"):
                skip = True
                continue
            if tok == "-c":
                continue
            if os.path.realpath(os.path.join(e.get("directory", root), tok)) == full:
                argv.append(committed)
                continue
            if tok.startswith("-I"):
                argv.append("-I" + redirect(tok[2:]))
                continue
            argv.append(redirect(tok))
        argv.insert(1, "-fsyntax-only")
        argv.insert(2, "-iquote")
        argv.insert(3, os.path.dirname(committed))
        r = subprocess.run(argv, cwd=e.get("directory", root), capture_output=True, text=True)
        checked += 1
        if r.returncode == 0:
            print(f"push-guard: ok   {f}")
        else:
            print(f"push-guard: FAIL {f}")
            for line in [l for l in r.stderr.splitlines() if "error:" in l][:6]:
                print("    " + line.replace(committed, f).replace(tree + "/", ""))
            fails.append(f)

for f in unknown:
    print(f"push-guard: NOT CHECKED {f} - CMake does not know this file (a new target needs a "
          f"configure); it is pushed unverified")
print(f"push-guard: {checked} file(s) compiled, {len(fails)} failed, {len(unknown)} unverified.")
if fails:
    print("push-guard: REFUSED. A commit that does not parse must not leave this machine - that is")
    print("push-guard: what cost Kathy her morning on 29 Oct 2026.")
    sys.exit(1)

# ---- AND NOTHING CHECKED IS NOT A PASS (Kathy, 30 Oct 2026) --------------------------------
#
# Her run compiled 0 of 13 and printed "pushed unverified", and the push went. Whatever the cause -
# the space in her path, a database that does not describe this tree, a configure nobody ran - a
# guard that checked NOTHING has not guarded anything, and the one line it prints reads like
# diligence. An individual file CMake genuinely does not know stays a warning, because a new
# harness target is a real and harmless case; all of them being unknown is the guard failing.
# ---- WHAT "NOTHING CHECKED" ACTUALLY HAS TO MEAN (31 Oct 2026) -----------------------------
#
# The first cut refused whenever nothing was compiled, and it immediately blocked a legitimate
# push: a commit touching only tools/host_rig/host_rig.mm, which NO database knows because its own
# script compiles it. That is not the fault Kathy reported. Hers was a guard that COULD NOT READ
# ITS INPUTS - the space in her path broke every database path - and then waved the push through.
#
# So the line is whether the guard could do its job, not whether these particular files happened to
# be in a database. No database readable at all: refuse, because nothing was verified and nothing
# could have been. Databases read fine and these files simply are not in them: say so by name and
# let it go, which is what it did before and is correct for a harness with its own build.
if dbs_read == 0:
    print("push-guard: REFUSED - not one database could be read, so NOTHING was verified and")
    print("push-guard: nothing could have been. This is the fault Kathy hit: a path this guard")
    print("push-guard: could not resolve, reported as 'unverified' and let through. Configure a")
    print("push-guard: build, or push with EJ_PUSH_GUARD_OFF=1 and say that nothing was compiled.")
    sys.exit(1)
if checked == 0 and unknown:
    print(f"push-guard: nothing to compile here - all {len(unknown)} file(s) are outside the "
          f"{dbs_read} database(s) read (a harness with its own build script, typically). The")
    print("push-guard: databases WERE readable, so this is not the unverified-pass fault.")
