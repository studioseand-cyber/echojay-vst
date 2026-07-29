/*
  EjmapWatchdog.h

  A deadline around any call that hands control to plugin code.

  NOT scan-specific. Two hangs were measured on this machine in one afternoon,
  at two different sites:

    - bloom.vst3 hangs inside findAllTypesForFile during Scan, past 90 s,
      and hangs ejextract's isolated worker in the same place.
    - UAD Ampeg B15N hangs inside cacheDisplayInRect on its own view tree,
      past 150 s, having loaded cleanly seconds earlier.

  Neither is a crash. The crash protocol does not fire, inflight.json stays on
  disk with the app alive and unresponsive, and the human's only move is a force
  quit that produces the right ledger row by accident.

  WHY IT MUST TERMINATE THE PROCESS. The stuck thread is inside third-party
  code. There is no cancellation, no interruption, and nothing safe to unwind:
  the plugin holds locks the message thread will never release. So the watchdog
  does not attempt recovery. It records the fact, flushes it, and stops the
  process. The existing inflight protocol then does the rest on relaunch, which
  is why this is cheap to add: the record naming the plugin is already on disk
  before the call begins.

  Deadlines are per site, because the sites are not alike: instantiating a
  plugin and waiting for an XPC editor to connect have different honest budgets.
  The default is 30 s, chosen against measurement rather than taste: the 860
  bundle scan averaged ~315 ms per bundle, so 30 s is roughly 95x the mean and
  will not fire on a slow-but-working plugin, while bloom was still going at 90.

  USE:
      Watchdog::Scope guard (watchdog, "findAllTypesForFile", pluginId, 30000);
      vst3->findAllTypesForFile (found, file);      // guard disarms on scope exit
*/

#pragma once

#include <juce_core/juce_core.h>
#include "EjmapLedger.h"

#include <atomic>
#include <cstdlib>

namespace ejmap
{

/** Exit code the watchdog uses, so a relaunching session (or a shell) can tell
    a watchdog stop from a crash or a clean quit.
*/
inline constexpr int kWatchdogExitCode = 87;

//==============================================================================
class Watchdog : private juce::Thread
{
public:
    /** DEADLINES ARE PER SITE, because the distributions are not alike.
        Measured on this machine, 21 AU plugins, 9 bridged and 12 native:

          createPluginInstance   median 644 ms, p90 2580 ms, max 3223 ms (OVox)
          createEditorIfNeeded   median  30 ms, p90  147 ms, max 5498 ms (Drawmer 1973)
          VST3 scan probe        mean   315 ms across 861 bundles

        The old single 30 s came from the scan mean and was ~9x the slowest
        observed instantiation, which is not enough headroom for a cold XPC
        start or a licence check. It fired on Cymatics Lotus, which instantiates
        in 407 ms: the fast quartile. That was a wrong deadline, not a slow
        plugin.
    */

    /** Scan probe, and anything that does not name a site. 95x the 315 ms scan
        mean. bloom.vst3 was still going at 90 s, so this separates the two.
    */
    static constexpr int kDefaultDeadlineMs = 30000;

    /** Instantiation. 28x the slowest observed load, with room for a licence
        dialog to appear and be dismissed by the human.
    */
    static constexpr int kInstantiateDeadlineMs = 90000;

    /** Editor creation. 11x the slowest observed, which was a native editor;
        bridged editors return in single-digit ms because the real work happens
        asynchronously afterwards, and the editor-ready wait covers that.
    */
    static constexpr int kEditorCreateDeadlineMs = 60000;

    explicit Watchdog (Ledger& l)
        : juce::Thread ("ejmap watchdog"), ledger (l)
    {
        startThread (juce::Thread::Priority::high);
    }

    ~Watchdog() override
    {
        signalThreadShouldExit();
        notify();
        stopThread (2000);
    }

