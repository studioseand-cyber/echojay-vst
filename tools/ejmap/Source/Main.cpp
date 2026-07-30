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

// Generated on every build by cmake/StampBuildInfo.cmake. Carries the git short
// hash of the commit actually compiled, plus "-dirty" when tracked files were
// modified. Deliberately has no #ifndef fallback: a build with no stamp should
// fail to compile rather than quietly claim a version it cannot know.
#include "EjmapBuildInfo.h"
#include "EjmapSupervisor.h"

#include <csignal>

class EjmapApplication  : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "ejmap"; }
    const juce::String getApplicationVersion() override { return EJMAP_VERSION; }
    bool moreThanOneInstanceAllowed() override          { return false; }

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
        juce::String attributeReport, afterExit, captureTestId, maskTestId, stallId;
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
            else if (args[i] == "--selftest-stall" && i + 2 < args.size())
                { stallId = args[i + 1]; stallN = args[i + 2].getIntValue(); i += 2; }
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

        // Headless release, so a quarantine can be undone through the same code
        // path the button uses rather than by hand-editing quarantine.json.
        if (releaseId.isNotEmpty())
        {
            ejmap::Ledger l (ledgerRoot);
            const bool was = l.isQuarantined (releaseId);
            l.releaseFromQuarantine (releaseId);
            std::cout << "release-quarantine: " << releaseId
                      << (was ? " -> released" : " -> was not quarantined") << std::endl;
            quit();
            return;
        }

        // Run the attribution logic against one crash report on disk, so it can
        // be checked against real reports rather than synthetic ones.
        if (attributeReport.isNotEmpty())
        {
            const juce::File f = juce::File::getCurrentWorkingDirectory().getChildFile (attributeReport);
            const auto facts = ejmap::Ledger::factsFromReport (f);
            std::cout << "attribute-report: " << f.getFileName() << "\n"
                      << "  parsed      : " << (facts.found ? "yes" : "no") << "\n"
                      << "  thread      : " << (facts.threadName.isEmpty() ? "(unnamed)" : facts.threadName) << "\n"
                      << "  top image   : " << (facts.topImage.isEmpty() ? "?" : facts.topImage) << "\n"
                      << "  attribution : " << facts.attribution << std::endl;
            quit();
            return;
        }

        if (quarantineId.isNotEmpty())
        {
            ejmap::Ledger l (ledgerRoot);
            l.quarantine (quarantineId, quarantineReason, quarantineStage);
            std::cout << "quarantine: " << quarantineId << " -> " << quarantineReason
                      << " (stage " << quarantineStage << ")" << std::endl;
            quit();
            return;
        }

        supervisedMode = supervised;
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
        if (stallId.isNotEmpty() && mainWindow->getMain() != nullptr)
        {
            auto id = stallId; auto n = stallN; auto* m = mainWindow->getMain();
            juce::MessageManager::callAsync ([m, id, n] { m->selfTestStall (id, n); });
        }
        else if (maskTestId.isNotEmpty() && mainWindow->getMain() != nullptr)
        {
            auto id = maskTestId; auto* m = mainWindow->getMain();
            juce::MessageManager::callAsync ([m, id] { m->selfTestNoiseMask (id); });
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

    void shutdown() override { mainWindow = nullptr; }

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

juce::JUCEApplicationBase* juce_CreateApplication();
juce::JUCEApplicationBase* juce_CreateApplication() { return new EjmapApplication(); }

int main (int argc, char* argv[])
{
    // The supervisor must run before any GUI exists, so it is handled here
    // rather than in initialise(). A direct launch skips it entirely, which is
    // what every headless flag relies on.
    for (int i = 1; i < argc; ++i)
        if (juce::String (argv[i]) == "--supervise")
            return ejmap::runSupervisor (argc, argv);

    juce::JUCEApplicationBase::createInstance = &juce_CreateApplication;
    return juce::JUCEApplicationBase::main (argc, (const char**) argv);
}
