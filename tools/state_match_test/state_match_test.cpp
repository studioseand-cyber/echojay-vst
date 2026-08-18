/*
    State-chunk matching + deadman coverage self-test (CHAINHOST_BRIEF Part D).

    WHY THIS EXISTS. restoreSavedChain resolved a slot by name alone and pushed
    the saved chunk on that match, though the save side had written format,
    version and uid since Session B. And the deadman marker covered thin-VST3
    validation only, while setStateInformation — the one call that runs
    third-party code on data we did not author — sat inside a try/catch that
    cannot catch a segfault. Both are now fixed in ChainHost; this holds them.

    IT CALLS THE SHIPPING CODE. Every case goes in through the public
    restoreSavedChain and out through getSlotProcessor / getStateNotes, so the
    resolve, the policy, restoreNextSlot, loadPluginAsync and applyRestoredState
    that run here are the ones the plugin runs. A friend struct reaches the
    deadman file, the blacklist and the entries pool for the checks that need
    them; nothing was made public and nothing is reimplemented here.

    THE STAND-IN PLUGIN. A third-party plugin is not deterministic across
    machines (the brief forbids comparing plugin sets between them), so the
    slot under test is a tiny built-in device registered by THIS translation
    unit through the same BuiltinDeviceRegistrar every shipping device uses.
    ChainHost loads it through loadBuiltinNow, synchronously, which is what
    lets the whole restore run without a message loop. Its setStateInformation
    counts its calls, reads the deadman marker while it is inside the call,
    and throws on request — the three things a real plugin cannot be asked to
    report.

    DISK. ChainHost persists to ~/Library/Application Support/EchoJay, and the
    deadman cases write a marker there and blacklist a path. JUCE resolves "~"
    through NSHomeDirectory(), which IGNORES $HOME on macOS, so this binary
    carries its own NSHomeDirectory (below) that answers EJ_STATE_TEST_HOME;
    the linker binds the static lib's call to the definition in this
    executable rather than Foundation's. And it REFUSES to run unless JUCE's
    app-data directory actually resolves under that path, so if the override
    ever stops binding the run stops before it touches the real folder.

    HOUSE DISCIPLINE: the last section is a negative control. A false
    assertion is fed to the harness and the harness must report it; a suite
    that passes with zero failures has proven nothing about itself.
*/

#include <CoreFoundation/CoreFoundation.h>   // before JUCE: MacTypes' Point vs juce::Point
#include <JuceHeader.h>
#include "ChainHost.h"
#include "EedDeviceRegistry.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

// ---------------------------------------------------------------------------
// The home-directory override. See DISK in the header comment. Returns a
// toll-free-bridged CFString (an NSString* to the caller) that lives for the
// process. Set EJ_STATE_TEST_HOME to an existing scratch directory.
// ---------------------------------------------------------------------------
extern "C" CFStringRef NSHomeDirectory (void)
{
    static CFStringRef s = [] () -> CFStringRef
    {
        const char* home = std::getenv ("EJ_STATE_TEST_HOME");
        return CFStringCreateWithCString (kCFAllocatorDefault,
                                          home != nullptr ? home : "/nonexistent",
                                          kCFStringEncodingUTF8);
    }();
    return s;
}

// ---------------------------------------------------------------------------
// The stand-in device
// ---------------------------------------------------------------------------
namespace
{
constexpr const char* kProbeName       = "EJ State Probe";
constexpr const char* kProbeIdentifier = "echojay:test:stateprobe";
constexpr int         kProbeUid        = 0x454A5350;   // 'EJSP'
constexpr int         kThrowValue      = 0xDEAD;       // a chunk that asks the probe to throw

struct ProbeLog
{
    int          setStateCalls = 0;
    bool         markerExisted = false;   // deadman present INSIDE setStateInformation
    juce::String markerText;              // and what it said
    void reset() { *this = ProbeLog(); }
};
ProbeLog& probeLog() { static ProbeLog l; return l; }
}   // namespace

/** The friend declared in ChainHost.h. */
struct EchoJayStateMatchTestAccess
{
    static juce::File deadmanFile()   { return ChainHost::getDeadmanFile(); }
    static juce::File blacklistFile() { return ChainHost::getBlacklistFile(); }
    static bool isBlacklisted (const ChainHost& h, const juce::String& path)
    {
        return h.isBlacklisted (path);
    }
    static juce::String blacklistReason (const ChainHost& h, const juce::String& path)
    {
        std::lock_guard<std::mutex> lock (h.pluginsMutex_);
        return h.blacklistMeta_[path];
    }
    /** A scanner row, as the thin VST3 scan would record it. */
    static void addEntry (ChainHost& h, const juce::PluginDescription& d)
    {
        std::lock_guard<std::mutex> lock (h.pluginsMutex_);
        h.entries_.add (d);
    }
};

