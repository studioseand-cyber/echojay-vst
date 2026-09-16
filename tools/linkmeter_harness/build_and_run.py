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
cmd = ["clang++"] + out + ["-I", os.path.join(ROOT, "Source"), SRC, LIB]
for f in FRAMEWORKS: cmd += ["-framework", f]
cmd += ["-lcurl", "-o", OUT]

print("compiling harness ...", flush=True)
r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
if r.returncode != 0:
    print("COMPILE FAILED:\n" + r.stderr[-4000:]); sys.exit(3)
print("compiled -> " + OUT, flush=True)

print("running harness (isolated ECHOJAY_STATE_HOME set inside) ...", flush=True)
r = subprocess.run([OUT], cwd=ROOT, capture_output=True, text=True, timeout=120)
sys.stdout.write(r.stdout); sys.stderr.write(r.stderr)
print("\nharness exit code: %d  (nonzero == RED, as required on the current binary)" % r.returncode)
sys.exit(r.returncode)
