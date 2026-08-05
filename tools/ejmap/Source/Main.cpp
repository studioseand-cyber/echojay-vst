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
        bool cacheTest = false, progressTest = false;
        juce::String attributeReport, afterExit, captureTestId, maskTestId, stallId, promoSuppressId;
        juce::String sweepTestId, sweepTestParam, typedTestId, typedTestParam, assignTestId;
        juce::String bandTestId, bandTestMembers, bandTestImposter, catTestId, dupTestId;
        juce::String fitTestId; int fitHoldSeconds = 0;
        juce::String mbandsTestId, mbandsMode;
        juce::String conlyTestId, conlyCat;
        bool worklist = false, worklistNext = false;
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
            else if (args[i] == "--next")
                worklistNext = true;
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
        else if (worklist && mainWindow->getMain() != nullptr)
        {
            auto* m = mainWindow->getMain(); const bool nx = worklistNext;
            juce::MessageManager::callAsync ([m, nx] { m->printWorklist (nx); });
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
            if (a == "--release-quarantine" && i + 1 < argc) release = argAt (argc, argv, ++i);
            else if (a == "--attribute-report" && i + 1 < argc) report = argAt (argc, argv, ++i);
            else if (a == "--quarantine" && i + 2 < argc)
            {
                qId = argAt (argc, argv, i + 1); qReason = argAt (argc, argv, i + 2); i += 2;
                const auto nxt = argAt (argc, argv, i + 1);
                if (nxt.isNotEmpty() && ! nxt.startsWith ("--")) qStage = argAt (argc, argv, ++i);
            }
        }

        if (release.isEmpty() && qId.isEmpty() && report.isEmpty())
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

        ejmap::Ledger l (root);

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

int main (int argc, char* argv[])
{
    // The supervisor must run before any GUI exists, so it is handled here
    // rather than in initialise(). A direct launch skips it entirely, which is
    // what every headless flag relies on.
    for (int i = 1; i < argc; ++i)
        if (juce::String (argv[i]) == "--supervise")
            return ejmap::runSupervisor (argc, argv);

    // File-touching flags first: they must never be bounced.
    const int cliResult = runHeadlessCli (argc, argv);
    if (cliResult >= 0)
        return cliResult;

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
