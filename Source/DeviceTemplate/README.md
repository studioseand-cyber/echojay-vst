# Built-in device template

Copy this folder's files to `Source/` and rename `Template` -> your device name to
add a built-in device. **These files are deliberately NOT in the CMake source
list** — they are a skeleton, not a compiled device. Nothing here builds until you
copy it out and add the copies to `CMakeLists.txt`.

## The five-minute version

```bash
cd Source
for f in DeviceTemplate/EedTemplate*; do
  base=$(basename "$f")
  cp "$f" "${base/Template/Chorus}"
done
# then: sed -i '' 's/Template/Chorus/g; s/TEMPLATE/CHORUS/g' EedChorus*
```

Then:

1. **Fill in the schema** in `EedChorusProcessor.cpp`. This is the step that
   matters: the schema is simultaneously what the model is taught, what the
   server validates against, and what `setParamValue` maps. A knob with no
   `ParamSpec` is not dialable, and by house rule does not ship.
2. **Write the engine.** JUCE-free, in `Source/`, with a g++ test in `test/`.
   Skip the engine only when the DSP is a single arithmetic op — see
   `EedPhaseInvertProcessor` for the documented exception.
3. **Pick a uid.** Four ASCII bytes, `'Ej' + two letters` (`0x456A....`). It is
   written into saved chain XML, so it is frozen the moment the device ships.
   Check it is not already taken: `grep -rn "0x456A" ../Source`.
4. **Add your files to `CMakeLists.txt`** — both the `EchoJay` and `EchoJayLink`
   target source lists, in the alphabetised built-in device block. This is the
   only shared file you touch.
5. **Build, and confirm the device appears** in the add-menu and in the
   `[AVAILABLE BUILTINS]` advertisement. Both are generated from the registry, so
   if the registrar ran, it is there.

## Two traps worth knowing before you hit them

**ASCII only in registry text.** `juce::String`'s `const char*` constructor reads
its input as ASCII, so a UTF-8 em-dash in a device `name`/`summary`/
`descriptiveName` arrives double-encoded and ships mojibake into the AI prompt.
It builds and runs; you only see it in the bytes. Use a plain hyphen.
`tools/builtin_registry_test.cpp` pins this.

**Everything goes through the schema.** Have your editor call
`setParamValue`/`getParamValue` rather than the engine directly. Then a knob turn
and an AI move take the identical path and cannot disagree about clamping — and a
param that is turnable is automatically also dialable.

## What you do NOT edit

Nothing else. Specifically not: `ChainHost` (name matching, hosting, dispatch),
`EchoJayAPI` (the advertisement), the add-menu, or any central device list. All
of those read from `BuiltinDeviceRegistry`, which your registrar adds to from your
own `.cpp`. That is what lets several Wave 1 sessions work in parallel without
merge conflicts (see `PARALLEL_SESSIONS.md` for the git worktree setup).

## If your device does not appear

Almost always the linker dropped it. A registrar is a file-scope static whose
constructor is the device's only outside-visible effect, and a static-library
member with no referenced symbol gets discarded. `CMakeLists.txt` force-loads the
shared-code archive to prevent exactly this — check that block still applies to
the target you are building.
