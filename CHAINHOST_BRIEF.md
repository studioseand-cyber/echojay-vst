# Brief: state-chunk matching + deadman coverage in ChainHost

**For a Claude Code session in `~/echojay-vst` on the second Mac
(`macbookpro-lan`).** Branch `feat/dashboard-tab`, based on `7cff677`.

Paste this whole document in as the session prompt.

---

## Run this first

```bash
b=$(git --no-optional-locks rev-parse --abbrev-ref HEAD)
echo "branch: $b"
[ "$b" = "feat/dashboard-tab" ] || { echo "WRONG BRANCH. STOP."; exit 1; }
git --no-optional-locks status --short
git --no-optional-locks log --oneline -1
pwd
```

Expect `7cff677 build: 2.26.4` and a clean tree apart from an untracked
`.claude/`. Report the path you are in.

## Standing context, not optional

- A **second Mac** works this same repo on `integration/reasoning-plus-pitch`,
  on the audio path. Do not touch that branch, do not push to it.
- That branch adds **~2,525 lines to `PluginEditor.cpp`**. Every line you change
  in that file is a merge conflict later. It changes `ChainHost.cpp` by only
  +57, so this work is comparatively safe — but keep `PluginEditor.cpp` edits
  minimal and say exactly which lines and why.
- Push `feat/dashboard-tab` when you have something worth keeping. **Never force
  push.**
- This machine has a different plugin set from the other one. Do not compare
  scan counts or param maps across machines.

---

## 1. THE FINDING THAT DRIVES THIS WORK

`PLUGIN_DASHBOARD_PARITY_SPEC.md` §2c says, twice, that the deadman marker
already records a `state restore` phase and blacklists a plugin that dies
restoring a chunk. The product owner chose the **"attempt the chunk on version
mismatch"** policy *on the strength of that claim*.

**The claim is false.** Verified across `7cff677`,
`origin/feat/plugin-dashboard` and `origin/integration/reasoning-plus-pitch`:

- The deadman (`ChainHost.cpp:3168-3169`, consumed at `565-579`) covers **thin
  VST3 instantiation only**. It writes `desc.fileOrIdentifier`, polls
  validation, and deletes the file on success. AU and fat VST3 loads are not
  covered either.
- `setStateInformation` (`ChainHost.cpp:4395`) sits inside a bare `try/catch`
  and nothing else. **A `try/catch` does not catch a segfault**, which is the
  actual failure mode of a plugin mis-parsing a chunk.
- There is no `state restore` phase anywhere in the repo.

So the chosen policy is currently unsafe. **Part A below makes the claim true.
Do it first.** Part B is worthless without it.

Alongside that, `restoreSavedChain` resolves each slot **by name alone** — no
`uid`, no `format`, no `version` check — and pushes the saved chunk on that
match. The save side has written all three fields since Session B
(`ChainHost.cpp:4783-4787`: `plugin`, `manufacturer`, `format`, `version`,
`uid`). They are written and then ignored. **This means a VST3 chunk can be
pushed into an AU build today, on the existing Chain tab load path** — it is not
a risk the dashboard introduces.

---

## PART A — the deadman covers state restore

### A1. Give the deadman file a phase

Current format is one line: `desc.fileOrIdentifier`. Make it two, with the
second optional, so a file written by the current build still reads correctly.

In `ChainHost.cpp`, the consumer at ~565-579 becomes:

```cpp
auto deadman = getDeadmanFile();
if (deadman.existsAsFile())
{
    // TWO LINES since 2.26.5: identifier, then phase. A file written by an
    // older build has one line, and an ABSENT phase means "load" — which is
    // the only phase that existed when it was written. Absent is not a new
    // state; it is the old one, and it must keep blacklisting exactly as it
    // did before.
    auto lines = juce::StringArray::fromLines (deadman.loadFileAsString());
    const juce::String crashed = lines.size() > 0 ? lines[0].trim() : juce::String();
    const juce::String phase   = lines.size() > 1 ? lines[1].trim() : juce::String ("load");

    if (crashed.isNotEmpty())
        addToBlacklist (crashed,
                        phase == "state restore"
                            ? juce::String ("crashed the host restoring its saved settings (deadman)")
                            : juce::String ("crashed the host during load (deadman)"));
    deadman.deleteFile();
}
```

