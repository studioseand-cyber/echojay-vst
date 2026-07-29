/*
  WatchdogTest.cpp

  Proves the watchdog actually fires.

  A guard that has never been observed failing is not a guard, and this one ends
  in _Exit, so it cannot be asserted from inside the process that runs it. The
  test therefore IS the hang: it arms a short deadline, then blocks forever. The
  shell around it checks the exit code and the rows left on disk.

  Expected: exit code 87 (kWatchdogExitCode), one timeout row in ledger.json,
  the plugin id quarantined, and inflight.json cleared.

  Writes to a throwaway directory passed as argv[1]. Never the real ledger.
*/

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include "EjmapLedger.h"
#include "EjmapWatchdog.h"

#include <iostream>

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    if (argc < 2) { std::cerr << "usage: ejmap-watchdog-test <temp-dir>\n"; return 2; }

    juce::String rootPath = juce::String (juce::CharPointer_UTF8 (argv[1]));
    juce::File root = juce::File::getCurrentWorkingDirectory().getChildFile (rootPath);
    root.createDirectory();

    ejmap::Ledger   ledger (root);
    ejmap::Watchdog watchdog (ledger);

    const juce::String pid = "VST3:/fake/path/HangsForever.vst3";

    // Same order the app uses: recovery first, which is also where emergency
    // rows from a previous watchdog stop get folded back into the ledger.
    const auto recovered = ledger.recoverFromCrash();
    std::cout << "run_id=" << ledger.currentRunId()
              << " recovered=" << (recovered.isEmpty() ? "(none)" : recovered) << std::endl;
    ledger.beginLoad (pid, "HangsForever", "TestVendor", "VST3", "1.0",
                      "scan", "findAllTypesForFile");

    const bool holdLock = (argc >= 3 && juce::String (argv[2]) == "--hold-lock");

    {
        // 800 ms deadline, then block for far longer. This is the shape of
        // bloom.vst3: a call into plugin code that never comes back.
        ejmap::Watchdog::Scope guard (watchdog, "findAllTypesForFile", pid,
                                      "HangsForever", "VST3", "scan", 800);

        if (holdLock)
        {
            // The nastier case: the wedged thread is also holding the ledger
            // lock, so the watchdog's normal write path can never complete.
            // It must fall back to the lock-free emergency file.
            std::cout << "armed; hanging WITH the ledger lock held" << std::endl;
            std::cout.flush();
            ledger.testOnlyHoldLock (20000);
        }
        else
        {
            std::cout << "armed; hanging now" << std::endl;
            std::cout.flush();
            juce::Thread::sleep (20000);
        }
    }

    // Only reached if the watchdog did NOT fire.
    std::cerr << "FAIL: watchdog did not fire; the hang returned on its own" << std::endl;
    return 1;
}
