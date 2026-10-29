#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>
#include "EJMatchProposal.h"   // kMatchBandFloorDb (X1's 2 dB), juce::String
#include "EJBandScheme.h"      // the band masks' order (B1)
#include "EJMatchWindow.h"     // HopRange, for the window powers read off a hop series

// =============================================================================
//  MATCH ATTRIBUTION: where one band move goes, how big it is, and what it says.
// =============================================================================
//
// C4 (CONTRACT_MATCH_ATTRIBUTION_2026-10-27, revision 4) sections 2 to 5 and 9.
// For one band move this:
//   - adds each Link's own trim back (section 2);
//   - routes the move by the rules of section 4, in order, with the 4a role table;
//   - sizes a track or pair move by section 5;
//   - and returns the target, the shares, valueDb, measuredDb, coverage, the
//     sentence and the caveat flag.
//
// ONE PLACE DECIDES (C4 section 1). The plugin runs this and sends the result;
// the server checks bounds and never re-applies the routing or the sizing. So a
// rule here that differs from C4 is a rule the product ships.
//
// NOTHING ROUTES ON CREST (C4 rev 4, rule 2 off before the beta). The routing
// takes no crest input at all, so no band move can be withheld for one: rules
// 1 and 3 to 8 are the whole of it. Crest is logged by EJMatchReport.h and
// decides nothing; decision 4's sentence is the card's, once (EJMatchReport.h).
//
// PURE. No JUCE audio type, no global, no I/O. Strings are juce::String, as in
// EJMatchProposal.h. Every sentence is C4's wording, and a pin reads it.
//
// TWO FIGURES ARE NOT C4's, and they are marked where they are defined:
// kAttribSilentBandPower and kAttribShareTolerance.
// =============================================================================

