// ---- THE MATCH TAB'S IMPLEMENTATION (A0, 27-28 Oct 2026) -------------------------------------
//
// Carved out of PluginEditor.cpp with NO BEHAVIOUR CHANGE: every line below was moved verbatim, in
// order, from the three places it lived (the Match sides, the page's pickers and the one writer for
// its mix side, and the page itself). `docs/CONTRACT_MATCH_HOST_2026-10-27.md` is the contract this
// serves and `~/Desktop/FOR_REVIEW_22oct/CONTRACT_PLUGINEDITOR_REGIONS_2026-10-27.md` is the fence:
// from here on the Match front end is K's file and the editor is A's, and the two meet at MatchHost.
//
// THE MOVE WAS VERBATIM; THIS IS THE RE-POINT (A0b, 28 Oct 2026). The carve changed the file and
// nothing else, so its green gate said the move was clean and said nothing more. This commit is the
// second half: the page's 37 reaches through `owner` - an EchoJayEditor* into a 50,000-line class -
// are now calls on `host`, the MatchHost interface above. Separate commits on purpose, because a
// red row now means one specific thing.
//
// Four of the reaches were not calls at all but reads of the editor's own members (matchRefIsTop_,
// processorRef.cmpAudible, processorRef.cmpStream[].playing, processorRef.getSampleRate), and each
// is now a question with a name: matchRefIsTop(), compareSlotAudible(), compareSlotPlaying(),
// streamSampleRate(). That is the part of the boundary that was load-bearing: a page that can read
// a processor member can read any of them, and no interface would have been able to say so.
//
// This TU includes PluginEditor.h on purpose: MatchPanel is still declared inside EchoJayEditor (the
// header region the regions contract gives to K), and a nested class's members may be defined in any
// translation unit that can see the declaration.

#include "PluginEditor.h"
#include "EJMatchTab.h"
#include "EJBandScheme.h"
#include "EJCompareMenuIds.h"   // the menu id bands the mix picker decodes
using echojay::kCompareMenuBandSize;
using echojay::kCompareMenuRefNameLen;
#include <cmath>
#include <algorithm>

// THE MATCH SIDES (20 Sep 2026). The Match screen is not drawn yet and nothing
// in this file calls either of these; they exist so that the proposal is
// REACHABLE, which is this commit's whole scope (MATCH_SCREEN_CONTRACT §10).
//
// EVERY ANSWER COMES FROM A MEMBER THAT ALREADY ANSWERS IT: getSlotMeterData
// for the figures, getSlotSpectralEvidence for the macro bands and their stamp
// (which for a reference is the whole-file average and not the ballistic tail),
// slotDurationSeconds for the length, and refBarIsTop for which slot the
// reference bar drives. Nothing is re-derived here, and the Live rules are not
// here either: they belong to echojay::matchSideFrom, so a second caller cannot
// get them wrong.
echojay::MatchSide EchoJayEditor::buildMatchSide(const CompareSlotState& slot) const
{
    const MeterData               md = getSlotMeterData(slot);
    const echojay::SpectralEvidence ev = getSlotSpectralEvidence(slot);
    return echojay::matchSideFrom(echojay::computeCompareFig(md, ev), ev,
                                  slotDurationSeconds(slot),
                                  slot.kind == CompareSlotState::Kind::Live);
}

EchoJayEditor::MatchSlotPair EchoJayEditor::matchSlots() const
{
    // THE ROLE WAS DECIDED ON ENTRY AND IS HELD, not derived here. This used to
    // read refBarIsTop(), which answers "which slot holds a reference" and
    // therefore MOVED when a pick landed in the other slot, flipping which side
    // was the mix under the user.
    const bool refIsTop = matchRefIsTop_;
    MatchSlotPair p;
    p.ref = refIsTop ? &compareTop_ : &compareBot_;
    p.mix = refIsTop ? &compareBot_ : &compareTop_;
    return p;
}

EchoJayEditor::MatchSides EchoJayEditor::buildMatchSides() const
{
    const auto slots = matchSlots();
    MatchSides s;
    s.ref = buildMatchSide(*slots.ref);
    s.mix = buildMatchSide(*slots.mix);
    return s;
}

void EchoJayEditor::openMatchMixPicker()
{
    juce::PopupMenu menu;
    menu.addSectionHeader ("YOUR MIX");
    menu.addItem (1, "Live signal", true,
                  matchSlots().mix->kind == CompareSlotState::Kind::Live);

    const auto snaps = processorRef.getSnapshots();
    if (! snaps.empty())
    {
        menu.addSeparator();
        menu.addSectionHeader ("CAPTURES");
        for (int i = 0; i < (int) snaps.size() && i < kCompareMenuBandSize - 1; ++i)
            menu.addItem (100 + i, snaps[(size_t) i].name.substring (0, kCompareMenuRefNameLen));
    }

    // The chat captures, through the SAME ordered id list the slot menu builds,
    // so the label the user picked and the review the slot binds cannot drift.
    compareMenuReviewIds_.clear();
    {
        juce::PopupMenu chat;
        for (auto& r : workspace.getReviews())
        {
            if ((int) compareMenuReviewIds_.size() >= kCompareMenuBandSize - 1) break;
            chat.addItem (200 + (int) compareMenuReviewIds_.size(),
                          compareReviewLabel (r).substring (0, kCompareMenuRefNameLen));
            compareMenuReviewIds_.push_back (r.id);
        }
        if (compareMenuReviewIds_.size() > 0)
        {
            menu.addSeparator();
            menu.addSubMenu ("Chat captures", chat);
        }
    }

    auto safeThis = juce::Component::SafePointer<EchoJayEditor> (this);
    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetComponent (&matchPanel_)
                            .withParentComponent (this),
                        [safeThis] (int result)
    {
        if (safeThis == nullptr || result == 0) return;
        CompareSlotState next;
        if (result == 1)
        {
            next.kind = CompareSlotState::Kind::Live;
            next.label = "Live signal";
        }
        else if (result >= 100 && result < 200)
        {
            const auto snaps = safeThis->processorRef.getSnapshots();
            const int idx = result - 100;
            if (idx < 0 || idx >= (int) snaps.size()) return;
            next.kind = CompareSlotState::Kind::Snapshot;
            next.index = idx;
            next.label = snaps[(size_t) idx].name;
        }
        else if (result >= 200 && result < 300)
        {
            const int nth = result - 200;
            if (nth < 0 || nth >= (int) safeThis->compareMenuReviewIds_.size()) return;
            next.kind = CompareSlotState::Kind::WsCapture;
            next.wsReviewId = safeThis->compareMenuReviewIds_[(size_t) nth];
            for (auto& r : safeThis->workspace.getReviews())
                if (r.id == next.wsReviewId) next.label = safeThis->compareReviewLabel (r);
        }
        else return;

        safeThis->matchApplyMixSlot (next);
    });
}

// THE ONE WRITER for the Match page's mix side. It writes into whichever slot
// is NOT the reference side, so the roles decided on entry survive every pick.
//
// A DISPLACEMENT IS ANNOUNCED, not performed silently. Taking a slot ends codec
// mode and restarts a stream, and both of those are visible on a page the user
// is not looking at. setRefStatus is the surface that already exists for this,
// and its text survives until Compare is next entered, so the message is
// waiting there rather than having flashed past here.
void EchoJayEditor::matchApplyMixSlot (const CompareSlotState& next)
{
    auto& slot = matchRefIsTop_ ? compareBot_ : compareTop_;
    const int  slotIdx      = matchRefIsTop_ ? 1 : 0;
    const bool endedCodec   = codecModeActive_;
    const bool wasStreaming = (processorRef.cmpAudible.load() == slotIdx);

    slot = next;
    codecModeActive_ = false;

    processorRef.stopCompareStream (slotIdx);
    updateCompareSlotBtn (! matchRefIsTop_);
    startCompareStream (slotIdx);
    processorRef.cmpBothCaptures.store (bothSlotsAreCaptures());
    updateComparePlayBtns();

    if (endedCodec)
        setRefStatus ("Match took the " + juce::String (matchRefIsTop_ ? "B" : "A")
                          + " slot, so the codec A/B ended.", RefStatusKind::Info);
    else if (wasStreaming)
        setRefStatus ("Match changed the " + juce::String (matchRefIsTop_ ? "B" : "A")
                          + " slot, so Compare's playback restarted.", RefStatusKind::Info);

    matchPanel_.repaint();
    repaint();
}

// IT FITS AT THE SMALLEST PAGE. The smallest this product produces is 565 x 373
// (pg PIN5), which is the rect resized() hands this panel. The graph takes a
// share with a floor, and the words below are laid out at 12 pt and, only if
// they would not fit, smaller, down to 9 pt: a narrow window loses type size
// before it loses a refusal, because the refusals ARE the feature.
//
// THE MATCH SCREEN, STANDING STILL (20 Sep 2026). MATCH_SCREEN_CONTRACT §3 is
// what it draws and §10 is what it must not: NOTHING SENDS, nothing is written,
// nothing animates, and there is no axis control, because a control that cannot
// send is a dead control. The morph is the next commit.
//
// TWO LAYERS FROM DIFFERENT DATA, and the whole point is that they are not
// styled alike: the CURVES are each side's measured 64 log bins, and the BLOCKS
// are the six macro band moves drawn as STEPS. A smooth line through six
// numbers would assert shape between band centres that the arithmetic never
// had, which is the failure MATCH_REFERENCE_PLAN §2 exists to prevent.
//
// EVERY RECT AND EVERY SENTENCE COMES FROM EJMatchPage.h, so the suite pins the
// same geometry and the same words the user reads; this paint computes none of
// its own beyond the anchors, which come from the bins it is about to draw.

// ===========================================================================
// THE MATCH PAGE'S FILE-SCOPE DRAWING CODE
// ===========================================================================
//
// THESE FUNCTIONS ARE OUTSIDE THE EDITOR CLASS, so nothing the class brought
// into scope is in scope here: not its C alias, and not echojay. Two names are
// used often enough in the ribbon code that qualifying every occurrence would
// bury the arithmetic, so they are named ONCE here as using-declarations.
// Everything else in this block stays explicitly echojay:: qualified, because a
// reader has to be able to tell at a glance which names come from the pinned
// header and which are local to this file.
//
// THIS IS NOT A STYLE NOTE. It is where eight compile errors came from: the
// using-declarations sat BELOW the first function that used them, so
// matchFastRow saw neither. Declared here, above everything that needs them.
using echojay::kMatchRibbonCols;
using echojay::MatchRibbonRow;

/** The Match curves' frame lerp, the same rate paintSpectrumCurve uses for the
    other three surfaces (kSpectrumVisLerp). Named here because this page smooths
    the 64 STORED bins rather than the expanded curve.

    FED ONCE PER HOP, NOT ONCE PER TICK, since the trail moved onto the hop
    counter. 0.5 per hop at 43 Hz is a 16 ms time constant against 0.5 per tick
    at 30 Hz's 23 ms: slightly quicker, which is right for a filament whose job
    is to be current, and it is the constant the other three surfaces use
    unchanged rather than a number invented for this page. */
static constexpr float kMatchBinLerp = 0.5f;

// DEFINED HERE RATHER THAN FORWARD DECLARED, and the reason is a pin. A text
// pin finds a body with functionBody, which takes the FIRST match of a
// signature and reads to the next brace in column one; with a declaration
// above the definition it would read the span between them and report on a
// function it never saw. Caught by dry-running mr PIN31 before claiming it.


/** THE SCALE BOTH LAYERS SHARE. The trail and the ribbons must be mapped
    through the SAME range or the cloud would sit beside the filament rather
    than around it, and a picture whose two layers disagree about what a decibel
    is would be the fault this page's shared dB range exists to prevent. */
static std::pair<float, float> matchAxisRange (echojay::MatchAxis a,
                                               const echojay::MatchSide& mix,
                                               const echojay::MatchSide& ref,
                                               const std::array<float, 64>& mixBins,
                                               const std::array<float, 64>& refBins,
                                               bool haveBins)
{
    switch (a)
    {
        case echojay::MatchAxis::Spectrum:
        {
            if (! haveBins) return { -18.0f, 18.0f };
            auto meanOf = [] (const std::array<float, 64>& b)
            {
                double s = 0.0; int n = 0;
                for (float v : b) if (v > -120.0f) { s += v; ++n; }
                return n > 0 ? (float) (s / n) : 0.0f;
            };
            const float mM = meanOf (mixBins), rM = meanOf (refBins);
            float lo = -18.0f, hi = 18.0f;
            for (int i = 0; i < 64; ++i)
            {
                lo = juce::jmin (lo, juce::jmin (mixBins[(size_t) i] - mM,
                                                 refBins[(size_t) i] - rM) - 3.0f);
                hi = juce::jmax (hi, juce::jmax (mixBins[(size_t) i] - mM,
                                                 refBins[(size_t) i] - rM) + 3.0f);
            }
            return { lo, hi };
        }
        case echojay::MatchAxis::Loudness:
            return { juce::jmin (mix.integrated, ref.integrated) - 7.0f,
                     juce::jmax (mix.integrated, ref.integrated) + 7.0f };
        case echojay::MatchAxis::Dynamics: return { -22.0f, 22.0f };
        case echojay::MatchAxis::Stereo:   return { -20.0f, 20.0f };
    }
    return { -20.0f, 20.0f };
}


/** THE FAST ROW: the same shape as the axis's filament, built from a LIVE
    FRAME instead of the slow figure. This is what is smoked into the trail.

    Each axis reads the fastest honest source MeterData carries:
      SPECTRUM   the frame's own 64 bins, level-normalised like the filament.
      LOUDNESS   momentary, a 400 ms window with a 20 dB/s release.
      DYNAMICS   crestFactor, peak over RMS of a 0.5 s RMS and a 3 s peak
                 decay, which is the fastest crest the engine has.
      STEREO     instWidth and instCorr, the PER BLOCK pair, published for this
                 and nothing else. DISPLAY ONLY: see mr PIN31. */
static void matchFastRow (echojay::MatchAxis a, const MeterData& md,
                          const echojay::MatchSide& slow, float sign,
                          MatchRibbonRow& out)
{
    switch (a)
    {
        case echojay::MatchAxis::Spectrum:
        {
            double sum = 0.0; int n = 0;
            for (float v : md.spectrum) if (v > -120.0f) { sum += v; ++n; }
            const float mean = n > 0 ? (float) (sum / n) : 0.0f;
            for (int i = 0; i < kMatchRibbonCols; ++i)
            {
                const float u  = (float) i / (float) (kMatchRibbonCols - 1);
                const float fb = u * 63.0f;
                const int   b0 = juce::jlimit (0, 63, (int) fb);
                const int   b1 = juce::jlimit (0, 63, b0 + 1);
                out[(size_t) i] = juce::jmap (fb - (float) b0,
                                              md.spectrum[(size_t) b0],
                                              md.spectrum[(size_t) b1]) - mean;
            }
            break;
        }
        case echojay::MatchAxis::Loudness:
            for (int i = 0; i < kMatchRibbonCols; ++i)
                out[(size_t) i] = md.momentary;
            break;

        case echojay::MatchAxis::Dynamics:
        {
            const float amp = juce::jlimit (0.5f, 9.0f, md.crestFactor * 0.55f);
            for (int i = 0; i < kMatchRibbonCols; ++i)
            {
                const float u = (float) i / (float) (kMatchRibbonCols - 1);
                out[(size_t) i] = sign * 6.0f
                                + std::sin (u * juce::MathConstants<float>::twoPi * 2.2f + sign * 6.0f) * amp;
            }
            break;
        }
        case echojay::MatchAxis::Stereo:
        {
            const float wNorm = juce::jlimit (0.0f, 1.0f, md.instWidth / 100.0f);
            const float tight = 1.0f - 0.45f * juce::jlimit (0.0f, 1.0f, md.instCorr);
            const float bow   = wNorm * 16.0f * tight;
            for (int i = 0; i < kMatchRibbonCols; ++i)
            {
                const float u = (float) i / (float) (kMatchRibbonCols - 1);
                out[(size_t) i] = sign * bow * std::sin (u * juce::MathConstants<float>::pi);
            }
            break;
        }
    }
    juce::ignoreUnused (slow);
}

/** THE PICTURE, ALL FOUR AXES. Defined further down this file, beside the
    ribbon renderer they all share, because they are one family and read as one. */
void matchPaintAxis (juce::Graphics& g, juce::Rectangle<int> plot,
                     juce::Rectangle<int> readout, echojay::MatchAxis a,
                     const echojay::MatchSide& mix, const echojay::MatchSide& ref,
                     const std::array<float, 64>& mixBins, const std::array<float, 64>& refBins,
                     bool anyBins, const std::array<float, 6>& moves, float morph, float phase,
                     const juce::String& mixName, const juce::String& refName);

/** THE GUIDES, BEHIND EVERYTHING. Defined beside matchTraceRange because the
    one line the stereo axis gets is placed through that range, and a guide
    that placed itself would be free to drift from the trace it marks. */
void matchPaintGuides (juce::Graphics& g, juce::Rectangle<int> plot, echojay::MatchAxis a);

