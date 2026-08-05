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
#include "EjmapDiskStamp.h"

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

    /** Name of the thread that faulted, read from the crash report, and who it
        is attributed to: "plugin", "host", or "unknown". Empty when no report
        was found.
    */
    juce::String crashThread, crashTopImage, attribution;

    /** DEATH ROWS ONLY, and a different question from `attribution`.

        `attribution` asks WHO faulted, by reading the stack. `certainty` asks
        the prior question -- whether a fault happened at all:

          "corroborated"  a crash report was found for this death
          "unattributed"  none was, so this may be an operator force-quit of a
                          slow load rather than a plugin fault

        Absence is WEAK evidence: reports can be delayed or disabled. Measured
        on this machine, 26 of 28 death rows found one, so the test does
        discriminate here -- but it is recorded as an observation, not resolved
        into a cause.
    */
    juce::String certainty;

    /** True when the run that died had nobody at the keyboard. Carried into the
        row because the NEXT launch is what counts these, and by then the run
        that could have been force-quit no longer exists to be asked. */
    bool unattended = false;
    juce::String lastClosedId, lastClosedOutcome, lastClosedStage;

    /** Wall-clock milliseconds the load took, -1 when not recorded (every row
        written before 5 Aug 2026). The quarantine row carries the series so an
        operator can see whether the deaths were fast faults or slow ones, since
        a slow load is what gets force-quit.
    */
    int loadMs = -1;

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
        if (crashThread.isNotEmpty())   o->setProperty ("crash_thread", crashThread);
        if (crashTopImage.isNotEmpty()) o->setProperty ("crash_top_image", crashTopImage);
        if (attribution.isNotEmpty()) o->setProperty ("attribution", attribution);
        if (certainty.isNotEmpty())   o->setProperty ("certainty", certainty);
        if (unattended)               o->setProperty ("unattended", true);
        if (loadMs >= 0)              o->setProperty ("load_ms", loadMs);
        if (recoveredByRunId.isNotEmpty())
            o->setProperty ("recovered_by_run", recoveredByRunId);
        return juce::var (o);
    }
};

//==============================================================================
/** How many corroborated deaths at one stage before a plugin is quarantined.

    NOT CHOSEN -- computed from 7,055 ledger records over 917 plugins. Nine
    plugins have both failed a load and succeeded at one, and that is the
    population a single-crash rule loses. The worst observed failure rate among
    them is 33.3% (Solid EQ, 1 of 3), so the probability of quarantining a
    working plugin by bad luck is 33.3% at N=1, 11.1% at N=2, 3.7% at N=3 and
    1.2% at N=4.

    N=3 is the knee. N=4 buys 2.5 points and costs a third more load time on the
    ~350 plugins that always fail. Three of the nine are M9's own signed test
    subjects: today's rule would have quarantined the tool's own fixtures.
*/
inline constexpr int kRetryAttempts = 3;

/** What the ledger knows about one plugin at one stage, read at the moment a
    quarantine decision is made. Nothing here is newly recorded -- every number
    is already on disk -- but nothing read it before, so the decision was made
    blind.

    It changes what the operator does: prior_ok 0 means do not bother, prior_ok
    4 means retry.
*/
struct RetryEvidence
{
    juce::String pluginId, stage;

    int attempts = 0;               // same-stage rows for this BINARY
    int failures = 0;               // deaths among them
    int corroboratedFailures = 0;   // ...with a crash report found
    int unattributedFailures = 0;   // ...without one
    int unattendedFailuresN = 0;    // ...of those, from a run with no operator

    /** Unattributed deaths that no human could have caused. See
        Ledger::setUnattended: the discount exists for the force-quit, and a
        sweep has nobody to do the quitting. */
    int unattendedFailures() const { return unattendedFailuresN; }

    juce::StringArray outcomes;     // in ledger order
    juce::Array<int>  loadMs;       // -1 where the row predates the field