namespace
{
using TA = EchoJayStateMatchTestAccess;

class ProbeProcessor final : public juce::AudioProcessor
{
public:
    ProbeProcessor()
        : juce::AudioProcessor (BusesProperties()
              .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
              .withOutput ("Output", juce::AudioChannelSet::stereo(), true)) {}

    int value = 0;   // the one thing the chunk carries

    const juce::String getName() const override { return kProbeName; }
    void prepareToPlay (double, int) override {}
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& dest) override
    {
        dest.setSize (sizeof (int));
        dest.copyFrom (&value, 0, sizeof (int));
    }

    void setStateInformation (const void* data, int size) override
    {
        auto& log = probeLog();
        ++log.setStateCalls;
        // Observed from INSIDE the call: this is the window the deadman exists
        // to cover, and the only place its presence can be witnessed.
        const auto marker = TA::deadmanFile();
        log.markerExisted = marker.existsAsFile();
        log.markerText    = marker.loadFileAsString();

        int v = 0;
        if (size >= (int) sizeof (int)) std::memcpy (&v, data, sizeof (int));
        if (v == kThrowValue) throw std::runtime_error ("probe: chunk refused");
        value = v;
    }
};

BuiltinDevice makeProbeDevice()
{
    BuiltinDevice d;
    d.name            = kProbeName;
    d.category        = "Utility";
    d.descriptiveName = "state-match self-test probe";
    d.summary         = "test only";
    d.identifier      = kProbeIdentifier;
    d.uid             = kProbeUid;
    d.create          = [] { return std::make_unique<ProbeProcessor>(); };
    return d;
}

// The same line that integrates every shipping device.
const BuiltinDeviceRegistrar probeRegistrar { makeProbeDevice() };

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------
int failures = 0;

void check (bool ok, const juce::String& what, const juce::String& detail = {})
{
    if (ok) return;
    ++failures;
    std::printf ("  FAIL  %s%s\n", what.toRawUTF8(),
                 detail.isNotEmpty() ? ("  [" + detail + "]").toRawUTF8() : "");
}

juce::String join (const juce::StringArray& a) { return a.joinIntoString (" | "); }

bool anyNoteContains (const juce::StringArray& notes, const juce::String& needle)
{
    for (auto& n : notes) if (n.contains (needle)) return true;
    return false;
}

juce::String chunkFor (int value)
{
    return juce::Base64::toBase64 (&value, sizeof (int));
}

/** One saved slot as the shared chain format writes it (see buildChainSlotsVar).
    An empty string for format / version / uid means the field is NOT WRITTEN,
    which is how a chain saved before Session B looks. */
juce::var savedSlot (const juce::String& plugin, const juce::String& format,
                     const juce::String& version, const juce::String& uid, int n = 1)
{
    auto o = std::make_unique<juce::DynamicObject>();
    o->setProperty ("n",        n);
    o->setProperty ("plugin",   plugin);
    o->setProperty ("bypassed", false);
    if (format.isNotEmpty())  o->setProperty ("format",  format);
    if (version.isNotEmpty()) o->setProperty ("version", version);
    if (uid.isNotEmpty())     o->setProperty ("uid",     uid);
    juce::Array<juce::var> arr;
    arr.add (juce::var (o.release()));
    return juce::var (arr);
}

juce::var stateFor (const juce::String& b64)
{
    auto o = std::make_unique<juce::DynamicObject>();
    o->setProperty ("1", b64.isEmpty() ? juce::var() : juce::var (b64));
    return juce::var (o.release());
}

void clearRack (ChainHost& h)
{
    for (int i = h.getNumSlots() - 1; i >= 0; --i) h.removeSlot (i);
}

ProbeProcessor* probeInSlot0 (ChainHost& h)
{
    return h.getNumSlots() > 0 ? dynamic_cast<ProbeProcessor*> (h.getSlotProcessor (0)) : nullptr;
}

/** Restore one saved slot into an empty rack and report what happened. */
struct Outcome
{
    bool             loaded  = false;
    bool             applied = false;   // the probe's value became the saved one
    int              calls   = 0;
    juce::StringArray notes;
};

