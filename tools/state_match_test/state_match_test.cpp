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
#include <unistd.h>   // getpid — the per-pid death-mark file name
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
    /** The identity a past load captured (what loadPluginAsync consults before
        validating a thin row). */
    static void addKnown (ChainHost& h, const juce::PluginDescription& d)
    {
        std::lock_guard<std::mutex> lock (h.pluginsMutex_);
        h.knownPlugins_.addType (d);
    }
    static void addFormat (ChainHost& h, std::unique_ptr<juce::AudioPluginFormat> f)
    {
        h.formatManager_.addFormat (std::move (f));
    }
    static juce::PluginDescription slotDesc (const ChainHost& h, int i)
    {
        return h.slots_[(size_t) i].desc;
    }
};

namespace
{
using TA = EchoJayStateMatchTestAccess;

/** An AudioPluginInstance (not a bare AudioProcessor) so the SAME class can
    reach ChainHost two ways: as the built-in device the registry creates, and
    as the instance ProbeFormat below hands to createPluginInstanceAsync — the
    route a validated thin VST3 takes through completeLoad. */
class ProbeProcessor final : public juce::AudioPluginInstance
{
public:
    explicit ProbeProcessor (juce::PluginDescription d = {})
        : juce::AudioPluginInstance (BusesProperties()
              .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
              .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          desc_ (std::move (d)) {}

    int value = 0;   // the one thing the chunk carries

    void fillInPluginDescription (juce::PluginDescription& d) const override { d = desc_; }
    const juce::String getName() const override
    {
        return desc_.name.isNotEmpty() ? desc_.name : juce::String (kProbeName);
    }
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
        // Observed from INSIDE the call: this is the window the death mark
        // exists to cover, and the only place its presence can be witnessed.
        // Since the trunk merge (21 Aug 2026) the mark is a per-pid file of
        // tab-separated "phase\tpath\tname" lines, not the old single-path
        // deadman file.
        const auto marker = TA::deadmanFile().getSiblingFile (
            "chain_load_deadman." + juce::String ((int) getpid()) + ".txt");
        log.markerExisted = marker.existsAsFile();
        log.markerText    = marker.loadFileAsString();

        int v = 0;
        if (size >= (int) sizeof (int)) std::memcpy (&v, data, sizeof (int));
        if (v == kThrowValue) throw std::runtime_error ("probe: chunk refused");
        value = v;
    }

private:
    juce::PluginDescription desc_;
};

// ---------------------------------------------------------------------------
// The thin-VST3 stand-in: a plugin FORMAT that answers to the name "VST3" for
// exactly one path. It is added to the host's format manager AFTER the real
// VST3 format, and the real one declines the path (fileMightContainThisPluginType
// requires an existing .vst3), so findFormatForDescription lands here. What
// ChainHost then does — createPluginInstanceAsync -> completeLoad (inst,
// fullDesc) -> slot.desc = fullDesc — is the same convergence point the
// validation thread reaches through pollVST3Validation, with the entries_ row
// still THIN (uid 0, no version) at resolve time. That is the shape under test.
// ---------------------------------------------------------------------------
constexpr const char* kThinName    = "EJ Thin Probe";
constexpr const char* kThinPath    = "/nonexistent/EchoJayStateMatchTest/EJ Thin Probe.vst3";
constexpr int         kThinUid     = 0x54484E50;   // 'THNP', the uid the load "discovers"
// A UNIQUE format name. JUCE's manager refuses a second format called "VST3"
// (jassertfalse in addFormat), and its real VST3 format cannot instantiate a
// fake plugin. So the probe format has its own name and the enriched
// description (from knownPlugins_) carries it, which is what routes
// createPluginInstanceAsync here. See the section body for how the thin row
// still advertises "VST3" so loadPluginAsync's enrichment gate fires.
constexpr const char* kProbeFormat = "PVST3";

class ProbeFormat final : public juce::AudioPluginFormat
{
public:
    juce::String getName() const override { return kProbeFormat; }
    void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>&, const juce::String&) override {}
    bool fileMightContainThisPluginType (const juce::String& f) override { return f == kThinPath; }
    juce::String getNameOfPluginFromIdentifier (const juce::String& f) override { return f; }
    bool pluginNeedsRescanning (const juce::PluginDescription&) override { return false; }
    bool doesPluginStillExist (const juce::PluginDescription&) override { return true; }
    bool canScanForPlugins() const override { return false; }
    bool isTrivialToScan() const override { return true; }
    juce::StringArray searchPathsForPlugins (const juce::FileSearchPath&, bool, bool) override { return {}; }
    juce::FileSearchPath getDefaultLocationsToSearch() override { return {}; }
    bool requiresUnblockedMessageThreadDuringCreation (const juce::PluginDescription&) const override { return false; }
    void createPluginInstance (const juce::PluginDescription& d, double, int,
                               PluginCreationCallback cb) override
    {
        cb (std::make_unique<ProbeProcessor> (d), {});
    }
};