/** THE FILAMENT, OVER THE TRAIL, FROM THE SAME PER COLUMN READING. */
void matchPaintVisFilament (juce::Graphics& g, juce::Rectangle<int> plot,
                            const std::vector<float>& row, juce::Colour c);

/** THE THREE-PASS GLOW, AND THE ONE PLACE IT LIVES.

    EVERY BRIGHT LINE ON THIS PAGE GOES THROUGH HERE: the spectrum's filament
    and, since 22 Sep, each trace line on loudness, dynamics and stereo image.
    Wide and faint under narrow and bright, so a line reads as lit through the
    cloud rather than drawn over it.

    alphaScale EXISTS FOR ONE REASON AND IT IS WORTH READING BEFORE TURNING IT.
    Spectrum draws ONE line per side. The other three draw up to FIVE, so the
    same glow lands five times over and may read as busy where Spectrum reads
    as one thread. If it does, kMatchTraceGlow is the number to turn, and it is
    a separate constant precisely so those three can be calmed WITHOUT
    touching the spectrum picture Kathy has already approved.

    SETCOLOUR FIRST, SETOPACITY SECOND, three times. setColour replaces the
    whole FillType including the alpha, so the other order discards each pass's
    alpha and all three land at full strength: one fat opaque line and no glow
    at all. That is the defect that cost four rounds; open list 210. */
void matchStrokeGlow (juce::Graphics& g, const juce::Path& p, juce::Colour c,
                      float alphaScale);

/** The glow's two scales, HERE rather than beside matchStrokeGlow because
    MatchPanel::paint reads kMatchTraceGlow and is defined above it, and a free
    constant must be DECLARED BEFORE USE. (Append-only is the rule for struct
    fields; this is the other one.) */
inline constexpr float kMatchSpectrumGlow = 1.0f;
inline constexpr float kMatchTraceGlow    = 1.0f;

/** The dB/correlation range each traced axis is drawn against. Declared here
    for the same reason: the trace glow in paint places its lines through it. */
static std::pair<float, float> matchTraceRange (echojay::MatchAxis a);

