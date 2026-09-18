#!/usr/bin/env python3
# Generic harness builder: compile a single-file harness with the SAME flags
# PluginEditor.cpp was built with (from build-release/compile_commands.json),
# link the CURRENT shared lib, and run it. Usage: harness_build.py <harness.cpp>
import json, os, shlex, subprocess, sys

ROOT = "/Users/SeanD/echojay-vst"
CC   = os.path.join(ROOT, "build-release/compile_commands.json")
LIB  = os.environ.get("EJ_LIB") or os.path.join(ROOT, "build-release/EchoJay_artefacts/Release/libEchoJay V2_SharedCode.a")   # EJ_LIB: a before/after lib for RED/GREEN runs
SRC  = os.path.abspath(sys.argv[1])
OUT  = "/private/tmp/claude-502/-Users-SeanD-echojay-vst/8b86da2a-378d-4ecf-97c0-0e33f4993ece/scratchpad/" + \
       os.path.splitext(os.path.basename(SRC))[0] + "_bin"

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
