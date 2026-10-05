/*
  Main.cpp

  The title bar carries the version and the git short hash, because the adopted
  convention on this project is that the on-screen version is the only proof of
  what is loaded. Version numbers do not indicate lineage: each branch counts up
  from its own base.
*/

#include <juce_gui_extra/juce_gui_extra.h>
#include "MainComponent.h"
#include "EjmapSchema.h"
#include "EjmapMouth.h"

// Generated on every build by cmake/StampBuildInfo.cmake. Carries the git short
// hash of the commit actually compiled, plus "-dirty" when tracked files were
// modified. Deliberately has no #ifndef fallback: a build with no stamp should
// fail to compile rather than quietly claim a version it cannot know.
#include "EjmapBuildInfo.h"
#include "EjmapSupervisor.h"
#include "EjmapMarks.h"
#include "EjmapCertDriver.h"
#include "EjmapTunerProfile.h"

#include <map>
#include <csignal>
#include <unistd.h>
#include <signal.h>

class EjmapApplication  : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "ejmap"; }
    const juce::String getApplicationVersion() override { return EJMAP_VERSION; }
    /** JUCE's own single-instance check runs BEFORE initialise() and quits the
        second process with exit 0. That made every headless flag a silent no-op
        whenever a session was open: --release-quarantine printed nothing, did
        nothing, and reported success. Single-instance is now enforced in main(),
        where it can be loud and where the file-touching flags never reach it.
    */
    bool moreThanOneInstanceAllowed() override          { return true; }

    void initialise (const juce::String& commandLine) override
    {
        // --ledger-root DIR         write the ledger somewhere throwaway
        // --selftest-reentry ID     scripted double-click proof, then quit
        // preserveQuotedStrings keeps the quotes IN the token, and JUCE quotes
        // any argument containing a space when it rebuilds the command line. A
        // VST3 path like "TDR SlickEQ M.vst3" therefore arrives wrapped in
        // literal quote characters and matches nothing in the quarantine.
        auto args = juce::StringArray::fromTokens (commandLine, true);
        for (auto& a : args)
            a = a.unquoted();
        juce::File ledgerRoot;
        juce::String selfTestId;
        bool cacheTest = false, progressTest = false, mapIndexCost = false;
        juce::String attributeReport, afterExit, captureTestId, maskTestId, stallId, promoSuppressId;
        juce::String sweepTestId, sweepTestParam, typedTestId, typedTestParam, assignTestId;
        juce::String bandTestId, bandTestMembers, bandTestImposter, catTestId, dupTestId;
        juce::String fitTestId; int fitHoldSeconds = 0;
        juce::String mbandsTestId, mbandsMode;
        juce::String conlyTestId, conlyCat;
        bool worklist = false, worklistNext = false, categoriseOnly = false, scanOnly = false, retryLicence = false;
        bool sweepReport = false, sweepReportPerRun = false, sweepReportByVendor = false;
        bool sweep = false, sweepDry = false, sweepCaptures = false; int sweepLimit = 0;
        juce::String resweepTargetsPath;
        bool sendPending = false, sendPendingDry = false;
        juce::String typedReasonId, parkTestId;
        juce::String applyMapId, applyMapPath, applyMapImposter;
        juce::String ctrlTestId, ctrlTestMode, ctrlTestLabel, ctrlTestNames;
        juce::String uploadTestId, uploadTestMap, uploadTestTester;
        juce::String settleId; int settleIdx = -1;
        juce::String resubmitId;
        juce::String gateM9Mode;
        bool probeBatch = false;
        juce::String probeMaps, probeOut, probeOnly;
        int stallN = 0;
        bool supervised = false;
        int  restartCount = 0;
        juce::String releaseId, quarantineId, quarantineReason, quarantineStage { "load" };

        for (int i = 0; i < args.size(); ++i)
        {
            if (args[i] == "--ledger-root" && i + 1 < args.size())
                ledgerRoot = juce::File::getCurrentWorkingDirectory().getChildFile (args[++i]);
            else if (args[i] == "--selftest-reentry" && i + 1 < args.size())
                selfTestId = args[++i];
            else if (args[i] == "--selftest-cache")
                cacheTest = true;
            else if (args[i] == "--selftest-mapindex")
                mapIndexCost = true;
            else if (args[i] == "--selftest-progress")
                progressTest = true;
            else if (args[i] == "--selftest-capture" && i + 1 < args.size())
                captureTestId = args[++i];
            else if (args[i] == "--selftest-noisemask" && i + 1 < args.size())
                maskTestId = args[++i];
            else if (args[i] == "--selftest-promosuppress" && i + 1 < args.size())
                promoSuppressId = args[++i];
            else if (args[i] == "--selftest-sweep" && i + 1 < args.size())
            {
                sweepTestId = args[++i];
                if (i + 1 < args.size() && ! args[i + 1].startsWith ("--"))
                    sweepTestParam = args[++i];
            }
            else if (args[i] == "--selftest-typed" && i + 1 < args.size())
            {
                typedTestId = args[++i];
                if (i + 1 < args.size() && ! args[i + 1].startsWith ("--"))
                    typedTestParam = args[++i];
            }
            else if (args[i] == "--selftest-assign" && i + 1 < args.size())
                assignTestId = args[++i];
            else if (args[i] == "--selftest-category" && i + 1 < args.size())
                catTestId = args[++i];
            else if (args[i] == "--selftest-dupescape" && i + 1 < args.size())
                dupTestId = args[++i];
            else if (args[i] == "--selftest-park" && i + 1 < args.size())
                parkTestId = args[++i];
            else if (args[i] == "--selftest-typedreason" && i + 1 < args.size())
                typedReasonId = args[++i];
            else if (args[i] == "--send-pending")
                sendPending = true;
            else if (args[i] == "--send-pending-dry")
            { sendPending = true; sendPendingDry = true; }
            else if (args[i] == "--worklist")
                worklist = true;
            else if (args[i] == "--sweep-report")
                sweepReport = true;
            else if (args[i] == "--all")
                sweepReportPerRun = true;
            else if (args[i] == "--by-vendor")
                sweepReportByVendor = true;
            else if (args[i] == "--categorise")
                categoriseOnly = true;
            else if (args[i] == "--scan")                      // the Scan button, headless (2 Oct: the stranger's runbook has no GUI step)
                scanOnly = true;
            else if (args[i] == "--retry-licence")             // re-probe the bundles the window watch stopped: the licence is back
                retryLicence = true;
            else if (args[i] == "--next")
                worklistNext = true;
            else if (args[i] == "--sweep")
                sweep = true;
            else if (args[i] == "--sweep-limit" && i + 1 < args.size())
                sweepLimit = args[++i].getIntValue();
            else if (args[i] == "--resweep-targets" && i + 1 < args.size())
                resweepTargetsPath = args[++i];
            else if (args[i] == "--dry-run")
                sweepDry = true;
            else if (args[i] == "--captures")
                sweepCaptures = true;
            else if (args[i] == "--selftest-controlsonly" && i + 1 < args.size())
            {
                conlyTestId = args[++i];
                if (i + 1 < args.size() && ! args[i + 1].startsWith ("--"))
                    conlyCat = args[++i];
            }
            else if (args[i] == "--selftest-manualbands" && i + 1 < args.size())
            {
                mbandsTestId = args[++i];
                if (i + 1 < args.size() && ! args[i + 1].startsWith ("--"))
                    mbandsMode = args[++i];
            }
            else if (args[i] == "--selftest-editorfit" && i + 1 < args.size())
            {
                fitTestId = args[++i];
                if (i + 1 < args.size() && ! args[i + 1].startsWith ("--"))
                    fitHoldSeconds = args[++i].getIntValue();
            }
            else if (args[i] == "--selftest-upload" && i + 3 < args.size())
            { uploadTestId = args[i+1]; uploadTestMap = args[i+2]; uploadTestTester = args[i+3]; i += 3; }
            else if (args[i] == "--selftest-controls" && i + 1 < args.size())
            {
                ctrlTestId = args[++i];
                for (auto* dst : { &ctrlTestMode, &ctrlTestLabel, &ctrlTestNames })
                    if (i + 1 < args.size() && ! args[i + 1].startsWith ("--")) *dst = args[++i];
            }
            else if (args[i] == "--selftest-applymap" && i + 3 < args.size())
            { applyMapId = args[i + 1]; applyMapPath = args[i + 2]; applyMapImposter = args[i + 3]; i += 3; }
            else if (args[i] == "--selftest-bands" && i + 2 < args.size())
            {
                bandTestId = args[i + 1]; bandTestMembers = args[i + 2]; i += 2;
                if (i + 1 < args.size() && ! args[i + 1].startsWith ("--"))
                    bandTestImposter = args[++i];
            }
            else if (args[i] == "--selftest-stall" && i + 2 < args.size())
                { stallId = args[i + 1]; stallN = args[i + 2].getIntValue(); i += 2; }
            else if (args[i] == "--measure-settle" && i + 2 < args.size())
                { settleId = args[i + 1]; settleIdx = args[i + 2].getIntValue(); i += 2; }
            else if (args[i] == "--resubmit" && i + 1 < args.size())
                resubmitId = args[++i];
            else if (args[i] == "--probe-batch")
            {
                probeBatch = true;
                for (int j = i + 1; j + 1 < args.size(); j += 2)
                {
                    if (args[j] == "--maps") probeMaps = args[j + 1];
                    else if (args[j] == "--out") probeOut = args[j + 1];
                    else if (args[j] == "--only") probeOnly = args[j + 1];
                }
            }
            else if (args[i] == "--gate-m9")
            { gateM9Mode = "run"; if (i + 1 < args.size() && ! args[i + 1].startsWith ("--")) gateM9Mode = args[++i]; }
            else if (args[i] == "--attribute-report" && i + 1 < args.size())
                attributeReport = args[++i];
            else if (args[i] == "--child")
                supervised = true;
            else if (args[i] == "--restarted" && i + 1 < args.size())
                restartCount = args[++i].getIntValue();
            else if (args[i] == "--after-exit" && i + 1 < args.size())
                afterExit = args[++i];
            else if (args[i] == "--release-quarantine" && i + 1 < args.size())
                releaseId = args[++i];
            else if (args[i] == "--quarantine" && i + 2 < args.size())
            {
                quarantineId = args[i + 1]; quarantineReason = args[i + 2]; i += 2;
                if (i + 1 < args.size() && ! args[i + 1].startsWith ("--"))
                    quarantineStage = args[++i];
            }
        }

        // Supervisor test hooks. Deliberately crude and deliberately here: the
        // supervisor must be provable against a child that exits 0, exits 87,
        // or segfaults, without involving a plugin or a window.
        for (int i = 0; i < args.size(); ++i)
        {
            if (args[i] == "--selftest-mark-loaded")
            {
                auto root = ledgerRoot != juce::File()
                              ? ledgerRoot
                              : juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                    .getChildFile ("ejmap");
                root.createDirectory();
                ejmap::loadOkMarker (root).replaceWithText ("ok");
            }
            else if (args[i] == "--selftest-exit" && i + 1 < args.size())
            {
                const int code = args[i + 1].getIntValue();
                std::cout << "child: exiting with code " << code << std::endl;
                std::cout.flush();
                std::_Exit (code);
            }
            else if (args[i] == "--selftest-segv")
            {
                std::cout << "child: raising SIGSEGV" << std::endl;
                std::cout.flush();
                std::raise (SIGSEGV);
            }
        }




        supervisedMode = supervised;
        ledgerRootUsed = ledgerRoot;
        mainWindow = std::make_unique<MainWindow> (buildTitle(), ledgerRoot,
                                                   supervised, restartCount, afterExit);

        // Read back off the constructed window, not off buildTitle(). The title
        // bar is the only proof of what is running, and on a machine where the
        // shell cannot screenshot (Screen Recording denied) this line is the
        // only way to assert it against the live object rather than the source.
        std::cout << "ejmap window title: " << mainWindow->getName() << std::endl;

        // Self-tests that LOAD a plugin must run after the message loop is
        // going, AND from normal message context rather than a timer.
        //
        // From initialise(): the editor-ready wait's runDispatchLoopUntil does
        // not dispatch, so any editor needing a layout cycle times out and the
        // watchdog kills the process. Three plugins were briefly misdiagnosed as
        // never settling because of this.
        //
        // From a Timer callback: CoreFoundation traps outright, SIGTRAP in
        // CFRunLoopRunSpecific.cold.3, because a nested run loop cannot be
        // started from timer context.
        //
        // callAsync posts an ordinary message, which is the same context a button
        // click arrives in. That is why the UI path was never affected.
        if (probeBatch && mainWindow->getMain() != nullptr)
        {
            auto m = mainWindow->getMain();
            auto md = probeMaps, o = probeOut, oc = probeOnly;
            juce::MessageManager::callAsync ([m, md, o, oc] { m->probeBatch (md, o, oc); });
        }
        else if (gateM9Mode.isNotEmpty() && mainWindow->getMain() != nullptr)
        {
            auto m9 = gateM9Mode == "run" ? juce::String() : gateM9Mode;
            auto* m = mainWindow->getMain();
            juce::MessageManager::callAsync ([m, m9] { m->gateM9 (m9); });
        }
        else if (resubmitId.isNotEmpty() && mainWindow->getMain() != nullptr)
        {
            auto id = resubmitId; auto* m = mainWindow->getMain();
            juce::MessageManager::callAsync ([m, id] { m->resubmitAndUpload (id); });
        }
        else if (settleId.isNotEmpty() && mainWindow->getMain() != nullptr)
        {
            auto id = settleId; auto ix = settleIdx; auto* m = mainWindow->getMain();
            juce::MessageManager::callAsync ([m, id, ix] { m->measureSettle (id, ix); });
        }
        else if (stallId.isNotEmpty() && mainWindow->getMain() != nullptr)
        {
            auto id = stallId; auto n = stallN; auto* m = mainWindow->getMain();
            juce::MessageManager::callAsync ([m, id, n] { m->selfTestStall (id, n); });
        }
        else if (maskTestId.isNotEmpty() && mainWindow->getMain() != nullptr)
        {
            auto id = maskTestId; auto* m = mainWindow->getMain();
            juce::MessageManager::callAsync ([m, id] { m->selfTestNoiseMask (id); });
        }
        else if (promoSuppressId.isNotEmpty() && mainWindow->getMain() != nullptr)
        {
            auto id = promoSuppressId; auto* m = mainWindow->getMain();
            juce::MessageManager::callAsync ([m, id] { m->selfTestPromoSuppress (id); });
        }
        else if (sweepTestId.isNotEmpty() && mainWindow->getMain() != nullptr)
        {
            auto id = sweepTestId; auto ps = sweepTestParam; auto* m = mainWindow->getMain();
            juce::MessageManager::callAsync ([m, id, ps] { m->selfTestSweep (id, ps); });
        }
        else if (typedTestId.isNotEmpty() && mainWindow->getMain() != nullptr)
        {
            auto id = typedTestId; auto ps = typedTestParam; auto* m = mainWindow->getMain();
            juce::MessageManager::callAsync ([m, id, ps] { m->selfTestTyped (id, ps); });
        }
        else if (assignTestId.isNotEmpty() && mainWindow->getMain() != nullptr)
        {
            auto id = assignTestId; auto* m = mainWindow->getMain();
            juce::MessageManager::callAsync ([m, id] { m->selfTestAssign (id); });
        }
        else if (uploadTestId.isNotEmpty() && mainWindow->getMain() != nullptr)
        {
            auto id = uploadTestId; auto mp = uploadTestMap; auto tn = uploadTestTester;
            auto* m = mainWindow->getMain();
            juce::MessageManager::callAsync ([m, id, mp, tn] { m->selfTestUpload (id, mp, tn); });
        }
        else if (ctrlTestId.isNotEmpty() && mainWindow->getMain() != nullptr)
        {
            auto id = ctrlTestId; auto mn = ctrlTestMode; auto ml = ctrlTestLabel; auto en = ctrlTestNames;
            auto* m = mainWindow->getMain();
            juce::MessageManager::callAsync ([m, id, mn, ml, en] { m->selfTestControls (id, mn, ml, en); });
        }
        else if (applyMapId.isNotEmpty() && mainWindow->getMain() != nullptr)
        {
            auto id = applyMapId; auto mp = applyMapPath; auto im = applyMapImposter;
            auto* m = mainWindow->getMain();
            juce::MessageManager::callAsync ([m, id, mp, im] { m->selfTestApplyMap (id, mp, im); });
        }
        else if (catTestId.isNotEmpty() && mainWindow->getMain() != nullptr)
        {
            auto id = catTestId; auto* m = mainWindow->getMain();
            juce::MessageManager::callAsync ([m, id] { m->selfTestCategory (id); });
        }
        else if (dupTestId.isNotEmpty() && mainWindow->getMain() != nullptr)
        {
            auto id = dupTestId; auto* m = mainWindow->getMain();
            juce::MessageManager::callAsync ([m, id] { m->selfTestDupEscape (id); });
        }
        else if (parkTestId.isNotEmpty() && mainWindow->getMain() != nullptr)
        {
            auto id = parkTestId; auto* m = mainWindow->getMain();
            juce::MessageManager::callAsync ([m, id] { m->selfTestPark (id); });
        }
        else if (typedReasonId.isNotEmpty() && mainWindow->getMain() != nullptr)
        {
            auto id = typedReasonId; auto* m = mainWindow->getMain();
            juce::MessageManager::callAsync ([m, id] { m->selfTestTypedReason (id); });
        }
        else if (sendPending && mainWindow->getMain() != nullptr)
        {
            auto* m = mainWindow->getMain(); auto d = sendPendingDry;
            juce::MessageManager::callAsync ([m, d] { m->sendPendingMaps (d); });
        }
        else if (sweep && mainWindow->getMain() != nullptr)
        {
            auto* m = mainWindow->getMain();
            const int lim = sweepLimit; const bool dry = sweepDry, caps = sweepCaptures;
            const auto rt = resweepTargetsPath;
            juce::MessageManager::callAsync ([m, lim, dry, caps, rt]
            {
                if (rt.isNotEmpty() && ! m->setResweepTargets (rt))
                { std::cout << "SWEEP: --resweep-targets file not readable or empty: " << rt << std::endl; juce::JUCEApplication::quit(); return; }
                m->runSweep (lim, dry, caps);
            });
        }
        else if (scanOnly && mainWindow->getMain() != nullptr)
        {
            // --scan [--categorise]: exactly what the Scan button (then the Categorise button) does, then quit. The
            // scan writes scan-cache.xml and resumes from scan-progress.jsonl if an earlier attempt died in a bundle.
            auto* m2 = mainWindow->getMain(); const bool alsoCat = categoriseOnly, rl = retryLicence;
            juce::MessageManager::callAsync ([m2, alsoCat, rl]
            {
                std::cout << "SCAN: starting (the Scan button, headless" << (rl ? ", re-probing licence stops" : "") << ")" << std::endl;
                m2->scanFromCli (rl);
                std::cout << "SCAN: done - " << m2->scanSummaryLine() << std::endl;
                if (alsoCat) m2->categoriseFromCli();
                juce::JUCEApplication::quit();
            });
        }
        else if (categoriseOnly && mainWindow->getMain() != nullptr)
        {
            auto* m2 = mainWindow->getMain();
            juce::MessageManager::callAsync ([m2]
            {
                m2->categoriseFromCli();
                juce::JUCEApplication::quit();
            });
        }
        else if (worklist && mainWindow->getMain() != nullptr)
        {
            auto* m = mainWindow->getMain(); const bool nx = worklistNext;
            juce::MessageManager::callAsync ([m, nx] { m->printWorklist (nx); });
        }
        else if (sweepReport && mainWindow->getMain() != nullptr)
        {
            // Reads the run logs and quits. Opens no plugin, needs no scan and
            // no server: a report about runs that already happened must not
            // depend on anything that could fail today.
            auto* m = mainWindow->getMain(); const bool per = sweepReportPerRun;
            const bool bv = sweepReportByVendor;
            juce::MessageManager::callAsync ([m, per, bv]
            {
                m->reportSweepRuns (per, bv);
                juce::JUCEApplication::quit();
            });
        }
        else if (conlyTestId.isNotEmpty() && mainWindow->getMain() != nullptr)
        {
            auto id = conlyTestId; auto* m = mainWindow->getMain();
            auto ct = conlyCat.isEmpty() ? juce::String ("compressor") : conlyCat;
            juce::MessageManager::callAsync ([m, id, ct] { m->selfTestControlsOnly (id, ct); });
        }
        else if (mbandsTestId.isNotEmpty() && mainWindow->getMain() != nullptr)
        {
            auto id = mbandsTestId; auto md = mbandsMode; auto* m = mainWindow->getMain();
            juce::MessageManager::callAsync ([m, id, md] { m->selfTestManualBands (id, md); });
        }
        else if (fitTestId.isNotEmpty() && mainWindow->getMain() != nullptr)
        {
            auto id = fitTestId; auto hs = fitHoldSeconds; auto* m = mainWindow->getMain();
            juce::MessageManager::callAsync ([m, id, hs] { m->selfTestEditorFit (id, hs); });
        }
        else if (bandTestId.isNotEmpty() && mainWindow->getMain() != nullptr)
        {
            auto id = bandTestId; auto ms = bandTestMembers; auto im = bandTestImposter;
            auto* m = mainWindow->getMain();
            juce::MessageManager::callAsync ([m, id, ms, im] { m->selfTestBands (id, ms, im); });
        }
        else if (captureTestId.isNotEmpty() && mainWindow->getMain() != nullptr)
        {
            auto id = captureTestId; auto* m = mainWindow->getMain();
            juce::MessageManager::callAsync ([m, id] { m->selfTestCapture (id); });
        }
        else if (progressTest && mainWindow->getMain() != nullptr)
            mainWindow->getMain()->selfTestProgressAndRelease();
        else if (mapIndexCost && mainWindow->getMain() != nullptr)
            mainWindow->getMain()->selfTestMapIndexCost();
        else if (cacheTest && mainWindow->getMain() != nullptr)
            mainWindow->getMain()->selfTestScanCache();
        else if (selfTestId.isNotEmpty() && mainWindow->getMain() != nullptr)
            mainWindow->getMain()->selfTestReentry (selfTestId);
    }

    void shutdown() override
    {
        mainWindow = nullptr;

        // Release the single-instance lock. A watchdog _Exit or a crash skips
        // this, which is why the lock is validated with kill(pid, 0) rather than
        // trusted for existing.
        auto root = ledgerRootUsed != juce::File()
                      ? ledgerRootUsed
                      : juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                          .getChildFile ("ejmap");
        auto lock = root.getChildFile ("instance.lock");
        if (lock.existsAsFile() && lock.loadFileAsString().trim().getIntValue() == (int) getpid())
            lock.deleteFile();
    }

    static juce::File ledgerRootUsed;

    void systemRequestedQuit() override { quit(); }