void EchoJayEditor::MatchPanel::paint (juce::Graphics& g)
{
    // THE INSTRUMENT. Started before anything is drawn, stopped at every
    // return, so a short-circuited paint is measured as the short paint it is
    // rather than being left out of the mean.
    const auto paintT0 = std::chrono::high_resolution_clock::now();
    struct PaintTimer
    {
        MatchPanel& p;
        std::chrono::high_resolution_clock::time_point t0;
        ~PaintTimer()
        {
            const double ms = std::chrono::duration<double, std::milli> (
                                  std::chrono::high_resolution_clock::now() - t0).count();
            p.paintSumMs += ms;                                  // the addition
            if (ms > p.paintMaxMs) p.paintMaxMs = ms;            // the comparison
            if (++p.paintCount >= 100)
            {
                std::cout << "[MATCHPAINT] n=" << p.paintCount
                          << " mean=" << juce::String (p.paintSumMs / p.paintCount, 1)
                          << "ms max=" << juce::String (p.paintMaxMs, 1)
                          << "ms interval=" << juce::String (1000.0 / 60.0, 1) << "ms"
                          << std::endl;
                p.paintSumMs = 0.0; p.paintMaxMs = 0.0; p.paintCount = 0;
            }
        }
    } paintTimer { *this, paintT0 };

    if (host == nullptr) return;
    const auto area = getLocalBounds();


    // THE PAGE FADES IN rather than cutting. 250 ms, through one transparency
    // layer so the whole screen arrives together instead of in pieces.
    const float fade = juce::jlimit (0.0f, 1.0f, openFade);
    if (fade < 0.999f) g.beginTransparencyLayer (fade);

    g.setColour (C::bg2);
    g.fillRoundedRectangle (area.toFloat(), 10.0f);
    g.setColour (C::border);
    g.drawRoundedRectangle (area.toFloat().reduced (0.5f), 10.0f, 1.0f);

    const auto R = echojay::matchPageLayout (area);

    // ---- the data ----------------------------------------------------------
    const auto slots = host->matchSlots();
    // ONE BUILD PER TICK, read here rather than repeated. Between ticks this is
    // up to 33 ms old, which is shorter than the frame it is being drawn into;
    // the first paint of an opening has no tick behind it yet and builds its
    // own, because a default-constructed pair would draw -100 LUFS and no bands
    // as though they were measurements.
    const auto sides = tickSidesValid ? tickSides : host->buildMatchSides();
    const auto evMix = host->slotSpectralEvidence (*slots.mix);
    const auto evRef = host->slotSpectralEvidence (*slots.ref);
    const auto prop  = echojay::computeMatchProposal (sides.mix, sides.ref);
    const auto ready = echojay::matchReadiness (prop);
    const auto moves = echojay::matchBandMoves (prop);

    std::array<float, 6> deltas {};
    const bool haveDeltas = echojay::matchBandDeltas (sides.mix, sides.ref, deltas);

    // ---- THE BANNER IS GONE, AND THE PRESS STILL ANSWERS --------------------
    //
    // It drew ready.line permanently above the button and flashed refusedText
    // over it for 2.5 s. The PERMANENT half is what was wrong with it: a line
    // that is always there is furniture, and this page has none. The picture
    // took its 16 px.
    //
    // WHAT REPLACED IT IS THE SAME TWO FACTS IN TWO PLACES, both only while a
    // refusal is live, drawn further down this function:
    //   the BUTTON wears refusedBadge in place of "AI MATCH", so the control
    //     you pressed is the thing that changed;
    //   the SENTENCE draws over the plot, which is where there is room for it.
    //
    // NOTHING IS DRAWN HERE and R.status is an empty rect. ready.line is still
    // computed, and matchPressRefusedText now RETURNS it, so the reason the
    // banner used to state permanently is the reason the press states on
    // demand. It is not lost, it is no longer shouted.

    // ---- the setup row: your capture, the link, the button, the reference ---
    {
        // THE TWO PICKERS. Each says what its side is set to and opens on a
        // press, so the row is the controls rather than two labels beside one
        // control. Painted, like the button, and hit-tested in mouseUp.
        auto picker = [&] (juce::Rectangle<int> r, const juce::String& text,
                           bool isMix, bool hotHere)
        {
            if (r.getWidth() <= 0) return;
            g.setColour (hotHere ? C::bg3 : C::bg2);
            g.fillRoundedRectangle (r.toFloat(), 6.0f);
            g.setColour (hotHere ? C::blue.withAlpha (0.55f) : C::border);
            g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 6.0f, 1.0f);

            // The chevron sits on the OUTER edge of each picker, so the two
            // read as opening outward from the button rather than mirroring.
            auto body = r.reduced (7, 0);
            auto chev = isMix ? body.removeFromRight (10) : body.removeFromLeft (10);
            g.setColour (C::text3);
            {
                const float cx = (float) chev.getCentreX(), cy = (float) chev.getCentreY();
                juce::Path p;
                p.startNewSubPath (cx - 3.0f, cy - 1.5f);
                p.lineTo (cx,        cy + 2.0f);
                p.lineTo (cx + 3.0f, cy - 1.5f);
                g.strokePath (p, juce::PathStrokeType (1.2f));
            }
            g.setFont (juce::Font (juce::FontOptions (11.5f, juce::Font::bold)));
            g.setColour (isMix ? C::text : C::text2);
            g.drawFittedText (text, body,
                              isMix ? juce::Justification::centredLeft
                                    : juce::Justification::centredRight, 2);
        };

        picker (R.mixPick, host->slotDisplayName (*slots.mix), true,  hotZone == 2);
        picker (R.refPick, host->slotDisplayName (*slots.ref), false, hotZone == 3);

        // THE CONNECTOR IS THE STRONGEST THING IN THIS ROW (20 Sep 2026): the
        // button REACHING OUT to both names, rather than two stubs beside it.
        //
        // TAPERED, NOT JUST FADED. It is a filled quad, 3.2 px at the button
        // end and 0.6 px at the name end, under a gradient that falls to fully
        // transparent. A line of one thickness that only loses alpha reads as a
        // dimmer line; losing weight as well reads as reaching away and letting
        // go, which is the thing being drawn. Nothing hard-stops at either end.
        //
        // AND IT STILL MOVES: a lit head travels from your capture toward the
        // reference, because that is the direction the match runs, and its
        // brightness follows the taper so it does not glow where the rail has
        // already faded out. It is the only thing on the page that moves before
        // the press, so the screen reads as ready rather than as a still.
        auto rail = [&] (juce::Rectangle<int> r, bool buttonOnRight, float phase)
        {
            if (r.getWidth() < 2) return;
            const float x0 = (float) r.getX(), x1 = (float) r.getRight();
            const float w  = juce::jmax (1.0f, x1 - x0);
            const float y  = (float) r.getCentreY();
            const float bEnd = buttonOnRight ? x1 : x0;   // heavy, at the button
            const float fEnd = buttonOnRight ? x0 : x1;   // thin, into the name
            constexpr float thick = 3.2f, thin = 0.6f;

            juce::Path p;
            p.startNewSubPath (bEnd, y - thick * 0.5f);
            p.lineTo (fEnd, y - thin * 0.5f);
            p.lineTo (fEnd, y + thin * 0.5f);
            p.lineTo (bEnd, y + thick * 0.5f);
            p.closeSubPath();

            juce::ColourGradient grad (C::blue.withAlpha (0.80f), bEnd, y,
                                       C::blue.withAlpha (0.0f),  fEnd, y, false);
            grad.addColour (0.45, C::blue.withAlpha (0.22f));
            g.setGradientFill (grad);
            g.fillPath (p);

            const float px   = x0 + juce::jlimit (0.0f, 1.0f, phase) * w;
            const float near = buttonOnRight ? (px - x0) / w : (x1 - px) / w;   // 1 at the button
            const float a    = 0.55f * juce::jlimit (0.0f, 1.0f, near);
            juce::ColourGradient head (C::blue2.withAlpha (0.0f), px - 24.0f, y,
                                       C::blue2.withAlpha (a),    px,         y, false);
            g.setGradientFill (head);
            g.fillRect (juce::Rectangle<float> (px - 24.0f, y - 1.8f, 24.0f, 3.6f)
                            .getIntersection (r.toFloat()));
        };
        rail (R.linkLeft,  true,  linkPhase);    // reaches left, toward your capture
        rail (R.linkRight, false, linkPhase);    // reaches right, toward the reference

        // THE BUTTON. Painted, not a child: the page has no other control and
        // a child would need the overlay pool the wave cards fill.
        //
        // LIVE MEANS PLAYABLE, NOT OPTIMISTIC. The button is lit when there is
        // something for the press to play, which is not the same question as
        // whether the line above it is the cheerful one: a capture with no
        // known length keeps its ceiling move and keeps its lit button, while
        // the line leads with the length.
        // ALSO FALSE ON AN AXIS THAT CANNOT BE PROPOSED, so the button does not
        // read as ready to play something the proposal never carried. It stays
        // PRESSABLE: the press then says why, rather than the control being
        // dead with no reason on screen.
        const bool live = ready.playable && echojay::matchAxisCanPropose (axis);
        const auto bg   = buttonHot && live ? C::bg3.brighter (0.25f) : C::bg3;
        g.setColour (bg);
        g.fillRoundedRectangle (R.button.toFloat(), 8.0f);
        g.setColour (live ? C::blue : C::border);
        g.drawRoundedRectangle (R.button.toFloat().reduced (0.5f), 8.0f, live ? 1.4f : 1.0f);
        if (live && (morphing || buttonHot))
        {
            g.setColour (C::blue.withAlpha (morphing ? 0.22f : 0.10f));
            g.drawRoundedRectangle (R.button.toFloat().expanded (2.5f), 10.0f, 2.0f);
        }
        // ONE LABEL, ALMOST ALWAYS. The button keeps its name after a play
        // rather than turning into "again": a control that renames itself is a
        // second control as far as the eye is concerned, and it is the same
        // press.
        //
        // THE ONE EXCEPTION IS A LIVE REFUSAL, and it earns itself. With the
        // status banner gone, a press that cannot do anything would otherwise
        // leave the button looking exactly as it did before it was pressed,
        // which is indistinguishable from the press not registering. For the
        // 2.5 s countdown it wears refusedBadge instead, in amber, and the
        // sentence draws over the plot.
        //
        // TWO WORDS AT MOST, so it fits kMatchButtonW's 132 px at the smallest
        // window WITHOUT shrinking the type: the 12 pt bold below is the same
        // size the label always uses. See echojay::matchRefusedBadge.
        const bool refusing = refusedFor > 0.0f && refusedBadge.isNotEmpty();
        g.setColour (refusing ? C::amber : (live ? C::text : C::text3));
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        g.drawText (refusing ? refusedBadge : juce::String ("AI MATCH"),
                    R.button, juce::Justification::centred);
    }

    // ---- a waveform under each picker, and the two are NOT the same span ----
    //
    // A LIVE SIDE IS A WINDOW ONTO SOMETHING STILL RUNNING; a capture or a
    // reference is the WHOLE of a thing. Drawn identically they would say the
    // two are comparable objects, which is the same lie section 3 refuses about
    // the curves. Two things say otherwise, and neither is decoration:
    //
    //   THE SPAN IS WRITTEN UNDER EACH STRIP in words, "live, last 3.4 s"
    //   against "whole file, 2:48", so the difference is stated in the units
    //   the user thinks in rather than implied by a shape.
    //
    //   A ROLLING SIDE FADES OUT AT ITS LEFT EDGE, because the audio continues
    //   before the window and the strip is a view onto it. A whole-file side
    //   gets a hairline at both ends instead: it begins where it begins and
    //   ends where it ends.
    {
        auto strip = [&] (juce::Rectangle<int> r, const CompareSlotState& slot,
                          bool isRef, bool hotHere, bool audibleHere)
        {
            if (r.getWidth() <= 4 || r.getHeight() <= 6) return;

            std::vector<float> pts;
            bool  rolling = false;
            float span    = 0.0f;
            const bool have = host->matchWavePoints (slot, pts, rolling, span);

            auto lane = r.withTrimmedBottom (9);       // the caption takes the rest
            // THE TRANSPORT GUTTER, on the outer edge so the two sides' controls
            // sit either side of the page rather than facing each other across
            // the middle. echojay::matchWaveLane is the shared answer, so the
            // press and the paint cannot disagree about where the wave is.
            const auto slotIdxHere = isRef ? (host->matchRefIsTop() ? 0 : 1)
                                           : (host->matchRefIsTop() ? 1 : 0);
            const bool liveHere    = (slot.kind == CompareSlotState::Kind::Live);
            const auto tBtn = echojay::matchWaveTransport (lane, isRef);
            lane = echojay::matchWaveLane (lane, isRef);

            g.setColour (audibleHere ? C::bg3 : C::bg2.withAlpha (0.7f));
            g.fillRoundedRectangle (lane.toFloat(), 4.0f);
            if (audibleHere || hotHere)
            {
                g.setColour ((audibleHere ? C::blue : C::border2).withAlpha (audibleHere ? 0.6f : 0.4f));
                g.drawRoundedRectangle (lane.toFloat().reduced (0.5f), 4.0f, 1.0f);
            }

            if (have && lane.getWidth() > 2)
            {
                const float midY  = (float) lane.getCentreY();
                const float halfH = (float) lane.getHeight() * 0.5f - 1.0f;
                const int   cols  = lane.getWidth();
                const auto  base  = isRef ? C::text2 : C::blue;
                for (int x = 0; x < cols; ++x)
                {
                    // The whole of what we have, squeezed to the strip: a
                    // capture is its own length whatever the panel is wide.
                    const size_t i0 = (size_t) ((double) x       / cols * (double) pts.size());
                    const size_t i1 = (size_t) ((double) (x + 1) / cols * (double) pts.size());
                    float peak = 0.0f;
                    for (size_t i = i0; i < juce::jmax (i0 + 1, juce::jmin (i1, pts.size())); ++i)
                        peak = juce::jmax (peak, pts[i]);
                    peak = juce::jlimit (0.0f, 1.0f, peak);

                    // THE ROLLING FADE: oldest sample faintest, so the strip
                    // reads as a window rather than a complete object.
                    const float a = rolling ? juce::jmap ((float) x, 0.0f, (float) cols, 0.25f, 0.95f)
                                            : 0.9f;
                    g.setColour (base.withAlpha (a));
                    const float h = juce::jmax (1.0f, peak * halfH);
                    g.fillRect ((float) lane.getX() + (float) x, midY - h, 1.0f, h * 2.0f);
                }
                if (! rolling)
                {
                    // A whole file BEGINS and ENDS. Two hairlines say so.
                    g.setColour (C::border2.withAlpha (0.8f));
                    g.fillRect ((float) lane.getX(), (float) lane.getY() + 2.0f, 1.0f,
                                (float) lane.getHeight() - 4.0f);
                    g.fillRect ((float) lane.getRight() - 1.0f, (float) lane.getY() + 2.0f, 1.0f,
                                (float) lane.getHeight() - 4.0f);
                }
            }
            else
            {
                g.setColour (C::text3.withAlpha (0.6f));
                g.setFont (juce::Font (juce::FontOptions (9.0f)));
                g.drawText (rolling ? "waiting for signal" : "no waveform",
                            lane, juce::Justification::centred, true);
            }

            // THE PLAYHEAD, from the STREAM'S OWN POSITION. A timer counting
            // seconds beside it would drift the moment the stream looped, was
            // seeked or was stopped, and would then be a confident wrong line.
            const float frac = host->compareStreamFrac (slotIdxHere);
            if (frac >= 0.0f && lane.getWidth() > 2)
            {
                const float x = (float) lane.getX() + frac * (float) lane.getWidth();
                g.setColour (C::blue2.withAlpha (0.9f));
                g.fillRect (x - 0.5f, (float) lane.getY(), 1.5f, (float) lane.getHeight());
            }

            // A PLAY AND STOP PER SIDE, showing which state it is in rather
            // than being a control whose effect you learn by pressing it.
            //
            // A LIVE MIX SIDE HAS NO TRANSPORT AT ALL, and says so instead of
            // showing a dead button: toggleComparePlay excludes a Live slot for
            // the reason in its own comment, that live is host passthrough with
            // no stored stream to toggle. Same rule as the two axes that cannot
            // be proposed: nothing looks pressable unless it does something.
            if (tBtn.getWidth() >= 8 && tBtn.getHeight() >= 8)
            {
                if (liveHere)
                {
                    g.setColour (C::text3.withAlpha (0.8f));
                    g.setFont (juce::Font (juce::FontOptions (8.0f)));
                    g.drawFittedText ("host", tBtn, juce::Justification::centred, 1);
                }
                else
                {
                    const bool rollingNow = host->compareSlotPlaying (slotIdxHere);
                    g.setColour ((hotHere ? C::blue : C::text2).withAlpha (0.9f));
                    const auto b = tBtn.toFloat().reduced (3.0f);
                    if (rollingNow)
                    {
                        // STOP: a square, which is what it will do next.
                        g.fillRect (b.reduced (b.getWidth() * 0.18f, b.getHeight() * 0.18f));
                    }
                    else
                    {
                        juce::Path p;
                        p.addTriangle (b.getX(), b.getY(), b.getX(), b.getBottom(),
                                       b.getRight(), b.getCentreY());
                        g.fillPath (p);
                    }
                }
            }

            g.setColour (C::text3);
            g.setFont (juce::Font (juce::FontOptions (9.0f)));
            g.drawText (echojay::matchWaveSpan (rolling, span),
                        r.removeFromBottom (9),
                        isRef ? juce::Justification::centredRight : juce::Justification::centredLeft,
                        true);
        };

        const bool refIsTop = host->matchRefIsTop();
        strip (R.mixWave, *slots.mix, false, hotZone == 4, host->compareSlotAudible (refIsTop ? 1 : 0));
        strip (R.refWave, *slots.ref, true,  hotZone == 5, host->compareSlotAudible (refIsTop ? 0 : 1));
    }

    // ---- the axis row: four tiles, one selected ----------------------------
    //
    // SELECTING A TILE CHANGES THE PICTURE BELOW AND NOTHING ELSE. Nothing is
    // sent, applied or written by a press here, which is why these controls are
    // in step one although section 10 excluded the ask.
    {
        g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::bold)));
        for (int i = 0; i < echojay::kMatchAxisCount; ++i)
        {
            const auto a    = (echojay::MatchAxis) i;
            const auto tile = echojay::matchAxisTile (R.axisRow, i);
            if (tile.getWidth() <= 0) continue;

            const bool sel = (a == axis);
            const bool hot = (hotZone == 10 + i);
            g.setColour (sel ? C::blue.withAlpha (0.16f) : (hot ? C::bg3 : C::bg2.withAlpha (0.6f)));
            g.fillRoundedRectangle (tile.toFloat(), echojay::kEjCellRadius);
            g.setColour (sel ? C::blue.withAlpha (0.7f) : C::border);
            g.drawRoundedRectangle (tile.toFloat().reduced (0.5f), echojay::kEjCellRadius,
                                    sel ? 1.4f : 1.0f);

            // AN AXIS THAT CANNOT BE PROPOSED SAYS SO ON ITS OWN TILE, quietly,
            // so the press is not the first place the user learns it.
            const bool proposable = echojay::matchAxisCanPropose (a);
            g.setColour (sel ? C::text : (proposable ? C::text2 : C::text3));
            g.drawFittedText (echojay::matchAxisName (a), tile.reduced (4, 0),
                              juce::Justification::centred, 1);
        }
    }

    // ---- the picture -------------------------------------------------------
    //
    // THE GROUND IS NEAR BLACK. The reference image is strands on black and
    // that contrast is most of the effect: on C::bg3's navy the same ink read
    // as a smear because the strands had nothing to be bright against. The
    // card keeps its thin border, which is chrome around the picture rather
    // than anything inside it.
    g.setColour (juce::Colour (0xff02040a));
    g.fillRoundedRectangle (R.graph.toFloat(), 10.0f);
    {
        // SCOPED, BECAUSE C::border IS WHITE AT FIVE PERCENT ALPHA.
        //
        // Graphics::setColour replaces the whole FillType, so the fill's
        // opacity becomes THE NEW COLOUR'S OWN ALPHA: it does not reset to 1.
        // A translucent token therefore leaves the context at 0.05 for every
        // later call that takes its opacity from the fill rather than setting
        // its own colour, and drawImageAt is exactly that. THE TRAIL WAS BEING
        // COMPOSITED AT FIVE PERCENT because of this one outline.
        //
        // ScopedSaveState rather than a setOpacity afterwards: the next person
        // to add a translucent colour here should not have to know this.
        juce::Graphics::ScopedSaveState ss (g);
        g.setColour (C::border);
        g.drawRoundedRectangle (R.graph.toFloat().reduced (0.5f), 10.0f, 1.0f);
    }

    const auto plot = echojay::matchGraphPlot (R.graph);
    // NO BIN WIDTH IS NEEDED HERE ANY MORE. It used to be computed for the
    // filament, from a hardcoded 44100 before that. The filament now reads the
    // SAME STORED ROW the trail was stroked from, so there is no second place
    // where a frequency axis could be got wrong: advanceTrail owns the bin
    // width, and it takes it from the session's real sample rate.

    // A TILE WITH A FIELD MISSING ON ONE SIDE SAYS SO RATHER THAN DRAWING A
    // DEFAULT. Every sentinel in MatchSide renders happily as a measurement:
    // -100 LUFS is a position, 0 LU is a width, -1 overs is a number.
    const auto axisState = echojay::matchAxisState (axis, sides.mix, sides.ref);
    if (! axisState.drawable)
    {
        g.setColour (C::text3);
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        g.drawFittedText (axisState.why, plot, juce::Justification::centred, 2);
    }
    else
    {
        // ONE CALL FOR ALL FOUR AXES. The picture is the same structure
        // everywhere, two ribbons and the light between them, so the branch
        // that used to choose between a spectrum block and three painters is
        // now a dispatch inside matchPaintAxis.
        //
        // THE BINS ARE THE SPECTRUM'S OWN DATA: 64 measurements per side, not
        // the six bands, which is why a smooth curve is honest there. THE SIX
        // BANDS ARE STILL WHAT THE PROPOSAL MOVES, passed here as the morph's
        // moves and stepped at the band edges by matchMorphedDb.
        const bool haveBins = evMix.valid && evRef.valid;

        // ---- THE GUIDES, FIRST, SO THE GHOSTS SIT ON TOP OF THEM ----------
        //
        // Before the trail's blit and before the filament, which is the whole
        // point: a guide the picture covers is a guide, and a guide over the
        // picture is furniture.
        matchPaintGuides (g, plot, axis);

        // ONCE PER NEW FFT, NOT ONCE PER FRAME.
        //
        // WHICH ENGINE DRIVES IT, because the two sides are NOT in step: each
        // side reads its own MeterEngine (the main one for Live, that slot's
        // cmpMeter when its stream is playing), and those publish hops on
        // their own audio blocks. THE MIX SIDE'S ENGINE DRIVES THE ADVANCE
        // when it has one, the reference's otherwise. A side whose engine
        // happens to publish between advances simply has its newest spectrum
        // read on the next one; nothing is dropped, because at 60 Hz against
        // 43 Hz there is at most one new hop per frame.
        auto* hopEngine = host->matchFastEngine (*slots.mix, host->matchRefIsTop() ? 1 : 0);
        if (hopEngine == nullptr)
            hopEngine = host->matchFastEngine (*slots.ref, host->matchRefIsTop() ? 0 : 1);
        const uint32_t hopNow = hopEngine != nullptr ? hopEngine->visHopCount() : 0;
        const bool newHop = hopEngine != nullptr && (! trailHopInit || hopNow != trailHop);

        // Guarded on the HOP for the reason it used to be guarded on the tick:
        // an extra repaint, a hover or a resize, must not advance history.
        // THE CURVES SMOOTH FRAME TO FRAME, one state per side, the way the
        // other three spectrum surfaces already do. Fed once per TICK for the
        // trail's reason: a repaint is not a frame of data.
        if (haveBins && newHop)
        {
            mixBinLerp.feed (evMix.bins, kMatchBinLerp);
            refBinLerp.feed (evRef.bins, kMatchBinLerp);
        }
        const auto& mixSmooth = mixBinLerp.init ? mixBinLerp.v : evMix.bins;
        const auto& refSmooth = refBinLerp.init ? refBinLerp.v : evRef.bins;

        // ---- THE CLOUD: this tick's FAST frame, smoked into the trail ------
        //
        // The cloud is CONTEXT and is never compared; the filament below is the
        // measurement. A side with no fast source gets no cloud at all.
        MeterData mixMd, refMd;
        const bool mixFastOk = host->matchFastFrame (*slots.mix, host->matchRefIsTop() ? 1 : 0, mixMd);
        const bool refFastOk = host->matchFastFrame (*slots.ref, host->matchRefIsTop() ? 0 : 1, refMd);

        echojay::MatchRibbonRow mixFast {}, refFast {};
        if (mixFastOk) matchFastRow (axis, mixMd, sides.mix, -1.0f, mixFast);
        if (refFastOk) matchFastRow (axis, refMd, sides.ref,  1.0f, refFast);

        if (newHop)
        {
            trailHop = hopNow;
            trailHopInit = true;
            mixVisOk = refVisOk = false;
            if (mixFastOk)
            {
                pushTrace (mixTrace, axis, mixMd);
                double hz = 0.0;
                if (auto* e = host->matchFastEngine (*slots.mix, host->matchRefIsTop() ? 1 : 0))
                    mixVisOk = e->getVisualSpectrum (mixVis, hz);
            }
            if (refFastOk)
            {
                pushTrace (refTrace, axis, refMd);
                double hz = 0.0;
                if (auto* e = host->matchFastEngine (*slots.ref, host->matchRefIsTop() ? 0 : 1))
                    refVisOk = e->getVisualSpectrum (refVis, hz);
            }

            // ---- AND THE REFERENCE'S STORED CURVE WHEN IT IS NOT ROLLING ----
            //
            // LIVE IS PREFERRED WHILE THE STREAM ROLLS, and the reason is that
            // the two are different measurements of different things: the live
            // analysis is what you are HEARING right now, the stored eqCurve is
            // the whole file's average. While audio is playing, the thing on
            // screen should be the thing in your ears; the moment it stops,
            // the whole-file average is strictly better than nothing and is
            // what the reference actually knows about itself.
            //
            // evRef.bins IS the eqCurve for a Reference slot, already fetched
            // by getSlotSpectralEvidence and stamped WholeFileAverage, so this
            // reads no analyser and re-derives nothing.
            refStaticOk = false;
            if (! refVisOk && evRef.valid)
            {
                const double sr = host->streamSampleRate() > 0.0
                                    ? host->streamSampleRate() : 44100.0;
                refStatic   = EchoJayEditor::expandLog64Spectrum (
                                  evRef.bins, sr / (double) MeterEngine::kVisFftSize);
                refStaticOk = true;
            }
        }

        const auto range = matchAxisRange (axis, sides.mix, sides.ref,
                                           mixSmooth, refSmooth, haveBins);
        advanceTrail (plot, mixFastOk ? &mixFast : nullptr, refFastOk ? &refFast : nullptr,
                      range.first, range.second);
        // THE IMAGE JUST COMPOSED, NOT THE ONE BEFORE IT. advanceTrail picks
        // dst as (trailUseA ? trailB : trailA) and then FLIPS trailUseA, so
        // after it returns the flag selects the OTHER image. Reading it the
        // same way here showed the previous frame, one hop stale, every frame.
        const bool  blitGuard = (trailPlot == plot);
        const auto& shownImg  = trailUseA ? trailA : trailB;

        if (blitGuard && ! shownImg.isNull())
            g.drawImageAt (shownImg, plot.getX(), plot.getY());

        // ---- THE FILAMENT, OVER THE TRAIL --------------------------------
        //
        // SPECTRUM ONLY, AND FROM THE VIS BINS RATHER THAN THE RIBBON ROWS.
        // The trail is stroked from mixVis/refVis, 2048 bins through
        // matchVisColumnYOffset; the ribbon rows are 96 columns off the 64 bin
        // curve on a different range. Drawing the thread from the rows would
        // put it NEAR the newest ghost instead of on it, which is the one
        // thing this line must not do. The other three axes keep their traces
        // as the only lines: those are figures over TIME, so a per column
        // frequency reading has nothing to say about them.
        //
        // REFERENCE FIRST, MIX OVER IT, the same order as the trail, so the
        // two threads overlap the way their clouds do.
        if (axis == echojay::MatchAxis::Spectrum)
        {
            // The filament follows the same source choice as the trail,
            // because it is drawn from the row the trail was stroked from.
            if ((refVisOk || refStaticOk) && visRowRefInit)
                matchPaintVisFilament (g, plot, visRowRef, C::text2);
            if (mixVisOk && visRowMixInit)
                matchPaintVisFilament (g, plot, visRowMix, C::blue);
        }
        else
        {
            // ---- THE SAME TREATMENT ON THE OTHER THREE AXES --------------
            //
            // Each trace line's CURRENT path, over the trail, through the
            // same matchStrokeGlow the spectrum filament uses. Not a second
            // glow: one function, two callers, so the two halves of the page
            // cannot drift apart in width or alpha.
            //
            // THE PATH IS BUILT THE WAY traceOf BUILDS IT, deliberately to
            // the same arithmetic: oldest at the left, newest at the right,
            // x by k/(n-1), y through matchTraceRange with the same half
            // stroke inset, so the bright line lands exactly on the newest
            // ghost rather than near it. If traceOf's mapping is ever
            // changed, this must change with it.
            //
            // UP TO FIVE LINES PER SIDE HERE AGAINST SPECTRUM'S ONE. See
            // kMatchTraceGlow: that is the knob if this reads as busy.
            const auto tr = matchTraceRange (axis);
            auto glowTrace = [&] (const Trace& t, juce::Colour c)
            {
                if (t.filled < 2) return;
                const int n = t.filled;
                for (int line = 0; line < kTraceLines; ++line)
                {
                    if (! t.have[(size_t) line]) continue;
                    juce::Path p;
                    for (int k = 0; k < n; ++k)
                    {
                        const int idx = (t.write - n + k + kTraceLen * 2) % kTraceLen;
                        const float x = (float) plot.getX()
                                      + (float) plot.getWidth() * (float) k / (float) (n - 1);
                        const float inset = 0.9f;
                        const float t01 = juce::jlimit (0.0f, 1.0f,
                                            (tr.second - t.v[(size_t) line][(size_t) idx])
                                                / juce::jmax (0.0001f, tr.second - tr.first));
                        const float y = (float) plot.getY() + inset
                                      + t01 * juce::jmax (1.0f, (float) plot.getHeight()
                                                                    - 2.0f * inset);
                        if (k == 0) p.startNewSubPath (x, y);
                        else        p.lineTo (x, y);
                    }
                    matchStrokeGlow (g, p, c, kMatchTraceGlow);
                }
            };
            // ---- A STOPPED REFERENCE DRAWS ITS STORED FIGURES FLAT -----
            //
            // SAME RULE AS SPECTRUM: live while rolling, stored otherwise. A
            // reference that is loaded but not playing has no live trace at
            // all (matchFastEngine and matchFastFrame both require
            // loaded && playing), so before this the whole reference side of
            // these three pictures simply was not there.
            //
            // A LINE ONLY WHERE A REAL STORED FIGURE EXISTS, and nothing
            // where there is none. The gate is echojay::matchHas*, the SAME
            // predicates the readout row reads, so the picture and the row
            // cannot disagree: a dash in the row means no line in the plot,
            // always. NOTHING IS INVENTED HERE: no sentinel is drawn, no zero
            // is substituted, and in particular a crest pinned at the 40 dB
            // clamp draws NO LINE rather than a confident flat one, which
            // would trade an absence for a plausible lie.
            //
            // THE BANDED SUB-LINES GET NOTHING, because MatchSide carries no
            // whole-file equivalent of bandCrestSub/Mid/Top or of
            // corrSub/Mid/Top. Their absence from a stopped reference is
            // correct rather than missing.
            auto flatLine = [&] (bool have, float value)
            {
                if (! have) return;
                const float inset = 0.9f;
                const float t01 = juce::jlimit (0.0f, 1.0f,
                                    (tr.second - value)
                                        / juce::jmax (0.0001f, tr.second - tr.first));
                const float y = (float) plot.getY() + inset
                              + t01 * juce::jmax (1.0f, (float) plot.getHeight() - 2.0f * inset);
                juce::Path p;
                p.startNewSubPath ((float) plot.getX(), y);
                p.lineTo ((float) plot.getRight(), y);
                matchStrokeGlow (g, p, C::text2, kMatchTraceGlow);
            };

            if (! refFastOk)
            {
                const auto& r = sides.ref;
                switch (axis)
                {
                    case echojay::MatchAxis::Loudness:
                        flatLine (echojay::matchHasIntegrated (r), r.integrated);
                        flatLine (echojay::matchHasTruePeak   (r), r.truePeak);
                        break;
                    case echojay::MatchAxis::Dynamics:
                        flatLine (echojay::matchHasCrest (r), r.crest);
                        break;
                    case echojay::MatchAxis::Stereo:
                        flatLine (echojay::matchHasCorrelation (r), r.correlation);
                        // Width rides the correlation scale the same way
                        // pushTrace maps it, so the flat line lands where the
                        // live trace would have put it.
                        flatLine (echojay::matchHasWidth (r),
                                  r.width / 100.0f * 2.0f - 1.0f);
                        break;
                    case echojay::MatchAxis::Spectrum: break;   // not a traced axis
                }
            }

            // Reference first, mix over it: the same order as the trail.
            glowTrace (refTrace, C::text2);
            glowTrace (mixTrace, C::blue);
        }

        matchPaintAxis (g, plot, R.readout, axis, sides.mix, sides.ref,
                        mixSmooth, refSmooth,
                        // EITHER side, not both: the message this feeds is
                        // now "is there nothing at all", and the picture was
                        // drawn above from whichever sides had data.
                        evMix.valid || evRef.valid,
                        echojay::matchBandMoves (prop),
                        juce::jlimit (0.0f, 1.0f, morphPos), linkPhase,
                        host->slotDisplayName (*slots.mix),
                        host->slotDisplayName (*slots.ref));

        // ---- THE REFUSAL, OVER THE PICTURE -------------------------------
        //
        // DRAWN OVER, NOT INSERTED INTO. It takes no height from the layout
        // and moves nothing: `plot` is the rect the picture was just drawn
        // in, and this composes on top of it for the countdown only. That is
        // the whole reason it can live here at all, because the page has no
        // spare row any more.
        //
        // LAST, so nothing paints over it: the trail blit, the filament and
        // the axis painter have all already run.
        //
        // AT THE BANNER'S OWN SIZE, 11 pt, which is what it was drawn at
        // above the button. A refusal that shrank on its way to a bigger
        // space would read as less important than the one it replaced.
        //
        // A SCRIM UNDER IT, because the sentence lands on a lit trail and
        // amber text over moving ghosts is the one thing on this page that
        // must be readable on the first glance. Scoped, because the fill is
        // translucent and drawImageAt is one call away: open list 210.
        if (refusedFor > 0.0f && refusedText.isNotEmpty())
        {
            juce::Graphics::ScopedSaveState ss (g);
            auto band = plot.withSizeKeepingCentre (plot.getWidth() - 24,
                                                    juce::jmin (plot.getHeight(), 52));
            g.setColour (juce::Colour (0xff02040a).withAlpha (0.82f));
            g.fillRoundedRectangle (band.toFloat(), 8.0f);
            g.setColour (C::amber);
            g.setFont (juce::Font (juce::FontOptions (11.0f)));
            g.drawFittedText (refusedText, band.reduced (10, 6),
                              juce::Justification::centred, 3);
        }
    }

    // THE PROVENANCE, ON THE PICTURE (contract §3): a whole-file average
    // against a ballistic tail is not a like-for-like curve, and prose
    // elsewhere is not the picture saying so.
    //
    // SPECTRUM ONLY. It describes the two SPECTRA's reductions, so under the
    // loudness, dynamics or stereo picture it would be a caption about
    // something not on screen.
    if (axis == echojay::MatchAxis::Spectrum)
    {
        // THE TILT LINE IS GONE, 22 Sep. It said "Tilted for display; the
        // figures below are not", and it came out because it describes a
        // DISPLAY CHOICE rather than a measurement: the readout row underneath
        // is untilted and is where every figure is read, so nothing measured
        // is lost with the sentence.
        //
        // THE PROVENANCE LINE STAYS, and that is a deliberate refusal rather
        // than an oversight. It was asked for too, but it is the only place
        // ANYTHING tells the reader that the two curves were reduced
        // differently: matchProvenanceText returns "" whenever both sides
        // match, so when it speaks the comparison is not like for like. The
        // model never receives it, because nothing on this page sends and the
        // 64 bin curves reach no payload; the COMPARE path carries
        // ev.macroReduction, which is the BANDS' provenance and a different
        // field by design (see EJSpectralEvidence.h: the two can legitimately
        // differ). So deleting this deletes the only statement of it.
        // THE PROVENANCE LINE IS GONE FROM THE DRAWING TOO, 22 Sep, for the
        // same reason as the dynamics notes: the two row table names the
        // sides in its left column, so a rolling live side sits against a
        // file name with the figures between them and the reader can see
        // which is which without a sentence about it.
        //
        // WHAT THIS GIVES UP, so nobody restores it blind. matchProvenanceText
        // speaks ONLY when the two sides were reduced DIFFERENTLY, which is
        // exactly the case where the curves are not like for like. The names
        // in the left column tell you the sides are of different KINDS; they
        // do not tell you the two spectra were REDUCED differently, and two
        // captures can be reduced differently while looking like the same kind
        // of thing. That residue is uncovered today.
        //
        // IT IS NOT DELETED. echojay::matchProvenanceText is still the single
        // source of this wording and is still pinned by mr PIN19, for whatever
        // carries it after the merge. Nothing drawn today calls it.
        juce::ignoreUnused (evMix, evRef);
    }

    if (fade < 0.999f) g.endTransparencyLayer();
}

