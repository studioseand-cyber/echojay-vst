/*
  EjmapLedger.h

  Persistent record of every plugin the tool has touched, plus the crash
  quarantine protocol.

  In-process hosting means a plugin crash takes the whole app down. There is no
  worker to catch it, because the human has to see the editor. So the protocol is
  a file on disk written before instantiation:

      beginLoad(id)   -> writes inflight.json
      endLoad(id, ok) -> deletes inflight.json, appends a ledger record

  If inflight.json exists at launch, the previous run died inside that plugin.
  Mark it crash_on_load, quarantine it, continue.

  Everything writes through immediately. Nothing is held until submit. A session
  that dies mid-plugin loses at most the current row.

  RUN IDENTITY. The ledger is append-only across every run the tool has ever
  made, so "how did this scan go" has no answer without one. Measured on the
  first two real scans: 1419 rows for 861 bundles, 558 of them duplicated
  between run 1 and run 2, and getOutcomeCounts summed both into a single
  meaningless tally. Every row now carries the run that produced it.

  THREAD SAFETY. The watchdog writes the expiry row from its own thread, while
  the message thread is stuck inside plugin code. Every mutation of the ledger,
  the quarantine and inflight takes the lock, and every write is flushed before
  it returns: a buffered row still in memory when _exit fires records nothing.
*/

#pragma once

#include <juce_core/juce_core.h>
#include "EjmapSchema.h"

namespace ejmap
{

/** Rows written before run identity existed. They are not attributed to the
    current run, because they did not happen in it.
*/
inline constexpr const char* kLegacyRunId = "legacy";

/** Pass to getOutcomeCounts to count across every run on disk. Deliberately
    explicit: summing runs is a thing you should have to ask for by name.
*/
inline constexpr const char* kAllRuns = "*";

struct LedgerRecord
{
    /** The format's own identifier string ("AudioUnit:Effects/aufx,SILM,ksWV",
        or a VST3 bundle path). See ScannedPlugin::pluginId: the XOR-derived
        uniqueId this used to be collides, measured, twice across four Waves
        components. Stable across versions; version is metadata below.
    */
    juce::String pluginId;
    juce::String name, vendor, format, version;
    LoadOutcome  outcome = LoadOutcome::timeout;
    juce::String detail;            // error text, or why it was quarantined
    juce::String at;                // ISO 8601
    int paramCount = 0;

    /** "load" or "scan". Both stages write ledger rows, because both can take
        the process down and both must be attributable. They are NOT the same
        event: a scan probe only asks the format what is in a file, a load
        instantiates and opens an editor.

        M1's outcome summary counts LOAD rows. Counting both would report one
        row per VST3 bundle on the machine as if a human had attempted it.
    */
    juce::String stage = "load";

    /** The run that produced this row. Filled in by the Ledger; a caller does
        not get to choose it, except for a crash row, which is attributed to the
        run that DIED rather than the run that found the wreckage.
    */
    juce::String runId;

    /** Set only on a crash or watchdog row recovered by a later run. Without it
        the ledger cannot say that run 2 wrote a row about run 1.
    */
    juce::String recoveredByRunId;

    juce::var toVar() const
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("run_id", runId);
        o->setProperty ("plugin_id", pluginId);
        o->setProperty ("stage", stage);
        o->setProperty ("name", name);
        o->setProperty ("vendor", vendor);
        o->setProperty ("format", format);
        o->setProperty ("version", version);
        o->setProperty ("outcome", toString (outcome));
        o->setProperty ("detail", detail);
        o->setProperty ("at", at);
        o->setProperty ("param_count", paramCount);
        if (recoveredByRunId.isNotEmpty())
            o->setProperty ("recovered_by_run", recoveredByRunId);
        return juce::var (o);
    }
};

//==============================================================================
/** One run's summary, read back off disk. */
struct RunSummary
{
    juce::String runId;
    juce::String startedAt;     // "at" of the first row seen for this run
    int rowCount = 0;
};

//==============================================================================
class Ledger
{
public:
    /** rootOverride exists so the watchdog test can prove expiry against a
        throwaway directory instead of the tester's real ledger. Default is the
        real location; nothing in the app passes anything else.
    */
    explicit Ledger (juce::File rootOverride = {})
    {
        root = rootOverride != juce::File()
                 ? rootOverride
                 : juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                       .getChildFile ("ejmap");
        root.createDirectory();
        root.getChildFile ("maps").createDirectory();
        root.getChildFile ("screenshots").createDirectory();

        ledgerFile    = root.getChildFile ("ledger.json");
        inflightFile  = root.getChildFile ("inflight.json");
        quarantineFile= root.getChildFile ("quarantine.json");
        emergencyFile = root.getChildFile ("watchdog-emergency.jsonl");

        runId = makeRunId();

        loadQuarantine();
    }

