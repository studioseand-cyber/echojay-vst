#!/usr/bin/env python3
"""downgrade_keys_guard - 11 October 2026.

THE STANDING RULE, which has been argued by hand at the end of every round:

    "Every new build must be downgrade-safe: a project or saved chain written by the new build must open
     sensibly in ship_2026-10-09a (new state fields additive, nothing the old build depends on removed or
     renamed)."

Sean asked for it as a LEG this round, and he is right to: the rule has been checked by reading the diff and
saying "additive", which is exactly the kind of check that holds until the day it does not. A removal round is
the worst possible round to be relying on that, because a removal is precisely how a key stops being written.

WHAT IT DOES. For each reference build, it reads that build's OWN SOURCE at its commit and collects the
top-level state keys its setStateInformation READS and the ones its getStateInformation WROTE. The keys AT RISK
are the ones in both - that build's live contract with itself - minus what this tree still writes. A key only
that build read, and that nothing wrote, is a legacy reader kept for an even older project (chainLoadedDescXml
is one: the single-plugin key from before chainSlotsXml) and its absence breaks nothing. Getting this wrong in
the lenient direction would be worse than no guard, so the two sets are both taken from the old build's own
source rather than assumed.

The other direction needs no check: juce::var object reads ignore unknown keys, so a key this build ADDS is
additive by construction - which is the half of the rule that was always safe.

A key may still be dropped deliberately. It then needs an entry in ALLOWED below, with the reason, and the
guard prints that reason every run - so the argument for a removal lives next to the removal instead of in a
commit message nobody re-reads.

WHAT IT CANNOT DO, said plainly rather than implied. It does not run the old binary, so it proves the SHAPE is
still fed, not that the old build is happy with the VALUES. A key whose meaning changed while its name stayed
would pass here. That is a real limit and the reason the GREEN.txt still carries the argument in words.
"""
import re, subprocess, sys

# The reference builds. 10-10b names its own commit in its GREEN.txt. 09a's folder is not tracked, so its
# commit is the last one before that gate ran (10 Oct 01:25) - derived, and said to be derived.
REFS = [
    ("ship_2026-10-10b", "51ef90a3", "named in its GREEN.txt"),
    ("ship_2026-10-09a", "157d1fa9", "derived: the last commit before its 01:25 gate (the folder is untracked)"),
]
FILES = ["Source/PluginProcessor.cpp", "Source/LinkProcessor.cpp"]

# A key this build stopped writing ON PURPOSE, with the reason it is safe for an older build to find it
# absent. Keyed by (file, key). Anything not in here is a failure.
ALLOWED = {
    ("Source/PluginProcessor.cpp", "borrowedRacks"):
        "11 Oct 2026, plan section 6. It carried an OPEN borrowed rack across a save, because a chain built "
        "on a Link lived only in the V2's mirror until the rack was left (Sean's 4 Oct bug). A build is a "
        "COMMAND now, so the Link has the rack before any save is possible and its own chunk carries it. An "
        "older build finds the key absent, which is the case it was always written to handle - it was "
        "additive from the start, present only while a borrow was open - and it needs nothing from it. The "
        "READER stays in this build, for the projects that still hold one: borrow_migration_guard.",
    # The three fields read from INSIDE a borrowedRacks entry. They go with it and have no other writer.
    ("Source/PluginProcessor.cpp", "uid"):     "a field of a borrowedRacks entry; see borrowedRacks above",
    ("Source/PluginProcessor.cpp", "baseRev"): "a field of a borrowedRacks entry; see borrowedRacks above",
    ("Source/PluginProcessor.cpp", "chain"):   "a field of a borrowedRacks entry; see borrowedRacks above",
}

def show(commit, path):
    try:
        return subprocess.run(["git", "show", f"{commit}:{path}"], capture_output=True, text=True,
                              check=True).stdout
    except subprocess.CalledProcessError:
        return ""

def body_of(src, signature):
    """The text of one function body, by brace balance from its opening brace."""
    i = src.find(signature)
    if i < 0:
        return ""
    j = src.find("{", i)
    if j < 0:
        return ""
    depth, k = 0, j
    while k < len(src):
        if src[k] == "{":
            depth += 1
        elif src[k] == "}":
            depth -= 1
            if depth == 0:
                return src[j:k + 1]
        k += 1
    return src[j:]

READ  = re.compile(r'(?:getProperty|hasProperty)\s*\(\s*"([A-Za-z0-9_]+)"')
WRITE = re.compile(r'setProperty\s*\(\s*"([A-Za-z0-9_]+)"')

SIGS = {
    "Source/PluginProcessor.cpp": ("void EchoJayProcessor::setStateInformation",
                                   "void EchoJayProcessor::getStateInformation"),
    "Source/LinkProcessor.cpp":   ("void LinkProcessor::setStateInformation",
                                   "void LinkProcessor::getStateInformation"),
}

def main():
    failures = 0
    print("downgrade_keys_guard: a key an older build READS must still be WRITTEN by this one")
    for path in FILES:
        with open(path) as f:
            cur = f.read()
        written = set(WRITE.findall(body_of(cur, SIGS[path][1])))
        if not written:
            print(f"  FAIL  {path}: could not read this tree's getStateInformation body")
            failures += 1
            continue
        print(f"  ..    {path}: this build writes {len(written)} top-level key(s)")
        for name, commit, how in REFS:
            old = show(commit, path)
            if not old:
                print(f"  FAIL  {path}: {name} ({commit}) is not in this repository - cannot check against it")
                failures += 1
                continue
            read = set(READ.findall(body_of(old, SIGS[path][0])))
            wrote = set(WRITE.findall(body_of(old, SIGS[path][1])))
            if not read or not wrote:
                print(f"  FAIL  {path}: could not read {name}'s state functions at {commit}")
                failures += 1
                continue
            at_risk = sorted((read & wrote) - written)
            broken, excused = [], []
            for k in at_risk:
                (excused if (path, k) in ALLOWED else broken).append(k)
            for k in excused:
                print(f"  ok    {path} vs {name}: \"{k}\" is no longer written, DELIBERATELY - {ALLOWED[(path, k)]}")
            if broken:
                print(f"  FAIL  {path} vs {name} ({commit}, {how}): {len(broken)} key(s) in that build's own "
                      f"read+write contract are no longer written, with no recorded reason: "
                      f"{', '.join(broken)}")
                failures += 1
            else:
                print(f"  ok    {path} vs {name} ({commit}): every key in its own read+write contract "
                      f"({len(read & wrote)}) is still written or excused")
    print(f"\n==== downgrade_keys_guard: {'GREEN' if failures == 0 else 'RED'} "
          f"({failures} check(s) failed) ====")
    return 0 if failures == 0 else 1

if __name__ == "__main__":
    sys.exit(main())