// THE PRESS. It plays the morph, or says why it cannot; it sends nothing and
// writes nothing (MATCH_SCREEN_CONTRACT §10). Replayable by design: a second
// press starts from the measured curve again rather than doing nothing because
// the first one finished.
void EchoJayEditor::MatchPanel::press()
{
    if (host == nullptr) return;
    // ONE read of the sides, not two: they are read from live slots, and two
    // reads could disagree about what the press was for.
    const auto sides = host->buildMatchSides();
    const auto ready = echojay::matchReadiness (echojay::computeMatchProposal (sides.mix, sides.ref));

    // TWO OF THE FOUR AXES CANNOT BE MATCHED, AND THE PRESS SAYS SO. The
    // proposal emits Gain, Ceiling and Band moves and nothing else, so dynamics
    // and stereo image have no move to play whatever the two sides hold.
    //
    // NOT A DISABLED BUTTON. A control that looks dead with no reason given is
    // what the headline rework removed, so this answers in the same words and
    // through the same refused-for-a-moment machinery the gain floor uses.
    if (! echojay::matchAxisCanPropose (axis))
    {
        refusedText  = echojay::matchAxisPressText (axis);
        refusedBadge = echojay::matchRefusedBadge (true, false);
        refusedFor   = 2.5f;
        morphing = false; morphDone = false; morphPos = 0.0f;
        startTimerHz (60);
        repaint();
        return;
    }

    // PLAYABLE, NOT POSSIBLE. The press asks whether the proposal carries a
    // move, which is what this test has always meant; `possible` now answers
    // the narrower question of whether the LINE is the optimistic one, and
    // gating the press on that would refuse a move the proposal is holding.
    if (! ready.playable)
    {
        // A REFUSED PROPOSAL CANNOT ANIMATE, so the press SAYS so rather than
        // playing nothing and leaving the user to guess whether it worked.
        refusedText  = echojay::matchPressRefusedText (ready);
        refusedBadge = echojay::matchRefusedBadge (false, ready.goodNews);
        refusedFor   = 2.5f;
        morphing = false; morphDone = false; morphPos = 0.0f;
        startTimerHz (60);
        repaint();
        return;
    }

    morphPos  = 0.0f;
    morphing  = true;
    morphDone = false;
    refusedFor = 0.0f;
    startTimerHz (60);
    repaint();
}

// ===========================================================================
// THE THREE PICTURES THAT ARE NOT THE SPECTRUM
// ===========================================================================
//
// ONE GRAMMAR, FOUR VOCABULARIES. Every one of these shows your mix, the
// reference, and THE GAP BETWEEN THEM with the gap the brightest thing in the
// rect. What differs is the terms, because a level, a dynamic range and a
// stereo image are not the same kind of quantity and drawing them alike would
// say they were.
//
// ONLY FIELDS THAT ARE WHOLE FILE ON BOTH SIDES (contract §2B). No PSR, which
// is two sliding windows on both sides and describes a reference's last three
// seconds; and no capture-only field, because a figure one side cannot have
// would be drawn against the other side's default and read as a difference.
//
// EVERY NUMBER HERE COMES OFF MatchSide, which is the one place the Live rules
// have already been applied.
// The same colour tokens the rest of this page uses. These painters are
// file-local functions rather than members, so the editor's own alias is not in
// scope and is named once here.
//
// AT FILE SCOPE AND NOT IN AN ANONYMOUS NAMESPACE, deliberately: mr PIN24 reads
// these bodies with functionBody, which ends a body at the first brace in
// column one. Indented inside a namespace, each "body" would run to the end of
// the block and the pin would be reading three painters while claiming to read
// one.
using MatchC = EchoJayLookAndFeel::Colours;

// ===========================================================================
// TWO LUMINOUS RIBBONS AND THE LIGHT BETWEEN THEM
// ===========================================================================
//
// ONE STRUCTURE ON ALL FOUR AXES, and only the ribbons' SHAPE differs. The
// reference is a soft ribbon, your mix a brighter filament in front, and the
// gap is the light between them: invisible where they touch and brightest
// where they are furthest apart.
//
// THICKNESS MEANS TOLERANCE AND NOTHING ELSE. The reference ribbon's
// half-thickness IS the proposal's floor, echojay::matchAxisZoneDb, so inside
// the ribbon means no move would be proposed. ON THE TWO AXES WITH NO FLOOR IT
// IS A FILAMENT, not a thin ribbon: a glow with any thickness at all would
// claim a tolerance that does not exist for dynamics or stereo image.
//
// NO FURNITURE. No rectangle, no rail, no divider, no gridline, no band label,
// no box outline and no step block: frequency and level are read from the
// numbers in the text line. Nothing here has an edge.

/** THE READOUT ROW, DRAWN IN THE MAIN METER STRIP'S LANGUAGE: cells separated
    by thin vertical dividers, label above in small muted uppercase, value
    below. No rounded boxes, no card.

    THE COLOURS ARE READ FROM THE SAME TWO CONSTANTS THE TRAILS ARE STROKED
    WITH, MatchC::blue and MatchC::text2, not copies of their values. If the
    trail's colours are ever changed this row follows them, because a key that
    can drift from the picture it explains is worse than no key. */
static void matchPaintReadout (juce::Graphics& g, juce::Rectangle<int> row,
                               const std::vector<echojay::MatchReadoutCell>& cells,
                               const juce::String& note,
                               const juce::String& mixName, const juce::String& refName)
{
    if (row.getHeight() < 16 || cells.empty()) return;

    // 2d: the axis's sentence, if it has one, on ONE muted line directly above
    // the table. An axis with none leaves no gap behind.
    if (note.isNotEmpty())
    {
        auto noteRow = row.removeFromTop (10);
        g.setColour (MatchC::text3);
        g.setFont (juce::Font (juce::FontOptions (8.5f)));
        g.drawText (note, noteRow, juce::Justification::centred, true);
    }

    // THE THREE HEIGHTS PREFER THE CONSTANTS AND GIVE WAY TO THE ROW. With a
    // note above it there are 24 px left rather than 34, and a table that kept
    // its preferred heights would simply draw its second side off the bottom.
    int headerH = echojay::kMatchReadoutHeaderH;
    int rowH    = echojay::kMatchReadoutRowH;
    if (row.getHeight() < headerH + 2 * rowH)
    {
        headerH = juce::jmax (7, row.getHeight() / 4);
        rowH    = juce::jmax (7, (row.getHeight() - headerH) / 2);
    }

    const int n = (int) cells.size();

    // ---- THE HEADER: short column labels, over the columns they name -------
    {
        auto header = row.removeFromTop (headerH);
        g.setColour (MatchC::text3);
        g.setFont (juce::Font (juce::FontOptions (8.0f, juce::Font::bold)));
        for (int i = 0; i < n; ++i)
        {
            auto col = echojay::matchReadoutColRect (header, i, n);
            if (col.getWidth() <= 0) continue;
            g.drawText (cells[(size_t) i].label.toUpperCase(), col,
                        juce::Justification::centred, true);
        }
    }

    // ---- THE TWO SIDES, ONE PER LINE ---------------------------------------
    //
    // THE NAME IS THE KEY AND IT IS DRAWN IN THE TRAIL'S OWN COLOUR, read from
    // MatchC::blue and MatchC::text2, the same two constants advanceTrail
    // strokes with. A key that can drift from the picture it explains is worse
    // than no key.
    //
    // TRUNCATED, NOT WRAPPED. drawText is a single line and the last argument
    // is useEllipsis, so a long reference name ends in an ellipsis inside its
    // gutter rather than pushing the figures out of line.
    auto paintSide = [&] (juce::Rectangle<int> line, const juce::String& name,
                          juce::Colour colour, bool wantMix)
    {
        auto gutter = echojay::matchReadoutNameRect (line);
        g.setColour (colour);
        g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::bold)));
        g.drawText (name, gutter.reduced (2, 0), juce::Justification::centredLeft, true);

        g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
        for (int i = 0; i < n; ++i)
        {
            auto col = echojay::matchReadoutColRect (line, i, n);
            if (col.getWidth() <= 0) continue;
            g.drawText (wantMix ? cells[(size_t) i].mixText : cells[(size_t) i].refText,
                        col, juce::Justification::centred, true);
        }
    };

    paintSide (row.removeFromTop (rowH), mixName, MatchC::blue,  true);
    paintSide (row.removeFromTop (rowH), refName, MatchC::text2, false);
}