Update the writer at ~3168-3169 to write the phase explicitly:

```cpp
deadman.replaceWithText (desc.fileOrIdentifier + "\nload");
```

### A2. Wrap the `setStateInformation` call

In `applyRestoredState` (~4362-4416), around the call at 4395. The marker must
be written **before** the call and deleted **after it returns**, so a process
that never returns leaves it behind.

```cpp
// THE DEADMAN, over the one call in this file that runs third-party code on
// data we did not author. A try/catch cannot catch a segfault, and a plugin
// mis-parsing a chunk segfaults — that is the whole failure mode. If this
// call never returns, the marker survives the crash and the next launch
// blacklists the plugin naming THIS phase, so the user gets one bad plugin
// withheld rather than a session that will not open twice.
//
// Written per slot, not per chain: the identifier has to be the plugin that
// actually died, not the last one in the list.
appSupportDir().createDirectory();
auto deadman = getDeadmanFile();
deadman.replaceWithText (identifier + "\nstate restore");

bool applied = false;
try
{
    proc->setStateInformation (mo.getData(), (int) mo.getDataSize());
    applied = true;
}
catch (...)
{
    addStateNote (slotName + ": rejected its saved settings,"
                             " so it loaded at its defaults");
}

deadman.deleteFile();
if (! applied) return;
```

`applyRestoredState` does not currently receive the plugin's
`fileOrIdentifier`. Add it as a parameter — the caller has it on
`items[idx].desc`. Update the declaration in `ChainHost.h:909-910` and both
call sites.

> **Watch the other deadman writer.** `loadPluginAsync` writes the same file for
> thin-VST3 validation. These never overlap in practice — a slot finishes
> loading before its state is applied — but confirm that while reading A2, and
> if you find a path where they can interleave, say so rather than working
> around it.

---

## PART B — uid / format / version matching

### B1. `RestoreItem` carries the saved identity

`ChainHost.h:903-904`, currently:

```cpp
struct RestoreItem { juce::PluginDescription desc; bool bypassed; float wet = 1.0f;
                     juce::String stateBase64; bool expectState = false; };
```

Add a decision, computed at resolve time and honoured later:

```cpp
struct RestoreItem { juce::PluginDescription desc; bool bypassed; float wet = 1.0f;
                     juce::String stateBase64; bool expectState = false;
                     /** Set by restoreSavedChain when the chunk must NOT be
                         pushed. The session-XML path leaves it false: there the
                         description came from the same file as the chunk, so
                         format and uid match by construction. */
                     bool withholdState = false; };
```

### B2. The policy, in `restoreSavedChain`

After `resolveByName` succeeds (~4825-4839), read the three saved fields and
decide. **Only compare a field the saved chain actually carries** — a chain
written before a field existed must not read as a mismatch. Same discipline as
the `imports` `-1` sentinel and the D3.2 legacy-`source` rule.

```cpp
// FORMAT, UID, VERSION. All three have been written since Session B
// (see the save side above) and none of them was ever read. A chunk is
// format-bound: a VST3 chunk does not load into the AU build of the same
// plugin, and pushing it anyway is worse than pushing nothing — best case
// ignored, realistic case garbage parameters that sound wrong and look
// deliberate, worst case a dead host.
//
// ABSENT IS NOT A MISMATCH. A chain saved before a field existed carries
// an empty string, and an empty string must compare as "no opinion" or
// every legacy chain silently loses its settings.
const juce::String savedFormat  = o->getProperty ("format").toString().trim();
const juce::String savedVersion = o->getProperty ("version").toString().trim();
const juce::String savedUid     = o->getProperty ("uid").toString().trim();
const juce::String foundUid (desc.uniqueId);

bool withhold = false;

if (savedFormat.isNotEmpty()
    && ! savedFormat.equalsIgnoreCase (desc.pluginFormatName))
{
    withhold = true;
    addStateNote (name + ": saved as " + savedFormat + " but loaded here as "
                  + desc.pluginFormatName
                  + ", so its settings were not applied (settings do not"
                    " transfer between formats)");
}
else if (savedUid.isNotEmpty() && savedUid != foundUid)
{
    // Same name, same format, different plugin. A rescan can change a uid,
    // but so can two plugins sharing a name, and we cannot tell which from
    // here. Withholding costs one line of text; guessing costs the rack.
    withhold = true;
    addStateNote (name + ": this is a different build from the one saved,"
                         " so its settings were not applied");
}
else if (savedVersion.isNotEmpty() && savedVersion != desc.version)
{
    // ATTEMPTED, deliberately, and said out loud. Most plugins version their
    // own chunks and tolerate an older one; some do not. The deadman from
    // Part A now covers this call, so the bad case is one plugin recorded and
    // withheld on the next launch rather than a session that will not open.
    // The note exists because "never claim a dial you did not perform" cuts
    // both ways: we DID perform it, and the user should still check it.
    addStateNote (name + ": saved from version " + savedVersion
                  + ", this machine has " + desc.version
                  + " — settings were applied, worth checking");
}
```

