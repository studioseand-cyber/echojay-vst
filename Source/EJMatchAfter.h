#pragma once

#include <JuceHeader.h>
#include <array>
#include <cmath>
#include <optional>
#include "EJMatchPage.h"     // MatchPageView, matchPageFor's view; CaptureSnapshot
#include "EJMatchReport.h"   // MatchReportAfter, the payload's "after" block

// =============================================================================
//  ROUND 8c: THE "AFTER" CURVE AND THE CLOSED LOOP (K6; plan section 7)
// =============================================================================
//
// DESIGN_MATCH_TAB rev 7 section 4 ("Passes"): after the plan runs, a verify
// capture against the same reference and window, and the closing compares
// before, after and the reference per band. Section 5 point 3: a third curve,
// "after", and a band that got worse shows in the warning colour. D8 and MR1:
// both sets of six band deltas are recorded.
//
// WHAT "THE SAME REFERENCE AND WINDOW" IS CHECKED AGAINST. A CaptureSnapshot
// carries no reference and no window, so the page remembers what each plan was
// made for when "Make a plan" sends it (matchNotePlanFor): the reference's key
// and name, and the window. The after curve is drawn only when the page is on
// that same reference and window; otherwise there is no after curve, and the
// line says why.
//
// BEFORE AND AFTER ARE THE SAME QUANTITY. D is the mix minus the reference, per
// band, on the six band relatives (matchBandRelatives), over the window. Before
// is the mix slot's, as the page view has it; after is the verify capture's,
// over the same window: its whole-run bands in "Whole song", its own loudest
// 30 s in "Loudest 30 s". The reference side is the view's, so the two sets
// share one reference.
//
// NOT PRESENT IS NOT "NOTHING CHANGED" (C2 section 4). No verify capture gives
// `present: false` and no figures, never six unchanged bands.
// =============================================================================