/** The whole picture, once the two rows of values exist. */
static void matchPaintRibbonPair (juce::Graphics& g, juce::Rectangle<int> plot,
                                  const MatchRibbonRow& mixV, const MatchRibbonRow& refV,
                                  float lo, float hi, float zoneUnits, float phase)
{
    // ONE THIN BRIGHT FILAMENT PER SIDE, AND NOTHING ELSE IN THE PLOT.
    //
    // WHAT THIS GIVES UP, RECORDED HERE RATHER THAN LOST QUIETLY:
    //
    //   THE REFERENCE'S RIBBON THICKNESS WAS THE PROPOSAL'S FLOOR. A band of
    //   exactly kMatchBandFloorDb said "inside here, nothing would be moved",
    //   which is the most honest thing this picture has ever drawn.
    //
    //   THE GAP LIGHT WAS THE THING BEING COMPARED, drawn as the brightest
    //   object on the page, which is what the contract asks a match screen to
    //   put first.
    //
    // BOTH WERE HONEST AND BOTH ARE REMOVED BECAUSE THEY BURY THE TRAIL. The
    // comparison still reads, from the two filaments and the distance between
    // them, and every number, move and refusal is unchanged. THIS IS A DEBT
    // AND NOT AN OVERSIGHT: the floor and the gap want to come back in some
    // quieter form once the look is settled, and matchAxisZoneDb and
    // matchInsideBandZone are left in place for when they do.
    //
    // zoneUnits and phase are kept in the signature for the same reason: the
    // thickness and the breath are what comes back first.
    juce::ignoreUnused (zoneUnits, phase);

    // CURVED, NOT CORNERED. 96 points across a thousand-odd pixels is one
    // point every dozen, and straight lineTo segments make every one of them a
    // visible corner: that is the jaggedness, and it is SPATIAL rather than
    // temporal. Averaging across frames would have fixed the look by
    // destroying the strands, which is exactly what the trail must not have.
    //
    // THE SAME CATMULL-ROM paintSpectrumCurve uses: endpoints duplicated so
    // the end tangents stay flat and never overshoot, control points at a
    // sixth of the span between the neighbours, and CONTROL-POINT Y CLAMPED
    // INTO THE PLOT so a single outlying bin cannot overshoot into a needle.
    auto filament = [&] (const MatchRibbonRow& row, juce::Colour c, float alpha)
    {
        const float dx = (float) plot.getWidth() / (float) (kMatchRibbonCols - 1);
        auto clampY = [&] (float v)
        { return juce::jlimit ((float) plot.getY(), (float) plot.getBottom(), v); };
        auto ptAt = [&] (int i)
        {
            const int j = juce::jlimit (0, kMatchRibbonCols - 1, i);
            const float y = (float) plot.getY()
                          + juce::jlimit (0.0f, 1.0f, (hi - row[(size_t) j])
                                                          / juce::jmax (0.0001f, hi - lo))
                                * (float) plot.getHeight();
            return juce::Point<float> ((float) plot.getX() + dx * (float) j, y);
        };

        juce::Path p;
        p.startNewSubPath (ptAt (0));
        for (int i = 0; i < kMatchRibbonCols - 1; ++i)
        {
            const auto p0 = ptAt (i - 1), p1 = ptAt (i), p2 = ptAt (i + 1), p3 = ptAt (i + 2);
            const juce::Point<float> c1 (p1.x + (p2.x - p0.x) / 6.0f,
                                         clampY (p1.y + (p2.y - p0.y) / 6.0f));
            const juce::Point<float> c2 (p2.x - (p3.x - p1.x) / 6.0f,
                                         clampY (p2.y - (p3.y - p1.y) / 6.0f));
            p.cubicTo (c1, c2, p2);
        }
        // ONE PASS, 1.0 px, at a twentieth of the alpha it carried. A FAINT
        // THREAD LAID OVER THE TRAILS, not the main line: the trail is the
        // picture and this is the mark that says where the measurement is.
        g.setColour (c.withAlpha (alpha));
        g.strokePath (p, juce::PathStrokeType (1.0f, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
    };

    // THE EMPHASIS IS INVERTED: THE TRAIL IS THE PICTURE and the filament is a
    // quiet reference mark through it, almost dissolved into the cloud. It is
    // still the thing being compared and still the only thing any number is
    // read from; it simply stops shouting over the history behind it.
    // THE FILAMENT IS NO LONGER DRAWN. The ghost trail is the whole picture,
    // and a thread laid over it competed for the same attention without adding
    // a reading: every number is in the row along the bottom now.
    //
    // THE LAMBDA IS LEFT DEFINED AND UNCALLED, the same treatment as
    // matchInsideBandZone, matchAxisHasZone, matchBandBlock, kMatchPlotSpanDb,
    // matchAxisZoneDb and tickSeq. What it draws may come back, and deleting
    // it is a decision for whoever decides that, not a side effect of a look.
    juce::ignoreUnused (filament);
}

// ---------------------------------------------------------------------------
// WHAT DIFFERS PER AXIS IS ONLY THE RIBBONS' SHAPE
// ---------------------------------------------------------------------------

/** SPECTRUM: FROM THE 64 BIN CURVE, NOT THE SIX BANDS.

    §3 FORBIDS A SMOOTH LINE THROUGH SIX NUMBERS AND THIS DOES NOT BREAK IT.
    The rule exists because six band figures assert nothing between their
    centres, so a curve drawn through them invents shape the arithmetic never
    had. These bins are SIXTY FOUR MEASUREMENTS, a real continuous reading on
    both sides that the page already holds through getSlotSpectralEvidence, so
    a smooth curve is what they are. THE SIX BANDS REMAIN WHAT THE PROPOSAL
    MOVES and what the morph steps; they are simply no longer drawn as blocks.

    BOTH CURVES ARE LEVEL-NORMALISED, each by its own mean, and that is what
    makes the ribbon's thickness meaningful: the floor is a threshold on band
    RELATIVES, so a ribbon of +/- 2 dB around an absolute curve would claim a
    tolerance in the wrong units and would move with the gain difference the
    level axis handles on its own. */
static void matchSpectrumRows (const std::array<float, 64>& mixBins,
                               const std::array<float, 64>& refBins,
                               const std::array<float, 6>& moves, float morph,
                               MatchRibbonRow& mixV, MatchRibbonRow& refV)
{
    auto meanOf = [] (const std::array<float, 64>& b)
    {
        double s = 0.0; int n = 0;
        for (float v : b) if (v > -120.0f) { s += v; ++n; }
        return n > 0 ? (float) (s / n) : 0.0f;
    };
    const float mMean = meanOf (mixBins), rMean = meanOf (refBins);

    for (int i = 0; i < kMatchRibbonCols; ++i)
    {
        const float u = (float) i / (float) (kMatchRibbonCols - 1);
        const float fb = u * 63.0f;
        const int   b0 = juce::jlimit (0, 63, (int) fb);
        const int   b1 = juce::jlimit (0, 63, b0 + 1);
        const float fr = fb - (float) b0;

        const float m = juce::jmap (fr, mixBins[(size_t) b0], mixBins[(size_t) b1]) - mMean;
        const float r = juce::jmap (fr, refBins[(size_t) b0], refBins[(size_t) b1]) - rMean;

        // The morph walks the mix curve by ITS OWN BAND'S move, stepped at the
        // band edges exactly as the proposal steps: matchMorphedDb is the same
        // arithmetic the blocks used.
        const double lof = std::log2 (echojay::kMatchPlotLoHz), hif = std::log2 (echojay::kMatchPlotHiHz);
        const double hz  = std::pow (2.0, lof + (double) u * (hif - lof));
        mixV[(size_t) i] = echojay::matchMorphedDb (m, hz, moves, morph);
        refV[(size_t) i] = r;
    }

}

static void matchPaintLoudness (juce::Graphics& g, juce::Rectangle<int> plot,
                         juce::Rectangle<int> readout,
                         const echojay::MatchSide& mix, const echojay::MatchSide& ref,
                         float phase,
                         const juce::String& mixName, const juce::String& refName)
{
    // NEAR FLAT RIBBONS at each side's integrated level. A whisper of drift
    // along the length, so they are alive rather than ruled.
    MatchRibbonRow mixV {}, refV {};
    for (int i = 0; i < kMatchRibbonCols; ++i)
    {
        const float u = (float) i / (float) (kMatchRibbonCols - 1);
        const float w = std::sin (u * juce::MathConstants<float>::twoPi * 0.6f) * 0.18f;
        mixV[(size_t) i] = mix.integrated + w;
        refV[(size_t) i] = ref.integrated - w * 0.6f;
    }
    // THE SHARED SCALE, so the cloud sits AROUND the filament rather than
    // beside it: one range for both layers on every axis.
    const std::array<float, 64> noBins {};
    const auto range = matchAxisRange (echojay::MatchAxis::Loudness, mix, ref, noBins, noBins, false);
    matchPaintRibbonPair (g, plot, mixV, refV, range.first, range.second,
                          echojay::matchAxisZoneDb (echojay::MatchAxis::Loudness), phase);

    // THE NUMBERS GO IN THE READOUT ROW, in the order they appeared in the
    // sentence this replaces. Built HERE, from this axis's own fields, rather
    // than in a shared table: the values must come from the same reads the
    // shape came from, and mr PIN24 pins that this painter is the thing that
    // touches them.
    std::vector<echojay::MatchReadoutCell> cells;
    cells.push_back ({ "INT",
                       echojay::matchReadoutValue (echojay::matchHasIntegrated (mix), mix.integrated, 1, "LUFS"),
                       echojay::matchReadoutValue (echojay::matchHasIntegrated (ref), ref.integrated, 1, "LUFS") });
    cells.push_back ({ "TRUE PEAK",
                       echojay::matchReadoutValue (echojay::matchHasTruePeak (mix), mix.truePeak, 1, "dBTP"),
                       echojay::matchReadoutValue (echojay::matchHasTruePeak (ref), ref.truePeak, 1, "dBTP") });
    matchPaintReadout (g, readout, cells, {}, mixName, refName);
}

static void matchPaintDynamics (juce::Graphics& g, juce::Rectangle<int> plot,
                         juce::Rectangle<int> readout,
                         const echojay::MatchSide& mix, const echojay::MatchSide& ref,
                         float phase,
                         const juce::String& mixName, const juce::String& refName)
{
    // RIBBONS THAT RIPPLE, AMPLITUDE FROM CREST, so a more dynamic track
    // visibly ripples more. THE LOUDNESS RANGE VARIES THE RIPPLE ALONG THE
    // LENGTH: a wide range swells and settles, a narrow one is even.
    // THE FILAMENT IS THE SLOW FIGURE WHERE THERE IS ONE. A capture or a
    // reference holds its STORED whole-file crest, which is genuinely
    // accumulated. A LIVE side has no whole-file crest to hold: MeterEngine
    // computes crest as peak over RMS of a 0.5 s RMS and a 3 s peak decay, so
    // its filament IS a rolling figure and the line below says "rolling".
    // DYNAMICS IS THEREFORE THE AXIS WHERE THE CLOUD AND THE FILAMENT SIT
    // CLOSEST TOGETHER: a slower reading of the same thing rather than a
    // different kind of figure. Said, not hidden.
    auto row = [] (const echojay::MatchSide& s, MatchRibbonRow& out, float bias)
    {
        const float amp = juce::jlimit (0.5f, 9.0f, s.crest * 0.55f);
        const bool  even = s.lra <= 0.0f;
        for (int i = 0; i < kMatchRibbonCols; ++i)
        {
            const float u = (float) i / (float) (kMatchRibbonCols - 1);
            // A side with no range RIPPLES EVENLY, and says so in words below:
            // varying it with a zero would draw an absent figure as a flat one.
            const float env = even ? 1.0f
                                   : 0.55f + 0.45f * std::sin (u * juce::MathConstants<float>::twoPi
                                                               * juce::jlimit (0.6f, 3.0f, s.lra * 0.25f));
            out[(size_t) i] = bias + std::sin (u * juce::MathConstants<float>::twoPi * 2.2f
                                               + bias) * amp * env;
        }
    };
    MatchRibbonRow mixV {}, refV {};
    row (mix, mixV, -6.0f);
    row (ref, refV,  6.0f);
    const std::array<float, 64> noBins {};
    const auto range = matchAxisRange (echojay::MatchAxis::Dynamics, mix, ref, noBins, noBins, false);
    matchPaintRibbonPair (g, plot, mixV, refV, range.first, range.second, 0.0f, phase);

    // CELLS FOR THE NUMBERS, in the order the old sentence carried them, and
    // the SENTENCES on one muted line above the row (2d). The overs count
    // keeps its own rule: -1 is a dash, not a zero.
    std::vector<echojay::MatchReadoutCell> cells;
    cells.push_back ({ "CREST",
                       // THE CLAMP IS NOT A MEASUREMENT: matchHasCrest rejects 40.0 dB,
                       // so the row prints a dash where it used to print the
                       // ceiling as though it were a reading.
                       echojay::matchReadoutValue (echojay::matchHasCrest (mix), mix.crest, 1, "dB"),
                       echojay::matchReadoutValue (echojay::matchHasCrest (ref), ref.crest, 1, "dB") });
    cells.push_back ({ "RANGE",
                       echojay::matchReadoutValue (mix.lra > 0.0f, mix.lra, 1, "LU"),
                       echojay::matchReadoutValue (ref.lra > 0.0f, ref.lra, 1, "LU") });
    cells.push_back ({ "OVERS",
                       echojay::matchReadoutCount (mix.overs),
                       echojay::matchReadoutCount (ref.overs) });

    // BOTH SIDE NOTES ARE GONE FROM THE DRAWING, 22 Sep, AND THE REASON IS
    // THE READOUT TABLE RATHER THAN A DECISION THAT THEY DID NOT MATTER.
    //
    // They said two things: that a side's crest is a rolling figure rather
    // than a whole track, and that a side has no loudness range. THE TWO ROW
    // TABLE NOW NAMES THE SIDES IN ITS LEFT COLUMN, so "Live signal" sits
    // directly against a file name with the numbers between them. A reader
    // sees which side is live at the same glance as the figure, which is what
    // the first note was for; and the RANGE column prints a DASH for a side
    // with no loudness range, in that side's own colour, which is what the
    // second was for. The notes were saying in a sentence what the row now
    // says by its shape.
    //
    // DO NOT RESTORE THEM WITHOUT CHECKING THAT FIRST. If the left column
    // ever stops naming the sides, or the dash ever becomes a zero, both
    // facts go silent and these lines are what has to come back.
    //
    // echojay::matchDynamicsSideNote IS DELIBERATELY STILL THERE and still
    // pinned by mr PIN23 and mr PIN29. It is the single source of this
    // wording for whatever carries it after the merge, and nothing that is
    // drawn today calls it.
    matchPaintReadout (g, readout, cells, {}, mixName, refName);
}

static void matchPaintStereo (juce::Graphics& g, juce::Rectangle<int> plot,
                       juce::Rectangle<int> readout,
                       const echojay::MatchSide& mix, const echojay::MatchSide& ref,
                         float phase,
                         const juce::String& mixName, const juce::String& refName)
{
    // RIBBONS THAT BOW OUTWARD WITH WIDTH: a wide image is a fat lens and a
    // mono one is nearly straight. CORRELATION TIGHTENS OR LOOSENS THE BOW,
    // so two images that measure alike close into one shape.
    auto row = [] (const echojay::MatchSide& s, MatchRibbonRow& out, float sign)
    {
        const float wNorm = juce::jlimit (0.0f, 1.0f, s.width / 100.0f);
        const float tight = 1.0f - 0.45f * juce::jlimit (0.0f, 1.0f, s.correlation);
        const float bow   = wNorm * 16.0f * tight;
        for (int i = 0; i < kMatchRibbonCols; ++i)
        {
            const float u = (float) i / (float) (kMatchRibbonCols - 1);
            out[(size_t) i] = sign * bow * std::sin (u * juce::MathConstants<float>::pi);
        }
    };
    MatchRibbonRow mixV {}, refV {};
    row (mix, mixV, -1.0f);
    row (ref, refV,  1.0f);
    const std::array<float, 64> noBins {};
    const auto range = matchAxisRange (echojay::MatchAxis::Stereo, mix, ref, noBins, noBins, false);
    matchPaintRibbonPair (g, plot, mixV, refV, range.first, range.second, 0.0f, phase);

    // WIDTH TO ONE DECIMAL, NOT ZERO, and correlation to two: 2b's rule, and
    // it corrects the old row, which printed width with no decimals at all.
    std::vector<echojay::MatchReadoutCell> cells;
    // `true` WAS WRONG FOR AN EMPTY SIDE. Width and correlation have no
    // sentinel of their own, so this printed 0.0 % and 0.00 for a slot with
    // nothing in it, which reads as a measured mono, perfectly correlated
    // source. matchHasWidth/matchHasCorrelation ask whether the SIDE has any
    // real figure at all, which is the question that can actually be
    // answered, and the picture reads the same two predicates.
    cells.push_back ({ "WIDTH",
                       echojay::matchReadoutValue (echojay::matchHasWidth (mix), mix.width, 1, "%"),
                       echojay::matchReadoutValue (echojay::matchHasWidth (ref), ref.width, 1, "%") });
    cells.push_back ({ "CORR",
                       echojay::matchReadoutValue (echojay::matchHasCorrelation (mix), mix.correlation, 2, {}),
                       echojay::matchReadoutValue (echojay::matchHasCorrelation (ref), ref.correlation, 2, {}) });
    matchPaintReadout (g, readout, cells, {}, mixName, refName);
}

void matchPaintAxis (juce::Graphics& g, juce::Rectangle<int> plot,
                     juce::Rectangle<int> readout,
                     echojay::MatchAxis a,
                     const echojay::MatchSide& mix, const echojay::MatchSide& ref,
                     const std::array<float, 64>& mixBins, const std::array<float, 64>& refBins,
                     bool anyBins, const std::array<float, 6>& moves, float morph, float phase,
                     const juce::String& mixName, const juce::String& refName)
{
    if (plot.getWidth() <= 8 || plot.getHeight() <= 8) return;
    switch (a)
    {
        case echojay::MatchAxis::Spectrum:
        {
            // THE MESSAGE APPEARS ONLY WHEN NOTHING DRAWS (22 Sep).
            //
            // It used to be gated on haveBins, which was evMix.valid AND
            // evRef.valid, and it read "No spectrum to draw for one of the two
            // sides." So with a live mix and no reference it printed itself
            // ACROSS A PICTURE THAT WAS ALREADY DRAWN: the trail and the
            // filament are stroked in MatchPanel::paint before this runs, and
            // they only ever needed one side.
            //
            // ONE SIDE IS A PICTURE. The readout row's dashes already say
            // which side is missing, in that side's own colour, so a sentence
            // over the top is a second statement of the same fact obscuring
            // the first. NEITHER side is the only case with nothing to look
            // at, and then the sentence is the whole of the screen and stays.
            if (! anyBins)
            {
                g.setColour (MatchC::text3);
                g.setFont (juce::Font (juce::FontOptions (11.0f)));
                g.drawFittedText ("Neither side has a spectrum to draw yet.",
                                  plot, juce::Justification::centred, 2);
                return;
            }
            MatchRibbonRow mixV {}, refV {};
            matchSpectrumRows (mixBins, refBins, moves, morph, mixV, refV);
            const auto range = matchAxisRange (echojay::MatchAxis::Spectrum, mix, ref,
                                               mixBins, refBins, true);
            matchPaintRibbonPair (g, plot, mixV, refV, range.first, range.second,
                                  echojay::matchAxisZoneDb (echojay::MatchAxis::Spectrum), phase);

            // THE SPECTRUM'S OWN NUMBERS: the six band relatives are the thing
            // the proposal moves, so the row carries the two that a reader
            // most needs, the overall tilt and the largest single gap, taken
            // from the SAME relatives the ribbons were built from.
            // ALL SIX BANDS, ONE PER COLUMN, because the figures exist on both
            // sides and the table now has room for them. The old row could
            // only afford two cells, so it carried the worst gap and the tilt:
            // a summary of the bands, chosen for it. Six columns say the same
            // thing without choosing, and the reader picks the worst gap out
            // by eye because the two rows line up.
            //
            // THEY DO NOT NEED A PROPOSAL AND THEY DO NOT NEED THIRTY SECONDS.
            // matchSideFrom copies macro and hasMacro straight off the
            // evidence with no duration gate, and a Live side gets them from
            // getBoundedBands as soon as ONE audible block has arrived. The
            // 30 s rule is kMatchBandMinSeconds and it gates the band PROPOSAL
            // only, which is a different question from whether the figures are
            // measured.
            //
            // EACH SIDE IS TESTED ON ITS OWN. A reference this build never
            // analysed sits under a live mix that has bands, and the table
            // prints that side's dashes rather than refusing both: six dashes
            // in the reference's colour say which side is missing, where one
            // "BANDS -/-" cell said only that something was.
            {
                std::array<float, 6> mRel {}, rRel {};
                const bool mOk = mix.hasMacro && echojay::matchBandRelatives (mix.macro, mRel);
                const bool rOk = ref.hasMacro && echojay::matchBandRelatives (ref.macro, rRel);

                std::vector<echojay::MatchReadoutCell> cells;
                for (int b = 0; b < 6; ++b)
                    cells.push_back ({ juce::String (echojay::macroBandName (b)),
                                       echojay::matchReadoutValue (mOk, mRel[(size_t) b], 1, {}),
                                       echojay::matchReadoutValue (rOk, rRel[(size_t) b], 1, {}) });

                // NO NOTE LINE. It read "band level relative to each side's
                // own mean, dB" and came out 22 Sep.
                //
                // THE ROW GETS THE TEN PIXELS BACK. matchPaintReadout takes 10
                // px off the top for a note and then compresses the header and
                // the two side lines into what is left; with no note the table
                // draws at its preferred 10 + 12 + 12.
                //
                // WHAT WENT WITH IT, recorded rather than discovered later: the
                // six band columns now carry NO UNIT anywhere on the page. The
                // header names the band and the cells hold a number. They are
                // dB relative to that side's own six-band mean, which is a
                // thing a reader now has to know rather than read.
                matchPaintReadout (g, readout, cells, {}, mixName, refName);
            }
            break;
        }
        case echojay::MatchAxis::Loudness: matchPaintLoudness (g, plot, readout, mix, ref, phase, mixName, refName); break;
        case echojay::MatchAxis::Dynamics: matchPaintDynamics (g, plot, readout, mix, ref, phase, mixName, refName); break;
        case echojay::MatchAxis::Stereo:   matchPaintStereo   (g, plot, readout, mix, ref, phase, mixName, refName); break;
    }
}

/** ONE TICK'S FIGURES INTO A SIDE'S RING. What each axis traces:

      LOUDNESS   momentary (400 ms), short term (3 s), and peak. Three
                 different time constants, so three different speeds, which is
                 what makes the lines cross instead of stacking.
      DYNAMICS   crest, plus the three BAND crests.
      STEREO     correlation, the three BANDED correlations, and width.

    THE BAND CRESTS AND BANDED CORRELATIONS ARE CAPTURE-ONLY AS COMPARISONS and
    are read here anyway, deliberately. The rule this page holds is that THE
    CLOUD IS CONTEXT AND IS NEVER COMPARED: they have no reference equivalent to
    be compared against, which is why mr PIN24 keeps them off the tiles, but as
    a live trace of the side producing them they are a real measurement of that
    side. NOT IN THE THREE PAINTERS, for exactly that reason.

    A figure that is unavailable sets have[] false for its line rather than
    writing a sentinel into the ring: -1 band crest is "not measured", and a
    line drawn at -1 would be a measurement of something. */
void EchoJayEditor::MatchPanel::pushTrace (Trace& t, echojay::MatchAxis a, const MeterData& md)
{
    std::array<float, (size_t) kTraceLines> v {};
    std::array<bool,  (size_t) kTraceLines> ok {};

    switch (a)
    {
        case echojay::MatchAxis::Loudness:
            v[0] = md.momentary;  ok[0] = md.momentary > -99.0f;
            v[1] = md.shortTerm;  ok[1] = md.shortTerm > -99.0f;
            v[2] = juce::jmax (md.peakL, md.peakR);
            ok[2] = v[2] > -99.0f;
            break;

        case echojay::MatchAxis::Dynamics:
            v[0] = md.crestFactor;  ok[0] = md.crestFactor > 0.0f;
            v[1] = md.bandCrestSub; ok[1] = md.bandCrestSub >= 0.0f;
            v[2] = md.bandCrestMid; ok[2] = md.bandCrestMid >= 0.0f;
            v[3] = md.bandCrestTop; ok[3] = md.bandCrestTop >= 0.0f;
            break;

        case echojay::MatchAxis::Stereo:
            v[0] = md.correlation; ok[0] = true;
            v[1] = md.corrSub;     ok[1] = true;
            v[2] = md.corrMid;     ok[2] = true;
            v[3] = md.corrTop;     ok[3] = true;
            v[4] = md.width / 100.0f * 2.0f - 1.0f;   // onto the correlation scale
            ok[4] = true;
            break;

        case echojay::MatchAxis::Spectrum:
            return;                                   // the spectrum has its own shape
    }

    // THE TERRACING, AND IT IS FIXED HERE RATHER THAN WHERE THE TRACE IS DRAWN.
    //
    // THE CAUSE, from the engine and not from the look: several of these
    // figures only CHANGE every 100 ms, because they are derived from
    // 100 ms LUFS blocks. MeterEngine accumulates samplesPerBlock100ms, pushes
    // one LufsBlock, and shortTerm is then the mean over the last 30 of them
    // (MeterEngine.cpp:705-731). integrated and loudnessRange move on the same
    // boundary. THIS RING SAMPLES AT THE PAGE'S 30 Hz, so a figure that
    // changes at 10 Hz writes THE SAME VALUE THREE TIMES and draws a stair:
    // three samples flat, then a vertical, then three flat again.
    //
    // A ONE-POLE AS IT IS WRITTEN turns that step into a ramp. RECOUNTED FOR
    // THE HOP RATE: this ring is now written once per FFT hop, so the 100 ms
    // stair repeats over 4.3 hops rather than 3 ticks. k = 0.35 is 35 percent
    // closed after one hop, 73 after three and 82 after four, so the stair
    // still becomes a continuous rise over exactly the interval it repeats on,
    // and anything genuinely fast still lands within a hop or two. It smooths
    // ALONG THE LINE'S OWN HISTORY, which is a different thing from averaging
    // the spectrum across frames: no strand is destroyed because each line is
    // one figure, not a shape.
    constexpr float kTraceSmooth = 0.35f;
    const int prev = (t.write - 1 + kTraceLen) % kTraceLen;

    for (int i = 0; i < kTraceLines; ++i)
    {
        const float target = v[(size_t) i];
        const float last   = t.filled > 0 ? t.v[(size_t) i][(size_t) prev] : target;
        t.v[(size_t) i][(size_t) t.write] = last + (target - last) * kTraceSmooth;
        if (ok[(size_t) i]) t.have[(size_t) i] = true;
    }
    t.write  = (t.write + 1) % kTraceLen;
    t.filled = juce::jmin (kTraceLen, t.filled + 1);
}

/** The scale a traced axis is drawn on. Fixed per axis rather than fitted to
    the data: a scale that rescaled itself would turn every change in the
    figures into a change in the picture's own geometry, and the eye would
    read the axis moving as the signal moving. */
static std::pair<float, float> matchTraceRange (echojay::MatchAxis a)
{
    switch (a)
    {
        // LOUDNESS: -33 TO +3, NOT -60 TO 0, AND THE THREE IS NOT A TYPO.
        //
        // WHAT WAS WRONG, as arithmetic rather than as taste. This axis draws
        // momentary LUFS, short term LUFS and sample peak in dBFS. A master
        // sits between about -24 and -6 LUFS and its peak sits within a
        // decibel of 0. On a -60 to 0 range, position from the top is
        // (0 - v) / 60, so -6 landed 10% down, -24 landed 40% down and the
        // PEAK LINE LANDED AT 0.5%, pinned to the ceiling. Typical material
        // therefore lived in the top 40% of the plot with the bottom 36 dB
        // holding nothing but the occasional fade, which is what "it is all in
        // the top quarter" is describing.
        //
        // WHERE -33 AND +3 COME FROM. Put -6 a quarter of the way down and -24
        // three quarters of the way down, so typical material fills the MIDDLE
        // HALF:
        //     (hi + 6) / (hi - lo) = 0.25     (hi + 24) / (hi - lo) = 0.75
        // Subtracting gives 18 / (hi - lo) = 0.5, so the span is 36 dB, and
        // then hi = +3 and lo = -33.
        //
        // THE POSITIVE TOP IS DELIBERATE. Peak is in dBFS and a master's sits
        // just under 0, so a ceiling AT 0 puts that line on the frame where it
        // cannot be read and cannot be seen to move. Three decibels of sky
        // gives it somewhere to be, and leaves an over-range peak somewhere to
        // go before it clamps.
        case echojay::MatchAxis::Loudness: return { -33.0f,  3.0f };   // dB
        case echojay::MatchAxis::Dynamics: return {   0.0f, 30.0f };   // dB crest
        case echojay::MatchAxis::Stereo:   return {  -1.0f,  1.0f };   // correlation
        case echojay::MatchAxis::Spectrum: break;
    }
    return { -1.0f, 1.0f };
}

// ---------------------------------------------------------------------------
// THE GUIDES
// ---------------------------------------------------------------------------
//
// SPECTRUM GETS THREE VERTICALS AND NOTHING ELSE. No horizontals and no box:
// a dB gridline would have to claim a scale, and the trail's scale is each
// frame's OWN MEAN plus or minus kMatchTrailSpanDb, which is not a scale
// anybody reads a number off. The three decade marks are placed through
// echojay::matchFreqToX, the same mapping the curve is drawn with, so they
// cannot drift from it.
//
// THE OTHER THREE AXES GET AT MOST ONE LINE, AND ONLY WHERE A VALUE MEANS
// SOMETHING. Time along the bottom tells you nothing, so there are no
// verticals anywhere but the spectrum.
//
//   STEREO     ONE line at correlation zero, the boundary between a coherent
//              image and a phasey one, placed through matchTraceRange so it
//              lands exactly where the traces read zero.
//   LOUDNESS   NONE. Its range is -60 to 0 dB and neither end is a value you
//              aim at; 0 dBFS is the top edge of the plot, not a target.
//   DYNAMICS   NONE. Its range is 0 to 30 dB of crest and no crest figure is
//              a reference point.
//
// WIDTH'S HUNDRED PERCENT IS DELIBERATELY NOT DRAWN. It is a real reference
// value, but the stereo plot's vertical axis is CORRELATION, -1 to +1, and
// width is a percentage that does not live on it. A line at "100%" would have
// to invent a position on a scale that is not measuring it, which is the
// horizontal version of the mistake the readout row exists to prevent.
//
// EVERY COLOUR HERE IS TRANSLUCENT AND THE WHOLE FUNCTION IS SCOPED. The blit
// is the next thing that happens after this returns, and drawImageAt takes its
// opacity from the fill: that is exactly what composited the trail at five
// percent, and open list 210 is the record of it.
void matchPaintGuides (juce::Graphics& g, juce::Rectangle<int> plot, echojay::MatchAxis a)
{
    if (plot.getWidth() <= 8 || plot.getHeight() <= 8) return;

    juce::Graphics::ScopedSaveState ss (g);
    g.setFont (juce::Font (juce::FontOptions (8.0f)));

    if (a == echojay::MatchAxis::Spectrum)
    {
        for (int i = 0; i < (int) echojay::kMatchGuideHz.size(); ++i)
        {
            const float x = echojay::matchFreqToX (plot, echojay::kMatchGuideHz[(size_t) i]);
            // LOW ENOUGH THAT YOU FIND IT ONLY BY LOOKING. The trail's faintest
            // ghosts are the picture; a guide that competes with them is
            // furniture, which this page does not have.
            g.setColour (MatchC::text3.withAlpha (0.20f));
            g.fillRect (x, (float) plot.getY(), 1.0f, (float) plot.getHeight());
            g.setColour (MatchC::text3.withAlpha (0.40f));
            g.drawText (echojay::matchGuideLabel (i),
                        juce::Rectangle<float> (x + 3.0f, (float) plot.getY() + 2.0f, 24.0f, 10.0f),
                        juce::Justification::centredLeft, false);
        }
        return;
    }

    if (a == echojay::MatchAxis::Stereo)
    {
        const auto  tr = matchTraceRange (a);
        const float y  = (float) plot.getY()
                       + (tr.second - 0.0f) / juce::jmax (0.0001f, tr.second - tr.first)
                             * (float) plot.getHeight();
        g.setColour (MatchC::text3.withAlpha (0.20f));
        g.fillRect ((float) plot.getX(), y, (float) plot.getWidth(), 1.0f);
        g.setColour (MatchC::text3.withAlpha (0.40f));
        g.drawText ("0",
                    juce::Rectangle<float> ((float) plot.getX() + 3.0f, y - 11.0f, 20.0f, 10.0f),
                    juce::Justification::centredLeft, false);
    }
}

// ---------------------------------------------------------------------------
// THE FILAMENT
// ---------------------------------------------------------------------------
//
// IT IS BACK, AND IT IS BUILT FROM THE TRAIL'S OWN COLUMN READING. The reason
// it went was that a second line competed with the trail without adding a
// reading. What brings it back is the glow: three passes of ONE path, wide and
// faint under narrow and bright, so it reads as a lit thread through the cloud
// rather than a drawn line over it.
//
// IT SITS EXACTLY ON THE NEWEST GHOST, not near it, because it calls
// matchVisColumnYOffset with the same bins, the same bin width, the same mean
// and the same span the trail's stroke just used. Two copies of that
// arithmetic is how a picture starts lying about which stroke is the
// measurement.
//
// SETCOLOUR FIRST, SETOPACITY SECOND, three times. setColour replaces the
// whole FillType including the alpha, so the other order discards the pass's
// alpha and all three passes land at full strength, which is one fat opaque
// line and no glow at all. That is the defect that cost four rounds.
// THE ONE GLOW. See the declaration above for why alphaScale exists.
//
// THE THREE AXES THAT ARE NOT SPECTRUM CARRY UP TO FIVE LINES PER SIDE, where
// spectrum carries one. Thirty stroked paths a frame on stereo image against
// spectrum's six is the same glow doing five times the work, and it may read
// as busy rather than as lit. WRITTEN HERE RATHER THAN DISCOVERED: if Kathy
// says it is noisy, lower kMatchTraceGlow and leave kMatchSpectrumGlow alone.
// The two are separate so that calming one cannot disturb the other.
void matchStrokeGlow (juce::Graphics& g, const juce::Path& p, juce::Colour c,
                      float alphaScale)
{
    // THE SAME LIFT TOWARD WHITE THE TRAIL USES, from the same constant, so a
    // thread is the same hue as the ghosts under it.
    const juce::Colour lit = c.interpolatedWith (juce::Colours::white, echojay::kMatchTrailWhite);

    struct GlowPass { float width, alpha; };
    static constexpr GlowPass kPasses[3] { { 6.0f, 0.10f }, { 3.0f, 0.20f }, { 1.2f, 0.95f } };

    juce::Graphics::ScopedSaveState ss (g);
    for (const auto& pass : kPasses)
    {
        g.setColour (lit);
        g.setOpacity (juce::jlimit (0.0f, 1.0f, pass.alpha * alphaScale));
        g.strokePath (p, juce::PathStrokeType (pass.width, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));
    }
}

void matchPaintVisFilament (juce::Graphics& g, juce::Rectangle<int> plot,
                            const std::vector<float>& row, juce::Colour c)
{
    if (plot.getWidth() <= 8 || plot.getHeight() <= 8) return;

    const int cols = juce::jmax (2, plot.getWidth());
    if ((int) row.size() < cols) return;

    juce::Path p;
    for (int x = 0; x < cols; ++x)
    {
        const float y = (float) plot.getY()
                      + echojay::matchVisYOffset (row[(size_t) x],
                                                  echojay::kMatchTrailSpanDb,
                                                  plot.getHeight());
        if (x == 0) p.startNewSubPath ((float) plot.getX(), y);
        else        p.lineTo ((float) plot.getX() + (float) x, y);
    }

    matchStrokeGlow (g, p, c, kMatchSpectrumGlow);
}

/** THE TRAIL IMAGE: many past frames still on screen and fading, so the shape
    AND HOW MUCH IT MOVES are visible at once.

    ONE STROKE PER SIDE PER FRAME, NEVER ONE PER GHOST. Two images ping-ponged,
    following the spectroImg precedent rather than inventing a second way to
    keep a picture between frames: the old one is blitted into the new at
    reduced opacity, then the CURRENT curve is stroked in once. The fade is
    that opacity and nothing else.

    RESIZE AND FIRST FRAME ARE EXPLICIT: a plot of a different size, or a
    different axis, starts a new pair rather than stretching a history that was
    drawn to another shape. */
void EchoJayEditor::MatchPanel::advanceTrail (juce::Rectangle<int> plot,
                                              const MatchRibbonRow* mixFast,
                                              const MatchRibbonRow* refFast,
                                              float lo, float hi)
{
    // THE TWO CONSTANTS, AND THEY ARE NOT INDEPENDENT. Read this before
    // changing either.
    //
    // THE ADVANCE RATE IS THE FFT HOP RATE, not the timer's. MeterEngine
    // publishes one visual spectrum per kVisHopSamples (1024), so 43.07 Hz at
    // 44.1 kHz and 46.88 Hz at 48 kHz, and this advances exactly once per
    // published hop. The timer runs at 60 Hz so that no hop is ever missed;
    // frames with no new hop blit the image again and change nothing.
    //
    // THE FADE IS THREE SECONDS TO FIVE PERCENT AT WHATEVER THE HOP RATE IS,
    // computed below rather than written down, so a 48 kHz session and a
    // 44.1 kHz session show the same three seconds of history.
    //
    // THE INK, which is why a slower fade alone would have smeared. Each
    // frame's curve used to be stroked at FULL strength onto a barely faded
    // image, so with a slow fade everything the curve had ever touched went
    // white and the ghosts merged into a cloud. Stroked faint instead, so a
    // single frame is just visible and BRIGHTNESS ACCUMULATES ONLY WHERE MANY
    // FRAMES AGREE, which is what makes the lines distinct rather than a haze.
    //
    // THE SATURATION ARITHMETIC, which ties the two together: a pixel the
    // curve sits on EVERY frame settles at
    //     alpha / (1 - fade)
    // because it gains alpha per tick and loses (1 - fade) of what it holds.
    // AND THE MODEL EVERY EARLIER VERSION OF THIS COMMENT USED WAS WRONG.
    // alpha / (1 - fade) is the ADDITIVE steady state. JUCE composites
    // SOURCE-OVER, so a stroke does not add its alpha, it covers what is
    // there:
    //     a_next = ink + (a_prev * fade) * (1 - ink)
    // whose fixed point is
    //     a = ink / (1 - fade * (1 - ink))
    // At the old 0.090 that is 0.75 for the mix and 0.70 for the reference,
    // NOT the 2.73 the additive model predicted, and NOTHING EVER CLIPPED:
    // source-over approaches 1 asymptotically and only reaches it at ink 1.
    // That is why raising the ink did brighten the trail when the arithmetic
    // said it could not.
    //
    // THE NUMBER THAT MATTERS IS THE SINGLE STROKE, NOT THE STEADY STATE. A
    // ghost is made of pixels the curve visited ONCE and then left; the steady
    // state only describes the few it never leaves. So the ink is set by what
    // one visit is worth:
    //     one stroke, mix, weight 1.00:  0.380 = 97 of 255
    //     one stroke, ref, weight 0.80:  0.304 = 78 of 255
    // THE DWELLING CORE WILL SATURATE, and that is intended: the envelope a
    // signal holds goes white while everything it merely passed through stays
    // a clear, separately visible ghost.
    // RAISE THE FADE WITHOUT LOWERING THE INK AND EVERYTHING SATURATES TO
    // WHITE; lower the fade without raising the ink and the trail vanishes.
    // THE FADE IS DERIVED FROM THE HOP RATE, not from a fixed number, because
    // the trail now advances once per published FFT and that rate follows the
    // sample rate:
    //     hopRate = sampleRate / kVisHopSamples      43.07 Hz at 44.1k
    //                                                46.88 Hz at 48k
    //     fade    = exp (-ln (20) / (3 * hopRate))   three seconds to 5%
    //             = 0.9772 at 44.1 kHz, 0.9790 at 48 kHz
    const double sr      = host != nullptr && host->streamSampleRate() > 0.0
                             ? host->streamSampleRate() : 44100.0;
    const double hopRate = sr / (double) MeterEngine::kVisHopSamples;
    const float  kTrailFade = (float) std::exp (-std::log (20.0) / (3.0 * hopRate));
    // BOTH 1.0 BECAUSE THE APPROVED PICTURE IS THE OPAQUE ONE. Until now the
    // ordering below discarded these values and every stroke landed at alpha
    // 1.0 regardless; that is what is on screen and what was approved. The
    // ordering is fixed, so THE KNOBS NOW WORK: lower kTrailRefW to pull the
    // reference back behind the mix, lower kTrailInk for a fainter cloud.
    constexpr float kTrailInk  = 1.0f;
    // THE TRACE AXES GET THEIR OWN INK, AND SPECTRUM'S 1.0 DOES NOT MOVE.
    //
    // A SLOW LINE SATURATES ITS OWN TRAIL AND LEAVES THE FILAMENT NOWHERE TO
    // SHOW. At ink 1.0 the source-over fixed point for a pixel the line sits
    // on every hop is ink / (1 - fade*(1-ink)) = 1.0 / 1.0 = 1.0, reached on
    // the FIRST stroke: the fade does nothing where the line does not move.
    // Spectrum gets away with it because its curve moves several pixels a hop,
    // so the opaque pixels are spread over a wide fan with decaying ghosts
    // between them. Loudness, dynamics and stereo image are one-poled figures
    // that move well under the 1.8 px stroke width per hop (measured: 1.0 px
    // for 0.1 dB of loudness, 1.2 px for 0.1 dB of crest), so every stroke
    // lands on the last one and the band goes solid immediately. A filament
    // drawn in the SAME colour over a band already at alpha 1.0 of that colour
    // is not dim, it is invisible: it changes no pixel.
    //
    // 0.25 gives a stationary fixed point of 0.936 instead of 1.000, so the
    // ghosts build over about a second rather than instantly and the filament
    // has something to sit on top of.
    //
    // KATHY APPROVED THE SPECTRUM PICTURE AND IT MUST NOT MOVE, which is the
    // whole reason this is a second constant rather than a lower kTrailInk.
    constexpr float kTraceTrailInk = 0.25f;

    // THE TRACE TRAIL KEEPS ITS OWN HUE; ONLY THE FILAMENT IS LIFTED.
    //
    // THIS, NOT THE INK, IS WHAT MAKES THE FILAMENT VISIBLE HERE. Read that
    // before touching kTraceTrailInk above: it was lowered from 1.0 to 0.25 on
    // the hypothesis that a fainter stroke would let the line show, and the
    // arithmetic says it cannot. A line that sits in the same pixels for 129
    // hops (three seconds at 43.07 Hz) saturates its trail at ANY ink: the
    // fixed point ink / (1 - fade*(1-ink)) is 1.000 at ink 1.0 and still 0.936
    // at 0.25. Lowering it again chases the same symptom and will not work.
    //
    // THE ACTUAL DEFECT WAS THAT BOTH LAYERS WERE THE SAME COLOUR. The trail
    // and the filament both stroked c.interpolatedWith(white, kMatchTrailWhite),
    // so a bright line drawn over a saturated band of ITSELF changed no pixel.
    // Now the trace trail strokes the axis colour NEAT and the filament keeps
    // its lift, so the picture is a light line over a coloured band:
    //
    //     trail    mix #06B6D4 (luma 147)   ref #A0A0B8 (luma 162)
    //     filament mix #5DD0E3 (luma 185)   ref #C1C1D1 (luma 194)
    //     the filament is 26% brighter on the mix, 20% on the reference
    //
    // SPECTRUM IS NOT TOUCHED and must not be: kMatchTrailWhite still lifts
    // both of its layers, because its curve moves enough that its ghosts are a
    // fan of partial alphas rather than a solid band, and Kathy approved that
    // picture. That is the whole reason this is a separate constant.
    constexpr float kTraceTrailWhite = 0.0f;
    constexpr float kTrailRefW = 1.0f;
    // HOW FAR TOWARD WHITE, AND WHY IT CAME BACK DOWN. At 0.78 the two clouds
    // were nearly the same near-white, which was fine while the filament
    // carried the colour key. THE FILAMENT IS GONE, so the clouds are the ONLY
    // thing saying which fan is which, and ON EVERY AXIS THE TWO SIDES OCCUPY
    // THE SAME REGION OF THE PLOT: strokeVis maps both through one tLo/tHi
    // over the whole rect, traceOf maps both rings through one
    // matchTraceRange over the whole rect. Nothing separates them by position.
    // At 0.35 each keeps its own hue while still reading as light:
    //     mix  #5dd0e3   (C::blue  toward white)
    //     ref  #c1c1d1   (C::text2 toward white)
    // SHARED WITH THE FILAMENT, which lifts its colours the same way so the
    // bright line is the same hue as the ghosts it sits on.
    constexpr float kTrailWhite = echojay::kMatchTrailWhite;

    if (plot.getWidth() <= 8 || plot.getHeight() <= 8) return;
    if (trailPlot != plot || trailAxis != axis || trailA.isNull() || trailB.isNull())
    {
        trailA = juce::Image (juce::Image::ARGB, plot.getWidth(), plot.getHeight(), true);
        trailB = juce::Image (juce::Image::ARGB, plot.getWidth(), plot.getHeight(), true);
        trailPlot = plot;
        trailAxis = axis;
        trailUseA = true;
        trailTick = -1;

        // THE ONE-POLE STATE DIES WITH THE IMAGES, on the same trigger and in
        // the same place, because it describes the same columns: a row smoothed
        // for 667 columns says nothing about 800, and an axis change means the
        // numbers are no longer even the same quantity. Reset means UNSEEDED,
        // not zeroed: the next hop seeds each side with that hop's own value.
        visRowCols = plot.getWidth();
        visRowMix.assign ((size_t) visRowCols, 0.0f);
        visRowRef.assign ((size_t) visRowCols, 0.0f);
        visRowHop.assign ((size_t) visRowCols, 0.0f);
        visRowScratch.assign ((size_t) visRowCols, 0.0f);
        visRowMixInit = visRowRefInit = false;
    }
    // The caller has already established that this is a NEW HOP; this guard
    // keeps advanceTrail honest if it is ever called twice for one.
    if (trailTick == (int) trailHop) return;
    trailTick = (int) trailHop;

    auto& src = trailUseA ? trailA : trailB;
    auto& dst = trailUseA ? trailB : trailA;
    trailUseA = ! trailUseA;

    dst.clear (dst.getBounds());
    juce::Graphics ig (dst);
    ig.setOpacity (kTrailFade);
    ig.drawImageAt (src, 0, 0);


    // THE TRAIL GETS A WIDER SPAN THAN THE FILAMENT, and this is the third
    // reason it read as a smear. The filament's range is fitted to the two
    // stored curves, which is right for a comparison and squeezes the live
    // variation into a band a few pixels tall. THE TRAIL IS NOT A COMPARISON,
    // so it gets a fixed +/- 36 dB around each frame's own mean: the same
    // fan the reference image has at the low end, with somewhere for the
    // variation to go. THE FILAMENT'S OWN SCALE IS UNTOUCHED: this lo/hi is
    // local to the image and nothing read off the picture uses it.
    constexpr float kTrailSpanDb = echojay::kMatchTrailSpanDb;

    // THE BIN WIDTH IS THE ENGINE'S OWN, NOT A HARDCODED NYQUIST. This read
    // 24000.0 / kVisBins, which is 11.71875 Hz. The real width is
    // sampleRate / kVisFftSize, 10.76660 Hz at 44.1 kHz, so every bin index
    // was 8.8% too low and the whole picture sat 0.12 octaves below the axis
    // it was drawn against. getVisualSpectrum already returns this number in
    // its binHzOut parameter and both call sites were discarding it.
    const double visBinHz = sr / (double) MeterEngine::kVisFftSize;

    // AND FROM THE FINEST SPECTRUM A LIVE FRAME CARRIES. getVisualSpectrum
    // hands back kVisBins = 2048 magnitudes against MeterData::spectrum's 64:
    // thirty-two times the detail, so every frame wiggles in thirty-two times
    // as many places, which is thirty-two times as many strands.
    // ---- THE TIME CONSTANT, DERIVED RATHER THAN WRITTEN DOWN --------------
    //
    // A ONE-POLE PER COLUMN, advanced once per published hop, which is what
    // Dynamics already does to its five figures and the reason its trail
    // stacks into a sheet instead of a haze.
    //
    //     a = 1 - exp (-1 / (tau * hopRate))
    //
    // At tau = 0.30 s and 43.07 Hz that is 1 - exp (-1 / 12.92) = 0.0745, and
    // at 48 kHz's 46.88 Hz it is 0.0686. THE COEFFICIENT IS NOT HARDCODED for
    // the same reason the fade is not: a session at another sample rate must
    // see the same THIRD OF A SECOND, not the same number of frames.
    constexpr float kTrailTauSeconds = 0.30f;
    const float kTrailPole = (float) (1.0 - std::exp (-1.0 / (kTrailTauSeconds * hopRate)));

    /** One side's row: build this hop's smoothed reading, advance the one-pole
        into the stored row, and stroke THE STORED ROW. */
    auto strokeVis = [&] (const std::array<float, MeterEngine::kVisBins>& bins,
                          std::vector<float>& row, bool& seeded,
                          juce::Colour c, float weight)
    {
        const int cols = juce::jmax (2, plot.getWidth());
        if ((int) row.size() < cols || (int) visRowHop.size() < cols
            || (int) visRowScratch.size() < cols) return;

        // A SPAN PER COLUMN, NOT A POINT, then a sixth of an octave across
        // frequency. See matchVisRelRow: the point sample repeated one bin
        // across 39 columns at the bottom and threw away eighteen bins in
        // nineteen at the top.
        //
        // INTO visRowHop, NOT INTO row. The persistent row is what the one-pole
        // is about to read; handing it in as working space would overwrite the
        // previous values first, which silently turns the filter into a
        // pass-through.
        echojay::matchVisRelRow (bins.data(), MeterEngine::kVisBins, visBinHz,
                                 cols, visRowHop.data(), visRowScratch.data());

        if (! seeded)
        {
            // SEEDED WITH THIS HOP'S OWN VALUE, never with zero or with
            // silence. Seeding at the floor makes the whole picture sweep up
            // from the bottom of the plot every time the page is opened, which
            // looks like a measurement settling and is nothing of the kind.
            for (int x = 0; x < cols; ++x) row[(size_t) x] = visRowHop[(size_t) x];
            seeded = true;
        }
        else
        {
            for (int x = 0; x < cols; ++x)
                row[(size_t) x] += kTrailPole * (visRowHop[(size_t) x] - row[(size_t) x]);
        }

        juce::Path p;
        for (int x = 0; x < cols; ++x)
        {
            const float y = echojay::matchVisYOffset (row[(size_t) x], kTrailSpanDb,
                                                      plot.getHeight());
            if (x == 0) p.startNewSubPath (0.0f, y);
            else        p.lineTo ((float) x, y);
        }
        // SETCOLOUR FIRST, SETOPACITY SECOND. The other order discarded the
        // ink: setColour replaces the whole fill, including the alpha the
        // line before had just set, so every stroke landed at 1.0.
        ig.setColour (c.interpolatedWith (juce::Colours::white, kTrailWhite));
        ig.setOpacity (kTrailInk * weight);
        // 1.8 px, NOT 1.0. At 1.0 or less a sloped path spreads one stroke
        // across two pixel rows at about half coverage each, which is half the
        // brightness the ink above is asking for.
        ig.strokePath (p, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));
    };

    auto stroke = [&] (const MatchRibbonRow& row, juce::Colour c, float weight)
    {
        juce::Path p;
        const float dx = (float) plot.getWidth() / (float) (kMatchRibbonCols - 1);
        for (int i = 0; i < kMatchRibbonCols; ++i)
        {
            const float y = juce::jlimit (0.0f, 1.0f, (hi - row[(size_t) i])
                                                          / juce::jmax (0.0001f, hi - lo))
                          * (float) plot.getHeight();
            if (i == 0) p.startNewSubPath (0.0f, y);
            else        p.lineTo (dx * (float) i, y);
        }
        // FAINT AND THIN: 1 px, at the ink, so one frame is a trace and only
        // agreement between frames builds a line.
        // SETCOLOUR FIRST, SETOPACITY SECOND. The other order discarded the
        // ink: setColour replaces the whole fill, including the alpha the
        // line before had just set, so every stroke landed at 1.0.
        ig.setColour (c.interpolatedWith (juce::Colours::white, kTrailWhite));
        ig.setOpacity (kTrailInk * weight);
        // 1.8 px, NOT 1.0. At 1.0 or less a sloped path spreads one stroke
        // across two pixel rows at about half coverage each, which is half the
        // brightness the ink above is asking for.
        ig.strokePath (p, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));
    };

    // ---- THE TRACED AXES: THE FIGURE OVER TIME --------------------------
    //
    // The last six seconds of several figures, scrolling right to left, one
    // line each. They run at different time constants, so they cross and
    // separate where a single invented shape stacked on itself.
    if (axis != echojay::MatchAxis::Spectrum)
    {
        const auto tr = matchTraceRange (axis);
        auto traceOf = [&] (const Trace& t, juce::Colour c, float weight)
        {
            if (t.filled < 2) return;
            const int n = t.filled;
            for (int line = 0; line < kTraceLines; ++line)
            {
                if (! t.have[(size_t) line]) continue;
                juce::Path p;
                for (int k = 0; k < n; ++k)
                {
                    // Oldest at the left, newest at the right.
                    const int idx = (t.write - n + k + kTraceLen * 2) % kTraceLen;
                    const float x = (float) plot.getWidth() * (float) k / (float) (n - 1);
                    // CLAMPED, AND THE CLAMP IS MEANT TO SHOW. A value past
                    // either end draws a FLAT LINE ALONG THAT EDGE: it is
                    // neither folded back nor dropped, because a figure pinned
                    // at the top and a figure off the scale are different
                    // things and the picture must not make them look alike.
                    //
                    // INSET BY HALF THE STROKE so a pinned line is drawn at
                    // full brightness. At exactly 0 or exactly the height,
                    // half of a 1.8 px stroke falls outside the image and the
                    // clamped line comes out at half ink, which reads as a
                    // line FADING rather than a line STOPPED.
                    const float inset = 0.9f;
                    const float t01 = juce::jlimit (0.0f, 1.0f,
                                        (tr.second - t.v[(size_t) line][(size_t) idx])
                                            / juce::jmax (0.0001f, tr.second - tr.first));
                    const float y = inset + t01 * juce::jmax (1.0f, (float) plot.getHeight()
                                                                        - 2.0f * inset);
                    if (k == 0) p.startNewSubPath (x, y);
                    else        p.lineTo (x, y);
                }
                // setColour FIRST: see strokeVis.
                // kTraceTrailWhite, NOT kTrailWhite: the trace trail is the
                // axis colour NEAT so the filament's lifted version reads as a
                // light line over it. See the constants above.
                ig.setColour (c.interpolatedWith (juce::Colours::white, kTraceTrailWhite));
                ig.setOpacity (kTraceTrailInk * weight);
                ig.strokePath (p, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::rounded));
            }
        };
        traceOf (refTrace, C::text2, kTrailRefW);
        traceOf (mixTrace, C::blue,  1.0f);
        return;                      // the ribbon rows are the spectrum's alone
    }

    // A SIDE WITH NO FAST SOURCE GETS NO GHOST. Not a faked trail from one
    // stored number: nothing is happening to a still capture.
    //
    // The two sides differ by WEIGHT rather than by a second alpha on the
    // colour: one place decides how dark a frame is, so the saturation
    // arithmetic above holds for both.
    // THE REFERENCE FROM WHICHEVER SOURCE IT HAS. Rolling: the live
    // analysis. Stopped: its own stored whole-file curve, expanded onto this
    // same grid, so the picture keeps both sides instead of losing one.
    //
    // A STATIC CURVE SATURATES INTO A STEADY BAND, AND THAT IS CORRECT. It
    // strokes the SAME path every hop, so the trail stacks in exactly one
    // place and settles at ink / (1 - fade * (1 - ink)) rather than fanning
    // out. It will look like a bug to whoever reads it next: it is not. A
    // whole-file average does not move, and a fan would be drawing variation
    // that the measurement does not have. The mix's fan beside it is what
    // carries the movement.
    if      (refVisOk)          strokeVis (refVis, visRowRef, visRowRefInit, C::text2, kTrailRefW);
    else if (refStaticOk)       strokeVis (refStatic, visRowRef, visRowRefInit, C::text2, kTrailRefW);
    else if (refFast != nullptr) stroke (*refFast, C::text2, kTrailRefW);
    if      (mixVisOk)          strokeVis (mixVis, visRowMix, visRowMixInit, C::blue,  1.0f);
    else if (mixFast != nullptr) stroke (*mixFast, C::blue,  1.0f);
}

