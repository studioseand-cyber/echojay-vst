#pragma once

#include <JuceHeader.h>
#include <cmath>
#include <limits>

namespace echojay
{

/** THE ONE CHECK (COMP_PROFILE_SPEC_v1 section 7).

    A compressor set open-loop from its profile is checked ONCE and corrected AT MOST ONCE. That is the whole of
    the plugin's part: the drive seek is gone (letter (q)) because reading level in against level out could not
    tell compression from an engage switch left off, a saturation stage, a mix knob or auto makeup - each fix
    covered one plugin and the next plugin broke it.

      - After the dial settles and at least 10 s of loud material has been heard at that slot, measure slot output
        against slot input on the loud phrases. Subtract `static_gain_db` and any expected level change. The result
        is the observed drop.
      - Observed drop MORE than expected + 3 dB: move the amount control ONCE by the difference, toward less gain
        reduction, log it, no second move.
      - Observed drop UNDER 0.5 dB when expected is 1 dB or more: log `PROFILE_NOT_ENGAGING` with plugin, map_fp
        and the readings. Change NOTHING. This is how a wrong profile gets reported.
      - Then the hold matches level on OUT once, as now.

    Pure, so a guard drives all three outcomes with figures it chose, and so the decision cannot differ between
    the V2 and the Link - the same reason configFromBlock lives in a shared header. */
struct CompCheck
{
    /** One measured position of the amount control: its norm and the input level at which it reaches 1 dB of
        gain reduction. Declared FIRST because member function SIGNATURES are parsed where they appear, and an
        unqualified `Point` before this line resolves to juce::Point. */
    struct Point { float norm, eff; };

    static constexpr float kTooMuchMarginDb = 3.0f;   // "more than expected + 3 dB"
    static constexpr float kNotEngagingDb   = 0.5f;   // "under 0.5 dB"
    static constexpr float kExpectedFloorDb = 1.0f;   // "...when expected is 1 dB or more"
    static constexpr float kLoudHeardS      = 10.0f;  // "at least 10 s of loud material"

    enum class Outcome
    {
        notYet,         // the dial has not settled, or there is not 10 s of loud material yet
        noCheck,        // no profile or no expected figure: today's behaviour, nothing to check against
        inRange,        // the reading agrees with the profile: change nothing, go to the hold
        tooMuch,        // move the amount control once, toward less GR
        notEngaging     // log PROFILE_NOT_ENGAGING, change nothing
    };

    struct Reading
    {
        bool  dialSettled = false;
        float loudHeardSeconds = 0.0f;
        float levelChangeDb = std::numeric_limits<float>::quiet_NaN();   // slot OUT minus slot IN, loud phrases
        float expectedGrDb = std::numeric_limits<float>::quiet_NaN();
        float expectedLevelDb = std::numeric_limits<float>::quiet_NaN();
        float staticGainDb = 0.0f;
    };

    struct Result
    {
        Outcome outcome = Outcome::notYet;
        float observedDropDb = std::numeric_limits<float>::quiet_NaN();
        float correctionDb = 0.0f;      // how much LESS gain reduction to ask for; 0 unless tooMuch
        juce::String why;               // one line, for the log
    };

    /** THE OBSERVED DROP. levelChangeDb is OUT minus IN, so a compressor that reduces gain reads negative and the
        measured drop is its negation. `static_gain_db` is "output minus input well below threshold, with neutral
        applied" - a constant that is not compression - and `expected_level_db` is how much the amount control
        itself moves the level on an input_drive unit. Both are subtracted, exactly as section 7 says, so what is
        left is compression and nothing else. */
    static float observedDrop (const Reading& r) noexcept
    {
        if (! (r.levelChangeDb == r.levelChangeDb)) return std::numeric_limits<float>::quiet_NaN();
        const float expLevel = (r.expectedLevelDb == r.expectedLevelDb) ? r.expectedLevelDb : 0.0f;
        // THE SUBTRACTION IS FROM THE CHANGE, AND THE DROP IS ITS NEGATION - in that order, because doing it the
        // other way round flips the sign of both corrections. levelChangeDb is OUT minus IN; static_gain_db and
        // expected_level_db are parts of that change which are NOT compression, so they come off the change:
        //     compressionChange = levelChange - static_gain - expected_level
        //     drop              = -compressionChange
        // Worked twice, because the first cut of this had it inverted and a leg asserting the wrong number agreed
        // with it: a slot 2 dB LOUDER for free (static +2) whose level change is -4.5 is compressing 6.5 dB; an
        // input_drive unit whose own input knob costs 4 dB (expected -4.0) and whose change is -6.5 is compressing
        // 2.5 dB, not 10.5.
        const float compressionChange = r.levelChangeDb - r.staticGainDb - expLevel;
        return -compressionChange;
    }