namespace echojay
{

// -----------------------------------------------------------------------------
// THE CONSTANTS. PROPOSED means C4 chose it and nothing has measured it.
// -----------------------------------------------------------------------------

/** C4 section 3, PROPOSED: a known Link's frames cover at least this much of
    the window. */
inline constexpr double kAttribKnownCoverage = 0.90;

/** C4 section 2: a Link whose hop count differs from the bus's by MORE than
    this is unknown for the window. One hop short is known; two is not. */
inline constexpr int kAttribHopTolerance = 1;

/** C4 section 4 rule 3: any known share above this goes to the bus. */
inline constexpr double kAttribFaderShare = 1.0;

/** C4 section 4 rule 3, PROPOSED: a coverage above this goes to the bus. */
inline constexpr double kAttribFaderCoverage = 1.15;

/** C4 section 4 rule 4: a coverage below this goes to the bus. */
inline constexpr double kAttribLowCoverage = 0.6;

/** C4 section 4 rule 6: one track holding at least this much gets the move. */
inline constexpr double kAttribSingleShare = 0.5;

/** C4 section 4 rule 7: two tracks each holding at least this much... */
inline constexpr double kAttribPairEachShare = 0.25;
/** ...and together at least this much get the move between them. */
inline constexpr double kAttribPairSumShare = 0.7;

/** C4 section 5, PROPOSED: a track move larger than this goes to the bus. Not a
    taste cap: a track that needs more than 12 dB to move the mix by D is a sign
    the attribution is wrong. */
inline constexpr float kAttribTrackLimitDb = 12.0f;

/** NOT C4's. The silent floor, MeterEngine's own power floor: the value below
    which it reports -120 dB instead of taking a log, so only true digital
    silence counts. Since C4 rev 3 it plays two parts. A bus band at or under it
    gives no attribution (the guard below). A known Link's band at or under it
    gives that Link share 0 for the band (section 3); it never makes the Link
    unknown. */
inline constexpr double kAttribSilentBandPower = 1e-12;

/** NOT C4's. Shares are ratios of measured powers, and the boundaries C4 states
    (0.25 + 0.45 against 0.7, for one) do not land exactly in binary. Every
    share comparison carries this tolerance, which absorbs that rounding and
    nothing a measurement could produce. */
inline constexpr double kAttribShareTolerance = 1e-9;

// -----------------------------------------------------------------------------
// THE ROLE TABLE (C4 section 4a, PROPOSED)
// -----------------------------------------------------------------------------
//
// Deliberately generous: a wrong "yes" only sends a move to the bus, a wrong "no"
// puts a move on the wrong track. So a role this table does not list is read as
// unknown, which carries all six.

/** Bit b set when the role can carry band b. */
inline unsigned attribRoleBandMask (const juce::String& role)
{
    // sub, low, lowMid, mid, highMid, air = bits 0..5
    constexpr unsigned kAll        = 0x3Fu;
    constexpr unsigned kSubToMid   = 0x0Fu;   // sub, low, lowMid, mid
    constexpr unsigned kLowToAir   = 0x3Eu;   // low, lowMid, mid, highMid, air
    const auto r = role.trim().toLowerCase();
    if (r == "kick" || r == "bass")                      return kSubToMid;
    if (r == "drums" || r == "percussion")               return kAll;
    if (r == "vocal")                                    return kLowToAir;
    if (r == "guitar" || r == "keys" || r == "synth"
        || r == "strings" || r == "fx")                  return kLowToAir;
    return kAll;   // unknown or empty
}

inline bool attribRoleCanCarry (const juce::String& role, int band)
{
    return band >= 0 && band < 6 && (attribRoleBandMask (role) & (1u << (unsigned) band)) != 0;
}

// -----------------------------------------------------------------------------
// SECTION 2: THE TRIM, ADDED BACK
// -----------------------------------------------------------------------------

/** A Link's capture series is post its own trim (C2 section 7.2). Its pre-trim
    power is P_post / 10^(gainDb / 10), exact for a steady trim. */
inline double attribPreTrimPower (double postTrimPower, float gainDb)
{
    return postTrimPower / std::pow (10.0, (double) gainDb / 10.0);
}

// -----------------------------------------------------------------------------
// SECTION 5: SIZING
// -----------------------------------------------------------------------------

struct AttribSizing
{
    bool  feasible = false;   ///< false: g <= 1 - s, no cut on these tracks reaches D
    float xDb = 0.0f;         ///< the change on each track, when feasible
    float maxDb = 0.0f;       ///< when infeasible: the most a cut can take off the mix
    bool  tooLarge = false;   ///< feasible, but abs(x) > kAttribTrackLimitDb
};

/** The same change x on every track in a set of combined share s, to remove D
    from the mix's band:
        (1 - s) + s * 10^(x/10) = g,  g = 10^(-D/10)
        x = 10 * log10 ((g - (1 - s)) / s)
    It assumes the parts' powers add, which holds when they are uncorrelated.
    s = 1 gives x = -D, which is the bus move. */
inline AttribSizing attribSizeTrackMove (double s, float D)
{
    AttribSizing z;
    const double g = std::pow (10.0, -(double) D / 10.0);
    if (g <= 1.0 - s)
    {
        z.maxDb = (float) (-10.0 * std::log10 (1.0 - s));
        return z;
    }
    z.feasible = true;
    z.xDb      = (float) (10.0 * std::log10 ((g - (1.0 - s)) / s));
    z.tooLarge = std::abs (z.xDb) > kAttribTrackLimitDb;
    return z;
}

// -----------------------------------------------------------------------------
// THE SENTENCES. C4's words, nothing added. Shares print as whole percentages
// (share x 100, section 9), dB figures to one decimal.
// -----------------------------------------------------------------------------

inline juce::String attribPct (double share)  { return juce::String (juce::roundToInt (share * 100.0)); }
inline juce::String attribDb (double db)      { return juce::String (db, 1); }
inline juce::String attribBand (int band)     { return matchBandName (band); }   // "the low-mids" (6a)

/** Rule 3, WITH the bus input tap: a share over 1 can only be a fader. */
inline juce::String attribFaderSentenceWithTap()
{
    return "At least one track's fader is well below unity, so track shares can't be measured; "
           "this move goes on the mix bus.";
}

/** Rule 3, WITHOUT the tap: the bus chain is a second possible cause (C4 section 2). */
inline juce::String attribFaderSentenceNoTap()
{
    return "Track shares can't be measured here: a fader is well below unity, or the mix bus's "
           "own processing is changing this band. This move goes on the mix bus.";
}

inline juce::String attribLowCoverageSentence (int band)
{
    return "Most of " + attribBand (band) + " " + matchBandVerb (band, "comes", "come")
         + " from tracks without an EchoJay Link.";
}

inline juce::String attribUnknownSentence (const juce::String& name, int band)
{
    return name + " didn't give a full reading in this window and may hold "
         + attribBand (band) + ".";
}

inline juce::String attribSingleSentence (const juce::String& name, double s, int band)
{
    return name + " holds " + attribPct (s) + "% of " + attribBand (band) + ".";
}

inline juce::String attribPairSentence (const juce::String& a, const juce::String& b, double s, int band)
{
    return a + " and " + b + " hold " + attribPct (s) + "% of " + attribBand (band) + " between them.";
}

inline juce::String attribSpreadSentence (int band)
{
    return matchBandNameCap (band) + " " + matchBandVerb (band, "is", "are") + " spread across several tracks.";
}

/** Section 5, infeasible. For a pair the subject is "{a} and {b}" and the verb
    agrees with it ("hold"); C4 gives the sentence for one name only. */
inline juce::String attribInfeasibleSentence (const juce::String& subject, bool plural, double s,
                                              int band, float maxDb, float D)
{
    return subject + (plural ? " hold " : " holds ") + attribPct (s) + "% of " + attribBand (band)
         + "; cutting it can take at most " + attribDb (maxDb) + " dB off the mix, and the gap is "
         + attribDb (D) + " dB.";
}

inline juce::String attribTooLargeSentence (const juce::String& subject, float xDb, float D)
{
    return "Closing this on " + subject + " would need " + attribDb (xDb)
         + " dB; the mix bus does it with " + attribDb (-D) + " dB.";
}

/** Section 9: always shown when any move is placed on a track. */
inline juce::String attribPreFaderCaveatSentence()
{
    return "Track shares are measured before each fader, so they assume faders near unity.";
}

/** Section 9: the coverage line. */
inline juce::String attribCoverageSentence (double coverage, int band)
{
    return "Linked tracks hold " + attribPct (coverage) + "% of " + attribBand (band) + ".";
}

// -----------------------------------------------------------------------------
// INPUT AND OUTPUT
// -----------------------------------------------------------------------------

/** One Link, as the caller hands it over for one band. */
struct AttribLinkIn
{
    juce::String uid, name, role;
    double postTrimPower = 0.0;   ///< P_post[b], linear perOct, from the capture series
    float  gainDb = 0.0f;         ///< the Link's own trim, from the registry row
    int    hopCount = 0;          ///< this channel's hops in the window
    double frameCoverage = 0.0;   ///< 0..1: how much of the window its frames cover
};

// -----------------------------------------------------------------------------
// WHERE P_bus CAME FROM (C2 section 5.1, C4 section 2)
// -----------------------------------------------------------------------------

/** Input: the rack-input tap (pre-chain, pre-bus-trim). Output: the bus output
    with the bus trim added back, its chain included. */
enum class MatchBusTap { Input, Output };

/** C2 section 5.1: P_bus comes from hopFramesRackIn when it is non-empty, else
    from hopFrames. A template over the capture so the gate can drive it with a
    stand-in; CaptureSnapshot drops in with no adapter. */
template <typename Capture>
inline MatchBusTap matchBusTapOf (const Capture& c) noexcept
{
    return c.hopFramesRackIn.empty() ? MatchBusTap::Output : MatchBusTap::Input;
}

/** The payload's busTap value (C2 section 4). */
inline const char* matchBusTapName (MatchBusTap t) noexcept
{
    return t == MatchBusTap::Input ? "input" : "output";
}

/** One series over one window, as section 2 and section 3 read it. */
struct AttribSeriesWindow
{
    std::array<double, 6> power {};   ///< mean bandPower over the window's hops with frames
    int    hopCount = 0;              ///< the series' hops inside the window
    double frameCoverage = 0.0;       ///< hops with frames / the window's length in hops
};

/** A Link's or the bus's window figures from its hop series.

    WHERE C4 IS SILENT, K's READING: the power is the mean of bandPower over the
    hops that hold frames (bandFrames > 0 is the absence rule, C2 section 5's
    promise 3), silent hops included, because a quiet hop is a real level. The
    caller adds the trim back (section 2). */
template <typename Frame>
inline AttribSeriesWindow attribSeriesWindow (const std::vector<Frame>& frames, HopRange w)
{
    AttribSeriesWindow out;
    const std::size_t end = std::min (w.end, frames.size());
    if (w.end <= w.begin) return out;
    int withFrames = 0;
    std::array<double, 6> sum {};
    for (std::size_t h = w.begin; h < end; ++h)
    {
        ++out.hopCount;
        if (frames[h].bandFrames == 0) continue;
        ++withFrames;
        for (std::size_t b = 0; b < 6; ++b) sum[b] += (double) frames[h].bandPower[b];
    }
    if (withFrames > 0)
        for (std::size_t b = 0; b < 6; ++b) out.power[b] = sum[b] / withFrames;
    out.frameCoverage = (double) withFrames / (double) (w.end - w.begin);
    return out;
}

/** One band move to place. */
struct AttribIn
{
    int    band = -1;             ///< 0..5
    double busPower = 0.0;        ///< P_bus[b], ALREADY trim-corrected by the caller
    int    busHopCount = 0;
    float  D = 0.0f;              ///< measuredDb: mix minus reference; positive = mix hotter
    MatchBusTap busTap = MatchBusTap::Output;   ///< where P_bus came from: rule 3's sentence
    std::vector<AttribLinkIn> links;
};

enum class AttribTarget
{
    None,          ///< rule 1: under X1's floor, no move
    Bus,
    Track,
    Pair,
    Unavailable    ///< no bus band power, so nothing is attributable
};

/** Which branch decided, so a pin can see that a case reached its own rule and
    no other. */
enum class AttribRule
{
    Unavailable = 0,
    R1Floor,            ///< no R2: rule 2 is off before the beta (C4 rev 4)
    R3Fader, R4LowCoverage, R5Unknown,
    R6Single, R7Pair, R8Spread,
    S5Infeasible, S5TooLarge
};

struct AttribShare
{
    juce::String uid, name;
    double share = 0.0;
};

struct AttribName
{
    juce::String uid, name;
};

struct AttribResult
{
    AttribTarget target = AttribTarget::Unavailable;
    AttribRule   rule   = AttribRule::Unavailable;