/** The row the thin scan writes: name, format "VST3", path — nothing else.
    Its format is "VST3" so loadPluginAsync takes the needs-validation branch
    (version empty + format VST3), which is where the thin->validated
    enrichment from knownPlugins_ happens; its uniqueId is 0, so the
    resolve-time check in restoreSavedChain has no opinion, exactly as a real
    thin VST3 row before its first load. */
juce::PluginDescription thinRow()
{
    juce::PluginDescription d;
    d.name             = kThinName;
    d.pluginFormatName = "VST3";
    d.fileOrIdentifier = kThinPath;
    d.category         = "Effect";
    return d;
}

/** What a past load captured for that bundle, sitting in knownPlugins_: the
    real uid and version. Its format is the PROBE format, not "VST3", because
    the enriched description is what createPluginInstanceAsync routes on, and
    JUCE's real VST3 format cannot make a stand-in. The uid — the thing under
    test — is the validated one either way. */
juce::PluginDescription validatedRow (int uid = kThinUid, const juce::String& version = "2.0.0")
{
    juce::PluginDescription d;
    d.name             = kThinName;
    d.pluginFormatName = kProbeFormat;
    d.fileOrIdentifier = kThinPath;
    d.category         = "Effect";
    d.manufacturerName = "EJ Test";
    d.version          = version;
    d.uniqueId         = uid;
    d.deprecatedUid    = uid;
    return d;
}

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

/** Headless pump: JUCE mac messages and timers ride the main CFRunLoop
    (same as tools/bridged_readback_test). Runs until pred() or ~5 s. */