    /** STAGE-SCOPED, and that is the whole point of the field.

        Drawmer 1973 carries eight "ok" rows. Every one is stage "scan" with
        detail "1 description(s)" -- and a SCAN DESCRIBES A PLUGIN WITHOUT
        INSTANTIATING IT. Its AudioUnit has never once loaded successfully.

        An unscoped count would read "8 prior successes", treat a plugin that
        has never loaded as a well-behaved one having a bad roll, and keep
        releasing it from quarantine to re-crash on it nightly forever. A load
        failure is vouched for only by prior LOAD successes.
    */
    int priorOkInLedger = 0;

    /** Of those, the ones in the run being judged. A crash row is recovered by
        the NEXT launch, so "this session" means the session that died, matched
        by run_id -- which is on disk, so it survives the process that had it.
    */
    int priorOkThisSession = 0;

    bool nonDeterministic() const { return priorOkInLedger > 0; }

    /** The note that goes in the quarantine row when prior successes exist. It
        exists to stop an operator reading a quarantine as a verdict.
    */
    juce::String note() const
    {
        if (! nonDeterministic())
            return {};

        juce::String n;
        n << "NON-DETERMINISTIC: this plugin has loaded successfully "
          << priorOkInLedger << " time(s) before at stage " << stage;
        if (priorOkThisSession > 0)
            n << ", " << priorOkThisSession << " of them in this session";
        n << ". A quarantine here is a single bad roll, not a verdict on the plugin.";
        return n;
    }

    juce::var toVar() const
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("stage", stage);
        o->setProperty ("attempts", attempts);
        o->setProperty ("failures", failures);
        o->setProperty ("corroborated_failures", corroboratedFailures);
        o->setProperty ("unattributed_failures", unattributedFailures);
        o->setProperty ("unattended_failures", unattendedFailuresN);
        o->setProperty ("prior_ok_in_ledger", priorOkInLedger);
        o->setProperty ("prior_ok_this_session", priorOkThisSession);

        juce::Array<juce::var> outs, ms;
        for (const auto& x : outcomes) outs.add (x);
        for (auto x : loadMs)          ms.add (x);
        o->setProperty ("outcomes", outs);
        o->setProperty ("load_ms", ms);
        return juce::var (o);
    }
};

//==============================================================================
/** One quarantined plugin, with enough recorded to decide later whether the
    reason still applies.
*/
struct QuarantineEntry
{
    juce::String pluginId, reason;
    juce::String stage;          // "scan" or "load": they release differently
    juce::String at;

    /** Executable stamp at the moment of quarantine. Non-AU only; an AU id is a
        component identifier, not a path.
    */
    DiskStamp stamp;

    /** AU only. The registry's version string, used as the change signal in
        place of a stamp. WEAKER ON PURPOSE and recorded as such: a vendor can
        ship a fix without bumping the version, so this misses some updates. It
        is the only change signal the AudioComponent registry offers without
        mapping a component back to its .component bundle, and that mapping
        already failed for 14 components on this machine.
    */
    juce::String auVersion;

    /** What the ledger said at the moment this was written. Empty on entries
        written before the retry rule, and on hand quarantines.
    */
    RetryEvidence evidence;

    bool isAudioUnit() const { return pluginId.startsWith ("AudioUnit:"); }
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
            r.outcome  = LoadOutcome::diedDuringLoad;

            // Attributed to the run that DIED, not the one reading the wreckage.
            r.runId            = v.getProperty ("run_id", kLegacyRunId).toString();
            r.recoveredByRunId = runId;

            // WHO ACTUALLY FAULTED.
            //
            // The first real crash on this tool was ours: a SIGSEGV on the
            // "ejmap silent pump" thread caused by our own render ordering,
            // while loading soothe2. Quarantining soothe2 for it would have put
            // a false accusation in the permanent record, and quarantine is
            // deliberately manual to undo.
            //
            // The stack is on disk, so read it instead of guessing.
            auto facts = findCrashFacts (v.getProperty ("at", "").toString());
            if (auto* o = testOnlyCrashOverride())
            {
                facts.found       = o->found;
                facts.attribution = o->attribution;
            }
            r.crashThread   = facts.threadName.isNotEmpty() ? facts.threadName
                                                            : juce::String ("(unnamed)");
            r.crashTopImage = facts.topImage;
            r.attribution   = facts.found ? facts.attribution : juce::String ("unknown");
            const bool ours = r.attribution == "host";

