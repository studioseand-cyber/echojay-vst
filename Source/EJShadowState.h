#pragma once
// =============================================================================================
//  EJShadowState.h - FOLLOW layer 1 (24 Oct 2026): the live copy opens on the REAL plugin's
//                    settings, and a plugin that took the host down doing it is refused next time
// =============================================================================================
//  Sean's ask of 18:18, after the 14:27 crash was proven fixed:
//
//      *"FOLLOW (first): the V2's copy mirrors the real plugin on the Link continuously ... Today
//      the copy keeps its starting settings, so its view and meters drift from the real one."*
//
//  WHAT THE CODE DID. shadowFollowSelection created the copy from a PluginDescription and applied
//  NOTHING to it - no state, no parameters - so the view opened on the plugin's factory defaults,
//  or (since the keeper landed) on whatever the last parked copy of it was left holding. His
//  sentence is literally what the code does, and this layer is the initial sync: the Link is asked
//  for that slot's own settings in the SAME request that asks for the tap, and they are applied to
//  the copy BEFORE its first block and BEFORE its editor exists.
//
//  ---- WHY THE PULL ROAD IS THE RIGHT ONE, AND IS NOT NEW --------------------------------------
//
//  The control channel has served `pullSlotState` since the edit session: the Link calls
//  getStateInformation on that slot's real plugin, refuses a plugin that throws, refuses over
//  LinkShm::kLinkTransferMaxSlotBytes, and hands it back base64 in the ack. That carries EVERYTHING
//  the plugin keeps, parameters and opaque state alike, which is the half of his ask that a
//  parameter mirror can never reach. ONE CAP, NOT A SECOND ONE: this uses the transfer's own
//  number, because two caps on one road is two facts to keep in step.
//
//  ---- BEFORE PREPARE, WHILE DETACHED, AND THAT IS A CRASH FIX, NOT A PREFERENCE ---------------
//
//  ChainHost's own note of 16 Sep 2026, on the pre-image reseed: *"the pre-image reseed rides the
//  reattach so it lands WHILE DETACHED and before the node's prepare - no separate post-prepare
//  setStateInformation on a live node."*  The live copy obeys the same order: suspend off, seed,
//  then prepareToPlay, then the view starts. It is also why layer 1 never meets the question layer
//  3 will have to answer - seeding a copy whose Cocoa view is already attached.
//
//  ---- THE REPEAT-CRASH MARKER (his ruling, 24 Oct: "approved, with the repeat-crash marker") ---
//
//  A crash in the copy is NOT contained by a process boundary and cannot be: the V2 and the Link
//  are two audio units in one AUHostingService process, so a copy that dies takes his session with
//  it. What can be contained is the SECOND one. A marker naming the plugin and the phase is written
//  before the risky call and removed when it returns; a marker file whose pid is no longer running
//  was left by a process that died inside that call, and the next live view refuses THAT PLUGIN by
//  name with a line saying so, until he asks to try again.
//
//  WHAT IT COVERS, AND WHAT IT DELIBERATELY DOES NOT. Covered: seeding the copy with the Link's
//  settings, and building the copy's own interface - two bracketed calls into one named plugin, so
//  the attribution is exact. NOT covered, on the deadman's own reasoning: a death while the copy is
//  RENDERING or while its editor is simply open. Logic kills the AU host on quit without running
//  destructors, so a marker held for the length of a view would fire falsely on every force-quit
//  and refuse a plugin that never misbehaved. A guard that cries wolf is worse than no guard.
//
//  NARROWER THAN THE DEADMAN ON PURPOSE. ChainHost's death marks (17 Aug 2026) blacklist the plugin
//  at feed and at load - the right answer for a plugin that cannot be instantiated at all, and far
//  too much for this, because the Tube-Tech lives in his rack and must keep working there. This
//  refuses the LIVE VIEW of it and nothing else, and it is reversible from the card.
#include <JuceHeader.h>
#include "LinkShm.h"
#include "EJStateRoot.h"
#include <mutex>
#include <vector>
#include <signal.h>
#include <unistd.h>
#include <errno.h>

