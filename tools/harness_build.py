#!/usr/bin/env python3
# Generic harness builder: compile a single-file harness with the SAME flags
# PluginEditor.cpp was built with (from build-release/compile_commands.json),
# link the CURRENT shared lib, and run it. Usage: harness_build.py <harness.cpp>
import json, os, shlex, subprocess, sys

ROOT = "/Users/SeanD/echojay-vst"
CC   = os.path.join(ROOT, "build-release/compile_commands.json")
LIB  = os.environ.get("EJ_LIB") or os.path.join(ROOT, "build-release/EchoJay_artefacts/Release/libEchoJay V2_SharedCode.a")   # EJ_LIB: a before/after lib for RED/GREEN runs
SRC  = os.path.abspath(sys.argv[1])
# 2 Oct 2026: THE V2 ARCHIVE AND THE HEADERS MUST BE THE SAME TREE. This is the MIRROR of the Link-side refusal
# in tools/merge_gate_tests/compile_link_harness.sh (21t-m, dbc0e91), and it is here because that one's premise
# was wrong: it reads "ctest -L fast rebuilds build-guards, which is the V2 archive". build-guards is a DIFFERENT
# TREE from build-release, and the archive every V2-side harness links is the build-release one, below. Nothing in
# the gate rebuilds it.
#
# What that cost: the 1 Oct Part 1 gate built `--target EchoJayLink EchoJayProbe`, which is exactly what it was
# asked to build and does not include the V2 archive. libEchoJay V2_SharedCode.a therefore stayed as ANOTHER
# BRANCH had left it, and lease_id_guard, level_match_guard and role_snapshot_guard all compiled this branch's
# headers against it. The layouts disagreed, LinkSlotInfo.uid was read at the wrong offset and came out a garbage
# character pointer, and all three died with SIGSEGV in the V2 side before printing an assertion. That reads
# exactly like a product crash: it was attributed to three different causes over several hours, and the two
# guards that are in fact GREEN were reported as reds. A mismatch is a REFUSAL with its reason, not a segfault.
#
# EJ_LIB / EJ_SRC_ROOT deliberately skip the check, for the same reason the Link side skips it: pairing a
# PRE-ROUND archive with PRE-ROUND headers is how a RED run is taken, and that pairing is consistent.
def _refuse_stale_archive(lib):
    if os.environ.get("EJ_LIB") or os.environ.get("EJ_SRC_ROOT"):
        return
    if not os.path.isfile(lib):
        sys.exit("  FAIL  the V2 archive does not exist at %s - build it first:\n"
                 "        cmake --build build-release -j 4 --target EchoJay" % lib)
    lib_mtime = os.path.getmtime(lib)
    # 5 Oct 2026: COMPARE AGAINST WHAT THE ARCHIVE ACTUALLY CONTAINS.
    #
    # This walked every Source/*.{h,cpp} and refused if ANY was newer. But Source/LinkProcessor.cpp is compiled
    # into the LINK target only - it is not in the V2 archive - so a Link-only edit made every V2-side harness
    # refuse while the V2 archive was genuinely current. Overnight on 4/5 Oct that turned four two-sided guards
    # (alias_mirror, lease_id, level_match, role_snapshot) red at once, for a reason that had nothing to do with
    # them: their V2 half was never built, so their legs failed with empty aliases and null acks.
    #
    # The build directory knows exactly which sources became this archive, so the object list is the authority
    # rather than a hand-maintained exclusion list. EVERY HEADER still counts, because a header changes layout for
    # whatever includes it; only .cpp files are narrowed to the ones that are really in there. If the object dir
    # cannot be found we fall back to the old, broader rule - refusing too often is safe, refusing too little is not.
    # 6 Oct 2026: THE OBJECT DIR MUST BELONG TO THE ARCHIVE BEING CHECKED. This was hardcoded to EchoJay.dir, the
    # V2 target, so checking the LINK archive narrowed its .cpp list to V2's objects - i.e. it counted
    # PluginProcessor.cpp and friends, which the Link archive never compiled. On 6 Oct that refused all five
    # two-sided guards at once because a PluginProcessor.h edit was newer than a Link archive it cannot affect.
    # The target is derived from the archive's own name, which is the only thing that can be right for both.
    _base = os.path.basename(lib)
    _tgt  = "EchoJayLink.dir" if "Link" in _base else "EchoJay.dir"
    objdir = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(lib))),
                          "CMakeFiles", _tgt, "Source")
    if not os.path.isdir(objdir):
        objdir = os.path.join(ROOT, "build-release", "CMakeFiles", _tgt, "Source")
    archived_cpp = set()
    if os.path.isdir(objdir):
        for o in os.listdir(objdir):
            if o.endswith(".o"):
                archived_cpp.add(o[:-2])          # "ChainHost.cpp.o" -> "ChainHost.cpp"
    newer = []
    for root, _dirs, files in os.walk(os.path.join(ROOT, "Source")):
        for f in files:
            if not f.endswith((".h", ".cpp")):
                continue
            # A .cpp that is not in the archive cannot have changed the archive.
            if f.endswith(".cpp") and archived_cpp and f not in archived_cpp:
                continue
            fp = os.path.join(root, f)
            try:
                if os.path.getmtime(fp) > lib_mtime:
                    newer.append(os.path.relpath(fp, ROOT))
            except OSError:
                pass
    if newer:
        newer.sort()
        import time as _t
        sys.exit("  FAIL  the V2 archive is OLDER than the headers this would compile against, so the two would\n"
                 "        disagree about every object's layout. That is a segfault in the V2 side, not a result.\n"
                 "        archive: %s\n"
                 "        newer sources (%d; first 5):\n%s\n"
                 "        rebuild it:  cmake --build build-release -j 4 --target EchoJay"
                 % (_t.strftime('%b %d %H:%M:%S', _t.localtime(lib_mtime)), len(newer),
                    "\n".join("          " + n for n in newer[:5])))