Then set `item.withholdState = withhold;` alongside the existing field
assignments (~4841-4856).

### B3. Honour it

In `restoreNextSlot` (~4331-4345), carry `items[idx].withholdState` into the
lambda the same way `stateB64` and `expectState` are carried — **by value,
never by index afterwards**, per the existing comment at 4329-4330. Skip the
apply when set:

```cpp
if (! withholdState)
    applyRestoredState (lastSlot, stateB64, expectState, slotName, identifier);
```

Do **not** add a second note here. B2 already named the reason; a second line
saying "settings not applied" makes the panel repeat itself.

---

## PART C — the unsaved-rack confirm

The product owner asked for this explicitly.

`EchoJayEditor::openSavedChain` (`PluginEditor.cpp:25155`) closes hosted
editors, waits 80ms, then unconditionally runs

```cpp
for (int i = ch.getNumSlots() - 1; i >= 0; --i) ch.removeSlot(i);
```

There is no confirmation. **Add one, and put it where every future caller
inherits it** — the dashboard, feed rows and message attachments are all going
to reach this path.

- Confirm only when there is something to lose: the rack is non-empty. An empty
  rack should load silently.
- Use the same `juce::AlertWindow` shape the file already uses for its eleven
  other confirmations, with Cancel bound to `escapeKey` (see 24718, 24757,
  25009 for the house pattern).
- Wording says what will happen, not what the state is — the rule the chain row
  menu already follows for Favourite / Unfavourite.

**This is the one part that touches `PluginEditor.cpp`.** Keep it to the
smallest possible diff and report the exact line range you changed.

---

## PART D — verification

House discipline: **every suite carries a negative control that must fail**, and
a suite passing with zero failures has proven nothing.

Extend `tools/` with a state-matching test (follow `tools/dashboard_test/` for
shape — friend struct over the shipping code, never a copy of it):

1. **Format differs** → `withholdState` true, one note naming both formats, and
   assert `setStateInformation` was **not** called.
2. **uid differs, format same** → withheld, named.
3. **Version differs, format and uid same** → **applied**, and a note exists.
4. **All three match** → applied, **no note at all**.
5. **All three absent** (a legacy chain) → applied, no note. This is the
   regression that matters most: it is every chain saved before Session B.
6. **Deadman**: write a two-line marker with phase `state restore`, construct a
   `ChainHost`, assert the plugin is blacklisted with the state-restore reason
   and the file is gone.
7. **Deadman, one line** (an old-format file) → blacklisted with the *load*
   reason. Absent phase must not change existing behaviour.
8. **Negative control**: assert something you know is false and confirm the
   harness reports it.

Then build and run. **Do not report this as done until it compiles and the
suite runs.**

---

## What I could not do, and why you are doing this

I surveyed this from a cloud session with a file bridge to the Mac. Two limits:

1. **No toolchain.** The bridge is a Linux VM — no Xcode, no `xcodebuild`. I
   cannot compile a line of this.
2. **`git switch` is broken through the bridge.** It cannot unlink files:
   `error: unable to unlink old 'Source/ChainHost.cpp': Operation not
   permitted`. The switch to `feat/dashboard-tab` half-applied and left five
   files at the wrong content. I repaired it by streaming each file's committed
   content back in place, and the tree is clean now — but branch switches,
   merges and rebases are not safe from there.

The tree is currently on `feat/dashboard-tab` at `7cff677`, clean. Start from
that.