// WHICH PAINTED REGION A POINT IS IN. One function, so the press and the hover
// can never disagree about where a control is: they are the same rects from the
// same pure layout.
static int matchZoneAt (juce::Rectangle<int> bounds, juce::Point<int> p)
{
    const auto R = echojay::matchPageLayout (bounds);
    if (R.button .contains (p)) return 1;
    if (R.mixPick.contains (p)) return 2;
    if (R.refPick.contains (p)) return 3;
    if (R.mixWave.contains (p)) return 4;
    if (R.refWave.contains (p)) return 5;
    // 10..13 are the four axis tiles, in the enum's own order.
    for (int i = 0; i < echojay::kMatchAxisCount; ++i)
        if (echojay::matchAxisTile (R.axisRow, i).contains (p)) return 10 + i;
    return 0;
}

void EchoJayEditor::MatchPanel::mouseUp (const juce::MouseEvent& e)
{
    if (host == nullptr) return;
    const int z = matchZoneAt (getLocalBounds(), e.getPosition());
    switch (z)
    {
        case 1: press(); break;
        // THE MIX PICKER CANNOT PLACE A REFERENCE and the reference picker
        // writes into the slot that is already the reference side, which is why
        // no pick can swap the two roles.
        case 2: host->openMixPicker(); break;
        case 3: host->openReferenceBrowser (host->matchRefIsTop()); break;
        // PLAYBACK IS THE COMPARE DUAL-STREAM, not the A/B path: both streams
        // are already rolling and analysed, so swapping is click-free and
        // sample-aligned.
        //
        // A CLICK ON THE WAVE IS A SEEK, THE SAME GESTURE COMPARE HAS. It seeks,
        // plays and makes that side audible in one move, through the one
        // seekCompareStream both pages call. THIS REPLACES the plain
        // toggle-audible this strip had: the Compare gesture does both at once
        // and a click that only changed which side you heard, while the other
        // page's identical click also moved the playhead, would be two meanings
        // for one action.
        case 4:
        case 5:
        {
            const bool isRef   = (z == 5);
            const int  slotIdx = isRef ? (host->matchRefIsTop() ? 0 : 1)
                                       : (host->matchRefIsTop() ? 1 : 0);
            const auto R    = echojay::matchPageLayout (getLocalBounds());
            auto       lane = (isRef ? R.refWave : R.mixWave).withTrimmedBottom (9);
            const auto btn  = echojay::matchWaveTransport (lane, isRef);
            const auto wave = echojay::matchWaveLane (lane, isRef);
            const auto& s   = isRef ? *host->matchSlots().ref : *host->matchSlots().mix;

            // A LIVE SIDE HAS NO STORED STREAM, so neither gesture means
            // anything on it: toggleComparePlay says so in its own comment and
            // the strip draws "host" rather than a dead button.
            if (s.kind == CompareSlotState::Kind::Live) break;

            if (btn.contains (e.getPosition()))
                host->toggleComparePlay (slotIdx == 0);
            else if (wave.contains (e.getPosition()))
                host->seekCompareStream (slotIdx,
                                          echojay::matchWaveSeekFraction (wave, e.getPosition().x));
            repaint();
            break;
        }
        default:
            // THE AXIS TILES. Selecting one changes the picture below and
            // NOTHING else: nothing is sent, applied or written. A morph in
            // flight is dropped rather than carried onto a picture it was not
            // computed for.
            if (z >= 10 && z < 10 + echojay::kMatchAxisCount)
            {
                const auto next = (echojay::MatchAxis) (z - 10);
                if (next != axis)
                {
                    axis = next;
                    morphing = false;
                    morphPos = 0.0f;
                    morphDone = false;
                    refusedFor = 0.0f;
                    repaint();
                }
            }
            break;
    }
}