    /** The Links the move is placed on, with their shares: one for Track, two
        for Pair, none otherwise. Never more than 1, because a share over 1
        routes to the bus (C4 section 5). */
    std::vector<AttribShare> links;
    /** Every KNOWN Link with its share, for the card. Unknown Links are never
        in here (C4 section 9). */
    std::vector<AttribShare> known;
    /** Every unknown Link, named. Never folded into a share. */
    std::vector<AttribName>  unknown;

    std::optional<double> combinedShare;   ///< s for a Pair; nullopt otherwise
    std::optional<double> coverage;        ///< sum of the known shares
    std::optional<float>  valueDb;         ///< x for Track and Pair, -D for Bus; nullopt for no move
    float                 measuredDb = 0.0f;   ///< D

    juce::String reason;          ///< C4's sentence for the target, or "" for rule 1
    bool caveat = false;          ///< a move is on a track: the pre-fader caveat shows
};

// -----------------------------------------------------------------------------
// THE ROUTING (C4 section 4, in order: the first rule that applies decides)
// -----------------------------------------------------------------------------

inline AttribResult attributeBandMove (const AttribIn& in)
{
    AttribResult out;
    out.measuredDb = in.D;
    const int b = in.band;

    // Rule 1: under X1's floor. EJMatchProposal's floor is the same 2 dB and the
    // same ">=" convention, so there is one definition of it.
    if (std::abs (in.D) < kMatchBandFloorDb)
    {
        out.target = AttribTarget::None;
        out.rule   = AttribRule::R1Floor;
        return out;
    }

    // Rule 2 is off before the beta (C4 rev 4): there is nothing to test it on.

    // A D cannot exist without the mix's band power, so this is a guard and not
    // a routing rule. It says nothing rather than inventing a sentence.
    if (! (in.busPower > kAttribSilentBandPower) || b < 0 || b > 5)
    {
        out.target = AttribTarget::Unavailable;
        out.rule   = AttribRule::Unavailable;
        return out;
    }

    // Section 3: known or unknown, then the shares of the known.
    double coverage = 0.0;
    std::vector<bool> isUnknown (in.links.size(), false);
    for (std::size_t i = 0; i < in.links.size(); ++i)
    {
        const auto&  l     = in.links[i];
        const double p     = attribPreTrimPower (l.postTrimPower, l.gainDb);
        const bool   known = l.frameCoverage >= kAttribKnownCoverage
                          && std::abs (l.hopCount - in.busHopCount) <= kAttribHopTolerance;
        if (! known) { isUnknown[i] = true; out.unknown.push_back ({ l.uid, l.name }); continue; }
        // Rev 3: a silent band leaves the Link known, with share 0 for it.
        const double s = p > kAttribSilentBandPower ? p / in.busPower : 0.0;
        out.known.push_back ({ l.uid, l.name, s });
        coverage += s;
    }
    out.coverage = coverage;

    auto toBus = [&] (AttribRule r, const juce::String& why)
    {
        out.target  = AttribTarget::Bus;
        out.rule    = r;
        out.valueDb = -in.D;
        out.reason  = why;
        return out;
    };

    // Rule 3: a fader, or (without the tap) the bus chain.
    bool overOne = false;
    for (const auto& k : out.known)
        if (k.share > kAttribFaderShare + kAttribShareTolerance) overOne = true;
    if (overOne || coverage > kAttribFaderCoverage + kAttribShareTolerance)
        return toBus (AttribRule::R3Fader, in.busTap == MatchBusTap::Input ? attribFaderSentenceWithTap()
                                                          : attribFaderSentenceNoTap());

    // Rule 4: most of the band is not on a Link.
    if (coverage < kAttribLowCoverage - kAttribShareTolerance)
        return toBus (AttribRule::R4LowCoverage, attribLowCoverageSentence (b));

    // Rule 5: an unknown Link that could be holding this band.
    for (std::size_t i = 0; i < in.links.size(); ++i)
        if (isUnknown[i] && attribRoleCanCarry (in.links[i].role, b))
            return toBus (AttribRule::R5Unknown, attribUnknownSentence (in.links[i].name, b));

    // The known Links by share, largest first, input order on a tie.
    std::vector<AttribShare> ranked = out.known;
    std::stable_sort (ranked.begin(), ranked.end(),
                      [] (const AttribShare& x, const AttribShare& y) { return x.share > y.share; });

    auto place = [&] (std::vector<AttribShare> set, AttribRule r, const juce::String& placedWhy)
    {
        double s = 0.0;
        for (const auto& k : set) s += k.share;
        const bool   plural  = set.size() > 1;
        const juce::String subject = plural ? set[0].name + " and " + set[1].name : set[0].name;
        const auto z = attribSizeTrackMove (s, in.D);
        if (! z.feasible)
            return toBus (AttribRule::S5Infeasible,
                          attribInfeasibleSentence (subject, plural, s, b, z.maxDb, in.D));
        if (z.tooLarge)
            return toBus (AttribRule::S5TooLarge, attribTooLargeSentence (subject, z.xDb, in.D));
        out.target  = plural ? AttribTarget::Pair : AttribTarget::Track;
        out.rule    = r;
        out.links   = std::move (set);
        out.valueDb = z.xDb;
        if (plural) out.combinedShare = s;
        out.reason  = placedWhy;
        out.caveat  = true;
        return out;
    };

    // Rule 6: one track holds half or more.
    if (! ranked.empty() && ranked[0].share >= kAttribSingleShare - kAttribShareTolerance)
        return place ({ ranked[0] }, AttribRule::R6Single,
                      attribSingleSentence (ranked[0].name, ranked[0].share, b));

    // Rule 7: two tracks, each a quarter or more, together seven tenths or more.
    if (ranked.size() >= 2
        && ranked[0].share >= kAttribPairEachShare - kAttribShareTolerance
        && ranked[1].share >= kAttribPairEachShare - kAttribShareTolerance
        && ranked[0].share + ranked[1].share >= kAttribPairSumShare - kAttribShareTolerance)
        return place ({ ranked[0], ranked[1] }, AttribRule::R7Pair,
                      attribPairSentence (ranked[0].name, ranked[1].name,
                                          ranked[0].share + ranked[1].share, b));

    // Rule 8: spread.
    return toBus (AttribRule::R8Spread, attribSpreadSentence (b));
}

} // namespace echojay