            // WHETHER A FAULT HAPPENED AT ALL, which is a prior question to
            // whose fault it was. A SIGKILL and a SIGSEGV leave identical
            // evidence here -- a stake with no completion -- so the row stops
            // asserting a cause and records what was observed instead.
            r.certainty = facts.found ? "corroborated" : "unattributed";

            // Read from the STAKE, not from this process: the run that died is
            // the one that could or could not have had a human at it.
            const bool wasUnattended = (bool) v.getProperty ("unattended", false);
            r.unattended = wasUnattended;

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
            if (facts.found)
                r.detail << " [faulting thread: " << r.crashThread
                         << ", top image: " << (facts.topImage.isEmpty() ? "?" : facts.topImage)
                         << ", attributed to " << r.attribution << "]";

            // Read BEFORE this row is appended, or the row counts as its own
            // predecessor and every first death looks like a repeat.
            auto ev = retryEvidenceForLocked (id, r.stage, r.runId);

            // AN UNATTRIBUTED DEATH DOES NOT COUNT TOWARD THE THREE. One of
            // them may be the operator losing patience with a slow load, and
            // CLA-76 (m) takes 1.6 s against its sibling's 509 ms.
            //
            // The hole this leaves is real and bounded: a machine with crash
            // reporting disabled would never quarantine anything. Measured
            // here, 26 of 28 death rows found a report, so the discount applies
            // to ~7% of them -- and a plugin dying unattributed over and over
            // is NAMED in the row below rather than hidden, so it is visible
            // without being acted on from evidence that cannot support it.
            ev.attempts  += 1;
            ev.failures  += 1;
            ev.outcomes.add (toString (r.outcome));
            ev.loadMs.add (r.loadMs);
            (facts.found ? ev.corroboratedFailures : ev.unattributedFailures) += 1;
            if (! facts.found && wasUnattended) ev.unattendedFailuresN += 1;

            // An unattributed death counts when NOBODY COULD HAVE KILLED IT.
            // The whole basis of the discount is the operator's force-quit, and
            // an unattended run has no operator. Attended runs keep the
            // discount, because there the hazard is real.
            const int counted = ev.corroboratedFailures
                              + (wasUnattended ? ev.unattendedFailures() : 0);
            const bool quarantineNow = counted >= kRetryAttempts;

            r.detail << " [attempt " << counted << " of "
                     << kRetryAttempts << " at stage " << r.stage;
            if (ev.unattributedFailures > 0)
                r.detail << "; " << ev.unattributedFailures
                         << " unattributed"
                         << (wasUnattended ? " but unattended, so counted"
                                           : " death(s) not counted");
            r.detail << (quarantineNow ? ": quarantined]" : ": not quarantined yet]");

            // Kept from the single-crash rule, weakened to a label rather than
            // an exemption. The first real crash on this tool was OURS -- a
            // SIGSEGV on the ejmap silent pump thread while loading soothe2 --
            // and quarantining soothe2 for it would have been a false
            // accusation in a record that is deliberately manual to undo. At
            // N=3 the exemption is no longer needed to prevent that: three
            // separate launches dying is a fact about loading this plugin in
            // this tool whoever's fault it is, and the REASON says which so a
            // release pass can find them.
            if (ours)
                r.detail << " [the faulting thread was ours, not the plugin's]";

            if (ev.nonDeterministic())
                r.detail << " [" << ev.note() << "]";

            r.at = nowIso();
            appendLocked (r);

            if (quarantineNow)
                quarantineLocked (id, ours ? "died_during_load_host_fault"
                                           : "died_during_load",
                                  r.stage, r.version, &ev);
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
        o->setProperty ("unattended", unattended);