    juce::File   getRoot() const noexcept       { return root; }
    juce::String currentRunId() const noexcept  { return runId; }

    /** Artifact path scoped to this run. scan-errors.log and scan-census.log
        were replaced on every scan while the ledger appended, so the three
        artifacts describing the same scan disagreed by construction and the
        logs silently answered for whichever run went last.
    */
    juce::File runArtifact (const juce::String& stem, const juce::String& ext) const
    {
        return root.getChildFile (stem + "-" + runId + "." + ext);
    }

    //==========================================================================
    /** Call once at startup, before any scan. Returns the plugin id that killed
        the previous run, or an empty string. Quarantines it as a side effect.
    */
    juce::String recoverFromCrash()
    {
        absorbEmergencyRows();

        const juce::ScopedLock sl (lock);

        if (! inflightFile.existsAsFile())
            return {};

        auto v  = juce::JSON::parse (inflightFile.loadFileAsString());
        auto id = v.getProperty ("plugin_id", "").toString();

        if (id.isNotEmpty())
        {
            LedgerRecord r;
            r.pluginId = id;
            r.name     = v.getProperty ("name", "").toString();
            r.vendor   = v.getProperty ("vendor", "").toString();
            r.format   = v.getProperty ("format", "").toString();
            r.version  = v.getProperty ("version", "").toString();
            r.stage    = v.getProperty ("stage", "load").toString();
            r.outcome  = LoadOutcome::crashOnLoad;

            // Attributed to the run that DIED, not the one reading the wreckage.
            r.runId            = v.getProperty ("run_id", kLegacyRunId).toString();
            r.recoveredByRunId = runId;

            // Same outcome, honest about which site produced it. A VST3 that
            // kills the process while the format is reading its factory did not
            // die "while the editor was opening", and saying so would send the
            // next person looking in the wrong place.
            const auto site = v.getProperty ("site", "").toString();
            r.detail = site.isNotEmpty()
                         ? "process died inside " + site
                         : (r.stage == "scan"
                              ? "process died while the format was reading this file during Scan"
                              : "process died while the editor was opening");
            r.at = nowIso();

            appendLocked (r);
            quarantineLocked (id, "crash_on_load");
        }

        inflightFile.deleteFile();
        return id;
    }

    /** Written before any call that hands control to plugin code: instantiation,
        the VST3 scan probe, editor creation, snapshotting. site names the call,
        so a recovered crash row can say which one it died in.

        Flushed before it returns. The whole point is that it is on disk when the
        process stops existing.
    */
    void beginLoad (const juce::String& pluginId,
                    const juce::String& name,
                    const juce::String& vendor,
                    const juce::String& format,
                    const juce::String& version,
                    const juce::String& stage = "load",
                    const juce::String& site  = {})
    {
        const juce::ScopedLock sl (lock);

        auto* o = new juce::DynamicObject();
        o->setProperty ("run_id", runId);
        o->setProperty ("plugin_id", pluginId);
        o->setProperty ("name", name);
        o->setProperty ("vendor", vendor);
        o->setProperty ("format", format);
        o->setProperty ("version", version);
        o->setProperty ("stage", stage);
        o->setProperty ("site", site);
        o->setProperty ("at", nowIso());

        writeThrough (inflightFile, juce::JSON::toString (juce::var (o), false));
    }

    void endLoad (LedgerRecord r)
    {
        const juce::ScopedLock sl (lock);
        r.at    = nowIso();
        r.runId = runId;
        appendLocked (r);
        inflightFile.deleteFile();
    }

    /** The watchdog's exit path, called from the watchdog thread with the
        message thread wedged inside plugin code, immediately before _Exit.

        BY DESIGN the wedged thread does not hold this lock. Every call site
        releases it before entering plugin code: beginLoad returns before the
        Watchdog::Scope is constructed, isQuarantined returns before the probe,
        and PluginHost never touches the Ledger at all. The re-entrancy guard in
        MainComponent keeps it that way while the editor-ready wait pumps the
        message loop, since runScan is the one message-thread path that does
        take the lock.

        That is an argument, not a guarantee, and an argument is the wrong thing
        to bet the only record of a hang on. So this does not block: it tries
        for a bounded period, and if the lock never comes it writes the row to a
        separate lock-free file instead. A deadlock still produces a row.
    */
    void recordWatchdogExpiry (LedgerRecord r, const juce::String& quarantineReason)
    {
        r.at      = nowIso();
        r.runId   = runId;
        r.outcome = LoadOutcome::timeout;

        const auto giveUpAt = juce::Time::getMillisecondCounter()
                                + (juce::uint32) kWatchdogLockWaitMs;
        bool got = false;
        while (juce::Time::getMillisecondCounter() < giveUpAt)
        {
            if (lock.tryEnter()) { got = true; break; }
            juce::Thread::sleep (10);
        }

        if (got)
        {
            appendLocked (r);
            if (r.pluginId.isNotEmpty())
                quarantineLocked (r.pluginId, quarantineReason);
            inflightFile.deleteFile();
            lock.exit();
            return;
        }

        // Lock never came. Something holds it and is not going to let go, which
        // is exactly when losing the row would be least forgivable. Append to a
        // file nothing else writes, with no lock and no shared state. Absorbed
        // into ledger.json on the next launch.
        r.detail << " [ledger lock unavailable after " << kWatchdogLockWaitMs
                 << "ms; row written to " << emergencyFile.getFileName() << "]";

        juce::FileOutputStream out (emergencyFile);
        if (out.openedOk())
        {
            out.setPosition (emergencyFile.getSize());
            out.writeText (juce::JSON::toString (r.toVar(), true) + "\n", false, false, nullptr);
            out.flush();
        }

        // Clear inflight here too. Deleting a file needs no lock, and leaving it
        // makes the next launch record a SECOND row (crash_on_load) for the same
        // event, which would double-count one hang in the outcome summary.
        inflightFile.deleteFile();
    }