private:
    /** Supervision mode is on the title bar for the same reason the git hash
        is: a direct launch and a supervised one behave differently after a
        crash, and were otherwise indistinguishable.
    */
    static bool supervisedMode;

    static juce::String buildTitle()
    {
        return juce::String ("ejmap ") + EJMAP_VERSION
             + "  (" + EJMAP_GIT_HASH + ")"
             + "  schema " + ejmap::kMapSchemaString
             + (supervisedMode ? "  [supervised]" : "  [direct]");
    }

    class MainWindow  : public juce::DocumentWindow
    {
    public:
        MainWindow (const juce::String& name, juce::File ledgerRoot,
                    bool supervised, int restartCount, const juce::String& afterExit)
            : DocumentWindow (name,
                              juce::Colour (0xff10141c),
                              DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            main = new ejmap::MainComponent (ledgerRoot, supervised, restartCount, afterExit);
            setContentOwned (main, true);
            setResizable (true, true);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            JUCEApplication::getInstance()->systemRequestedQuit();
        }

        ejmap::MainComponent* getMain() const noexcept { return main; }

    private:
        ejmap::MainComponent* main = nullptr;   // owned by the window's content
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    std::unique_ptr<MainWindow> mainWindow;
};

bool EjmapApplication::supervisedMode = false;
juce::File EjmapApplication::ledgerRootUsed;

