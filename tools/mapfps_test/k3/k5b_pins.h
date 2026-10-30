#pragma once

// =============================================================================
//  ROUND 5b's PINS: THE PICTURE FOLLOWS THE WINDOW, HONESTLY (kl).
// =============================================================================
//
// The 64-bin curves are whole-file only, so in "Loudest 30 s" the picture says
// the curves are the whole song and the six band steps are the window. The
// caption is pinned in both modes, and a window's name never lands on a curve.
// =============================================================================

#include "EJMatchWindowSides.h"

namespace k5bpins
{
using namespace echojay;

template <typename Check>
inline void runK5bPins (Check&& check)
{
    const bool wholeCurves = matchCurveIsWholeSong (true, SpectralReduction::WholeFileAverage)
                          && matchCurveIsWholeSong (true, SpectralReduction::WholeWindowAverage);

    // kl PIN1 -- WHOLE SONG: the caption says so, in the switch's row, left of
    // the switch and clear of it.
    {
        const juce::Rectangle<int> graph { 0, 0, 900, 300 };
        const auto cr = matchPictureCaptionRect (graph);
        const auto sw = matchWindowSwitchRect (graph);
        check (wholeCurves && matchPictureCaption (MatchWindowMode::Whole, true, true) == "Whole song"
               && ! cr.isEmpty() && cr.getY() == sw.getY() && cr.getHeight() == sw.getHeight()
               && cr.getRight() <= sw.getX() - kMatchPictureCaptionGap && ! cr.intersects (sw)
               && matchPictureCaptionRect ({ 0, 0, 240, 300 }).isEmpty(),
               "kl PIN1: in Whole song the picture is captioned \"Whole song\", in the switch's row and clear of it",
               matchPictureCaption (MatchWindowMode::Whole, true, true));
    }

    // kl PIN2 -- LOUDEST 30 s: the curves are labelled whole song and the band
    // steps the window; no curve is ever given the window's name.
    {
        const auto t = matchPictureCaption (MatchWindowMode::Loudest30, true, true);
        bool windowOnCurve = false;
        for (auto mode : { MatchWindowMode::Whole, MatchWindowMode::Loudest30 })
            for (bool c : { false, true })
                for (bool st : { false, true })
                {
                    const auto x = matchPictureCaption (mode, c, st);
                    windowOnCurve = windowOnCurve || x.contains ("Curves: loudest")
                                 || (mode == MatchWindowMode::Whole && x.contains ("30 s"));
                }
        check (t == "Curves: whole song. Band steps: loudest 30 s." && ! windowOnCurve,
               "kl PIN2: in Loudest 30 s the curves are captioned whole song and the band steps loudest 30 s; "
               "no mode ever puts the window's name on a curve",
               t);
    }

    // kl PIN3 -- NOTHING IS CLAIMED THAT IS NOT SO. A live curve is never
    // called whole song, in either mode; a window that could not be taken gives
    // no "Band steps: loudest 30 s".
    {
        const bool liveWhole = matchCurveIsWholeSong (true, SpectralReduction::LiveInstant)
                            || matchCurveIsWholeSong (true, SpectralReduction::BallisticTail)
                            || matchCurveIsWholeSong (false, SpectralReduction::WholeFileAverage);
        check (! liveWhole
               && matchPictureCaption (MatchWindowMode::Whole, false, true).isEmpty()
               && matchPictureCaption (MatchWindowMode::Loudest30, false, true) == "Band steps: loudest 30 s."
               && matchPictureCaption (MatchWindowMode::Loudest30, true, false) == "Curves: whole song."
               && matchPictureCaption (MatchWindowMode::Loudest30, false, false).isEmpty(),
               "kl PIN3: a live or invalid curve is never captioned whole song, and an unwindowed side gives no "
               "\"Band steps: loudest 30 s\"",
               matchPictureCaption (MatchWindowMode::Loudest30, true, false));
    }
}

} // namespace k5bpins