namespace echojay
{

// -----------------------------------------------------------------------------
// WHAT THE PLAN WAS MADE FOR
// -----------------------------------------------------------------------------

struct MatchPlanFor
{
    juce::String refKey, refName;
    MatchWindowMode mode = MatchWindowMode::Whole;
};

inline juce::Identifier matchPagePlanForId() { return "ejMatchPlanFor"; }

/** Called when "Make a plan" sends: the plan is for this reference and window. */
inline void matchNotePlanFor (juce::NamedValueSet& page, const MatchPageView& v, const juce::String& refName)
{
    juce::Array<juce::var> a { juce::var (v.refKey), juce::var (refName),
                               juce::var (v.w.mode == MatchWindowMode::Whole ? "whole" : "loudest30") };
    page.set (matchPagePlanForId(), juce::JSON::toString (juce::var (a), true));
}

inline std::optional<MatchPlanFor> matchPlanForOf (const juce::NamedValueSet& page)
{
    const auto* raw = page.getVarPointer (matchPagePlanForId());
    if (raw == nullptr || ! raw->isString()) return std::nullopt;
    const juce::var a = juce::JSON::parse (raw->toString());
    if (! a.isArray() || a.size() != 3) return std::nullopt;
    MatchPlanFor p;
    p.refKey  = a[0].toString();
    p.refName = a[1].toString();
    p.mode    = a[2].toString() == "loudest30" ? MatchWindowMode::Loudest30 : MatchWindowMode::Whole;
    return p;
}

// -----------------------------------------------------------------------------
// THE AFTER SIDE
// -----------------------------------------------------------------------------

/** Under this much movement either way, a band is unchanged. K's figure,
    provisional, and in no contract. */
inline constexpr float kMatchAfterSameDb = 0.25f;

enum class MatchAfterTrend { Closing, Unchanged, Widening };

struct MatchAfter
{
    bool present = false;
    /** Why there is no after curve although a verify capture exists; "" when
        there is no verify capture at all, or the curve is drawn. */
    juce::String why;
    std::array<float, 6> beforeDb {}, afterDb {};   ///< D, mix (or after) minus reference
    std::array<float, 6> afterRel {};               ///< the verify capture's relatives: the curve
    std::array<MatchAfterTrend, 6> trend {};
    std::optional<float> integrated;                ///< the verify capture's, whole file
    float durationSeconds = 0.0f;
};

inline MatchAfterTrend matchAfterTrendOf (float before, float after) noexcept
{
    const float moved = std::abs (after) - std::abs (before);
    if (moved >  kMatchAfterSameDb) return MatchAfterTrend::Widening;
    if (moved < -kMatchAfterSameDb) return MatchAfterTrend::Closing;
    return MatchAfterTrend::Unchanged;
}

/** A band drawn in the warning colour: it got further from the reference. */
inline bool matchAfterWarns (MatchAfterTrend t) noexcept { return t == MatchAfterTrend::Widening; }

inline juce::String matchAfterWindowWords (MatchWindowMode m)
{
    return m == MatchWindowMode::Whole ? "the whole song" : "the loudest 30 s";
}

/** The after side for this view, from the host's verify capture and what the
    page's plan was made for. */
inline MatchAfter matchAfterOf (const CaptureSnapshot* verify, const MatchPageView& v,
                                const std::optional<MatchPlanFor>& planFor)
{
    MatchAfter a;
    if (verify == nullptr) return a;
    if (! planFor)
    {
        a.why = "There is an after capture, but no plan was made on this page, so there is nothing to check it against.";
        return a;
    }
    if (planFor->refKey != v.refKey)
    {
        a.why = "The after capture checked the plan against \"" + planFor->refName
              + "\", not this reference, so there is no after curve here.";
        return a;
    }
    if (planFor->mode != v.w.mode)
    {
        a.why = "The after capture checked the plan over " + matchAfterWindowWords (planFor->mode)
              + ", not " + matchAfterWindowWords (v.w.mode) + ", so there is no after curve here.";
        return a;
    }
    std::array<float, 6> mixRel {}, refRel {};
    if (! (v.w.mix.hasMacro && matchBandRelatives (v.w.mix.macro, mixRel)
           && v.w.ref.hasMacro && matchBandRelatives (v.w.ref.macro, refRel)))
    {
        a.why = "The after curve needs the six bands on both sides of the comparison before the plan.";
        return a;
    }

    std::array<float, 6> afterDb { -120, -120, -120, -120, -120, -120 };
    bool haveBands = false;
    if (v.w.mode == MatchWindowMode::Whole)
    {
        if (verify->hasMacroBandAccum) { afterDb = verify->macroBandAccum; haveBands = true; }
        else if (! verify->hopFrames.empty())
        {
            afterDb = matchWindowBandsDb (verify->hopFrames, HopRange { 0, verify->hopFrames.size() });
            haveBands = true;
        }
    }
    else
    {
        const auto L = loudest30Window (verify->hopFrames);
        if (! L.window)
        {
            a.why = loudest30RefusalMessage ("the after capture", L.sideSeconds);
            return a;
        }
        afterDb = matchWindowBandsDb (verify->hopFrames, *L.window);
        haveBands = true;
    }
    if (! haveBands || ! matchBandRelatives (afterDb, a.afterRel))
    {
        a.why = "The after capture has no six bands to compare.";
        return a;
    }

    a.present = true;
    for (std::size_t b = 0; b < 6; ++b)
    {
        a.beforeDb[b] = mixRel[b] - refRel[b];
        a.afterDb[b]  = a.afterRel[b] - refRel[b];
        a.trend[b]    = matchAfterTrendOf (a.beforeDb[b], a.afterDb[b]);
    }
    if (verify->averagedData.integrated > -99.0f) a.integrated = verify->averagedData.integrated;
    a.durationSeconds = verify->durationSeconds;
    return a;
}

/** The payload's "after" block, from the after side. */
inline MatchReportAfter matchReportAfterOf (const MatchAfter& a)
{
    MatchReportAfter r;
    r.present = a.present;
    r.why = a.why;
    if (! a.present) return r;
    r.integrated = a.integrated;
    r.durationSeconds = a.durationSeconds;
    for (std::size_t b = 0; b < 6; ++b)
    {
        r.beforeDb[b] = a.beforeDb[b];
        r.afterDb[b]  = a.afterDb[b];
    }
    return r;
}

/** Every string the after curve can put on the page. */
inline juce::StringArray matchAfterPageStrings()
{
    return { "after", "There is an after capture, but no plan was made on this page, so there is nothing to check it against.",
             "The after capture checked the plan against \"", "\", not this reference, so there is no after curve here.",
             "The after capture checked the plan over ", ", so there is no after curve here.",
             "The after curve needs the six bands on both sides of the comparison before the plan.",
             "The after capture has no six bands to compare." };
}

} // namespace echojay