juce::JUCEApplicationBase* juce_CreateApplication();
juce::JUCEApplicationBase* juce_CreateApplication() { return new EjmapApplication(); }

namespace
{
    juce::String argAt (int argc, char* argv[], int i)
    {
        return i < argc ? juce::String (juce::CharPointer_UTF8 (argv[i])).unquoted() : juce::String();
    }

    juce::File ledgerRootFrom (int argc, char* argv[])
    {
        for (int i = 1; i < argc; ++i)
            if (argAt (argc, argv, i) == "--ledger-root" && i + 1 < argc)
                return juce::File::getCurrentWorkingDirectory()
                         .getChildFile (argAt (argc, argv, i + 1));
        return {};
    }

    /** FILE-TOUCHING FLAGS RUN HERE, outside the GUI app.

        They read and write ejmap's own files and never open a window, so they
        have no business being subject to a single-instance check. Handling them
        before JUCEApplicationBase::main means they cannot be bounced, which is
        what made them silently no-op while a session was open.

        Returns an exit code, or -1 for "not a CLI invocation, carry on".
    */
    int runHeadlessCli (int argc, char* argv[])
    {
        juce::String release, qId, qReason, qStage { "load" }, report;
        juce::String evidenceId, evidenceStage { "load" }, signIn;
        bool whoami = false;
        bool retest = false, apply = false;

        // --issues
        // Every flagged and every unmappable plugin, for handover. Headless and
        // read-only, so it runs while a session is open.
        for (int i = 1; i < argc; ++i)
            if (juce::String (argv[i]) == "--issues")
            {
                auto root = ledgerRootFrom (argc, argv);
                if (root == juce::File())
                    root = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                             .getChildFile ("ejmap");
                const auto m = ejmap::Marks::load (root);
                std::cout << "FLAGGED (" << (int) m.issues.size() << ") -- keyed on the full "
                          << "identity, because an issue is about a build" << std::endl;
                for (const auto& kv : m.issues)
                    std::cout << "  " << kv.first << "   by " << kv.second.by
                              << " at " << kv.second.at << std::endl;
                std::cout << "UNMAPPABLE (" << (int) m.unmappable.size() << ") -- keyed on the "
                          << "product, because a utility stays a utility across versions"
                          << std::endl;
                for (const auto& kv : m.unmappable)
                    std::cout << "  " << kv.first << "   by " << kv.second.by
                              << " at " << kv.second.at << std::endl;
                if (m.issues.empty() && m.unmappable.empty())
                    std::cout << "  (nothing marked)" << std::endl;
                return 0;
            }

        // --cert-rederive <fixturesDir>   and   --cert-defaults --fixtures <dir> --probe <path> --out <dir> ...
        // THE CERTIFICATION DRIVER (EjmapCertDriver.h, docs/EJMAP_CERT_DRIVER.md). Headless: the
        // probe hosts every plugin in its own process; ejmap only orchestrates and derives.
        for (int i = 1; i < argc; ++i)
        {
            const auto a = argAt (argc, argv, i);
            auto cwdFile = [] (const juce::String& p) { return juce::File::getCurrentWorkingDirectory().getChildFile (p); };
            if (a == "--cert-rederive" && i + 1 < argc)
                return ejmap::cert::runRederive (cwdFile (argAt (argc, argv, i + 1)));
            if (a == "--scan-watch-selftest")
                return ejmap::cert::runScanWatchSelfTest (juce::File::getSpecialLocation (juce::File::currentExecutableFile));
            if (a == "--scan-watch-selftest-child" && i + 2 < argc)
                return ejmap::cert::runScanWatchSelfTestChild (juce::File (argAt (argc, argv, i + 1)), argAt (argc, argv, i + 2) == "window");
            if (a == "--cert-watch-selftest" && i + 1 < argc)
                return ejmap::cert::runWatchSelfTest (cwdFile (argAt (argc, argv, i + 1)));
            // --cert-probe-once <probe> <timeout-s> <probe args...>
            if (a == "--cert-probe-once" && i + 3 < argc)
            {
                juce::StringArray rest;
                for (int j = i + 3; j < argc; ++j) rest.add (juce::String (juce::CharPointer_UTF8 (argv[j])));
                return ejmap::cert::runProbeOnce (cwdFile (argAt (argc, argv, i + 1)),
                                                  rest, juce::jmax (1, argAt (argc, argv, i + 2).getIntValue()) * 1000);
            }
            // --cert-preflight: what a batch would use, with no flags - the probe beside this executable and its signature,
            // the cert root, the ledger, the iLok. The stranger's-Mac test's A1 evidence (docs/STRANGER_MAC_TEST.md).
            if (a == "--cert-preflight")
            {
                ejmap::cert::SweepOptions o;
                ejmap::cert::resolveCertPaths (o, juce::File::getSpecialLocation (juce::File::currentExecutableFile));
                return ejmap::cert::runPreflight (o, juce::File::getSpecialLocation (juce::File::currentExecutableFile));
            }
            // --cert-sweep-census [fixturesDir]: the store defaults to ~/Library/ejmap/cert/fixtures, as the sweep's does.
            if (a == "--cert-sweep-census")
            {
                bool includePace = false, retryRefused = false, retryAll = false;
                auto ledgerRoot = ejmap::cert::defaultEjmapLedger();
                auto fixturesDir = i + 1 < argc && ! argAt (argc, argv, i + 1).startsWith ("--") ? cwdFile (argAt (argc, argv, i + 1))
                                                                                                    : ejmap::cert::defaultCertRoot().getChildFile ("fixtures");
                for (int j = 1; j < argc; ++j)
                {
                    if (argAt (argc, argv, j) == "--include-pace") includePace = true;
                    if (argAt (argc, argv, j) == "--retry-refused") retryRefused = true;
                    if (argAt (argc, argv, j) == "--retry-refused-all") retryRefused = retryAll = true;
                    if (argAt (argc, argv, j) == "--ejmap-ledger" && j + 1 < argc) ledgerRoot = cwdFile (argAt (argc, argv, j + 1));
                }
                return ejmap::cert::runSweepCensus (fixturesDir, ledgerRoot, includePace, retryRefused, retryAll);
            }
            if (a == "--cert-sweep-rederive" && i + 4 < argc)
                return ejmap::cert::runSweepRederive (cwdFile (argAt (argc, argv, i + 1)), cwdFile (argAt (argc, argv, i + 2)),
                                                      cwdFile (argAt (argc, argv, i + 3)), cwdFile (argAt (argc, argv, i + 4)));
            // EXPORT TO SEAN'S ej_comp_profile/1 (COMP_PROFILE_SPEC v1.1), one exporter in EjmapProfileExport.h.
            if ((a == "--export-profile" && i + 2 < argc) || (a == "--export-profiles" && i + 2 < argc))
            {
                juce::String cand; for (int j = 1; j + 1 < argc; ++j) if (argAt (argc, argv, j) == "--candidate") cand = argAt (argc, argv, j + 1);
                return ejmap::cert::runExportProfiles (cwdFile (argAt (argc, argv, i + 1)), cwdFile (argAt (argc, argv, i + 2)), a == "--export-profiles", cand);
            }
            // --cert-plan <record.json>: what the plan would sweep (read-only): the amount control, its flags, candidates, or the refusal
            if (a == "--cert-plan" && i + 1 < argc)
            {
                const auto r = juce::JSON::parse (cwdFile (argAt (argc, argv, i + 1)).loadFileAsString());
                const auto pl = ejmap::sweep::planFromFixture (r);
                std::cout << r.getProperty ("product", "").toString() << ": " << (pl.ok ? (pl.thr >= 0 ? "amount [" + juce::String (pl.thr) + "] " + pl.thrName + " flags " + pl.thrFlags.joinIntoString (",") + " unit '" + pl.thrUnit + "'" : juce::String ((int) pl.candidates.size()) + " candidates: " + [&] { juce::StringArray a; for (const auto& c : pl.candidates) a.add ("[" + juce::String (c.index) + "] " + c.name); return a.joinIntoString (", "); }()) : "REFUSED: " + pl.why)
                          << (pl.pickNote.isNotEmpty() ? " | " + pl.pickNote : juce::String()) << std::endl;
                return 0;
            }
            // --cert-review-zip <zip|folder> [--against <zip|folder>] [--keep]: ONE plain report on a zipped-back follow-up folder against a
            // baseline (default: Sean's 4 Oct zip in ~/Downloads, when it is there) - hygiene, outcome changes by product, the
            // projected re-sweeps and how each ended, sidechain readings, review picks, tone checks per level, deep points,
            // inert / licence rows, crashes. Unzips to the temp folder; loads nothing; writes nothing outside that folder.
            if (a == "--cert-review-zip" && i + 1 < argc)
            {
                juce::File against = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Downloads/ejmap-cert-MacBook-Pro-4-20261004.zip");
                bool explicitBaseline = false, keep = false;
                for (int j = 1; j < argc; ++j) { if (argAt (argc, argv, j) == "--against" && j + 1 < argc) { against = cwdFile (argAt (argc, argv, j + 1)); explicitBaseline = true; } if (argAt (argc, argv, j) == "--keep") keep = true; }
                if (! explicitBaseline && ! against.existsAsFile()) { std::cout << "ZIP REVIEW: no --against given and the 4 Oct baseline is not at " << against.getFullPathName() << ": counts only, nothing projected" << std::endl; against = juce::File(); }
                return ejmap::cert::runReviewZip (cwdFile (argAt (argc, argv, i + 1)), against, juce::File::getSpecialLocation (juce::File::tempDirectory), keep);
            }
            // --cert-review-sheet <cert dir>: every needs_review record's candidates with their verdicts and 2 dB curves (read-only)
            if (a == "--cert-review-sheet" && i + 1 < argc) { ejmap::cert::printReviewSheet (cwdFile (argAt (argc, argv, i + 1)).getChildFile ("fixtures"), std::cout); return 0; }
            // --cert-states <dir of records>: what state outcomeForRecord gives every record now (read-only; the projected outcome for a zipped-back folder)
            if (a == "--cert-states" && i + 1 < argc)
            {
                const auto dir = cwdFile (argAt (argc, argv, i + 1)); std::map<juce::String, int> n;
                for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.json"))
                {
                    if (f.getFileName().endsWith (".defaults.json")) continue;
                    const auto r = juce::JSON::parse (f.loadFileAsString()); if (! r.isObject() || ! r.hasProperty ("product")) continue;
                    const auto o = ejmap::loop::outcomeForRecord (r); ++n[o.state];
                    std::cout << o.state << "\t" << r.getProperty ("product", "").toString() << "\t" << o.reason << std::endl;
                }
                for (const auto& [k, v] : n) std::cout << "STATES: " << k << " " << v << std::endl;
                return 0;
            }
            // --cert-range-gaps <dir of records> [--csv]: the range-gap census (read-only): per control with a word end or a missing end sample, its role
            if (a == "--cert-range-gaps" && i + 1 < argc)
            {
                const auto dir = cwdFile (argAt (argc, argv, i + 1)); int recs = 0, wordN = 0, missN = 0, roleWord = 0, roleMiss = 0; std::set<juce::String> prodsW, prodsM;
                for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.json"))
                {
                    if (f.getFileName().endsWith (".defaults.json")) continue;
                    const auto r = juce::JSON::parse (f.loadFileAsString()); if (! r.getProperty ("controls", {}).isArray()) continue; ++recs;
                    for (const auto& g : ejmap::profile::rangeGaps (r))
                    {
                        if (g.wordEnd) { ++wordN; prodsW.insert (g.product); if (g.role.isNotEmpty()) ++roleWord; }
                        if (g.missingEnd) { ++missN; prodsM.insert (g.product); if (g.role.isNotEmpty()) ++roleMiss; }
                        std::cout << (g.wordEnd ? "WORD-END  " : "          ") << (g.missingEnd ? "MISSING-END  " : "             ") << g.product << " | [" << g.index << "] " << g.control << " | role " << (g.role.isEmpty() ? "-" : g.role)
                                  << " | at0 '" << g.at0 << "' at1 '" << g.at1 << "' | instantiate '" << g.instantiate << "' @" << juce::String (g.instNorm, 3) << (g.instOutside ? " OUTSIDE the parsed range" : "") << std::endl;
                    }
                }
                std::cout << "RANGE GAPS: " << recs << " records; (a) word end: " << wordN << " controls across " << (int) prodsW.size() << " products, " << roleWord << " in a role; (b) missing end sample: " << missN << " controls across " << (int) prodsM.size() << " products, " << roleMiss << " in a role" << std::endl;
                return 0;
            }
            // --cert-tone-levels <profile.json>: the tone-check L the rule would use per level, nothing loaded (a dry read of the rule)
            // --cert-gain-cal <product> [--out <cert dir>] [--probe <path>] [--ejmap-ledger <dir>]: roadmap 2.1 PROTOTYPE (B1), nothing exported
            if (a == "--cert-gain-cal" && i + 1 < argc)
            {
                ejmap::cert::SweepOptions o; o.product = argAt (argc, argv, i + 1);
                for (int j = i + 2; j + 1 < argc; ++j) { const auto k = argAt (argc, argv, j), v = argAt (argc, argv, j + 1);
                    if (k == "--out") o.out = cwdFile (v); else if (k == "--probe") o.probe = cwdFile (v); else if (k == "--ejmap-ledger") o.ledger = cwdFile (v); }
                if (o.out == juce::File()) o.out = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/ejmap/cert");
                if (o.probe == juce::File()) o.probe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("EchoJayProbe");
                return ejmap::cert::runGainCal (o);
            }
            // --cert-saturation <product> [--out <cert dir>] [--probe <path>] [--ejmap-ledger <dir>]: roadmap 2.5 PROTOTYPE (B5), nothing exported
            if (a == "--cert-saturation" && i + 1 < argc)
            {
                ejmap::cert::SweepOptions o; o.product = argAt (argc, argv, i + 1);
                for (int j = i + 2; j + 1 < argc; ++j) { const auto k = argAt (argc, argv, j), v = argAt (argc, argv, j + 1);
                    if (k == "--out") o.out = cwdFile (v); else if (k == "--probe") o.probe = cwdFile (v); else if (k == "--ejmap-ledger") o.ledger = cwdFile (v); }
                if (o.out == juce::File()) o.out = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/ejmap/cert");
                if (o.probe == juce::File()) o.probe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("EchoJayProbe");
                return ejmap::cert::runSaturation (o);
            }
            // --cert-dynamics <product> [--kind transient|gate] [--out <cert dir>] [--probe <path>] [--ejmap-ledger <dir>]: roadmap 2.8 PROTOTYPE (R5), nothing exported
            if (a == "--cert-dynamics" && i + 1 < argc)
            {
                ejmap::cert::SweepOptions o; o.product = argAt (argc, argv, i + 1); juce::String kind;
                for (int j = i + 2; j + 1 < argc; ++j) { const auto k = argAt (argc, argv, j), v = argAt (argc, argv, j + 1);
                    if (k == "--out") o.out = cwdFile (v); else if (k == "--probe") o.probe = cwdFile (v); else if (k == "--ejmap-ledger") o.ledger = cwdFile (v); else if (k == "--kind") kind = v; }
                if (o.out == juce::File()) o.out = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/ejmap/cert");
                if (o.probe == juce::File()) o.probe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("EchoJayProbe");
                return ejmap::cert::runDynamics (o, kind);
            }
            // --cert-reverb-delay <product> [--kind reverb|delay] [--out <cert dir>] [--probe <path>] [--ejmap-ledger <dir>]: roadmap 2.7 PROTOTYPE (R4), nothing exported
            if (a == "--cert-reverb-delay" && i + 1 < argc)
            {
                ejmap::cert::SweepOptions o; o.product = argAt (argc, argv, i + 1); juce::String kind;
                for (int j = i + 2; j + 1 < argc; ++j) { const auto k = argAt (argc, argv, j), v = argAt (argc, argv, j + 1);
                    if (k == "--out") o.out = cwdFile (v); else if (k == "--probe") o.probe = cwdFile (v); else if (k == "--ejmap-ledger") o.ledger = cwdFile (v); else if (k == "--kind") kind = v; }
                if (o.out == juce::File()) o.out = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/ejmap/cert");
                if (o.probe == juce::File()) o.probe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("EchoJayProbe");
                return ejmap::cert::runReverbDelay (o, kind);
            }
            // --cert-eq <product> [--out <cert dir>] [--probe <path>] [--ejmap-ledger <dir>]: roadmap 2.2 PROTOTYPE (B4), nothing exported
            if (a == "--cert-eq" && i + 1 < argc)
            {
                ejmap::cert::SweepOptions o; o.product = argAt (argc, argv, i + 1);
                for (int j = i + 2; j + 1 < argc; ++j) { const auto k = argAt (argc, argv, j), v = argAt (argc, argv, j + 1);
                    if (k == "--out") o.out = cwdFile (v); else if (k == "--probe") o.probe = cwdFile (v); else if (k == "--ejmap-ledger") o.ledger = cwdFile (v); }
                if (o.out == juce::File()) o.out = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/ejmap/cert");
                if (o.probe == juce::File()) o.probe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("EchoJayProbe");
                return ejmap::cert::runEq (o);
            }
            // --cert-limiter <product> [--out <cert dir>] [--probe <path>] [--ejmap-ledger <dir>]: roadmap 2.4 PROTOTYPE (B3), nothing exported
            if (a == "--cert-limiter" && i + 1 < argc)
            {
                ejmap::cert::SweepOptions o; o.product = argAt (argc, argv, i + 1);
                for (int j = i + 2; j + 1 < argc; ++j) { const auto k = argAt (argc, argv, j), v = argAt (argc, argv, j + 1);
                    if (k == "--out") o.out = cwdFile (v); else if (k == "--probe") o.probe = cwdFile (v); else if (k == "--ejmap-ledger") o.ledger = cwdFile (v); }
                if (o.out == juce::File()) o.out = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/ejmap/cert");
                if (o.probe == juce::File()) o.probe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("EchoJayProbe");
                return ejmap::cert::runLimiter (o);
            }
            // --cert-timing <product> [--out <cert dir>] [--probe <path>] [--ejmap-ledger <dir>]: roadmap 2.3 PROTOTYPE (B2), nothing exported
            if (a == "--cert-timing" && i + 1 < argc)
            {
                ejmap::cert::SweepOptions o; o.product = argAt (argc, argv, i + 1);
                for (int j = i + 2; j + 1 < argc; ++j) { const auto k = argAt (argc, argv, j), v = argAt (argc, argv, j + 1);
                    if (k == "--out") o.out = cwdFile (v); else if (k == "--probe") o.probe = cwdFile (v); else if (k == "--ejmap-ledger") o.ledger = cwdFile (v); }
                if (o.out == juce::File()) o.out = juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Library/ejmap/cert");
                if (o.probe == juce::File()) o.probe = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("EchoJayProbe");
                return ejmap::cert::runTiming (o);
            }
            // --tuner-profile-draft <record> <out.json>: the ej_tuner_profile/1 PROPOSAL exporter (4 Oct A5) - nothing loaded, nothing published
            if (a == "--tuner-profile-draft" && i + 2 < argc)
            {
                const auto record = juce::JSON::parse (cwdFile (argAt (argc, argv, i + 1)).loadFileAsString());
                const auto e = ejmap::tunerprofile::exportTunerProfileDraft (record);
                if (! e.ok) { std::cout << "TUNER PROFILE DRAFT refused: " << e.refused << std::endl; return 2; }
                cwdFile (argAt (argc, argv, i + 2)).replaceWithText (juce::JSON::toString (e.profile) + "\n", false, false, "\n");
                std::cout << "TUNER PROFILE DRAFT (" << ejmap::tunerprofile::kStatus << ") for " << record.getProperty ("product", "").toString() << " -> " << argAt (argc, argv, i + 2) << std::endl;
                for (const auto& n : e.notes) std::cout << "  note: " << n << std::endl;
                return 0;
            }
            if (a == "--cert-tone-levels" && i + 1 < argc)
            {
                const auto profile = juce::JSON::parse (cwdFile (argAt (argc, argv, i + 1)).loadFileAsString());
                if (! profile.isObject()) { std::cout << "not a profile" << std::endl; return 2; }
                std::cout << "TONE LEVELS for " << profile.getProperty ("plugin", {}).getProperty ("name", "").toString() << " (nothing loaded; the rule only)" << std::endl;
                std::vector<double> levels { 2.0 }; for (int t : ejmap::profile::deepLevelsCarried (profile)) levels.push_back ((double) t);
                for (double g : levels)
                {
                    const auto tl = ejmap::profile::toneLevelFor (profile, g);
                    std::cout << "  g " << juce::String (g, 1) << ": allowance " << juce::String (tl.clampDb, 1) << " dB (v2.1); L_ref " << juce::String (tl.Lref, 2) << "; "
                              << (tl.ok ? "L " + juce::String (tl.L, 2) + " dBFS RMS (gap " + juce::String (tl.gapDb, 2) + "), pick norm " + juce::String (tl.pick.norm, 4) + ", its 1 dB point " + juce::String (tl.pick.pickOneDb, 2) + " (" + juce::String (tl.L - tl.pick.pickOneDb, 1) + " below L), expectation " + juce::String (tl.pick.expectedGrDb, 2) + " dB, tried " + juce::String (tl.tried)
                                       : "NO VALID L: " + tl.reason) << std::endl;
                }
                return 0;
            }
            // --cert-tone-check <profile.json> <record.json> [--out DIR] [--probe P] [--ejmap-ledger DIR] [--retry-licence] [--L x: override the per-level rule] [--g 2]
            if (a == "--cert-tone-check" && i + 2 < argc)
            {
                ejmap::cert::SweepOptions o; double L = ejmap::cert::kToneLevelByRule, g = 2.0; juce::String cand;   // L per level by rule unless --L is given
                for (int j = 1; j < argc; ++j)
                {
                    const auto k = argAt (argc, argv, j); const auto v = argAt (argc, argv, j + 1);
                    if      (k == "--probe" && j + 1 < argc) o.probe = cwdFile (v);
                    else if (k == "--out"   && j + 1 < argc) o.out = cwdFile (v);
                    else if (k == "--ejmap-ledger" && j + 1 < argc) o.ledger = cwdFile (v);
                    else if (k == "--retry-licence") o.retryLicence = true;
                    else if (k == "--L"     && j + 1 < argc) L = v.getDoubleValue();
                    else if (k == "--g"     && j + 1 < argc) g = v.getDoubleValue();
                    else if (k == "--candidate" && j + 1 < argc) cand = v;
                }
                ejmap::cert::resolveCertPaths (o, juce::File::getSpecialLocation (juce::File::currentExecutableFile));
                return ejmap::cert::runToneCheck (o, cwdFile (argAt (argc, argv, i + 1)), cwdFile (argAt (argc, argv, i + 2)), L, g, cand);
            }
            if (a == "--cert-detector" && i + 1 < argc)
            {
                ejmap::cert::SweepOptions o; juce::String cand;
                for (int j = 1; j < argc; ++j) { const auto k = argAt (argc, argv, j); const auto v = argAt (argc, argv, j + 1);
                    if (k == "--probe" && j + 1 < argc) o.probe = cwdFile (v); else if (k == "--out" && j + 1 < argc) o.out = cwdFile (v); else if (k == "--candidate" && j + 1 < argc) cand = v;
                    else if (k == "--ejmap-ledger" && j + 1 < argc) o.ledger = cwdFile (v); else if (k == "--retry-licence") o.retryLicence = true; }
                ejmap::cert::resolveCertPaths (o, juce::File::getSpecialLocation (juce::File::currentExecutableFile));
                return ejmap::cert::runDetector (o, cwdFile (argAt (argc, argv, i + 1)), cand);
            }
            if (a == "--cert-tuner")
            {
                ejmap::cert::SweepOptions o; o.hostVersion = EJMAP_VERSION;
                for (int j = 1; j < argc; ++j)
                {
                    const auto k = argAt (argc, argv, j); const auto v = argAt (argc, argv, j + 1);
                    if      (k == "--product"   && j + 1 < argc) o.product = v;
                    else if (k == "--probe"     && j + 1 < argc) o.probe = cwdFile (v);
                    else if (k == "--out"       && j + 1 < argc) o.out = cwdFile (v);
                    else if (k == "--timeout-s" && j + 1 < argc) o.timeoutMs = juce::jmax (1, v.getIntValue()) * 1000;
                }
                ejmap::cert::resolveCertPaths (o, juce::File::getSpecialLocation (juce::File::currentExecutableFile));
                if (o.product.isEmpty()) { std::cerr << "usage: ejmap --cert-tuner --product <name> [--out <dir>] [--probe <EchoJayProbe>] [--timeout-s N]" << std::endl; return 2; }
                return ejmap::cert::runCertTuner (o);
            }
            // TONE-CHECK-ONLY (v1.7): re-derive, re-export and tone-check every exported record of a cert folder; no sweeps.
            if (a == "--cert-tonecheck-all")
            {
                ejmap::cert::SweepOptions o;
                for (int j = 1; j < argc; ++j)
                {
                    const auto k = argAt (argc, argv, j); const auto v = argAt (argc, argv, j + 1);
                    if      (k == "--out"           && j + 1 < argc) o.out = cwdFile (v);
                    else if (k == "--probe"         && j + 1 < argc) o.probe = cwdFile (v);
                    else if (k == "--ejmap-ledger"  && j + 1 < argc) o.ledger = cwdFile (v);
                    else if (k == "--timeout-s"     && j + 1 < argc) o.timeoutMs = juce::jmax (1, v.getIntValue()) * 1000;
                    else if (k == "--slice"         && j + 1 < argc) { for (const auto& l : juce::StringArray::fromLines (cwdFile (v).loadFileAsString())) if (l.trim().isNotEmpty()) o.slice.add (l.trim()); }
                    else if (k == "--retry-licence")                 o.retryLicence = true;
                    else if (k == "--derive-only")                   o.deriveOnly = true;
                }
                ejmap::cert::resolveCertPaths (o, juce::File::getSpecialLocation (juce::File::currentExecutableFile));
                return ejmap::cert::runToneCheckAll (o);
            }
            if (a == "--cert-sweep" || a == "--cert-sweep-all")
            {
                ejmap::cert::SweepOptions o;
                juce::StringArray skip;
                o.hostVersion = EJMAP_VERSION;
                for (int j = 1; j < argc; ++j)
                {
                    const auto k = argAt (argc, argv, j);
                    const auto v = argAt (argc, argv, j + 1);
                    if      (k == "--fixtures"  && j + 1 < argc) o.fixtures = cwdFile (v);
                    else if (k == "--probe"     && j + 1 < argc) o.probe = cwdFile (v);
                    else if (k == "--out"       && j + 1 < argc) o.out = cwdFile (v);
                    else if (k == "--product"   && j + 1 < argc) o.product = v;
                    else if (k == "--arm"       && j + 1 < argc) o.armLabel = v;
                    else if (k == "--ejmap-ledger" && j + 1 < argc) o.ledger = cwdFile (v);
                    else if (k == "--skip"      && j + 1 < argc) skip.add (v);
                    else if (k == "--set"       && j + 1 < argc)
                        o.extraSets.push_back ({ v.upToFirstOccurrenceOf (":", false, false).getIntValue(),
                                                 (float) v.fromFirstOccurrenceOf (":", false, false).getDoubleValue() });
                    else if (k == "--timeout-s" && j + 1 < argc) o.timeoutMs = juce::jmax (1, v.getIntValue()) * 1000;
                    else if (k == "--slice" && j + 1 < argc) { for (const auto& l : juce::StringArray::fromLines (cwdFile (v).loadFileAsString())) if (l.trim().isNotEmpty()) o.slice.add (l.trim()); }   // the dress rehearsal only
                    else if (k == "--reset-per-hold")            o.resetPerHold = true;
                    else if (k == "--include-pace")              std::cout << "ignored: --include-pace (ruled 2 Oct: the window is the evidence; the scan's licence stops are carried forward)" << std::endl;
                    else if (k == "--retry-licence")             o.retryLicence = true;
                    else if (k == "--retry-refused")             o.retryRefused = true;
                    else if (k == "--retry-refused-all")         o.retryRefused = o.retryAll = true;
                    else if (k == "--profile")                   o.profile = true;
                }
                // THE DEFAULTS ARE THE HANDOVER PATH (EjmapCertDriver.h resolveCertPaths): ~/Library/ejmap/cert, its
                // fixtures/ as the store, the probe beside this executable. A mapper types none of them.
                ejmap::cert::resolveCertPaths (o, juce::File::getSpecialLocation (juce::File::currentExecutableFile));
                const bool all = a == "--cert-sweep-all";
                if ((o.product.isEmpty() != all) || (o.extraSets.empty() != o.armLabel.isEmpty()) || (all && ! o.extraSets.empty()))
                {
                    std::cerr << "usage: ejmap --cert-sweep --product <name>   (or --cert-sweep-all [--skip NAME]...)\n"
                                 "       [--fixtures <dir>  default ~/Library/ejmap/cert/fixtures] [--out <dir>  default ~/Library/ejmap/cert]\n"
                                 "       [--probe <EchoJayProbe>  default: beside ejmap] [--include-pace] [--profile] [--retry-refused | --retry-refused-all] [--timeout-s N per process]\n"
                                 "       [--arm LABEL --set IDX:NORM ...] [--reset-per-hold]" << std::endl;
                    return 2;
                }
                return all ? ejmap::cert::runSweepAll (o, skip) : ejmap::cert::runCertSweep (o);
            }
            if (a == "--cert-defaults")
            {
                ejmap::cert::Options o;
                for (int j = 1; j < argc; ++j)
                {
                    const auto k = argAt (argc, argv, j);
                    const auto v = argAt (argc, argv, j + 1);
                    if      (k == "--fixtures"      && j + 1 < argc) o.fixtures = cwdFile (v);
                    else if (k == "--probe"         && j + 1 < argc) o.probe = cwdFile (v);
                    else if (k == "--out"           && j + 1 < argc) o.out = cwdFile (v);
                    else if (k == "--timeout-s"     && j + 1 < argc) o.timeoutMs = juce::jmax (1, v.getIntValue()) * 1000;
                    else if (k == "--sign-identity" && j + 1 < argc) o.signIdentity = v;
                    else if (k == "--entitlements"  && j + 1 < argc) o.entitlements = cwdFile (v);
                    else if (k == "--include-pace")                  o.includePace = true;
                }
                ejmap::cert::resolveCertPaths (o, juce::File::getSpecialLocation (juce::File::currentExecutableFile));
                return ejmap::cert::runCertDefaults (o);
            }
        }

        // --registry-report [substring]
        // Every AU component the registry holds, resolved through the SAME
        // describeFromRegistry the scan and every load path use, printed as
        // identifier / name / uid / version. Headless and read-only: it
        // touches no ledger, so it can be run against a live session.
        //
        // Built 4 Aug 2026 to answer whether identities collapse. It is the
        // check that would have caught the space-padded-subtype defect on the
        // day it landed, because a collapse is invisible one plugin at a time
        // and obvious in a column of uids.
        for (int i = 1; i < argc; ++i)
            if (juce::String (argv[i]) == "--registry-report")
            {
                const juce::String filter = (i + 1 < argc && argv[i + 1][0] != '-')
                                              ? juce::String (argv[i + 1]).unquoted().toLowerCase()
                                              : juce::String();
                auto census = echojay::auregistry::buildCensus();
                std::cout << "registry: " << census.targets.size() << " components" << std::endl;
                std::map<juce::String, int> uidCount;
                juce::Array<juce::String> lines;
                int unresolved = 0;
                for (const auto& t : census.targets)
                {
                    auto d = echojay::auregistry::describeFromRegistry (t.identifier);
                    const auto uid = juce::String::toHexString (d.uniqueId);
                    if (d.name.isEmpty()) ++unresolved;
                    else ++uidCount[uid];
                    if (filter.isEmpty() || t.identifier.toLowerCase().contains (filter)
                         || d.name.toLowerCase().contains (filter)
                         || d.manufacturerName.toLowerCase().contains (filter))
                        lines.add (t.identifier + "\t" + (d.name.isEmpty() ? "(UNRESOLVED)" : d.name)
                                     + "\t" + uid + "\t" + d.version);
                }
                for (const auto& l : lines) std::cout << l << std::endl;
                int shared = 0, entriesOnShared = 0;
                for (auto& kv : uidCount)
                    if (kv.second > 1) { ++shared; entriesOnShared += kv.second; }
                std::cout << "resolved with a name: " << (census.targets.size() - unresolved)
                          << " | unresolved (refused, not guessed): " << unresolved
                          << " | uids shared by >1 component: " << shared
                          << " covering " << entriesOnShared << " components" << std::endl;
                return 0;
            }

        for (int i = 1; i < argc; ++i)
        {
        if (juce::String (argv[i]) == "--tester" && i + 1 < argc)
        {
            // EXPLICIT local tester name for provenance (M10). Never derived
            // from the hostname. File-touching, so it lives here where the
            // single-instance check can never bounce it.
            auto root = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                            .getChildFile ("ejmap");
            for (int j = 1; j < argc; ++j)
                if (juce::String (argv[j]) == "--ledger-root" && j + 1 < argc)
                    root = juce::File::getCurrentWorkingDirectory().getChildFile (argv[j + 1]);
            root.createDirectory();
            const juce::String name = juce::String (argv[i + 1]).unquoted();
            // The name ends up on an HTTP header line. Refuse here, where the
            // refusal can say so, not at upload time three sessions later.
            if (name.isEmpty() || ! ejmap::Mouth::headerValueSafe (name))
            {
                std::cerr << "tester: '" << name << "' cannot ride an HTTP header "
                          << "(printable ASCII only, no control characters)" << std::endl;
                return 5;
            }
            auto f = root.getChildFile ("tester.json");
            auto* o = new juce::DynamicObject();
            o->setProperty ("name", name);
            f.replaceWithText (juce::JSON::toString (juce::var (o), false));
            const auto back = juce::JSON::parse (f.loadFileAsString())
                                  .getProperty ("name", "").toString();
            if (back != name)
            { std::cerr << "tester: write failed" << std::endl; return 4; }
            std::cout << "tester: provenance name set to \"" << back << "\"" << std::endl;
            return 0;
        }

            const auto a = argAt (argc, argv, i);
            if (a == "--sign-in" && i + 1 < argc) signIn = argAt (argc, argv, ++i);
            else if (a == "--whoami") whoami = true;
            else if (a == "--release-quarantine" && i + 1 < argc) release = argAt (argc, argv, ++i);
            else if (a == "--retry-evidence" && i + 1 < argc)
            {
                evidenceId = argAt (argc, argv, ++i);
                const auto nxt = argAt (argc, argv, i + 1);
                if (nxt.isNotEmpty() && ! nxt.startsWith ("--")) evidenceStage = argAt (argc, argv, ++i);
            }
            else if (a == "--retest-nondeterministic") retest = true;
            else if (a == "--apply") apply = true;
            else if (a == "--attribute-report" && i + 1 < argc) report = argAt (argc, argv, ++i);
            else if (a == "--quarantine" && i + 2 < argc)
            {
                qId = argAt (argc, argv, i + 1); qReason = argAt (argc, argv, i + 2); i += 2;
                const auto nxt = argAt (argc, argv, i + 1);
                if (nxt.isNotEmpty() && ! nxt.startsWith ("--")) qStage = argAt (argc, argv, ++i);
            }
        }

        if (release.isEmpty() && qId.isEmpty() && report.isEmpty()
             && evidenceId.isEmpty() && ! retest && signIn.isEmpty() && ! whoami)
            return -1;

        juce::ScopedJuceInitialiser_GUI juceInit;
        const auto root = ledgerRootFrom (argc, argv);

        if (report.isNotEmpty())
        {
            const auto f = juce::File::getCurrentWorkingDirectory().getChildFile (report);
            if (! f.existsAsFile())
            {
                std::cerr << "attribute-report: no such file: " << f.getFullPathName() << std::endl;
                return 2;
            }
            const auto facts = ejmap::Ledger::factsFromReport (f);
            std::cout << "attribute-report: " << f.getFileName() << "\n"
                      << "  parsed      : " << (facts.found ? "yes" : "no") << "\n"
                      << "  thread      : " << (facts.threadName.isEmpty() ? "(unnamed)" : facts.threadName) << "\n"
                      << "  top image   : " << (facts.topImage.isEmpty() ? "?" : facts.topImage) << "\n"
                      << "  attribution : " << facts.attribution << std::endl;
            return facts.found ? 0 : 2;
        }

        // SIGN IN. One token, pasted once, stored 0600 -- and read back through
        // the same path that will use it, because a write that reports success
        // and a read that refuses is the shape worth catching here rather than
        // at the next send.
        if (signIn.isNotEmpty())
        {
            const auto err = ejmap::Mouth::saveMapperToken (root, signIn);
            if (err.isNotEmpty())
            { std::cerr << "sign-in: " << err << std::endl; return 4; }
            std::cout << "sign-in: " << ejmap::Mouth::resolveMapper (root).describe() << std::endl;
            return 0;
        }

        if (whoami)
        {
            std::cout << ejmap::Mouth::resolveMapper (root).describe() << std::endl;
            return ejmap::Mouth::resolveMapper (root).signedIn() ? 0 : 3;
        }

        ejmap::Ledger l (root);

        // WHY a plugin is or is not quarantined, read out of the ledger. The
        // numbers were always there; nothing read them, so the decision was
        // made blind and the operator could not see it either. prior_ok 0 means
        // do not bother; prior_ok 4 means retry.
        if (evidenceId.isNotEmpty())
        {
            const auto ev = l.retryEvidenceFor (evidenceId, evidenceStage);

            // THE NON-DEATH FAILURES ARE PRINTED, and their absence here used
            // to be the whole defect wearing an operator surface: this screen
            // showed `bloom` as "failures 1" while eighteen init_failed rows
            // for it sat in the ledger, so the one place built to explain a
            // quarantine decision agreed with the decision by leaving out the
            // same evidence.
            //
            // Both counts are shown separately rather than summed, because
            // they are answers to different questions -- a death is a fact
            // about hosting the plugin, an init_failed is a fact about
            // instantiating it -- and only the death count is ever discounted.
            std::cout << "retry-evidence: " << evidenceId << "  (stage " << evidenceStage << ")\n"
                      << "  quarantined           : " << (l.isQuarantined (evidenceId) ? "yes" : "no") << "\n"
                      << "  attempts at this stage: " << ev.attempts << "\n"
                      << "  deaths                : " << ev.failures
                      << "  (" << ev.corroboratedFailures << " corroborated, "
                      << ev.unattributedFailures << " unattributed -- counted only when the "
                                                    "run was unattended)\n"
                      << "  other failures        : " << ev.otherFailures
                      << "  (init_failed, timeout, sweep_timeout, no_params: the process "
                         "survived to report each, so none is ever discounted)\n"
                      << "  counted, unattended   : " << ev.countedFailures (true) << "\n"
                      << "  counted, attended     : " << ev.countedFailures (false) << "\n"
                      << "  threshold             : " << ejmap::kRetryAttempts
                      << "  (a hang is 2, or 1 at stage scan)\n"
                      << "  prior ok at this stage: " << ev.priorOkInLedger << "\n"
                      << "  outcomes              : " << ev.outcomes.joinIntoString (", ") << std::endl;
            if (ev.nonDeterministic())
                std::cout << "  " << ev.note() << std::endl;
            if (ev.attempts == 0)
                std::cout << "  (no rows at this stage. A load failure is vouched for only by\n"
                          << "   prior LOAD successes -- try --retry-evidence <id> scan)" << std::endl;
            return 0;
        }

        // THE NIGHTLY RE-TEST. Release on binary change alone leaves the nine
        // non-deterministic plugins waiting on a change that may never come:
        // nothing about them is broken, they simply lost a roll. Dry run by
        // default, the same discipline as every other batch tool here.
        if (retest)
        {
            const auto rows = l.nonDeterministicQuarantine();
            std::cout << "retest-nondeterministic: " << rows.size()
                      << " quarantine(s) with prior successes at the failing stage" << std::endl;
            for (const auto& e : rows)
            {
                std::cout << "  " << e.pluginId << "\n"
                          << "      " << e.reason << " at " << e.at
                          << ", " << e.evidence.corroboratedFailures << " corroborated failure(s), "
                          << e.evidence.priorOkInLedger << " prior ok\n"
                          << "      " << e.evidence.note() << std::endl;
                if (apply)
                {
                    l.releaseFromQuarantine (e.pluginId);
                    std::cout << "      -> released for re-test" << std::endl;
                }
            }
            if (! apply && rows.size() > 0)
                std::cout << "Dry run. Nothing changed. Add --apply to release these." << std::endl;
            return 0;
        }

        if (release.isNotEmpty())
        {
            const bool was = l.isQuarantined (release);
            l.releaseFromQuarantine (release);
            const bool now = l.isQuarantined (release);

            // Report the OUTCOME, verified by reading back, not the intent.
            if (! was)
            {
                std::cerr << "release-quarantine: " << release
                          << " was not quarantined; nothing to do" << std::endl;
                return 3;
            }
            if (now)
            {
                std::cerr << "release-quarantine: " << release
                          << " FAILED, still quarantined after the write" << std::endl;
                return 4;
            }
            std::cout << "release-quarantine: " << release << " -> released" << std::endl;
            return 0;
        }

        l.quarantine (qId, qReason, qStage);
        if (! l.isQuarantined (qId))
        {
            std::cerr << "quarantine: " << qId << " FAILED, not present after the write" << std::endl;
            return 4;
        }
        std::cout << "quarantine: " << qId << " -> " << qReason
                  << " (stage " << qStage << ")" << std::endl;
        return 0;
    }

