#pragma once
// =============================================================================
//  THE USER-STATE ROOT (6 Sep 2026 ruling: ISOLATION IS A PRECONDITION FOR
//  EVIDENCE, NOT HYGIENE). Every path the plugins use for USER STATE - the
//  Application Support/EchoJay files (auth.json, caches, the link registry and
//  rings), ~/.echojay (dev.json, the preview token), the event log - resolves
//  through these two functions. With ECHOJAY_STATE_HOME unset they return
//  exactly the JUCE special locations they replace: the shipped plugin is
//  byte-for-byte unchanged in behaviour. With it set, everything lands under
//  that directory, so a test harness can NEVER touch the user's live state
//  (twice today one did: a harness constructing the real processor rewrote
//  Sean's live auth.json, and harnesses planting registry rows overwrote each
//  other's - and Pro Tools' - slots in the live registry).
//  Read-only INPUTS (the installed plug-in folders the scanner walks, the
//  Downloads folder a chooser opens on) are not state and are not redirected.
// =============================================================================
#include <juce_core/juce_core.h>
#include <cstdlib>

namespace echojay
{
inline const char* stateHomeOverride() noexcept { return ::getenv ("ECHOJAY_STATE_HOME"); }
inline bool stateIsIsolated() noexcept { auto* e = stateHomeOverride(); return e != nullptr && *e != 0; }

/// ~/Library on macOS (juce::File::userApplicationDataDirectory), or
/// $ECHOJAY_STATE_HOME/Library under isolation.
inline juce::File userAppData()
{
    if (stateIsIsolated()) { juce::File r (juce::String::fromUTF8 (stateHomeOverride())); auto d = r.getChildFile ("Library"); d.createDirectory(); return d; }
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
}
/// ~ (juce::File::userHomeDirectory) for STATE kept under the home (~/.echojay,
/// the event log), or $ECHOJAY_STATE_HOME under isolation.
inline juce::File userStateHome()
{
    if (stateIsIsolated()) { juce::File r (juce::String::fromUTF8 (stateHomeOverride())); r.createDirectory(); return r; }
    return juce::File::getSpecialLocation (juce::File::userHomeDirectory);
}
/// For a harness: abort unless isolated. A test that cannot be isolated must
/// say so and refuse, never quietly use the user's live state.
inline void requireIsolationOrDie (const char* harnessName)
{
    if (! stateIsIsolated())
    {
        std::fprintf (stderr, "%s: REFUSING TO RUN - ECHOJAY_STATE_HOME is not set, so this harness would operate on the user's LIVE state (registry, auth.json, caches). Run it through its build_and_run.sh, which sets a private root.\n", harnessName);
        std::exit (3);
    }
    std::fprintf (stderr, "%s: isolated state root %s\n", harnessName, stateHomeOverride());
}
}