    //==========================================================================
    /** Arms the deadline. Every field is copied, because when this fires the
        caller's stack is wedged and nothing it owns can be dereferenced.
    */
    void arm (const juce::String& site,
              const juce::String& pluginId,
              const juce::String& pluginName,
              const juce::String& format,
              const juce::String& stage,
              int deadlineMs = kDefaultDeadlineMs)
    {
        const juce::ScopedLock sl (lock);
        armedSite   = site;
        armedId     = pluginId;
        armedName   = pluginName;
        armedFormat = format;
        armedStage  = stage;
        armedMs     = deadlineMs;
        expiresAt   = juce::Time::getMillisecondCounter() + (juce::uint32) deadlineMs;
        armed       = true;
        notify();
    }

    void disarm()
    {
        const juce::ScopedLock sl (lock);
        armed = false;
    }

    /** Extends the current deadline without re-arming. For a site that makes
        observable progress (an editor that is resizing is not hung), so a slow
        plugin is not killed for being slow.
    */
    void heartbeat()
    {
        const juce::ScopedLock sl (lock);
        if (armed)
            expiresAt = juce::Time::getMillisecondCounter() + (juce::uint32) armedMs;
    }

    //==========================================================================
    /** RAII arm/disarm. Use this rather than the raw calls: an early return
        that skips disarm() kills the next innocent plugin.
    */
    struct Scope
    {
        Scope (Watchdog& w,
               const juce::String& site,
               const juce::String& pluginId,
               const juce::String& pluginName,
               const juce::String& format,
               const juce::String& stage,
               int deadlineMs = kDefaultDeadlineMs)
            : dog (w)
        {
            dog.arm (site, pluginId, pluginName, format, stage, deadlineMs);
        }

        ~Scope() { dog.disarm(); }

        void heartbeat() { dog.heartbeat(); }

        Watchdog& dog;
        JUCE_DECLARE_NON_COPYABLE (Scope)
    };

private:
    void run() override
    {
        while (! threadShouldExit())
        {
            wait (250);
            if (threadShouldExit())
                return;

            juce::String site, id, name, format, stage;
            int ms = 0;
            {
                const juce::ScopedLock sl (lock);
                if (! armed || juce::Time::getMillisecondCounter() < expiresAt)
                    continue;

                site = armedSite; id = armedId; name = armedName;
                format = armedFormat; stage = armedStage; ms = armedMs;
                armed = false;   // never fire twice for one arming
            }

            fire (site, id, name, format, stage, ms);
        }
    }

    /** Does not return. */
    void fire (const juce::String& site, const juce::String& id, const juce::String& name,
               const juce::String& format, const juce::String& stage, int ms)
    {
        LedgerRecord r;
        r.pluginId = id;
        r.name     = name;
        r.format   = format;
        r.stage    = stage;
        // Report the deadline in a unit that survives it: integer seconds turned
        // an 800 ms deadline into "after 0s".
        const auto howLong = ms >= 1000 ? juce::String (ms / 1000.0, 1) + "s"
                                        : juce::String (ms) + "ms";
        r.detail = "no return from " + site + " after " + howLong
                     + "; process terminated by watchdog";

        // Written and flushed before the process stops existing.
        ledger.recordWatchdogExpiry (r, "hang_in_" + site);

        std::cerr << "ejmap watchdog: " << r.detail << " [" << id << "]" << std::endl;
        std::cerr.flush();

        // _Exit, not exit(): atexit handlers and static destructors would run on
        // a process whose message thread is inside third-party code holding
        // locks, which is how a hang becomes a hang plus a deadlock in teardown.
        std::_Exit (kWatchdogExitCode);
    }

    Ledger& ledger;

    juce::CriticalSection lock;
    bool armed = false;
    juce::uint32 expiresAt = 0;
    int armedMs = kDefaultDeadlineMs;
    juce::String armedSite, armedId, armedName, armedFormat, armedStage;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Watchdog)
};

} // namespace ejmap
