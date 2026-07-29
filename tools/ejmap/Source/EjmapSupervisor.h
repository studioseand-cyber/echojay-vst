/*
  EjmapSupervisor.h

  Relaunches ejmap after an abnormal exit.

  WHY A SEPARATE PROCESS, and not a signal handler or an exit hook.

  Two exit paths need covering and they have nothing in common from inside the
  process:

    - the watchdog's controlled _Exit(87), which deliberately runs no
      destructors and no atexit handlers, precisely so a wedged process cannot
      deadlock in teardown. Anything we hooked there would defeat the reason it
      exists.

    - an uncontrolled SIGSEGV. A handler for that runs on a corrupted process,
      under async-signal-safety rules that permit almost nothing: no malloc, no
      iostreams, no JUCE. Worse, every crash measured on this machine faulted on
      a thread we do not own (the audio pump, or a plugin's own worker), so the
      handler would run on a foreign stack after a fault that may itself be heap
      corruption.

  A parent sees both identically and reliably, from outside the wreckage:
  WIFEXITED(87) and WIFSIGNALED(SIGSEGV) are the same kind of fact to waitpid.
  It needs no code running in the dying process, touches no ledger, and links no
  JUCE GUI. ejextract already supervises its isolated workers this way.

  THE GUARD IS THE POINT. A loop that keeps restarting into a crash is worse
  than no auto-relaunch, because it hides that something is broken. Two
  independent limits:

    - kMaxConsecutive abnormal exits with NO successful load between them. The
      counter resets on a successful LOAD, not a successful launch: a crash
      during scan, or before the window appears, would otherwise reset it every
      time and spin forever.

    - kMaxFastDeaths deaths inside kFastDeathMs. That is the shape of a crash
      before the window exists, where no load can ever happen to clear the first
      counter.
*/

#pragma once

#include <juce_core/juce_core.h>

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

#include <iostream>
#include <vector>

namespace ejmap
{

/** Watchdog's exit code, duplicated here so the supervisor links nothing. */
inline constexpr int kSupervisorWatchdogExit = 87;

struct SupervisorLimits
{
    static constexpr int kMaxConsecutive = 3;
    static constexpr int kMaxFastDeaths  = 3;
    static constexpr int kFastDeathMs    = 10000;

    /** Hard ceiling per supervisor session, independent of every reset.

        Without it the guard has a hole: a child that manages one successful
        load and then dies resets the consecutive counter every time, so the
        loop never terminates. Measured, not reasoned: the first version of this
        file did exactly that and had to be killed by a test timeout. Any rule
        that can be reset needs a bound that cannot.
    */
    static constexpr int kMaxTotalRestarts = 10;
};

/** Written by the child the first time a load succeeds. The supervisor deletes
    it before each spawn and checks for it after, which is what makes "no
    successful load between them" measurable rather than assumed.
*/
inline juce::File loadOkMarker (const juce::File& root)
{
    return root.getChildFile ("load-ok.marker");
}

//==============================================================================
inline int runSupervisor (int argc, char* argv[])
{
    const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);

    // Mirror the child's --ledger-root so the marker and the log land together.
    juce::File root;
    for (int i = 1; i < argc; ++i)
        if (juce::String (argv[i]) == "--ledger-root" && i + 1 < argc)
            root = juce::File::getCurrentWorkingDirectory().getChildFile (argv[i + 1]);

    if (root == juce::File())
        root = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("ejmap");
    root.createDirectory();

    const auto marker = loadOkMarker (root);

    // Arguments to hand the child: everything except --supervise.
    std::vector<juce::String> base;
    for (int i = 1; i < argc; ++i)
        if (juce::String (argv[i]) != "--supervise")
            base.push_back (juce::String (argv[i]));

    // A fresh supervisor launch is a fresh session. Restart budget starts full.
    int consecutive = 0, fastDeaths = 0, totalRestarts = 0;
    juce::String lastCause;

    for (;;)
    {
        marker.deleteFile();

        std::vector<juce::String> childArgs = base;
        childArgs.push_back ("--child");
        if (consecutive > 0)
        {
            childArgs.push_back ("--restarted");
            childArgs.push_back (juce::String (consecutive));
            childArgs.push_back ("--after-exit");
            childArgs.push_back (lastCause);
        }

        std::vector<std::string> storage;
        storage.push_back (exe.getFullPathName().toStdString());
        for (const auto& a : childArgs)
            storage.push_back (a.toStdString());

        std::vector<char*> cargv;
        for (auto& sref : storage)
            cargv.push_back (const_cast<char*> (sref.c_str()));
        cargv.push_back (nullptr);

        const auto t0 = juce::Time::getMillisecondCounter();

        const pid_t pid = fork();
        if (pid < 0)
        {
            std::cerr << "supervisor: fork failed" << std::endl;
            return 1;
        }

        if (pid == 0)
        {
            execv (cargv[0], cargv.data());
            _exit (127);            // exec failed; nothing safe left to do
        }

        int status = 0;
        while (waitpid (pid, &status, 0) < 0 && errno == EINTR) {}

        const auto elapsed = (int) (juce::Time::getMillisecondCounter() - t0);
        const bool hadLoad = marker.existsAsFile();

        if (WIFEXITED (status) && WEXITSTATUS (status) == 0)
        {
            std::cout << "supervisor: child exited cleanly" << std::endl;
            return 0;
        }

        lastCause = WIFSIGNALED (status)
                      ? "signal:" + juce::String (WTERMSIG (status))
                      : "code:"   + juce::String (WIFEXITED (status) ? WEXITSTATUS (status) : -1);

        // Reset BEFORE counting this one: the budget is consecutive abnormal
        // exits with no successful load between them.
        // A successful load clears the consecutive counter, but NOT the
        // fast-death counter: three deaths inside ten seconds each is
        // pathological whether or not a load happened in between.
        if (hadLoad)
            consecutive = 0;

        ++consecutive;
        ++totalRestarts;
        fastDeaths = (elapsed < SupervisorLimits::kFastDeathMs) ? fastDeaths + 1 : 0;

        std::cout << "supervisor: child died (" << lastCause << ") after " << elapsed
                  << "ms; consecutive=" << consecutive
                  << " fastDeaths=" << fastDeaths
                  << " total=" << totalRestarts
                  << " loadSucceeded=" << (hadLoad ? "yes" : "no") << std::endl;

        if (consecutive >= SupervisorLimits::kMaxConsecutive)
        {
            std::cerr << "supervisor: STOPPING. " << consecutive
                      << " abnormal exits in a row with no successful load between them ("
                      << lastCause << "). Launch again manually once the cause is understood."
                      << std::endl;
            return 1;
        }

        if (fastDeaths >= SupervisorLimits::kMaxFastDeaths)
        {
            std::cerr << "supervisor: STOPPING. " << fastDeaths
                      << " deaths inside " << SupervisorLimits::kFastDeathMs
                      << "ms, so the session is dying before it can be used ("
                      << lastCause << ")." << std::endl;
            return 1;
        }

        if (totalRestarts >= SupervisorLimits::kMaxTotalRestarts)
        {
            std::cerr << "supervisor: STOPPING. " << totalRestarts
                      << " restarts this session, which is the hard ceiling. Something is"
                         " wrong with this machine's plugin set, not with one plugin."
                      << std::endl;
            return 1;
        }

        std::cout << "supervisor: relaunching (consecutive " << consecutive << " of "
                  << SupervisorLimits::kMaxConsecutive << ", total " << totalRestarts
                  << " of " << SupervisorLimits::kMaxTotalRestarts << ")" << std::endl;
    }
}

} // namespace ejmap