void EchoJayEditor::MatchPanel::mouseMove (const juce::MouseEvent& e)
{
    const int z = matchZoneAt (getLocalBounds(), e.getPosition());
    if (z != hotZone) { hotZone = z; buttonHot = (z == 1); repaint(); }
}

// THE CLOCK RUNS ONLY WHILE THE PAGE IS ON SCREEN. An invisible page asking for
// 30 repaints a second is the shape that makes a plugin feel heavy for no
// reason anyone can see.
void EchoJayEditor::MatchPanel::visibilityChanged()
{
    if (isVisible())
    {
        openFade = 0.0f;       // the page fades in every time it opens
        morphPos = 0.0f; morphing = false; morphDone = false;
        refusedFor = 0.0f; buttonHot = false;
        // The slots may have changed while this page was away, so the pair
        // stored by the last opening is not this opening's: the first paint
        // builds its own again rather than drawing what was true before.
        tickSidesValid = false;
        startTimerHz (60);
    }
    else
    {
        stopTimer();
    }
}

void EchoJayEditor::MatchPanel::timerCallback()
{
    // 60 Hz SINCE THE TRAIL WENT ON THE HOP COUNTER. This dt drives the page
    // fade, the link pulse, the morph and the refusal countdown, so leaving it
    // at 1/30 with a 60 Hz timer would have played all four at DOUBLE speed.
    constexpr float dt = 1.0f / 60.0f;
    bool busy = false;

    // THE CLOUDS STEP EVERY TICK, which is what makes a LIVE side come alive
    // for free: its measurements change, so its targets move, so its points
    // chase them. Nothing here smooths that into stillness.
    if (host != nullptr && isVisible())
    {
        // THE ONE BUILD OF THE TICK. paint reads what this stores.
        tickSides      = host->buildMatchSides();
        tickSidesValid = true;
        // The trail advances once per TICK, never once per repaint: a hover
        // must not run the history faster than time.
        ++tickSeq;
    }

    if (openFade < 1.0f) { openFade = juce::jmin (1.0f, openFade + dt * 4.0f); busy = true; }

    // The link's pulse runs while the page is open: it is the one thing that
    // moves before the press, and it says the screen is ready rather than
    // stalled. 0.22 of the run per second, so it reads as a drift and not a
    // scan line.
    linkPhase += dt * 0.22f;
    if (linkPhase > 1.0f) linkPhase -= 1.0f;
    busy = true;

    if (morphing)
    {
        morphPos += dt / 0.9f;                   // 900 ms end to end
        if (morphPos >= 1.0f) { morphPos = 1.0f; morphing = false; morphDone = true; }
        busy = true;
    }

    if (refusedFor > 0.0f)
    {
        refusedFor = juce::jmax (0.0f, refusedFor - dt);
        busy = true;
    }

    if (! busy) stopTimer();
    repaint();
}