Outcome restoreOne (ChainHost& h, const juce::var& slots, const juce::var& state, int savedValue)
{
    clearRack (h);
    probeLog().reset();
    h.restoreSavedChain (slots, state);      // synchronous for a built-in
    Outcome o;
    o.notes = h.getStateNotes();
    o.calls = probeLog().setStateCalls;
    if (auto* p = probeInSlot0 (h))
    {
        o.loaded  = true;
        o.applied = (p->value == savedValue);
    }
    return o;
}

const juce::String kBuiltinFormat (kEchoJayBuiltinFormat);
const juce::String kProbeUidStr (kProbeUid);
}   // namespace

int main()
{
    std::printf ("state-chunk matching + deadman self-test\n");

    juce::ScopedJuceInitialiser_GUI juceInit;

    // ---- the disk guard, before anything else touches ChainHost ------------
    {
        const char* home = std::getenv ("EJ_STATE_TEST_HOME");
        const auto appData = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                 .getFullPathName();
        if (home == nullptr || ! appData.startsWith (juce::String (home)))
        {
            std::printf ("REFUSING TO RUN: app-data resolves to %s, which is not under "
                         "EJ_STATE_TEST_HOME (%s). Run via build_and_run.sh, which "
                         "sandboxes HOME.\n", appData.toRawUTF8(), home ? home : "(unset)");
            return 2;
        }
        std::printf ("  disk sandboxed under %s\n", appData.toRawUTF8());
    }

    const int  savedValue = 4242;
    const auto chunk      = chunkFor (savedValue);

    // ================================================================
    std::printf ("== the matching policy, through restoreSavedChain ==\n");
    {
        ChainHost host;

        // 1. Format differs -> withheld, one note naming both formats, and the
        //    probe's setStateInformation is never called.
        {
            auto o = restoreOne (host, savedSlot (kProbeName, "VST3", "1.0.0", kProbeUidStr),
                                 stateFor (chunk), savedValue);
            check (o.loaded,       "format mismatch: the plugin still loads");
            check (! o.applied,    "format mismatch: settings NOT applied");
            check (o.calls == 0,   "format mismatch: setStateInformation was not called",
                   juce::String (o.calls));
            check (o.notes.size() == 1, "format mismatch: exactly one note", join (o.notes));
            check (anyNoteContains (o.notes, "saved as VST3")
                   && anyNoteContains (o.notes, "loaded here as " + kBuiltinFormat)
                   && anyNoteContains (o.notes, "not applied"),
                   "format mismatch: the note names both formats", join (o.notes));
            std::printf ("  ok    1. format differs -> withheld, named\n");
        }

        // 2. uid differs, format same -> withheld, named.
        {
            auto o = restoreOne (host, savedSlot (kProbeName, kBuiltinFormat, "1.0.0", "12345"),
                                 stateFor (chunk), savedValue);
            check (o.loaded && ! o.applied && o.calls == 0, "uid mismatch: withheld",
                   "calls=" + juce::String (o.calls));
            check (o.notes.size() == 1 && anyNoteContains (o.notes, "different build"),
                   "uid mismatch: one note naming a different build", join (o.notes));
            std::printf ("  ok    2. uid differs -> withheld, named\n");
        }

        // 3. Version differs, format and uid same -> APPLIED, and a note exists.
        {
            auto o = restoreOne (host, savedSlot (kProbeName, kBuiltinFormat, "0.9.0", kProbeUidStr),
                                 stateFor (chunk), savedValue);
            check (o.loaded && o.applied && o.calls == 1, "version mismatch: applied",
                   "calls=" + juce::String (o.calls));
            check (o.notes.size() == 1
                   && anyNoteContains (o.notes, "saved from version 0.9.0")
                   && anyNoteContains (o.notes, "this machine has 1.0.0"),
                   "version mismatch: one note naming both versions", join (o.notes));
            std::printf ("  ok    3. version differs -> applied, said out loud\n");
        }

        // 4. All three match -> applied, no note at all.
        {
            auto o = restoreOne (host, savedSlot (kProbeName, kBuiltinFormat, "1.0.0", kProbeUidStr),
                                 stateFor (chunk), savedValue);
            check (o.loaded && o.applied && o.calls == 1, "all match: applied");
            check (o.notes.isEmpty(), "all match: no note", join (o.notes));
            std::printf ("  ok    4. all three match -> applied, silent\n");
        }

        // 5. All three absent (a chain saved before Session B) -> applied, no
        //    note. The regression that matters most.
        {
            auto o = restoreOne (host, savedSlot (kProbeName, {}, {}, {}),
                                 stateFor (chunk), savedValue);
            check (o.loaded && o.applied && o.calls == 1, "legacy chain: applied");
            check (o.notes.isEmpty(), "legacy chain: no note", join (o.notes));
            std::printf ("  ok    5. all three absent -> applied, silent (legacy)\n");
        }

        // 5b. Format matches, uid and version absent -> applied, silent.
        //     Absent must mean no opinion FIELD BY FIELD, not chain by chain.
        {
            auto o = restoreOne (host, savedSlot (kProbeName, kBuiltinFormat, {}, {}),
                                 stateFor (chunk), savedValue);
            check (o.loaded && o.applied && o.notes.isEmpty(),
                   "format only: applied, silent", join (o.notes));
        }

        // 5c. A mismatch with NO chunk for the slot decides nothing and says
        //     nothing about it: there is no dial to describe. The only note is
        //     applyRestoredState's own "no settings were saved for this slot".
        {
            auto o = restoreOne (host, savedSlot (kProbeName, "VST3", "0.1", "1"),
                                 stateFor ({}), savedValue);
            check (o.loaded && o.calls == 0, "no chunk: nothing pushed");
            check (! anyNoteContains (o.notes, "not applied")
                   && ! anyNoteContains (o.notes, "worth checking"),
                   "no chunk: no format / uid / version note", join (o.notes));
            check (anyNoteContains (o.notes, "no settings were saved"),
                   "no chunk: the existing default-settings note still fires", join (o.notes));
            std::printf ("  ok    5b/5c. absent-field and no-chunk edges\n");
        }

        // ---- the deadman AROUND the call ---------------------------------
        // Witnessed from inside setStateInformation by the probe: the marker
        // is on disk, names this plugin, and says which phase; and it is
        // gone once the call has returned.
        {
            auto o = restoreOne (host, savedSlot (kProbeName, kBuiltinFormat, "1.0.0", kProbeUidStr),
                                 stateFor (chunk), savedValue);
            check (o.calls == 1 && probeLog().markerExisted,
                   "deadman: the marker exists INSIDE setStateInformation");
            // Read the way the consumer reads it (StringArray::fromLines, then
            // trim): File::replaceWithText writes "\r\n" line endings by default,
            // and the consumer's tolerance of that is part of what is under test.
            const auto lines = juce::StringArray::fromLines (probeLog().markerText);
            check (lines.size() == 2
                   && lines[0].trim() == kProbeIdentifier
                   && lines[1].trim() == "state restore",
                   "deadman: identifier then phase", join (lines));
            check (! TA::deadmanFile().existsAsFile(),
                   "deadman: the marker is gone after the call returns");
            std::printf ("  ok    A2. marker present inside the call, gone after\n");
        }

        // A chunk the plugin REJECTS (throws): the catch still runs, the note
        // says so, the marker is still cleaned up, and the value is untouched.
        {
            auto o = restoreOne (host, savedSlot (kProbeName, kBuiltinFormat, "1.0.0", kProbeUidStr),
                                 stateFor (chunkFor (kThrowValue)), kThrowValue);
            check (o.loaded && o.calls == 1 && ! o.applied, "rejected chunk: attempted, not applied");
            check (anyNoteContains (o.notes, "rejected its saved settings"),
                   "rejected chunk: named", join (o.notes));
            check (! TA::deadmanFile().existsAsFile(), "rejected chunk: marker cleaned up");
            std::printf ("  ok    A2. a thrown rejection is noted and leaves no marker\n");
        }

        clearRack (host);
    }

    // ================================================================
    std::printf ("== the deadman consumer, at construction ==\n");
    const juce::String crashedPath = "/Library/Audio/Plug-Ins/VST3/EJ Deadman Test.vst3";
    {
        // 6. Two lines, phase "state restore".
        TA::deadmanFile().getParentDirectory().createDirectory();
        TA::deadmanFile().replaceWithText (crashedPath + "\nstate restore");
        ChainHost host;
        check (TA::isBlacklisted (host, crashedPath), "state-restore marker: blacklisted");
        const auto reason = TA::blacklistReason (host, crashedPath);
        check (reason.startsWith ("crashed the host restoring its saved settings (deadman)"),
               "state-restore marker: reason names the phase", reason);
        check (! TA::deadmanFile().existsAsFile(), "state-restore marker: file consumed");
        std::printf ("  ok    6. two-line marker -> blacklisted with the state-restore reason\n");
    }
    // A fresh path for the next case: the FIRST reason recorded for a path
    // wins (addToBlacklist), and the blacklist file persists between hosts.
    const juce::String crashedPath2 = "/Library/Audio/Plug-Ins/VST3/EJ Deadman Old Format.vst3";
    {
        // 7. One line, the format every build before this one wrote.
        TA::deadmanFile().replaceWithText (crashedPath2);
        ChainHost host;
        check (TA::isBlacklisted (host, crashedPath2), "one-line marker: blacklisted");
        const auto reason = TA::blacklistReason (host, crashedPath2);
        check (reason.startsWith ("crashed the host during load (deadman)"),
               "one-line marker: absent phase reads as load", reason);
        check (! TA::deadmanFile().existsAsFile(), "one-line marker: file consumed");
        std::printf ("  ok    7. one-line marker -> blacklisted with the load reason\n");
    }
    {
        // 7b. Two lines with an explicit "load" phase (what the thin-VST3
        //     writer now emits) reads exactly as the one-line file did.
        const juce::String p3 = "/Library/Audio/Plug-Ins/VST3/EJ Deadman Load Phase.vst3";
        TA::deadmanFile().replaceWithText (p3 + "\nload");
        ChainHost host;
        check (TA::isBlacklisted (host, p3)
               && TA::blacklistReason (host, p3).startsWith ("crashed the host during load (deadman)"),
               "explicit load phase: the load reason", TA::blacklistReason (host, p3));
        std::printf ("  ok    7b. explicit load phase -> the load reason\n");
    }
    {
        // 7c. An empty marker blacklists nothing and is still removed.
        TA::deadmanFile().replaceWithText ("\n\n");
        ChainHost host;
        check (! TA::isBlacklisted (host, juce::String()), "empty marker: nothing blacklisted");
        check (! TA::deadmanFile().existsAsFile(), "empty marker: file consumed");
    }

    // ================================================================
    std::printf ("== found-side absence: a thin VST3 row has no opinion ==\n");
    // A thin scan row carries uid 0 and no version until its first load
    // validates it. A chain saved from the validated slot carries the real
    // uid; comparing that against 0 would withhold a chain from its own
    // plugin. The decision happens synchronously inside restoreSavedChain,
    // so it is checked right after the call; the load itself then fails
    // (there is no such bundle) on the message loop, which is pumped until
    // that lands so the host outlives its own timer.
    {
        ChainHost host;
        juce::PluginDescription thin;
        thin.name             = "EJ Thin Probe";
        thin.pluginFormatName = "VST3";
        thin.fileOrIdentifier = "/nonexistent/EchoJayStateMatchTest/EJ Thin Probe.vst3";
        thin.category         = "Effect";
        TA::addEntry (host, thin);

        host.restoreSavedChain (savedSlot ("EJ Thin Probe", "VST3", "3.1.0", "777"),
                                stateFor (chunk));
        const auto notesNow = host.getStateNotes();
        check (! anyNoteContains (notesNow, "different build"),
               "thin row: a real saved uid against uid 0 is not a mismatch", join (notesNow));
        check (! anyNoteContains (notesNow, "saved from version"),
               "thin row: a saved version against no version is not a mismatch", join (notesNow));

        // Let the validation fail and settle before the host goes away.
        bool settled = false;
        for (int i = 0; i < 100 && ! settled; ++i)
        {
            // Headless pump: JUCE mac messages and timers ride the main
            // CFRunLoop (same as tools/bridged_readback_test).
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.05, false);
            settled = anyNoteContains (host.getStateNotes(), "could not load");
        }
        check (settled, "thin row: the missing bundle fails to load and says so",
               join (host.getStateNotes()));
        check (! TA::deadmanFile().existsAsFile(), "thin row: validation marker cleaned up");
        std::printf ("  ok    9. thin VST3 row -> no false mismatch\n");
    }

    // ================================================================
    std::printf ("== negative control ==\n");
    {
        const int before = failures;
        check (2 + 2 == 5, "NEGATIVE CONTROL - this line is SUPPOSED to fail");
        const bool caught = (failures == before + 1);
        failures = before;   // the control is not a defect in the code under test
        check (caught, "the harness reports a false assertion");
        std::printf (caught ? "  ok    8. the harness caught the planted failure\n"
                            : "  FAIL  8. the harness DID NOT catch the planted failure\n");
    }

    if (failures)
    {
        std::printf ("state_match_test: %d FAILED\n", failures);
        return 1;
    }
    std::printf ("state_match_test: PASS\n");
    return 0;
}