    /** Single-instance, enforced loudly. A stale lock from a watchdog _Exit is
        ignored by checking the pid is actually alive.
    */
    bool anotherInstanceIsLive (const juce::File& root, int& otherPid)
    {
        auto lock = root.getChildFile ("instance.lock");
        if (! lock.existsAsFile())
            return false;

        otherPid = lock.loadFileAsString().trim().getIntValue();
        if (otherPid <= 0 || otherPid == (int) getpid())
            return false;

        return ::kill ((pid_t) otherPid, 0) == 0;
    }
}

/** Whether THIS launch should supervise itself.

    A mapper double-clicks an app. They will never type --supervise, and the
    relaunch after a crash is the thing that has to be invisible -- so a GUI
    launch supervises by default and the flag becomes the way to say NO.

    THREE EXCLUSIONS, each for a reason:

      --child          this IS the supervised process. Supervising it would
                       fork forever.
      --selftest-*     a diagnostic that CRASHES ON PURPOSE. Relaunching it
                       would loop, and --selftest-segv exists precisely to die.
      --no-supervise   the escape hatch, for debugging under a debugger, where
                       a fork puts the crash in a process the debugger is not
                       attached to.

    Headless flags need no exclusion: runHeadlessCli returns first and this is
    never reached.
*/
static bool shouldSelfSupervise (int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        if (a == "--child" || a == "--no-supervise" || a.startsWith ("--selftest")
             || a == "--gate-m9")
            return false;
    }
    return true;
}