    static Result decide (const Reading& r)
    {
        Result out;
        if (! (r.expectedGrDb == r.expectedGrDb))
        { out.outcome = Outcome::noCheck; out.why = "no expected gain reduction on the block: nothing to check";
          return out; }
        if (! r.dialSettled)
        { out.outcome = Outcome::notYet; out.why = "the dial has not settled yet"; return out; }
        if (r.loudHeardSeconds < kLoudHeardS)
        { out.outcome = Outcome::notYet;
          out.why = "only " + juce::String (r.loudHeardSeconds, 1) + " s of loud material so far, "
                  + juce::String (kLoudHeardS, 0) + " needed";
          return out; }
        const float drop = observedDrop (r);
        out.observedDropDb = drop;
        if (! (drop == drop))
        { out.outcome = Outcome::notYet; out.why = "no level reading on the loud phrases yet"; return out; }

        if (drop > r.expectedGrDb + kTooMuchMarginDb)
        {
            out.outcome = Outcome::tooMuch;
            out.correctionDb = drop - r.expectedGrDb;    // ask for exactly the difference back
            out.why = "observed " + juce::String (drop, 1) + " dB against an expected "
                    + juce::String (r.expectedGrDb, 1) + " dB - more than " + juce::String (kTooMuchMarginDb, 0)
                    + " dB over, so the amount moves once by " + juce::String (out.correctionDb, 1)
                    + " dB toward less";
            return out;
        }
        if (drop < kNotEngagingDb && r.expectedGrDb >= kExpectedFloorDb)
        {
            out.outcome = Outcome::notEngaging;
            out.why = "observed " + juce::String (drop, 1) + " dB against an expected "
                    + juce::String (r.expectedGrDb, 1) + " dB: it is not compressing at all";
            return out;
        }
        out.outcome = Outcome::inRange;
        out.why = "observed " + juce::String (drop, 1) + " dB against an expected "
                + juce::String (r.expectedGrDb, 1) + " dB - near enough, nothing moves";
        return out;
    }

    // ---- the amount control, through the profile's own curve -------------------------------------------------
    /** `amount.curve` is a list of { norm, eff_threshold_dbfs } sorted by norm: the input level at which gain
        reduction reaches 1 dB with the amount control at that position. LESS gain reduction means a HIGHER
        effective threshold, for both topologies the client handles - on an input_drive unit the amount control is
        an input or peak-reduction knob, and backing it off raises the level at which the unit starts working.
        So one rule serves both: ask for an effective threshold `lessGrDb` higher, and read the norm back off the
        curve by interpolation. Returns the current norm unchanged when the curve cannot answer. */
    static float amountNormForLessGr (const juce::var& profile, float currentNorm, float lessGrDb)
    {
        const auto curve = curveOf (profile);
        if (curve.size() < 2) return currentNorm;
        const float currentEff = effForNorm (curve, currentNorm);
        if (! (currentEff == currentEff)) return currentNorm;
        const float wanted = normForEff (curve, currentEff + lessGrDb);
        // ITEM 4 (COMP_PROFILE_SPEC_v1 v1.2, amount.stepped): A STEPPED CONTROL HAS NO IN-BETWEEN. "The curve then
        // lists every detent, and the server only ever picks a listed point, never an interpolated norm" - and the
        // same holds for this correction, because a norm between two detents lands on whichever one the plugin
        // rounds to, which is a position nobody chose. It moves to the ADJACENT listed detent in the direction of
        // less gain reduction: adjacent, not nearest-to-the-ideal, so one correction is one step and the move is
        // always in the direction the check asked for even when the ideal lies past the next detent.
        if (isStepped (profile))
            return adjacentDetentForLessGr (curve, currentNorm);
        return wanted;
    }

    static bool isStepped (const juce::var& profile)
    {
        if (auto* a = profile.getProperty ("amount", juce::var()).getDynamicObject())
            return (bool) a->getProperty ("stepped");
        return false;
    }

    /** The next listed detent toward LESS gain reduction - a higher effective threshold - from wherever the
        control is now. The curve is sorted by norm; "less" follows the curve's own direction rather than assuming
        it rises, because an input_drive unit's amount can run either way. Already at the last one: stay there. */
    static float adjacentDetentForLessGr (const std::vector<Point>& c, float currentNorm)
    {
        if (c.empty()) return currentNorm;
        // Which listed detent are we at (or nearest to)?
        size_t at = 0;
        float best = std::abs (c[0].norm - currentNorm);
        for (size_t i = 1; i < c.size(); ++i)
        { const float d = std::abs (c[i].norm - currentNorm); if (d < best) { best = d; at = i; } }
        const bool effRisesWithNorm = c.back().eff >= c.front().eff;
        if (effRisesWithNorm)
            return (at + 1 < c.size()) ? c[at + 1].norm : c[at].norm;
        return (at > 0) ? c[at - 1].norm : c[at].norm;
    }