bool EchoJayEditor::MatchPanel::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey && host != nullptr)
    {
        host->goBackToCompare();
        return true;
    }
    // RETURN AND SPACE PRESS THE BUTTON. It is painted rather than a child, so
    // without this the page's one control is reachable by mouse only, and the
    // panel already takes keyboard focus when it opens.
    if (k == juce::KeyPress::returnKey || k == juce::KeyPress::spaceKey)
    {
        press();
        return true;
    }
    return false;
}



// =================================================================================================
//  MatchHost (A0b, 28 Oct 2026) - the editor answering the Match tab's one door
// =================================================================================================
//
// They live in THIS file rather than PluginEditor.cpp on purpose: they exist for the Match tab, they
// are read beside the code that calls them, and the next person changing the boundary changes one
// file. The four that refuse are the ones whose other half is not built yet; each says so in words,
// because a stub that quietly succeeds is how a caller comes to believe something happened.

const CaptureSnapshot* EchoJayEditor::latestCapture() const
{
    const auto snaps = processorRef.getSnapshots();          // by value: kept below for the caller
    if (snaps.empty()) return nullptr;
    latestCaptureCache_ = snaps.back();
    return &latestCaptureCache_;
}

const CaptureSnapshot* EchoJayEditor::verifyCapture() const
{
    // K6's closed loop takes a SECOND capture after the plan has been applied, and nothing takes one
    // yet. Null is the honest answer and C2 says what it means: "no verify capture exists", which is
    // not the same as "nothing changed".
    return nullptr;
}

const ReferenceResult* EchoJayEditor::selectedReference() const
{
    // WHICH slot is the reference is matchRefIsTop_'s business and matchSlots answers it; the slot
    // then names the reference by INDEX INTO getReferences(), which is the convention
    // applyReferenceToSlot writes and the browser reads. Matching on the NAME instead would break
    // the moment two references share one, and two files called "master.wav" is the normal case.
    const auto pair = matchSlots();
    if (pair.ref == nullptr || pair.ref->kind != CompareSlotState::Kind::Reference) return nullptr;
    const auto refs = processorRef.getReferenceAnalyser().getReferences();
    if (pair.ref->index < 0 || pair.ref->index >= (int) refs.size()) return nullptr;
    selectedRefCache_ = refs[(size_t) pair.ref->index];   // by value: kept here for the caller
    return &selectedRefCache_;
}

std::vector<echojay::MatchLink> EchoJayEditor::liveLinks() const
{
    std::vector<echojay::MatchLink> out;
    for (const auto& li : processorRef.getLinkSlotInfos())
    {
        echojay::MatchLink m;
        m.uid  = li.uid;
        m.name = processorRef.resolveLinkDisplayName (li.uid).isNotEmpty()
                     ? processorRef.resolveLinkDisplayName (li.uid) : li.name;
        // NO ROLE HERE, DELIBERATELY. The channel's role is the run record's to know (C6 §1 settles
        // the same question for taste events: two sides sending one fact is how they disagree).
        // Nothing in this process holds a trustworthy role for a Link, so this leaves it empty
        // rather than inventing one from a track name.
        m.framesReceived = li.framesRead;
        if (li.dialCapable) m.caps.add ("dial_step");
        if (li.connected)   m.caps.add ("link_meters");
        out.push_back (std::move (m));
    }
    return out;
}

bool EchoJayEditor::pickMixSlot (const juce::String& name, juce::String& why)
{
    // BY NAME, per MATCH_SCREEN_CONTRACT §2B - never by which bar is on top. The side effects of a
    // pick (ending codec mode, stopping and restarting the stream, redefining AI Compare's subject)
    // belong to the editor and stay here, which is the whole reason this is a host call.
    for (const auto& snap : processorRef.getSnapshots())
        if (snap.name == name)
        {
            CompareSlotState next;
            next.kind  = CompareSlotState::Kind::Snapshot;
            next.label = snap.name;
            matchApplyMixSlot (next);
            return true;
        }
    why = "there is no capture called \"" + name + "\" to put on the mix side";
    return false;
}

bool EchoJayEditor::pickReferenceSlot (const juce::String& name, juce::String& why)
{
    const auto refs = processorRef.getReferenceAnalyser().getReferences();
    for (int i = 0; i < (int) refs.size(); ++i)
        if (refs[(size_t) i].name == name)
        {
            applyReferenceToSlot (matchRefIsTop_, i);
            return true;
        }
    why = "there is no reference called \"" + name + "\" in this session";
    return false;
}

void EchoJayEditor::setAudible (echojay::MatchRefSide side)
{
    toggleCompareAudible (side == echojay::MatchRefSide::Top);
}

void EchoJayEditor::postToSessionChat (const juce::String& kind, const juce::var&)
{
    // U2 builds the Whole session chat. Until it exists there is nowhere to post to, and saying so is
    // better than dropping a card on the floor.
    EchoJay_NSLog (("EJMatch: a \"" + kind + "\" card was offered to the Whole session chat, which is "
                    "not built yet (U2) - nothing was posted").toRawUTF8());
}

void EchoJayEditor::draftPlan (const juce::var&,
                               std::function<void (bool, juce::String, juce::String)> cb)
{
    if (cb) cb (false, {}, "the plan road is not built yet (A2), so Make a plan has nowhere to send "
                           "this - the report is still yours to read");
}

void EchoJayEditor::openSessionChatAt (const juce::String&)
{
    EchoJay_NSLog ("EJMatch: asked to open the Whole session chat at a plan, which is not built yet (U2)");
}

/** TRUE, AND IT IS COMPILE-TIME TRUE. "Is this the V2" has a real answer here because the Link is a
    separate target with its own editor (LinkEditor): nothing that runs this class is a Link. It is
    still a host call rather than a constant in the page, because the page must not be the thing that
    knows which binary it is in - that is how a shared front end grows two behaviours. */
bool EchoJayEditor::isV2() const        { return true; }
bool EchoJayEditor::agentOn() const     { return agentApprovalMode() != AgentApproval::Off; }
bool EchoJayEditor::hasCap (const juce::String& cap) const
{
    // The plugin has no capability LIST of its own - capabilities are what the server sends the
    // model. What this build can actually answer for are its own roads, so it answers those and
    // says no to anything else rather than guessing yes.
    if (cap == "real_editor")   return ChainHost::devModeActive();
    if (cap == "live_view")     return echojay::kShadowViewEnabled;
    if (cap == "match")         return true;
    return false;
}
juce::String EchoJayEditor::pluginVersion() const { return JucePlugin_VersionString; }

echojay::SpectralEvidence EchoJayEditor::slotSpectralEvidence (const CompareSlotState& slot) const
{
    return getSlotSpectralEvidence (slot);
}

double EchoJayEditor::streamSampleRate() const { return processorRef.getSampleRate(); }

/** BOUNDS-CHECKED, because the index comes from the page. cmpStream is two entries and an out of
    range slot used to be a read past them; the page asking about a third slot now gets "not
    playing" rather than whatever sat after the array. */
bool EchoJayEditor::compareSlotPlaying (int slotIdx) const
{
    if (slotIdx < 0 || slotIdx >= 2) return false;
    return processorRef.cmpStream[slotIdx].playing.load();
}