        writeThrough (inflightFile, juce::JSON::toString (juce::var (o), false));
    }

    void endLoad (LedgerRecord r)
    {
        const juce::ScopedLock sl (lock);
        r.at    = nowIso();
        r.runId = runId;

        // TIMED FROM THE STAKE, not from a caller's stopwatch. beginLoad is
        // written immediately before control passes to plugin code and this is
        // the return, so the interval is exactly the thing a "was it a slow
        // load?" question is asking about -- and it costs nothing at 20+ call
        // sites, none of which had to be touched to get it.
        //
        // A death has no return, so its own row keeps -1. What the retry rule
        // needs is the SERIES: whether the loads that did complete were slow
        // is what says whether an unattributed death is plausibly a force-quit.
        if (r.loadMs < 0 && inflightFile.existsAsFile())
        {
            const auto stake = juce::JSON::parse (inflightFile.loadFileAsString())
                                   .getProperty ("at", "").toString();
            const auto began = juce::Time::fromISO8601 (stake);
            if (began != juce::Time())
                r.loadMs = (int) (juce::Time::getCurrentTime() - began).inMilliseconds();
        }

        // ONE EVENT, ONE ROW. PluginHost::load plants its own stake and closes
        // it, deliberately, so no caller can forget -- and callers that ALSO
        // close it wrote a second row for the same load. 124 such pairs are on
        // disk. The retry rule counts rows, so a duplicate is not cosmetic.
        //
        // The discriminator is the STAKE: a close arriving when no stake is
        // open, naming the same plugin and the same outcome as the close that
        // just happened, is the second half of one event. A genuine second load
        // plants a new stake first, which clears this.
        const bool stakeOpen = inflightFile.existsAsFile();
        if (! stakeOpen
             && r.pluginId.isNotEmpty()
             && r.pluginId == lastClosedId
             && toString (r.outcome) == lastClosedOutcome
             && r.stage == lastClosedStage)
            return;

        if (stakeOpen)
        {
            lastClosedId      = r.pluginId;
            lastClosedOutcome = toString (r.outcome);
            lastClosedStage   = r.stage;
        }

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
            // THE RETRY IS FOR LOADS ONLY, and the two cases have opposite costs.
            //
            // A wrong LOAD quarantine silently drops a mappable plugin from the
            // queue, and release is manual: the cost lands on one plugin, out of
            // sight. So a first load timeout is recorded and retried, which is
            // what Cymatics Lotus needed (it instantiates in 407 ms and was
            // quarantined by a deadline that was too tight).
            //
            // A SCAN hang blocks everything behind it. Sparing it means the next
            // scan walks into the same bundle, hangs again, and the loop never
            // converges: TDR SlickEQ M and Solid Dynamics each cost a full 4.5
            // minute rescan for exactly this reason. Scan probes quarantine on
            // the first timeout.
            const bool isScanProbe = (r.stage == "scan");
            const int  priors      = countPriorOutcomeLocked (r.pluginId, "timeout");
            const bool quarantineNow = isScanProbe || priors > 0;

            if (! quarantineNow)
                r.detail << " [not quarantined: first LOAD timeout here, it gets one retry]";
            else if (isScanProbe && priors == 0)
                r.detail << " [quarantined on first timeout: a scan hang blocks every "
                            "bundle behind it]";

            appendLocked (r);

            if (r.pluginId.isNotEmpty() && quarantineNow)
                quarantineLocked (r.pluginId, quarantineReason, r.stage, r.version);
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
    /** Everything the ledger knows about this BINARY at this stage.

        Keyed on plugin_id, never on the display name. Drawmer 1973 ships an AU
        and a VST3 that are DIFFERENT BINARIES by the same vendor at the same
        version; the AU has never loaded and the VST3 has no failure history at
        all. Keying on "Drawmer 1973" would quarantine a working VST3 because
        its AU sibling died, and would pool two independent determinism
        populations into one meaningless rate.
    */
    RetryEvidence retryEvidenceFor (const juce::String& pluginId,
                                    const juce::String& stage,
                                    const juce::String& sessionRunId = {}) const
    {
        const juce::ScopedLock sl (lock);
        return retryEvidenceForLocked (pluginId, stage, sessionRunId);
    }

    /** Quarantine entries whose plugin has prior successes at the failing
        stage. These are the nightly re-test population: release on binary
        change alone leaves the nine non-deterministic plugins waiting on a
        change that may never come.
    */
    juce::Array<QuarantineEntry> nonDeterministicQuarantine() const
    {
        const juce::ScopedLock sl (lock);
        juce::Array<QuarantineEntry> out;
        for (const auto& id : quarantined)
            if (entries[id].evidence.nonDeterministic())
                out.add (entries[id]);
        return out;
    }

    /** An unattended run: --sweep, or anything else with nobody at the keyboard.

        THIS IS THE ONLY THING THAT MAKES THE UNATTRIBUTED DISCOUNT SAFE.
        The discount exists for ONE reason -- an operator force-quitting a slow
        load leaves evidence identical to a SIGSEGV -- and in a sweep there is
        no operator. Three force-quits of the same plugin across three separate
        launches, with nobody watching, is not a thing that happens.

        MEASURED, and it is why this exists: three Drawmer 1973 deaths in a live
        supervised sweep all recorded `unattributed`, so the count stayed at
        "attempt 0 of 3" and the campaign re-crashed on it every relaunch until
        the supervisor's ceiling. macOS wrote no crash report for any of them --
        and none for anything since 3 August. Waiting for corroboration that
        never arrives is not a safety property, it is a hang.
    */
    void setUnattended (bool b) { unattended = b; }

    /** TEST ONLY. Substitutes the crash-report lookup, which reads a directory
        this process does not own and cannot seed. Without it the retry rule is
        untestable: every simulated death would find no report, be recorded
        unattributed, and never reach the threshold -- so the test would pass by
        never quarantining anything, which is the failure it is meant to catch.
    */
    struct TestCrashOverride { bool found = false; juce::String attribution = "plugin"; };
    static TestCrashOverride*& testOnlyCrashOverride()
    {
        static TestCrashOverride* p = nullptr;
        return p;
    }

    bool isQuarantined (const juce::String& pluginId) const
    {
        const juce::ScopedLock sl (lock);
        return quarantined.contains (pluginId);
    }

    void quarantine (const juce::String& pluginId, const juce::String& reason,
                     const juce::String& stage = "load",
                     const juce::String& pluginVersion = {})
    {
        const juce::ScopedLock sl (lock);
        quarantineLocked (pluginId, reason, stage, pluginVersion);
    }

    /** Every quarantine entry with its recorded change signal, so a caller can
        decide whether the reason still applies.
    */
    juce::Array<QuarantineEntry> getQuarantineEntries() const
    {
        const juce::ScopedLock sl (lock);
        juce::Array<QuarantineEntry> out;
        for (const auto& id : quarantined)
            out.add (entries[id]);
        return out;
    }

    /** Deliberately manual. A plugin that crashed once usually crashes again,
        and silently retrying it turns one lost session into a loop.
    */
    void releaseFromQuarantine (const juce::String& pluginId)
    {
        const juce::ScopedLock sl (lock);
        quarantined.removeString (pluginId);
        entries.remove (pluginId);
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

    /** What a crash report actually says about who faulted. */
    struct CrashFacts
    {
        juce::String threadName;    // usually empty: plugin worker threads are unnamed
        juce::String topImage;      // image of the top frame
        juce::String attribution;   // "host" | "plugin" | "unknown"
        bool found = false;
    };

    /** ATTRIBUTION KEYS ON FRAMES, NOT ON THE THREAD NAME.

        The name-based version got MNotepad wrong. It crashed in
        libMeldaProductionAudioPluginKernelV11.dylib on one of its own worker
        threads, and that thread has no "name" key at all: of 30 threads in that
        report only 6 were named, all of them ours or Apple's. An anonymous
        thread IS the plugin case, so keying on the name failed exactly where it
        needed to work, fell through to "unknown", and quarantined by the
        conservative fallback rather than by identifying anything.

        The frames say it plainly:

          any frame in the ejmap image        -> host. Our code, or JUCE compiled
                                                 into us, is on the stack. The
                                                 pump SIGSEGV looked like a
                                                 plugin fault at the top frame
                                                 (_platform_memmove) and was ours.
          no ejmap frame, top frame in a
          non-system image                    -> plugin. Third-party code
                                                 faulting with nothing of ours
                                                 below it.
          anything else                       -> unknown, which quarantines.
    */
    static CrashFacts factsFromReport (const juce::File& reportFile)
    {
        CrashFacts f;

        // .ips is a one-line JSON header followed by the JSON body.
        auto text = reportFile.loadFileAsString();
        const auto nl = text.indexOfChar ('\n');
        if (nl < 0)
            return f;

        auto body = juce::JSON::parse (text.substring (nl + 1));
        if (body.getDynamicObject() == nullptr)
            return f;

        const int faulting = (int) body.getProperty ("faultingThread", -1);
        auto* threads = body.getProperty ("threads", juce::var()).getArray();
        auto* images  = body.getProperty ("usedImages", juce::var()).getArray();
        if (threads == nullptr || ! juce::isPositiveAndBelow (faulting, threads->size()))
            return f;

        const auto& thread = (*threads)[faulting];
        f.threadName = thread.getProperty ("name", "").toString();
        f.found = true;

        auto imageAt = [images] (int idx, juce::String& name, juce::String& path)
        {
            name = path = {};
            if (images != nullptr && juce::isPositiveAndBelow (idx, images->size()))
            {
                name = (*images)[idx].getProperty ("name", "").toString();
                path = (*images)[idx].getProperty ("path", "").toString();
            }
        };

        // A system image is Apple's. Anything else that is not us is third party.
        auto isSystem = [] (const juce::String& path)
        {
            return path.startsWith ("/System/") || path.startsWith ("/usr/lib/")
                || path.startsWith ("/usr/libexec/");
        };

        bool sawEjmap = false;
        juce::String topName, topPath;
        bool haveTop = false;

        if (auto* frames = thread.getProperty ("frames", juce::var()).getArray())
        {
            for (const auto& fr : *frames)
            {
                juce::String name, path;
                imageAt ((int) fr.getProperty ("imageIndex", -1), name, path);

                if (! haveTop) { topName = name; topPath = path; haveTop = true; }
                if (name == "ejmap")
                    sawEjmap = true;
            }
        }

        f.topImage = topName;

        if (sawEjmap)                                        f.attribution = "host";
        else if (haveTop && topName.isNotEmpty()
                   && ! isSystem (topPath))                  f.attribution = "plugin";
        else                                                 f.attribution = "unknown";

        return f;
    }

    /** Newest ejmap crash report at or after the inflight timestamp, within a
        10 minute window.

        Deliberately tolerant: a missing or unparseable report yields "unknown",
        which quarantines, i.e. the old behaviour. Being unable to read the stack
        must never be the reason a plugin escapes quarantine.
    */
    static CrashFacts findCrashFacts (const juce::String& inflightAt)
    {
        auto dir = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                       .getChildFile ("Library/Logs/DiagnosticReports");
        if (! dir.isDirectory())
            return {};

        const auto since = juce::Time::fromISO8601 (inflightAt);

        juce::File newest;
        for (const auto& e : juce::RangedDirectoryIterator (dir, false, "ejmap-*.ips"))
        {
            const auto file = e.getFile();
            const auto t    = file.getLastModificationTime();

            if (since != juce::Time() &&
                 (t < since - juce::RelativeTime::seconds (5)
                   || t > since + juce::RelativeTime::minutes (10)))
                continue;

            if (newest == juce::File() || t > newest.getLastModificationTime())
                newest = file;
        }

        if (newest == juce::File())
            return {};

        return factsFromReport (newest);
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

    /** ONE PASS over the ledger for one binary at one stage.

        Both death spellings match: rows written before 5 Aug 2026 say
        "crash_on_load" and are the only determinism evidence this project has.
        A rename that dropped them would reset every retry count to zero and
        hand ~350 always-failing plugins three fresh attempts each.
    */
    RetryEvidence retryEvidenceForLocked (const juce::String& pluginId,
                                          const juce::String& stage,
                                          const juce::String& sessionRunId) const
    {
        RetryEvidence e;
        e.pluginId = pluginId;
        e.stage    = stage;

        if (! ledgerFile.existsAsFile())
            return e;

        juce::StringArray lines;
        lines.addLines (ledgerFile.loadFileAsString());
        for (const auto& line : lines)
        {
            if (line.trim().isEmpty()) continue;
            auto v = juce::JSON::parse (line);

            if (v.getProperty ("plugin_id", "").toString() != pluginId)  continue;
            if (v.getProperty ("stage", "load").toString() != stage)     continue;

            const auto outcome = v.getProperty ("outcome", "").toString();
            ++e.attempts;
            e.outcomes.add (outcome);
            e.loadMs.add (v.hasProperty ("load_ms") ? (int) v.getProperty ("load_ms", -1) : -1);

            if (outcome == toString (LoadOutcome::diedDuringLoad)
                 || outcome == kLegacyDeathOutcome)
            {
                ++e.failures;

                // A row with no certainty predates the field. It is counted as
                // corroborated, because it was written under the old rule where
                // it would have quarantined outright -- reading silence as
                // "unattributed" would retroactively forgive every recorded
                // death on this machine.
                const auto c = v.getProperty ("certainty", "").toString();
                if (c == "unattributed")
                {
                    ++e.unattributedFailures;
                    if ((bool) v.getProperty ("unattended", false))
                        ++e.unattendedFailuresN;
                }
                else ++e.corroboratedFailures;
            }
            else if (outcome == "ok")
            {
                ++e.priorOkInLedger;
                if (sessionRunId.isNotEmpty()
                     && v.getProperty ("run_id", "").toString() == sessionRunId)
                    ++e.priorOkThisSession;
            }
        }
        return e;
    }

    int countPriorCrashesLocked (const juce::String& pluginId) const
    {
        return countPriorOutcomeLocked (pluginId, toString (LoadOutcome::diedDuringLoad))
             + countPriorOutcomeLocked (pluginId, kLegacyDeathOutcome);
    }

    int countPriorOutcomeLocked (const juce::String& pluginId,
                                 const juce::String& outcome) const
    {
        int n = 0;
        if (! ledgerFile.existsAsFile())
            return 0;

        juce::StringArray lines;
        lines.addLines (ledgerFile.loadFileAsString());
        for (const auto& line : lines)
        {
            if (line.trim().isEmpty()) continue;
            auto v = juce::JSON::parse (line);
            if (v.getProperty ("plugin_id", "").toString() == pluginId
                 && v.getProperty ("outcome", "").toString() == outcome)
                ++n;
        }
        return n;
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

    void quarantineLocked (const juce::String& pluginId, const juce::String& reason,
                           const juce::String& stage = "load",
                           const juce::String& pluginVersion = {},
                           const RetryEvidence* evidence = nullptr)
    {
        if (quarantined.contains (pluginId))
            return;

        QuarantineEntry e;
        e.pluginId  = pluginId;
        e.reason    = reason;
        e.stage     = stage;
        e.at        = nowIso();
        e.auVersion = pluginVersion;
        if (evidence != nullptr)
            e.evidence = *evidence;

        // A path gets a real stamp; an AU identifier gets the version string.
        if (! e.isAudioUnit())
            e.stamp = diskStampFor (juce::File (pluginId));

        quarantined.add (pluginId);
        entries.set (pluginId, e);
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
        entries.clear();
        if (! quarantineFile.existsAsFile())
            return;

        auto v = juce::JSON::parse (quarantineFile.loadFileAsString());
        if (auto* arr = v.getArray())
        {
            for (const auto& raw : *arr)
            {
                auto id = raw.getProperty ("plugin_id", "").toString();
                if (id.isEmpty())
                    continue;

                QuarantineEntry e;
                e.pluginId         = id;
                e.reason           = raw.getProperty ("reason", "").toString();
                // Entries written before stage existed are load entries: scan
                // quarantines did not exist then.
                e.stage            = raw.getProperty ("stage", "load").toString();
                e.at               = raw.getProperty ("at", "").toString();
                e.auVersion        = raw.getProperty ("au_version", "").toString();
                e.stamp.modifiedMs = (juce::int64) (double) raw.getProperty ("stamp_mtime", 0);
                e.stamp.size       = (juce::int64) (double) raw.getProperty ("stamp_size", 0);
                e.stamp.valid      = (bool) raw.getProperty ("stamped", false);

                // The evidence has to survive a relaunch or the nightly
                // re-test cannot find the non-deterministic entries: they are
                // selected by prior successes, which is what this carries.
                if (auto ev = raw.getProperty ("evidence", juce::var()); ev.isObject())
                {
                    e.evidence.stage    = ev.getProperty ("stage", "load").toString();
                    e.evidence.attempts = (int) ev.getProperty ("attempts", 0);
                    e.evidence.failures = (int) ev.getProperty ("failures", 0);
                    e.evidence.corroboratedFailures = (int) ev.getProperty ("corroborated_failures", 0);
                    e.evidence.unattributedFailures = (int) ev.getProperty ("unattributed_failures", 0);
                    e.evidence.priorOkInLedger      = (int) ev.getProperty ("prior_ok_in_ledger", 0);
                    e.evidence.priorOkThisSession   = (int) ev.getProperty ("prior_ok_this_session", 0);
                    if (auto* outs = ev.getProperty ("outcomes", juce::var()).getArray())
                        for (const auto& o : *outs) e.evidence.outcomes.add (o.toString());
                    if (auto* ms = ev.getProperty ("load_ms", juce::var()).getArray())
                        for (const auto& m : *ms) e.evidence.loadMs.add ((int) m);
                }

                quarantined.add (id);
                entries.set (id, e);
            }
        }
    }

    void saveQuarantineLocked()
    {
        juce::Array<juce::var> out;
        for (const auto& id : quarantined)
        {
            const auto& e = entries[id];
            auto* o = new juce::DynamicObject();
            o->setProperty ("plugin_id", id);
            o->setProperty ("reason", e.reason);
            o->setProperty ("stage", e.stage);
            o->setProperty ("at", e.at);
            if (e.stamp.valid)
            {
                o->setProperty ("stamped", true);
                o->setProperty ("stamp_mtime", (double) e.stamp.modifiedMs);
                o->setProperty ("stamp_size", (double) e.stamp.size);
            }
            if (e.auVersion.isNotEmpty())
            {
                o->setProperty ("au_version", e.auVersion);
                o->setProperty ("au_version_is_weak_signal", true);
            }
            if (e.evidence.attempts > 0)
            {
                o->setProperty ("evidence", e.evidence.toVar());
                if (e.evidence.nonDeterministic())
                    o->setProperty ("note", e.evidence.note());
            }
            out.add (juce::var (o));
        }
        writeThrough (quarantineFile, juce::JSON::toString (juce::var (out), true));
    }

    static juce::String nowIso()
    {
        return juce::Time::getCurrentTime().toISO8601 (true);
    }

    bool unattended = false;
    juce::String lastClosedId, lastClosedOutcome, lastClosedStage;
    juce::File root, ledgerFile, inflightFile, quarantineFile, emergencyFile;
    juce::String runId;
    juce::StringArray quarantined;
    mutable juce::HashMap<juce::String, QuarantineEntry> entries;
    juce::HashMap<juce::String, int> counts;
    mutable juce::CriticalSection lock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Ledger)
};

} // namespace ejmap
