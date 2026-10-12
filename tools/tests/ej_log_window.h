// ej_log_window.h - reading the plugin's rolling log from a guard, ACROSS A ROTATION.
//
// 12 Oct 2026. Three legs in ui_round_guard read the log by marking the LENGTH of
// FileLog::currentPath() and later re-reading that path and taking substring(mark). That assumes
// currentPath() still names the same file. It does not: FileLog keeps five 2 MB files and moves to
// another when one fills, and ui_guard emits ~175,000 lines per run. When a rotation lands inside a
// leg, the substring slices past the whole of the NEW file and the leg reports that the product said
// nothing - which is indistinguishable from the product actually having gone quiet, and is exactly
// the failure these legs exist to catch. Two separate gates went RED on two different legs of this
// one shape, each passing in the other run purely because the rotation fell elsewhere.
//
// The technique was only ever safe because rotation was BROKEN: every writer tracked its own byte
// count and never re-stat'd the file, so files sailed past the cap (one was measured at 2,433,420
// bytes against a 2 MB cap on 11 Oct) and the current file effectively never changed identity. The
// rotation fix made that assumption false, so the guards have to read across it.
//
// TWO RULES HERE:
//   * a window is {path, length, instant} - never a bare offset, because the offset alone cannot
//     tell "nothing was written" from "it was written somewhere else";
//   * when the window cannot be reconstructed, SAY SO through `how`. A leg that fails for the wrong
//     reason costs more than a leg that fails.
#pragma once
#include <JuceHeader.h>
#include "EchoJayFileLog.h"

namespace ejtest
{
    struct LogMark
    {
        juce::String path;
        int          length = 0;
        juce::Time   at;
    };

    inline juce::File logDir()
    {
        return juce::File (juce::String (echojay::FileLog::instance().currentPath())).getParentDirectory();
    }

    // Every rolling file, concatenated. For a leg that only asks "did this line ever appear", this is
    // both the simplest and the safest read: no offset to be invalidated.
    inline juce::String logAllText()
    {
        juce::String out;
        juce::Array<juce::File> fs;
        logDir().findChildFiles (fs, juce::File::findFiles, false, "echojay-*.log");
        for (const auto& f : fs) out << f.loadFileAsString();
        return out;
    }

    inline LogMark markLog()
    {
        const juce::String p (echojay::FileLog::instance().currentPath());
        return { p, juce::File (p).loadFileAsString().length(), juce::Time::getCurrentTime() };
    }

    // The text written since `m`, following however many rotations happened. `how` records what it
    // had to do, for the leg's own failure message.
    inline juce::String logSince (const LogMark& m, juce::String& how)
    {
        const juce::File marked (m.path);
        juce::String out;

        if (marked.existsAsFile())
        {
            const auto all = marked.loadFileAsString();
            if (all.length() >= m.length)
            {
                out << all.substring (m.length);
                how << "marked file from offset " << m.length << "; ";
            }
            else
            {
                // rotated away and rewritten: the offset means nothing now
                out << all;
                how << "marked file was REWRITTEN (now " << all.length() << "b < mark " << m.length << "b), taken whole; ";
            }
        }
        else
        {
            how << "the marked file is GONE (deleted by a rotation); ";
        }

        juce::Array<juce::File> sibs;
        logDir().findChildFiles (sibs, juce::File::findFiles, false, "echojay-*.log");
        struct ByTime
        {
            static int compareElements (const juce::File& a, const juce::File& b)
            {
                const auto x = a.getLastModificationTime(), y = b.getLastModificationTime();
                return x < y ? -1 : (y < x ? 1 : 0);
            }
        };
        ByTime c; sibs.sort (c);

        int extra = 0;
        for (const auto& f : sibs)
        {
            if (f.getFullPathName() == marked.getFullPathName()) continue;
            if (f.getLastModificationTime() < m.at)              continue;   // already full before the mark
            out << f.loadFileAsString(); ++extra;
        }
        if (extra > 0)    how << extra << " rotated-into file(s) read whole; ";
        if (out.isEmpty()) how << "NOTHING readable - do NOT read this as 'the product said nothing'; ";
        return out;
    }
}