    /** The control's name, so the caller writes the one the profile measured and not a guess. */
    static juce::String amountControl (const juce::var& profile)
    {
        if (auto* a = profile.getProperty ("amount", juce::var()).getDynamicObject())
            return a->getProperty ("control").toString().trim();
        return {};
    }

    static float staticGainOf (const juce::var& profile)
    {
        if (auto* o = profile.getDynamicObject())
            if (o->hasProperty ("static_gain_db")) return (float) (double) o->getProperty ("static_gain_db");
        return 0.0f;
    }

    static std::vector<Point> curveOf (const juce::var& profile)
    {
        std::vector<Point> out;
        auto* a = profile.getProperty ("amount", juce::var()).getDynamicObject();
        if (a == nullptr) return out;
        // v1.3: `in_at_gr_dbfs["1"]` IS THE THRESHOLD FIELD. `eff_threshold_dbfs` is now "optional,
        // informational ... The server never reads it", and the v1.3 example omits it from every point - so a curve
        // built only from that field is EMPTY on a v1.3 profile, and the one correction then silently does nothing.
        // Read in_at_gr_dbfs["1"] first and fall back to eff_threshold_dbfs, so v1.2 and v1.3 profiles both work.
        if (auto* arr = a->getProperty ("curve").getArray())
            for (const auto& pv : *arr)
                if (auto* p = pv.getDynamicObject())
                {
                    if (! p->hasProperty ("norm")) continue;
                    float eff = std::numeric_limits<float>::quiet_NaN();
                    if (auto* g = p->getProperty ("in_at_gr_dbfs").getDynamicObject())
                    {
                        const auto one = g->getProperty ("1");
                        if (one.isDouble() || one.isInt() || one.isInt64()) eff = (float) (double) one;
                        // null means "the sweep never reached 1 dB here" - that point cannot anchor the curve.
                    }
                    if (! (eff == eff) && p->hasProperty ("eff_threshold_dbfs"))
                        eff = (float) (double) p->getProperty ("eff_threshold_dbfs");
                    if (eff == eff)
                        out.push_back ({ (float) (double) p->getProperty ("norm"), eff });
                }
        std::sort (out.begin(), out.end(), [] (const Point& l, const Point& r) { return l.norm < r.norm; });
        return out;
    }

    static float effForNorm (const std::vector<Point>& c, float norm) noexcept
    {
        if (c.empty()) return std::numeric_limits<float>::quiet_NaN();
        if (norm <= c.front().norm) return c.front().eff;
        if (norm >= c.back().norm)  return c.back().eff;
        for (size_t i = 1; i < c.size(); ++i)
            if (norm <= c[i].norm)
            {
                const float span = c[i].norm - c[i - 1].norm;
                const float t = span > 1.0e-9f ? (norm - c[i - 1].norm) / span : 0.0f;
                return c[i - 1].eff + t * (c[i].eff - c[i - 1].eff);
            }
        return c.back().eff;
    }

    /** The inverse, clamped to the curve's own ends: a correction the control cannot make is made as far as it
        goes, which is honest, and the caller's log says what it asked for. */
    static float normForEff (const std::vector<Point>& c, float eff) noexcept
    {
        if (c.empty()) return 0.0f;
        const bool rising = c.back().eff >= c.front().eff;
        if (rising ? (eff <= c.front().eff) : (eff >= c.front().eff)) return c.front().norm;
        if (rising ? (eff >= c.back().eff)  : (eff <= c.back().eff))  return c.back().norm;
        for (size_t i = 1; i < c.size(); ++i)
        {
            const bool between = rising ? (eff <= c[i].eff) : (eff >= c[i].eff);
            if (between)
            {
                const float span = c[i].eff - c[i - 1].eff;
                const float t = std::abs (span) > 1.0e-9f ? (eff - c[i - 1].eff) / span : 0.0f;
                return juce::jlimit (0.0f, 1.0f, c[i - 1].norm + t * (c[i].norm - c[i - 1].norm));
            }
        }
        return c.back().norm;
    }

    /** The PROFILE_NOT_ENGAGING line, composed once so the log and any report read the same string. */
    static juce::String notEngagingLine (const juce::String& plugin, const juce::String& mapFp,
                                         const Reading& r, const Result& res)
    {
        return "PROFILE_NOT_ENGAGING plugin=\"" + plugin + "\" map_fp=" + (mapFp.isEmpty() ? "(none)" : mapFp)
             + " expected_gr_db=" + juce::String (r.expectedGrDb, 1)
             + " observed_drop_db=" + juce::String (res.observedDropDb, 2)
             + " level_change_db=" + juce::String (r.levelChangeDb, 2)
             + " static_gain_db=" + juce::String (r.staticGainDb, 2)
             + " loud_heard_s=" + juce::String (r.loudHeardSeconds, 1);
    }
};

} // namespace echojay