    //==========================================================================
    bool isQuarantined (const juce::String& pluginId) const
    {
        const juce::ScopedLock sl (lock);
        return quarantined.contains (pluginId);
    }

    void quarantine (const juce::String& pluginId, const juce::String& reason)
    {
        const juce::ScopedLock sl (lock);
        quarantineLocked (pluginId, reason);
    }

    /** Deliberately manual. A plugin that crashed once usually crashes again,
        and silently retrying it turns one lost session into a loop.
    */
    void releaseFromQuarantine (const juce::String& pluginId)
    {
        const juce::ScopedLock sl (lock);
        quarantined.removeString (pluginId);
        quarantineReasons.remove (pluginId);
        saveQuarantineLocked();
    }

    juce::StringArray getQuarantined() const
    {
        const juce::ScopedLock sl (lock);
        return quarantined;
    }

    //==========================================================================
    /** Counts by outcome, for the gate in M1. Read from the stored file, never
        from an in-memory tally: the rule adopted in this project is that
        reported numbers are asserted against stored data.

        Scoped to ONE run by default, because the ledger spans every run ever
        made and a sum across runs describes nothing that happened. Pass
        kAllRuns to sum deliberately.
    */
    juce::HashMap<juce::String, int>& getOutcomeCounts (const juce::String& stage = "load",
                                                        const juce::String& forRunId = {})
    {
        const auto want = forRunId.isEmpty() ? runId : forRunId;

        counts.clear();
        for (const auto& v : readAll())
        {
            // Rows written before the stage field existed have none; they are
            // load rows, because scan did not write rows then.
            if (v.getProperty ("stage", "load").toString() != stage)
                continue;

            if (want != kAllRuns && rowRunId (v) != want)
                continue;

            auto k = v.getProperty ("outcome", "").toString();
            counts.set (k, counts[k] + 1);
        }
        return counts;
    }

    /** Every run on disk, oldest first, read back out of the file. Makes
        "counts from disk" checkable without trusting any in-memory tally.
    */
    juce::Array<RunSummary> listRuns() const
    {
        juce::Array<RunSummary> out;
        for (const auto& v : readAll())
        {
            const auto id = rowRunId (v);
            bool found = false;
            for (auto& s : out)
                if (s.runId == id) { ++s.rowCount; found = true; break; }

            if (! found)
            {
                RunSummary s;
                s.runId     = id;
                s.startedAt = v.getProperty ("at", "").toString();
                s.rowCount  = 1;
                out.add (s);
            }
        }
        return out;
    }

    juce::Array<juce::var> readAll() const
    {
        const juce::ScopedLock sl (lock);

        juce::Array<juce::var> out;
        if (! ledgerFile.existsAsFile())
            return out;

        // One JSON object per line. Append-only, survives a mid-write crash
        // losing at most the final line.
        juce::StringArray lines;
        lines.addLines (ledgerFile.loadFileAsString());
        for (const auto& line : lines)
        {
            if (line.trim().isEmpty()) continue;
            auto v = juce::JSON::parse (line);
            if (v.isObject()) out.add (v);
        }
        return out;
    }

    /** How long the watchdog waits for the ledger lock before writing to the
        emergency file instead. Short: the process is about to stop existing and
        a row somewhere beats a row nowhere.
    */
    static constexpr int kWatchdogLockWaitMs = 2000;

