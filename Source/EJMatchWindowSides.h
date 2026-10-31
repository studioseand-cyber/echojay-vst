#pragma once

#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <vector>
#include "EJMatchProposal.h"   // MatchSide
#include "EJMatchWindow.h"     // MatchWindowMode, HopRange, loudest30Window and its refusal

// =============================================================================
//  K1: THE MATCH WINDOW ON THE PAGE. Which window each reference is compared
//  over, and the two sides as that window sees them.
// =============================================================================
//
// DESIGN_MATCH_TAB rev 3, D4 and K1; C4 rev 4 section 7; C3 rev 3 section 2.
// The page reads the mix slot's own capture (MatchHost::captureForSlot, round
// 5a) for the mix's hop series and MatchHost::selectedReference()'s for the
// reference (C2 section 5.2), and this header turns the two whole-file sides
// into the window's. EJMatchHandOff.h is where the page asks.
//
// WHAT THE WINDOW CHANGES, AND WHAT IT LEAVES (C3 rev 2 point 1). In "loudest30"
// the six bands come from the window's hops on each side. integrated stays the
// WHOLE FILE in both windows, and the duration stays the side's own length, so
// the duration refusals still judge the measurement rather than the window.
// PSR and PLR are unavailable in "loudest30" (C4 section 7, C3's rule).
//
// NO SILENT FALLBACK. A side that cannot be windowed (Live, no hop series, or
// under 30 s) has no bands in "loudest30", never its whole-file bands under the
// window's name, and `why` says which. A Live mix side is never windowed: the
// one line above the button asks for a capture (contract section 2A).
//
// PURE but for the memory, which is one process-wide map. No JUCE audio type,
// no I/O.
// =============================================================================

namespace echojay
{

/** The one line's precedence-1 sentence for a Live mix side (DESIGN_MATCH_TAB
    rev 3, D4). One definition, here rather than in EJMatchPage.h since fix 8a:
    EJMatchPage.h now includes this header for the page view (matchPageFor),
    so this one cannot include it back. */
inline juce::String matchLiveNeedsCaptureLine()
{
    return "Capture at least 30 s of the chorus to make a plan.";
}

// -----------------------------------------------------------------------------
// THE MEMORY: whole song by default, remembered per reference
// -----------------------------------------------------------------------------

/** Which window each reference was last compared over, keyed by the
    reference's path. Whole song until the user picks otherwise.

    FOR THE LIFE OF THE PROCESS, NOT ACROSS SESSIONS. Keeping it across
    sessions needs the editor's state blob, which is A's. Every instance in one
    host process shares it, so a reference reads the same window wherever it is
    opened. */
class MatchWindowMemory
{
public:
    MatchWindowMode get (const juce::String& referenceKey) const
    {
        const auto it = modes.find (referenceKey);
        return it == modes.end() ? MatchWindowMode::Whole : it->second;
    }
    void set (const juce::String& referenceKey, MatchWindowMode m) { modes[referenceKey] = m; }

private:
    std::map<juce::String, MatchWindowMode> modes;
};

inline MatchWindowMemory& matchWindowMemory()
{
    static MatchWindowMemory m;
    return m;
}

/** The switch's two labels, in the order they are drawn. */
inline const char* matchWindowLabel (MatchWindowMode m) noexcept
{
    return m == MatchWindowMode::Whole ? "Whole song" : "Loudest 30 s";
}

// -----------------------------------------------------------------------------
// THE SWITCH'S GEOMETRY. Inside the graph card's top-right corner, so no rect
// of MatchPageRects moves (open list 207: the suite links across that struct).
// -----------------------------------------------------------------------------

inline constexpr int kMatchWindowSwitchW   = 176;
inline constexpr int kMatchWindowSwitchH   = 18;
inline constexpr int kMatchWindowSwitchPad = 10;

/** The whole switch, or empty when the card is too small to hold it. */
inline juce::Rectangle<int> matchWindowSwitchRect (juce::Rectangle<int> graph)
{
    const auto inner = graph.reduced (kMatchWindowSwitchPad);
    if (inner.getWidth() < kMatchWindowSwitchW || inner.getHeight() < kMatchWindowSwitchH) return {};
    return { inner.getRight() - kMatchWindowSwitchW, inner.getY(), kMatchWindowSwitchW, kMatchWindowSwitchH };
}

/** One half: 0 is "Whole song", 1 is "Loudest 30 s". */
inline juce::Rectangle<int> matchWindowSwitchHalf (juce::Rectangle<int> graph, int half)
{
    auto r = matchWindowSwitchRect (graph);
    if (r.isEmpty()) return {};
    const int w = r.getWidth() / 2;
    return half == 0 ? r.removeFromLeft (w) : r.withTrimmedLeft (w);
}

// -----------------------------------------------------------------------------
// THE SIDES OVER THE WINDOW
// -----------------------------------------------------------------------------

struct MatchWindowedSides
{
    MatchSide mix, ref;
    MatchWindowMode mode = MatchWindowMode::Whole;
    /** Both sides' bands are the window's. Always true in "whole". */
    bool windowed = true;
    /** Why a side could not be windowed, in the user's words; "" when both were. */
    juce::String why;
    std::optional<HopRange> mixWindow, refWindow;
};

/** The six bands over [w.begin, w.end), frame-weighted, so the figure is the
    mean of every FFT frame in the window: the same statistic as the whole-run
    accumulator (bandMeanFromSum), over the window. A band with no power is the
    -120 floor, as everywhere else. */
template <typename Frame>
inline std::array<float, 6> matchWindowBandsDb (const std::vector<Frame>& frames, HopRange w)
{
    std::array<double, 6> sum {};
    double n = 0.0;
    for (std::size_t h = w.begin; h < std::min (w.end, frames.size()); ++h)
    {
        const double k = frames[h].bandFrames;
        if (k <= 0.0) continue;
        n += k;
        for (std::size_t b = 0; b < 6; ++b) sum[b] += (double) frames[h].bandPower[b] * k;
    }
    std::array<float, 6> db { -120, -120, -120, -120, -120, -120 };
    if (n <= 0.0) return db;
    for (std::size_t b = 0; b < 6; ++b)
    {
        const double p = sum[b] / n;
        db[b] = p > 1e-12 ? (float) (10.0 * std::log10 (p)) : -120.0f;
    }
    return db;
}

namespace detail
{
    template <typename Frame>
    inline bool windowSide (MatchSide& s, const std::vector<Frame>* hops, const juce::String& name,
                            std::optional<HopRange>& at, juce::String& why)
    {
        if (hops == nullptr || hops->empty())
        {
            if (why.isEmpty())
                why = "The loudest 30 s needs " + name + "'s hop series, and " + name
                    + " has none; measure it again to use this window.";
            return false;
        }
        const auto L = loudest30Window (*hops);
        if (! L.window)
        {
            if (why.isEmpty()) why = loudest30RefusalMessage (name, L.sideSeconds);
            return false;
        }
        at = L.window;
        s.macro = matchWindowBandsDb (*hops, *L.window);
        s.hasMacro = true;
        for (float d : s.macro) if (d <= -119.0f) s.hasMacro = false;
        s.macroReduction = SpectralReduction::WholeWindowAverage;
        return true;
    }