namespace echojay
{

// ---- THE DECISION: DO WE SEED THE COPY, AND IF NOT, WHAT DOES HE READ? ----------------------
//
// A pure function, so the gate can hold every road without a Link, a plugin or a window - the same
// shape as liveViewInputPolicy. The lines are the ones he sees on the card: a view that is showing
// the wrong settings must SAY it is, because silent drift is the fault he reported this morning.

enum class ShadowSeed { Apply, None };

struct ShadowSeedDecision
{
    ShadowSeed what = ShadowSeed::None;
    juce::String line;              // empty only when `what` is Apply
};

/** `ackHadStateKey` - the Link's ack carried a `state` field at all (an older Link carries neither
    that nor a reason, and that difference is the whole downgrade rule here).
    `bytes` - the decoded size, 0 when nothing decoded.
    `linkWhy` - the Link's OWN reason, in its own words, when it declined. */
inline ShadowSeedDecision shadowSeedDecision (bool ackHadStateKey, int bytes,
                                              const juce::String& linkWhy)
{
    if (linkWhy.isNotEmpty())
        return { ShadowSeed::None, "showing this plugin's defaults - " + linkWhy };

    if (! ackHadStateKey)
        return { ShadowSeed::None, "showing this plugin's defaults - this Link is an older build "
                                   "and cannot send the slot's settings yet" };

    if (bytes <= 0)
        return { ShadowSeed::None, "showing this plugin's defaults - the Link sent no settings for "
                                   "that slot" };

    // THE V2 REFUSES TOO, although the Link already refused before encoding. A gate that only runs
    // on the writer's side is one binary's promise; this one is checked where the bytes are about
    // to be handed to a plugin ("refusal after apply" is the fault family, and the answer is a
    // refusal BEFORE it).
    if (bytes > LinkShm::kLinkTransferMaxSlotBytes)
        return { ShadowSeed::None, "showing this plugin's defaults - its settings are "
                                   + juce::File::descriptionOfSizeInBytes ((juce::int64) bytes)
                                   + ", too large to carry" };

    return { ShadowSeed::Apply, {} };
}

// ---- THE MARKER -------------------------------------------------------------------------------

/** The phase written into the marker, so the refusal can say what we were doing. One string, used
    by the writer and read back by the reader, rather than two spellings of one fact. */
inline const char* kShadowPhaseSeed() noexcept
{ return "reading the Link's settings into the live copy"; }

inline juce::File shadowStateDir()
{
    auto d = echojay::userAppData().getChildFile ("EchoJay");
    d.createDirectory();
    return d;
}
/** Per-PID, exactly as the deadman is, and for the same reason: Logic constructs EchoJay instances
    at any moment, so one instance's live marker must never be consumed by another's start-up. */
inline juce::File shadowMarkerFile (int pid = (int) ::getpid())
{ return shadowStateDir().getChildFile ("shadow_deadman." + juce::String (pid) + ".txt"); }
inline juce::File shadowRefuseFile()
{ return shadowStateDir().getChildFile ("shadow_refuse.txt"); }

/** A plugin's identity for this list: its file path when it has one (the deadman's key, and the one
    thing a rename cannot move), else its name. NEVER the display name alone when a path exists -
    that is the proxy-for-identity family. */
inline juce::String shadowRefuseKey (const juce::PluginDescription& d)
{ return d.fileOrIdentifier.isNotEmpty() ? d.fileOrIdentifier : d.name; }

namespace detail
{
struct ShadowMark { int id; juce::String phase, key, name; };
inline std::mutex& shadowMarkMutex() { static std::mutex m; return m; }
inline std::vector<ShadowMark>& shadowMarks() { static auto* const v = new std::vector<ShadowMark>(); return *v; }

inline void writeShadowMarksLocked()
{
    auto f = shadowMarkerFile();
    if (shadowMarks().empty()) { f.deleteFile(); return; }
    juce::String out;
    for (const auto& m : shadowMarks()) out << m.phase << "\t" << m.key << "\t" << m.name << "\n";
    f.replaceWithText (out);
}
inline bool pidAlive (int pid)
{
    if (pid <= 0) return false;
    if (::kill ((pid_t) pid, 0) == 0) return true;
    return errno == EPERM;                     // it exists, it is simply not ours
}
} // namespace detail

/** Written BEFORE the risky call. Returns the id to hand back to shadowClearRisky, or 0 when there
    is nothing to mark (a built-in, which cannot be a third party's crash). */
inline int shadowMarkRisky (const juce::PluginDescription& d, const juce::String& phase)
{
    const auto key = shadowRefuseKey (d);
    if (key.isEmpty()) return 0;
    std::lock_guard<std::mutex> lk (detail::shadowMarkMutex());
    static int nextId = 1;
    const int id = nextId++;
    detail::shadowMarks().push_back ({ id, phase, key, d.name });
    detail::writeShadowMarksLocked();
    return id;
}

/** Removed when the call RETURNS. Paired with the push and only when this id was really holding
    one, so a double clear cannot drop somebody else's marker. */
inline void shadowClearRisky (int id)
{
    if (id == 0) return;
    std::lock_guard<std::mutex> lk (detail::shadowMarkMutex());
    auto& v = detail::shadowMarks();
    v.erase (std::remove_if (v.begin(), v.end(),
                             [id] (const detail::ShadowMark& m) { return m.id == id; }), v.end());
    detail::writeShadowMarksLocked();
}

/** Turn every marker left by a DEAD process into a refusal row. Returns how many rows it added.
    Our own pid is skipped by definition - those marks are live calls, not deaths. */
inline int shadowConsumeDeadMarkers()
{
    int added = 0;
    auto existing = juce::StringArray::fromLines (shadowRefuseFile().loadFileAsString());
    existing.removeEmptyStrings();

    for (const auto& f : shadowStateDir().findChildFiles (juce::File::findFiles, false,
                                                          "shadow_deadman.*.txt"))
    {
        const int pid = f.getFileName().fromFirstOccurrenceOf ("shadow_deadman.", false, false)
                                       .upToFirstOccurrenceOf (".txt", false, false).getIntValue();
        if (pid == (int) ::getpid()) continue;
        if (detail::pidAlive (pid))      continue;

        for (const auto& line : juce::StringArray::fromLines (f.loadFileAsString()))
        {
            juce::StringArray cols;
            cols.addTokens (line, "\t", "");
            if (cols.size() < 2 || cols[1].trim().isEmpty()) continue;
            const auto phase = cols[0].trim(), key = cols[1].trim();
            const auto name  = cols.size() > 2 ? cols[2].trim() : key;
            const auto row   = key + "\t" + phase + "\t" + name;
            bool already = false;
            for (const auto& e : existing)
                if (e.upToFirstOccurrenceOf ("\t", false, false).trim() == key) { already = true; break; }
            if (already) continue;
            existing.add (row);
            ++added;
        }
        f.deleteFile();
    }
    if (added > 0) shadowRefuseFile().replaceWithText (existing.joinIntoString ("\n") + "\n");
    return added;
}

/** Consumed ONCE per process, before the first live view is considered. Repeating it would cost a
    directory walk per slot click and buy nothing: our own markers are skipped by pid, so the only
    rows it could ever find again are another process's, and that process has to die first. */
inline int shadowConsumeDeadMarkersOnce()
{
    static bool done = false;
    if (done) return 0;
    done = true;
    return shadowConsumeDeadMarkers();
}

/** Is the live view of this plugin refused, and why in his words? */
inline bool shadowRefusedBecause (const juce::PluginDescription& d, juce::String& whyOut)
{
    const auto key = shadowRefuseKey (d);
    if (key.isEmpty()) return false;
    for (const auto& line : juce::StringArray::fromLines (shadowRefuseFile().loadFileAsString()))
    {
        juce::StringArray cols;
        cols.addTokens (line, "\t", "");
        if (cols.size() < 2 || cols[0].trim() != key) continue;
        const auto name = cols.size() > 2 && cols[2].trim().isNotEmpty() ? cols[2].trim() : d.name;
        whyOut = "no live view of " + name + ": the last time one was opened, this plugin took the "
                 "session down while " + cols[1].trim() + ". Open it on the Link instead, or ask to "
                 "try again.";
        return true;
    }
    return false;
}

/** "Try again" - his road back, so a one-off crash is not a permanent refusal. */
inline void shadowAllowAgain (const juce::PluginDescription& d)
{
    const auto key = shadowRefuseKey (d);
    if (key.isEmpty()) return;
    juce::StringArray kept;
    for (const auto& line : juce::StringArray::fromLines (shadowRefuseFile().loadFileAsString()))
    {
        if (line.trim().isEmpty()) continue;
        juce::StringArray cols;
        cols.addTokens (line, "\t", "");
        if (cols.size() >= 1 && cols[0].trim() == key) continue;
        kept.add (line);
    }
    if (kept.isEmpty()) shadowRefuseFile().deleteFile();
    else                shadowRefuseFile().replaceWithText (kept.joinIntoString ("\n") + "\n");
}

} // namespace echojay
