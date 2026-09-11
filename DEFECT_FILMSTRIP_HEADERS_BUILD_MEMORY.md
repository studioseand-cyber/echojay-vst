# DEFECT (build health): image arrays baked into headers make every parallel build memory-fragile

**Filed:** 2026-09-03, per ruling - "file as a separate build-health defect,
do not fix now". Sibling: commit 1ba72f0 (the bare `-j` fix in
`tools/install_local.sh`), which removes the trigger but not the cause.

## What happened (measured, twice)

Two consecutive full builds killed the machine: Terminal's footprint reached
~140 GB and the Mac went into swap death. Neither the repo state nor the
pending edit was at fault. The trigger was `tools/install_local.sh` printing
`cmake --build <dir> -j` with a BARE `-j`, which under the Unix Makefiles
generator means UNLIMITED parallel jobs. Fixed in 1ba72f0.

## Why one bare -j was fatal here and would be harmless elsewhere

    Source/EchoJayWetKnobFilmstrip.h   443,077 lines   44.3 MB
    Source/EchoJayFaderFilmstrip.h      84,493 lines    8.4 MB

Both are PNG byte arrays pasted into headers as `inline const unsigned
char[]` initialisers. Clang materialises a multi-hundred-thousand-element
initialiser list as an AST, so every translation unit that includes one
costs gigabytes of resident memory to compile - independent of what the
TU itself does.

The 44 MB header is pulled in by four headers, not one source file:

    Source/ChainWetKnob.h               (direct include)
    Source/EchoJayDeviceLookAndFeel.h
    Source/LinkEditor.h
    Source/PluginEditor.h

so the cost fans out to every editor-side TU in both plugins, and the
project compiles its 74 sources roughly six times over (EchoJay and
EchoJayLink x AU/VST3/Standalone) plus eight console-app targets. Hundreds
of concurrent multi-GB clang processes is the 140 GB.

The consequence that matters going forward: ANY job count above a handful
is a memory gamble on this project, not a CPU choice. `-j 4` is the
current cap. `tools/reinstall-v2.sh` still defaults to `hw.ncpu`, which
on a high-core machine reproduces the failure - it is off-limits under the
standing constraints anyway, but the default is wrong for the same reason.

## The fix (later pass, not now)

Move both PNGs out of headers into binary resources via
`juce_add_binary_data` (the PNG files already exist under `Assets/`, per
the headers' own comments). The consumers then read
`BinaryData::wet_knob_filmstrip_png` / `_pngSize` instead of the inline
arrays; JUCE compiles the data once into its own small TU and the editor
TUs shrink back to ordinary size. Delete the two headers afterwards.

Acceptance for that pass: peak per-clang RSS on an editor TU back under
~1 GB, and a `-j 8` build of all four plugin targets completing without
swap on this machine.

## Standing rule (from this incident)

Never bare `-j` on this project. Every build command, in scripts and in
docs, carries an explicit job count.