    inline void unwindowed (MatchSide& s)
    {
        s.hasMacro = false;
        s.macro = { -120, -120, -120, -120, -120, -120 };
    }
}

/** The two sides over the window.

    In "whole" both sides are returned as they came, which is the whole-file
    comparison the page always made. In "loudest30" each side's bands become
    its loudest 30 s, PSR and PLR become unavailable, and a side that cannot be
    windowed loses its bands rather than keeping the whole file's. */
template <typename Frame>
inline MatchWindowedSides matchSidesForWindow (const MatchSide& mix, const MatchSide& ref,
                                               const std::vector<Frame>* mixHops,
                                               const std::vector<Frame>* refHops,
                                               MatchWindowMode mode, bool mixIsLive)
{
    MatchWindowedSides out;
    out.mix = mix; out.ref = ref; out.mode = mode;
    if (mode == MatchWindowMode::Whole) return out;

    out.mix.psr = out.ref.psr = -999.0f;
    out.mix.plr = out.ref.plr = -999.0f;
    bool okMix = false;
    if (mixIsLive) out.why = matchLiveNeedsCaptureLine();
    else okMix = detail::windowSide (out.mix, mixHops, juce::String ("your capture"), out.mixWindow, out.why);
    const bool okRef = detail::windowSide (out.ref, refHops, juce::String ("the reference"), out.refWindow, out.why);
    if (! okMix) detail::unwindowed (out.mix);
    if (! okRef) detail::unwindowed (out.ref);
    out.windowed = okMix && okRef;
    return out;
}

// -----------------------------------------------------------------------------
// THE PICTURE'S CAPTION (round 5b): which window each part of the picture is
// -----------------------------------------------------------------------------
//
// THE TWO PARTS OF THE PICTURE ARE NOT THE SAME WINDOW, AND THE CAPTION SAYS
// WHICH IS WHICH. The 64-bin curves are each slot's SpectralEvidence, and that
// is whole-file only (a reference's eqCurve, a capture's whole-capture average)
// or a live rolling reading; no 64-bin curve is measured over the loudest 30 s.
// The six band steps the morph walks come from the proposal over the windowed
// sides, so in "loudest30" they are the window's.
//
// So the rule: a curve is called "whole song" only when both curves really are
// whole-file averages, and a window's name is NEVER put on a curve. The band
// steps take the window's name only when both sides were windowed.

/** A slot's 64-bin curve is a whole-file (or whole-capture) average. */
inline bool matchCurveIsWholeSong (bool valid, SpectralReduction r) noexcept
{
    return valid && reductionIsAverage (r);
}

/** The caption, or "" when the picture makes no claim worth one.
      "whole"      "Whole song", when both curves are whole-file averages;
      "loudest30"  "Curves: whole song. Band steps: loudest 30 s." when both
                   sides were windowed; without the curves' clause when a
                   curve is not whole-file; without the steps' clause when a
                   side could not be windowed (the one line says why). */
inline juce::String matchPictureCaption (MatchWindowMode mode, bool curvesWholeSong, bool stepsWindowed)
{
    const juce::String curves = curvesWholeSong ? "Curves: whole song." : "";
    if (mode == MatchWindowMode::Whole) return curvesWholeSong ? "Whole song" : "";
    const juce::String steps = stepsWindowed ? "Band steps: loudest 30 s." : "";
    return (curves + " " + steps).trim();
}

/** Where the caption goes: the switch's row, from the card's left edge to just
    short of the switch. Empty when the switch is not drawn or the room left is
    too narrow for a word. */
inline constexpr int kMatchPictureCaptionGap  = 8;
inline constexpr int kMatchPictureCaptionMinW = 60;

inline juce::Rectangle<int> matchPictureCaptionRect (juce::Rectangle<int> graph)
{
    const auto sw = matchWindowSwitchRect (graph);
    if (sw.isEmpty()) return {};
    const auto inner = graph.reduced (kMatchWindowSwitchPad);
    const int w = sw.getX() - kMatchPictureCaptionGap - inner.getX();
    if (w < kMatchPictureCaptionMinW) return {};
    return { inner.getX(), sw.getY(), w, sw.getHeight() };
}

} // namespace echojay