template <typename Pred>
bool pumpUntil (Pred pred)
{
    for (int i = 0; i < 100; ++i)
    {
        if (pred()) return true;
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.05, false);
    }
    return pred();
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
    int              loadedUid = 0;     // slots_[0].desc.uniqueId after the load
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
    std::setvbuf (stdout, nullptr, _IONBF, 0);   // a crash must not eat the lines before it
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
            // The mark line is "phase\tpath\tname" (see writeDeathMarksLocked);
            // parse it the way consumeDeathMarks does — tokenise on tab, trim.
            juce::StringArray lines;
            for (const auto& l : juce::StringArray::fromLines (probeLog().markerText))
                if (l.trim().isNotEmpty()) lines.add (l);
            juce::StringArray cols;
            if (lines.size() == 1) cols.addTokens (lines[0], "\t", "");
            check (lines.size() == 1 && cols.size() >= 3
                   && cols[0].trim() == "state restore"
                   && cols[1].trim() == kProbeIdentifier
                   && cols[2].trim() == kProbeName,
                   "deadman: phase, identifier, name on one mark line", join (lines));
            check (! TA::deadmanFile().getSiblingFile (
                        "chain_load_deadman." + juce::String ((int) getpid()) + ".txt")
                        .existsAsFile(),
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
            check (! TA::deadmanFile().getSiblingFile (
                        "chain_load_deadman." + juce::String ((int) getpid()) + ".txt")
                        .existsAsFile(),
                   "rejected chunk: marker cleaned up");
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
        const bool settled = pumpUntil ([&] {
            return anyNoteContains (host.getStateNotes(), "could not load"); });
        check (settled, "thin row: the missing bundle fails to load and says so",
               join (host.getStateNotes()));
        check (! TA::deadmanFile().getSiblingFile (
                    "chain_load_deadman." + juce::String ((int) getpid()) + ".txt")
                    .existsAsFile(),
               "thin row: validation marker cleaned up");
        std::printf ("  ok    9. thin VST3 row -> no false mismatch\n");
    }

    // ================================================================
    std::printf ("== the version note never survives a failed apply ==\n");
    // §2: the "settings were applied, worth checking" line is written only
    // AFTER the chunk applies. Force each failure path with a version that
    // differs from the built-in probe's (1.0.0) and assert the line is absent.
    {
        ChainHost host;
        const juce::String v = kBuiltinFormat;   // format matches, so version is the live field

        // (a) base64 that does not decode -> "could not be read", no version note.
        {
            auto o = restoreOne (host, savedSlot (kProbeName, v, "0.9.0", kProbeUidStr),
                                 stateFor ("!!! not base64 !!!"), savedValue);
            check (anyNoteContains (o.notes, "could not be read"), "decode-fail: the decode note fires",
                   join (o.notes));
            check (! anyNoteContains (o.notes, "worth checking"),
                   "decode-fail: NO version note", join (o.notes));
            check (o.calls == 0, "decode-fail: setStateInformation not called");
        }

        // (b) the plugin throws on the chunk -> "rejected", no version note.
        {
            auto o = restoreOne (host, savedSlot (kProbeName, v, "0.9.0", kProbeUidStr),
                                 stateFor (chunkFor (kThrowValue)), kThrowValue);
            check (anyNoteContains (o.notes, "rejected its saved settings"),
                   "throw: the rejection note fires", join (o.notes));
            check (! anyNoteContains (o.notes, "worth checking"),
                   "throw: NO version note (apply did not succeed)", join (o.notes));
        }

        // (c) the happy path with a version difference DOES get the note, so
        //     (a) and (b) are proving absence, not a note that never fires.
        {
            auto o = restoreOne (host, savedSlot (kProbeName, v, "0.9.0", kProbeUidStr),
                                 stateFor (chunk), savedValue);
            check (o.applied && anyNoteContains (o.notes, "worth checking"),
                   "control: a SUCCESSFUL version-differ still gets the note", join (o.notes));
        }

        clearRack (host);
        std::printf ("  ok    version note is written iff the chunk applied\n");
    }

    // ================================================================
    std::printf ("== thin VST3: the identity is known only AFTER the load ==\n");
    // The shape CHAINHOST_FOLLOWUP_BRIEF §1 names as unreachable before the
    // apply-time re-check:
    //   - entries_ holds the THIN row (format VST3, uid 0, no version), so
    //     resolveByName hands restoreSavedChain a description its check has no
    //     opinion on — foundUidKnown is false.
    //   - knownPlugins_ holds what the bundle validated as, with the real uid.
    //     loadPluginAsync's needs-validation branch enriches the thin row from
    //     it, so completeLoad runs with the validated description and
    //     slots_[idx].desc carries the REAL uid.
    // So the only place the saved uid can be compared against the truth is
    // applyRestoredState, after the load. The saved slot leaves FORMAT empty
    // on purpose: the probe's loaded format is a test-only name, and an empty
    // saved format is "no opinion" on both sides, which isolates the uid as
    // the single discriminator — the field this case is about.
    {
        auto restoreThin = [&] (int validatedUid, const juce::String& savedUid,
                                const juce::String& b64, int savedValue) -> Outcome
        {
            // A fresh host per case: knownPlugins_/entries_ and the registered
            // format are set up once here, so each case starts from the same
            // thin state.
            ChainHost host;
            TA::addFormat (host, std::make_unique<ProbeFormat>());
            TA::addEntry  (host, thinRow());
            TA::addKnown  (host, validatedRow (validatedUid));
            probeLog().reset();
            // format empty, version empty: uid is the only field with an opinion.
            host.restoreSavedChain (savedSlot (kThinName, {}, {}, savedUid),
                                    stateFor (b64));
            // createPluginInstanceAsync posts to the message thread.
            pumpUntil ([&] { return host.getNumSlots() > 0
                                 || anyNoteContains (host.getStateNotes(), "could not load"); });
            Outcome o;
            o.notes = host.getStateNotes();
            o.calls = probeLog().setStateCalls;
            if (auto* p = probeInSlot0 (host)) { o.loaded = true; o.applied = (p->value == savedValue); }
            // Record what the load actually put on the slot, for the premise check.
            o.loadedUid = host.getNumSlots() > 0 ? TA::slotDesc (host, 0).uniqueId : 0;
            clearRack (host);
            return o;
        };

        // The premise, asserted rather than assumed: the slot loaded through
        // the enrichment route and slots_[0].desc carries the VALIDATED uid,
        // not the thin 0. If this ever stops holding the two cases below prove
        // nothing, so it is checked first.
        {
            auto o = restoreThin (kThinUid, juce::String (kThinUid), chunk, savedValue);
            check (o.loaded, "thin premise: the stand-in loads through the enrichment route",
                   join (o.notes));
            check (o.loadedUid == kThinUid,
                   "thin premise: slots_[0].desc carries the validated uid, not 0",
                   juce::String (o.loadedUid));
        }

        // 10. Validated uid DIFFERS from the saved one -> withheld, one note,
        //     setStateInformation never called. This FAILS against the code as
        //     it stands (no apply-time re-check: the chunk is pushed into a
        //     different build) and PASSES once §1 lands.
        {
            auto o = restoreThin (kThinUid, juce::String (kThinUid + 1), chunk, savedValue);
            check (o.loaded, "thin uid differs: still loads");
            const bool withheld = (o.calls == 0 && ! o.applied);
            check (o.calls == 0, "thin uid differs: setStateInformation was NOT called",
                   "calls=" + juce::String (o.calls));
            check (! o.applied, "thin uid differs: settings NOT applied");
            check (o.notes.size() == 1 && anyNoteContains (o.notes, "different build"),
                   "thin uid differs: exactly one note, naming a different build", join (o.notes));
            std::printf ("  %s  10. thin VST3, validated uid differs -> %s\n",
                         withheld ? "ok  " : "FAIL",
                         withheld ? "withheld (the §1 fix is in)"
                                  : "PUSHED — this is the §1 gap, expected to fail pre-fix");
        }

        // 11. Validated uid MATCHES -> applied, silent. Guards against the
        //     re-check over-correcting into the false withhold that the
        //     found-side-absence rule (test 9) exists to prevent.
        {
            auto o = restoreThin (kThinUid, juce::String (kThinUid), chunk, savedValue);
            check (o.loaded && o.applied && o.calls == 1, "thin uid matches: applied",
                   "calls=" + juce::String (o.calls));
            check (o.notes.isEmpty(), "thin uid matches: no note", join (o.notes));
            std::printf ("  ok    11. thin VST3, validated uid matches -> applied, silent\n");
        }
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