_refuse_stale_archive(LIB)
# 28 Sep 2026: THE OUTPUT PATH CARRIES THE GUARD'S NAME, and the scratch directory is not baked in.
# It used to be <one fixed scratchpad>/<source basename>_bin, and FIVE guards have a file called v2_side.cpp
# (alias_mirror, lease_id, level_match, link_state, role_snapshot) - so all five compiled to ONE path. Four of
# them run back to back in the gate, and level_match_guard's V2 side was launched as lease_id_guard's binary:
# its v2.log holds "v2 side: compiled (EJ_LIG_HOME unset - the runner starts the pair)", the Apply was never
# pressed, and every leg that waits on a V2 file failed with the trims still at their starting values. A shared
# name between two harnesses is a harness fault that reads exactly like a product one.
SCRATCH = os.environ.get("EJ_SCRATCH") or \
          "/private/tmp/claude-502/-Users-SeanD-echojay-vst/8b86da2a-378d-4ecf-97c0-0e33f4993ece/scratchpad"
OUT  = os.path.join(SCRATCH, os.path.basename(os.path.dirname(SRC)) + "_" +
                    os.path.splitext(os.path.basename(SRC))[0] + "_bin")

cc = json.load(open(CC))
entry = next(e for e in cc if e["file"].endswith("PluginEditor.cpp"))
args = shlex.split(entry["command"]) if "command" in entry else list(entry["arguments"])

out, skip = [], False
for i, a in enumerate(args):
    if skip: skip = False; continue
    if i == 0: continue
    if a in ("-c", "-o", "-MF", "-MT", "-MQ", "-MJ"): skip = True; continue
    if a in ("-MMD", "-MD", "-MP", "-MG"): continue
    if a.endswith("PluginEditor.cpp") or a.endswith(".o"): continue
    out.append(a)

FRAMEWORKS = ["CoreAudioKit","DiscRecording","CoreAudio","CoreMIDI","AudioToolbox","Accelerate",
              "WebKit","Metal","MetalKit","QuartzCore","Cocoa","Foundation","IOKit","Security","OpenGL",
              "UniformTypeIdentifiers","AVFoundation","CoreMedia","AVKit"]
SRC_ROOT = os.environ.get("EJ_SRC_ROOT") or ROOT   # EJ_SRC_ROOT: a checkout whose headers match EJ_LIB (a RED run pairs the pre-round headers with the pre-round lib)
cmd = ["clang++"] + out + os.environ.get("EJ_CXXFLAGS", "").split() + ["-I", os.path.join(SRC_ROOT, "Source"), SRC, LIB]   # EJ_CXXFLAGS: e.g. -DEJ_GUARD_TODAY
for f in FRAMEWORKS: cmd += ["-framework", f]
cmd += ["-lcurl", "-o", OUT]

print("compiling %s ..." % os.path.basename(SRC), flush=True)
r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
if r.returncode != 0:
    print("COMPILE FAILED:\n" + r.stderr[-4000:]); sys.exit(3)
print("compiled -> " + OUT, flush=True)

print("running ...", flush=True)
r = subprocess.run([OUT], cwd=ROOT, capture_output=True, text=True, timeout=180)
sys.stdout.write(r.stdout); sys.stderr.write(r.stderr)
print("\nexit code: %d  (0 == GREEN, nonzero == RED)" % r.returncode)
rc = r.returncode

# SCRIBBLE LEG (18 Sep 2026 ruling, after the Pro Tools crash in roleByNameFor): a harness that exercises the real
# build/apply/bubble path runs TWICE - plain, then with malloc scribbling so a read of freed memory is deterministic
# rather than luck (the plain bubble_parity run was GREEN on the crashing lib; the scribbled run died in the same
# frame as Pro Tools). Opt in per script with EJ_SCRIBBLE_LEG=1. The script fails if EITHER run fails.
if os.environ.get("EJ_SCRIBBLE_LEG") == "1":
    env = dict(os.environ, MallocScribble="1", MallocPreScribble="1", MallocGuardEdges="1")
    print("\nSCRIBBLE LEG: running again with MallocScribble=1 MallocPreScribble=1 MallocGuardEdges=1 ...", flush=True)
    try:
        r2 = subprocess.run([OUT], cwd=ROOT, capture_output=True, text=True, timeout=300, env=env)
        sys.stdout.write(r2.stdout); sys.stderr.write(r2.stderr[-3000:])
        sig = (" (signal %d%s)" % (-r2.returncode, ", SIGSEGV" if r2.returncode == -11 else "")) if r2.returncode < 0 else ""
        print("\nscribble leg exit code: %d%s  (0 == GREEN, nonzero == RED)" % (r2.returncode, sig))
        if r2.returncode != 0: rc = rc or (r2.returncode if r2.returncode > 0 else 128 - r2.returncode)
    except subprocess.TimeoutExpired:
        print("\nscribble leg: TIMEOUT (300 s) -> RED"); rc = rc or 124
    print("\nBOTH LEGS: %s" % ("GREEN" if rc == 0 else "RED (plain %d, see above)" % r.returncode))
sys.exit(rc)
