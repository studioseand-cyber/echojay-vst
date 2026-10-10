#!/usr/bin/env python3
# Compile the real-assembly harness with the SAME flags PluginEditor.cpp was
# built with (from build-release/compile_commands.json), link the CURRENT shared
# lib, and run it. No CMake target churn; mirrors tools/chainguidance_test.
import json, os, shlex, subprocess, sys

ROOT = "/Users/SeanD/echojay-vst"
CC   = os.path.join(ROOT, "build-release/compile_commands.json")
LIB  = os.path.join(ROOT, "build-release/EchoJay_artefacts/Release/libEchoJay V2_SharedCode.a")
SRC  = os.path.join(ROOT, "tools/linkmeter_harness/harness.cpp")
OUT  = "/private/tmp/claude-502/-Users-SeanD-echojay-vst/8b86da2a-378d-4ecf-97c0-0e33f4993ece/scratchpad/linkmeter_harness"

cc = json.load(open(CC))
entry = next(e for e in cc if e["file"].endswith("PluginEditor.cpp"))
args = shlex.split(entry["command"]) if "command" in entry else list(entry["arguments"])

out, skip = [], False
for i, a in enumerate(args):
    if skip: skip = False; continue
    if i == 0: continue                                   # the compiler
    if a in ("-c", "-o", "-MF", "-MT", "-MQ", "-MJ"): skip = True; continue
    if a in ("-MMD", "-MD", "-MP", "-MG"): continue
    if a.endswith("PluginEditor.cpp") or a.endswith(".o"): continue
    out.append(a)

FRAMEWORKS = ["CoreAudioKit","DiscRecording","CoreAudio","CoreMIDI","AudioToolbox","Accelerate",
              "WebKit","Metal","MetalKit","QuartzCore","Cocoa","Foundation","IOKit","Security","OpenGL",
              "UniformTypeIdentifiers","AVFoundation","CoreMedia","AVKit"]
def _ej_fail(stderr):
    # 10 Oct 2026: this printed stderr[-4000:] only, and clang puts WARNINGS after the error, so a real failure
    # arrived as four thousand characters of -Wfloat-equal with the cause scrolled off. Errors and undefined
    # symbols are surfaced FIRST, every time, then the tail for context.
    lines = stderr.splitlines()
    keep, i = [], 0
    while i < len(lines):
        l = lines[i]
        if ("error:" in l) or ("Undefined symbols" in l) or ("symbol(s) not found" in l) or l.strip().startswith('"_'):
            keep.extend(lines[i:i + 4]); i += 4
        else:
            i += 1
    if keep:
        print("COMPILE/LINK ERRORS:\n" + "\n".join(keep[:120]))
    print("TAIL:\n" + stderr[-2500:])

cmd = ["clang++"] + out + ["-I", os.path.join(ROOT, "Source"), SRC, LIB]
# The V2 archive references the Playback grid's binary data (juce_add_binary_data(EchoJayPlaybackArt), linked
# into the EchoJay target by CMakeLists.txt), so anything linking that archive needs the art lib too or the link
# fails on EJPlaybackArt::laptop_jpg and its siblings. Same fix as tools/harness_build.py and tools/tests.
for _art in (os.path.join(ROOT, "build-release/libEchoJayPlaybackArt.a"),
             os.path.join(ROOT, "build/libEchoJayPlaybackArt.a")):
    if os.path.exists(_art): cmd.append(_art); break
for f in FRAMEWORKS: cmd += ["-framework", f]
cmd += ["-lcurl", "-o", OUT]

print("compiling harness ...", flush=True)
r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
if r.returncode != 0:
    print("COMPILE FAILED:"); _ej_fail(r.stderr); sys.exit(3)
print("compiled -> " + OUT, flush=True)

print("running harness (isolated ECHOJAY_STATE_HOME set inside) ...", flush=True)
r = subprocess.run([OUT], cwd=ROOT, capture_output=True, text=True, timeout=120)
sys.stdout.write(r.stdout); sys.stderr.write(r.stderr)
print("\nharness exit code: %d  (nonzero == RED, as required on the current binary)" % r.returncode)
sys.exit(r.returncode)