int main (int argc, char* argv[])
{
    // The supervisor must run before any GUI exists, so it is handled here
    // rather than in initialise().
    for (int i = 1; i < argc; ++i)
        if (juce::String (argv[i]) == "--supervise")
            return ejmap::runSupervisor (argc, argv);

    // File-touching flags first: they must never be bounced.
    const int cliResult = runHeadlessCli (argc, argv);
    if (cliResult >= 0)
        return cliResult;

    // A GUI launch supervises itself. This sits AFTER the headless CLI and
    // BEFORE the instance lock on purpose: the parent must not claim the lock,
    // or the child it spawns would refuse to start against it.
    if (shouldSelfSupervise (argc, argv))
        return ejmap::runSupervisor (argc, argv);

    // A GUI launch that would have been bounced now SAYS SO and fails.
    {
        juce::ScopedJuceInitialiser_GUI juceInit;
        auto root = ledgerRootFrom (argc, argv);
        if (root == juce::File())
            root = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                     .getChildFile ("ejmap");
        root.createDirectory();

        int otherPid = 0;
        if (anotherInstanceIsLive (root, otherPid))
        {
            std::cerr << "ejmap: another instance is already running (pid " << otherPid << ").\n"
                      << "       Refusing to start a second one: they would share one ledger, one\n"
                      << "       quarantine file and one scan cache.\n"
                      << "       This used to exit 0 silently, which made every flag a no-op."
                      << std::endl;
            return 5;
        }

        root.getChildFile ("instance.lock").replaceWithText (juce::String ((int) getpid()));
    }

    juce::JUCEApplicationBase::createInstance = &juce_CreateApplication;
    return juce::JUCEApplicationBase::main (argc, (const char**) argv);
}