    /** TEST ONLY. Holds the lock for a fixed period so the watchdog's emergency
        path can be OBSERVED rather than assumed. An untested fallback is how
        every silent drop in this project started.
    */
    void testOnlyHoldLock (int ms)
    {
        const juce::ScopedLock sl (lock);
        juce::Thread::sleep (ms);
    }

private:
    /** Rows the watchdog had to write lock-free get folded into the ledger on
        the next launch, so there is one place to look. Losing them to a second
        file nobody reads would be the silent-drop class wearing a hat.
    */
    void absorbEmergencyRows()
    {
        const juce::ScopedLock sl (lock);

        if (! emergencyFile.existsAsFile())
            return;

        juce::StringArray lines;
        lines.addLines (emergencyFile.loadFileAsString());

        juce::FileOutputStream out (ledgerFile);
        if (! out.openedOk())
            return;

        out.setPosition (ledgerFile.getSize());
        for (const auto& line : lines)
            if (line.trim().isNotEmpty())
                out.writeText (line + "\n", false, false, nullptr);
        out.flush();

        emergencyFile.deleteFile();
    }

    /** A row with no run_id predates run identity. It becomes one synthetic
        legacy run rather than being folded into the current one, which would
        credit this session with work it did not do.
    */
    static juce::String rowRunId (const juce::var& v)
    {
        auto id = v.getProperty ("run_id", "").toString();
        return id.isEmpty() ? juce::String (kLegacyRunId) : id;
    }

    /** Sorts chronologically, safe in a filename, and unique across two runs
        started in the same second. No separator that a path or a
        juce::Identifier would object to.
    */
    static juce::String makeRunId()
    {
        const auto t = juce::Time::getCurrentTime();
        juce::String s;
        s << juce::String (t.getYear())
          << juce::String (t.getMonth() + 1).paddedLeft ('0', 2)
          << juce::String (t.getDayOfMonth()).paddedLeft ('0', 2)
          << "T"
          << juce::String (t.getHours()).paddedLeft ('0', 2)
          << juce::String (t.getMinutes()).paddedLeft ('0', 2)
          << juce::String (t.getSeconds()).paddedLeft ('0', 2)
          << "-"
          << juce::String::toHexString (juce::Random::getSystemRandom().nextInt (0xffff))
                 .paddedLeft ('0', 4);
        return s;
    }

    /** Write and flush. juce::File::appendText leaves the bytes in the stream's
        buffer, which is exactly the wrong place for them when the next thing
        that happens is _exit.
    */
    void appendLocked (const LedgerRecord& r)
    {
        juce::FileOutputStream out (ledgerFile);
        if (out.openedOk())
        {
            out.setPosition (ledgerFile.getSize());
            out.writeText (juce::JSON::toString (r.toVar(), true) + "\n", false, false, nullptr);
            out.flush();
        }
    }

    static void writeThrough (const juce::File& f, const juce::String& text)
    {
        f.deleteFile();
        juce::FileOutputStream out (f);
        if (out.openedOk())
        {
            out.writeText (text, false, false, nullptr);
            out.flush();
        }
    }

    void quarantineLocked (const juce::String& pluginId, const juce::String& reason)
    {
        if (quarantined.contains (pluginId))
            return;

        quarantined.add (pluginId);
        quarantineReasons.set (pluginId, reason);
        saveQuarantineLocked();
    }

    // quarantine.json is an ARRAY of { plugin_id, reason }, not an object keyed
    // by plugin id. A plugin id is now the format's identifier string, and
    // juce::Identifier permits only [A-Za-z0-9_-:#@$%]: the '/' and ',' in
    // "AudioUnit:Effects/aufx,SILM,ksWV" make it an invalid property name. Using
    // it as a key would assert in debug and write a malformed file in release.
    void loadQuarantine()
    {
        const juce::ScopedLock sl (lock);
        quarantined.clear();
        quarantineReasons.clear();
        if (! quarantineFile.existsAsFile())
            return;

        auto v = juce::JSON::parse (quarantineFile.loadFileAsString());
        if (auto* arr = v.getArray())
        {
            for (const auto& entry : *arr)
            {
                auto id = entry.getProperty ("plugin_id", "").toString();
                if (id.isEmpty())
                    continue;

                quarantined.add (id);
                quarantineReasons.set (id, entry.getProperty ("reason", "").toString());
            }
        }
    }

    void saveQuarantineLocked()
    {
        juce::Array<juce::var> entries;
        for (const auto& id : quarantined)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("plugin_id", id);
            o->setProperty ("reason", quarantineReasons[id]);
            entries.add (juce::var (o));
        }
        writeThrough (quarantineFile, juce::JSON::toString (juce::var (entries), true));
    }

    static juce::String nowIso()
    {
        return juce::Time::getCurrentTime().toISO8601 (true);
    }

    juce::File root, ledgerFile, inflightFile, quarantineFile, emergencyFile;
    juce::String runId;
    juce::StringArray quarantined;
    juce::HashMap<juce::String, juce::String> quarantineReasons;
    juce::HashMap<juce::String, int> counts;
    mutable juce::CriticalSection lock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Ledger)
};

} // namespace ejmap
