// comp_profile_guard - COMP_PROFILE_SPEC_v1, the plugin's side of measured compressor profiles.
//
// Item 2: read `comp_profile` from the parameter-map payload, and `expected_gr_db` / `expected_level_db` from the
// chain block; store them on the slot. Missing means today's behaviour (letter (q)).
//
// A profile joins to a plugin by `map_fp` - "the same fingerprint EchoJay computes for that plugin's parameter
// map" - so the profile is read from paramMaps_[fp]["comp_profile"], which is the payload the server already
// publishes maps in. A built-in carries no fingerprint, so it can never have one; this guard uses Apple's AUDelay
// for the fingerprinted slot, the same plugin level_loop_guard (6a) uses and for the same reason.

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "ChainHost.h"
#include "EedDeviceRegistry.h"
#include "EedGainProcessor.h"
#include "EJCompCheck.h"
#include <cstdio>
#include <cmath>

namespace {
int failures = 0;
void check (bool ok, const juce::String& w, const juce::String& d = {})
{ std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
void pumpMs (int ms)
{ const double end = juce::Time::getMillisecondCounterHiRes() + ms;
  while (juce::Time::getMillisecondCounterHiRes() < end) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, false); } }

/** A profile exactly as spec section 3 prints it, with the fields a test wants to vary. */
juce::var profileVar (const juce::String& mapFp, const juce::String& topology = "threshold",
                      float maxErrorDb = 0.4f, const juce::String& schema = "ej_comp_profile/1",
                      bool withAmount = true)
{
    auto* pl = new juce::DynamicObject();
    pl->setProperty ("name", "EMO-D5 (s)"); pl->setProperty ("manufacturer", "Waves");
    pl->setProperty ("format", "AudioUnit"); pl->setProperty ("map_fp", mapFp);
    pl->setProperty ("version", "15.0.70");
    auto* fit = new juce::DynamicObject();
    fit->setProperty ("max_error_db", (double) maxErrorDb); fit->setProperty ("points", 341);
    auto* amount = new juce::DynamicObject();
    amount->setProperty ("control", "Comp Thresh");
    juce::Array<juce::var> curve;
    for (auto pr : { std::make_pair (0.0, -59.6), std::make_pair (0.5, -29.8), std::make_pair (1.0, 0.2) })
    { auto* pt = new juce::DynamicObject();
      pt->setProperty ("norm", pr.first); pt->setProperty ("eff_threshold_dbfs", pr.second);
      curve.add (juce::var (pt)); }
    amount->setProperty ("curve", curve);
    auto* o = new juce::DynamicObject();
    o->setProperty ("schema", schema);
    o->setProperty ("plugin", juce::var (pl));
    o->setProperty ("topology", topology);
    o->setProperty ("fit", juce::var (fit));
    if (withAmount) o->setProperty ("amount", juce::var (amount));
    o->setProperty ("static_gain_db", 0.0);
    return juce::var (o);
}

/** Put a map under `fp` carrying this profile, through the same door the server's maps arrive by. */
void publishProfile (ChainHost& h, const juce::String& fp, const juce::var& profile)
{
    auto* m = new juce::DynamicObject();
    m->setProperty ("category", "compressor");
    if (! profile.isVoid()) m->setProperty ("comp_profile", profile);
    auto* maps = new juce::DynamicObject();
    maps->setProperty (fp, juce::var (m));
    h.storeParamMaps (juce::var (maps));
    pumpMs (120);
}

void guardMain()
{
    std::printf ("== comp_profile_guard: COMP_PROFILE_SPEC_v1 items 2 and 3 ==\n");

    std::printf ("\n-- (F) the flag is OFF by default, and with it off nothing changes --\n");
    {
        // "Behind a flag, default OFF. With the flag off, behaviour is exactly (q)."
        check (! ChainHost::compProfilesEnabled(),
               "(F) compProfilesEnabled() is FALSE unless the flag file exists  (if this fails, the file is on "
               "this machine: rm ~/Library/EchoJay/comp_profiles_on.txt)",
               ChainHost::compProfilesEnabled() ? juce::String ("ON") : juce::String ("off"));
    }

    auto procPtr = std::make_unique<EchoJayProcessor>();
    auto& proc = *procPtr;
    proc.prepareToPlay (48000.0, 512);
    auto& h = proc.getChainHost();
    h.onNeedParamMaps = [] (const juce::StringArray&) {};

    std::printf ("\n-- (2a) a fingerprinted slot picks up the profile published under its fp --\n");
    juce::PluginDescription dly;
    dly.name = "AUDelay"; dly.pluginFormatName = "AudioUnit";
    dly.fileOrIdentifier = "AudioUnit:Effects/aufx,dely,appl";
    dly.uniqueId = dly.deprecatedUid = (int) (juce::int64) juce::String ("64607a6d").getHexValue64();
    h.loadPluginAsync (dly, ChainHost::LoadOrigin::User, [] (const juce::String&) {});
    for (int k = 0; k < 40 && h.getNumSlots() < 1; ++k) pumpMs (100);
    check (h.getNumSlots() == 1, "(2a) precondition: a real plugin loaded",
           juce::String (h.getNumSlots()) + " slot(s)");
    const auto fp = h.getSlotIdentity (0).fp;
    check (fp.isNotEmpty(), "(2a) precondition: it carries a fingerprint to join a profile by", fp.substring (0, 16));
    check (h.slotCompProfile (0).isVoid(),
           "(2a) with no profile published, the slot has none - which is spec 2.5, today's behaviour");
    if (fp.isNotEmpty())
    {
        publishProfile (h, fp, profileVar (fp));
        const auto got = h.slotCompProfile (0);
        check (got.isObject(),
               "(2a) once published under the slot's own map fingerprint, the slot HAS its profile  (RED as it "
               "stood: nothing read comp_profile at all)",
               got.isObject() ? juce::String ("present") : juce::String ("absent"));
        const auto info = ChainHost::readCompProfile (got);
        check (info.usable && info.plugin == "EMO-D5 (s)" && info.mapFp == fp && info.topology == "threshold",
               "(2a) ...and reads as usable, naming its plugin, its map_fp and its topology",
               info.plugin + " / " + info.topology + " / err " + juce::String (info.maxErrorDb, 2));
    }

    std::printf ("\n-- (2b) THE SERVER IS THE SINGLE GATE: the plugin uses what it is given (spec v1.4) --\n");
    if (fp.isNotEmpty())
    {
        // SUPERSEDED 1 Oct 2026: these legs asserted the plugin's OWN trust checks - schema, topology and
        // fit.max_error_db over 1.5. v1.4 names quality.point_error_db as THE trust gate and it is the SERVER's:
        // "it only attaches a comp_profile that passed its validator, so the plugin uses any comp_profile it
        // receives. One set of rules, in one place." Two copies of a rule is one too many - they drift, and v1.3
        // retiring `fit` for `quality` is exactly how that drift showed up. So the assertions are inverted: what
        // was refused is now used.
        //
        // Sean's own case: no `fit` block at all, and a quality.point_error_db of 0.2.
        {
            auto prof = profileVar (fp);
            auto* o = prof.getDynamicObject();
            o->removeProperty ("fit");
            auto* q = new juce::DynamicObject(); q->setProperty ("point_error_db", 0.2);
            q->setProperty ("method", "hold 2.5 s vs 5 s");
            o->setProperty ("quality", juce::var (q));
            publishProfile (h, fp, prof);
            check (h.slotCompProfile (0).isObject(),
                   "(2b) a profile with NO fit block and quality.point_error_db 0.2 IS USED  (RED as it stood: the "
                   "plugin ran its own fit.max_error_db gate, a rule that v1.3 had already retired - a second copy "
                   "of the server's rule, drifting)",
                   h.slotCompProfile (0).isObject() ? juce::String ("used") : juce::String ("REFUSED"));
        }
        // ...and what the old gates refused is now used, because the server already decided.
        publishProfile (h, fp, profileVar (fp, "threshold", 2.0f));
        check (h.slotCompProfile (0).isObject(),
               "(2b) a fit worse than 1.5 dB is USED - that judgement is the server's");
        publishProfile (h, fp, profileVar (fp, "other", 0.4f));
        check (h.slotCompProfile (0).isObject(),
               "(2b) topology \"other\" is USED - the server does not attach one it will not set");
        publishProfile (h, fp, profileVar (fp, "threshold", 0.4f, "ej_comp_profile/2"));
        check (h.slotCompProfile (0).isObject(),
               "(2b) a schema this client has not seen is USED - the server validated it");
        publishProfile (h, fp, profileVar (fp, "threshold", 0.4f, "ej_comp_profile/1", false));
        check (h.slotCompProfile (0).isObject(),
               "(2b) and a profile with NO amount control is used too: it names the compressor in the line, there "
               "is simply nothing to correct WITH");
        {   // ...and the one thing that is still read off it: whether there IS an amount control.
            const auto info = ChainHost::readCompProfile (h.slotCompProfile (0));
            check (info.usable && ! info.hasAmount,
                   "(2b) ...which is reported rather than hidden", info.hasAmount ? "has amount" : "no amount");
        }
        // THE ONE REFUSAL THAT REMAINS is not a trust judgement: a profile for ANOTHER BINARY.
        publishProfile (h, fp, profileVar (fp.substring (0, 12)));
        check (h.slotCompProfile (0).isVoid(),
               "(2b) a profile whose map_fp is not this slot's full fingerprint is still refused - that is not "
               "\"is this profile good\" but \"is this profile FOR THIS SLOT\", and using one measured on another "
               "binary would dial a threshold from somewhere else");
        publishProfile (h, fp, profileVar (fp));
        check (h.slotCompProfile (0).isObject(), "(2b) ...and the slot's own fingerprint is used");
    }

    std::printf ("\n-- (2c) the block's expectations are stored on the slot --\n");
    {
        check (! (h.slotExpectedGrDb (0) == h.slotExpectedGrDb (0)),
               "(2c) a slot nobody set expectations on has NO figure - NaN, not 0.0, because 0 dB of expected gain "
               "reduction is a different statement from \"the block said nothing\"",
               juce::String (h.slotExpectedGrDb (0)));
        h.setSlotExpectations (0, 2.0f, -1.5f);
        check (std::abs (h.slotExpectedGrDb (0) - 2.0f) < 0.001f
                   && std::abs (h.slotExpectedLevelDb (0) - -1.5f) < 0.001f,
               "(2c) ...and once the block carries them they are on the slot, both of them",
               juce::String (h.slotExpectedGrDb (0), 1) + " dB GR, "
                   + juce::String (h.slotExpectedLevelDb (0), 1) + " dB level");
        h.setSlotExpectations (0, std::numeric_limits<float>::quiet_NaN(),
                                  std::numeric_limits<float>::quiet_NaN());
        check (! (h.slotExpectedGrDb (0) == h.slotExpectedGrDb (0)),
               "(2c) ...and they clear back to nothing, so a later build without them is not held to an old figure");
    }

    std::printf ("\n-- (2d) a built-in can never have a profile, and says so by its fingerprint --\n");
    {
        const auto* gn = BuiltinDeviceRegistry::instance().findByName ("EchoJay Gain");
        check (gn != nullptr, "(2d) precondition: a built-in is registered");
        if (gn != nullptr)
        {
            static const auto pullIn = EedGainProcessor::schema().params().size(); juce::ignoreUnused (pullIn);
            h.loadPluginAsync (BuiltinDeviceRegistry::descriptionFor (*gn), ChainHost::LoadOrigin::User,
                               [] (const juce::String&) {});
            for (int k = 0; k < 40 && h.getNumSlots() < 2; ++k) pumpMs (100);
            check (h.getNumSlots() == 2, "(2d) precondition: the built-in landed",
                   juce::String (h.getNumSlots()) + " slot(s)");
            check (h.getSlotIdentity (1).fp.isEmpty() && h.slotCompProfile (1).isVoid(),
                   "(2d) it carries no fingerprint, so it has no profile and takes today's road - EchoJay's own "
                   "devices are not what profiles are for",
                   "fp=\"" + h.getSlotIdentity (1).fp + "\"");
        }
    }

    std::printf ("\n-- (3) THE ONE CHECK: all three outcomes, spec section 7 --\n");
    {
        using CC = echojay::CompCheck;
        const auto prof = profileVar ("abc123");
        auto reading = [] (float expectedGr, float levelChange, float loudHeard = 20.0f,
                           bool settled = true, float staticGain = 0.0f, float expLevel = std::numeric_limits<float>::quiet_NaN())
        {
            CC::Reading r;
            r.dialSettled = settled; r.loudHeardSeconds = loudHeard;
            r.levelChangeDb = levelChange; r.expectedGrDb = expectedGr;
            r.staticGainDb = staticGain; r.expectedLevelDb = expLevel;
            return r;
        };
        // ---- OUTCOME 1: in range. The reading agrees with the profile, so nothing moves.
        {
            const auto res = CC::decide (reading (2.0f, -2.3f));
            check (res.outcome == CC::Outcome::inRange && std::abs (res.observedDropDb - 2.3f) < 0.01f
                       && res.correctionDb == 0.0f,
                   "(3) a slot dropping 2.3 dB against an expected 2.0 is IN RANGE: nothing moves",
                   res.why);
        }
        // ---- OUTCOME 2: too much. One move, by the difference, toward less.
        {
            const auto res = CC::decide (reading (2.0f, -6.5f));
            check (res.outcome == CC::Outcome::tooMuch && std::abs (res.correctionDb - 4.5f) < 0.01f,
                   "(3) 6.5 dB against an expected 2.0 is more than expected + 3, so the amount moves ONCE by the "
                   "difference  (RED as it stood: nothing checked, and a profile that over-compressed stayed where "
                   "the server put it)",
                   res.why);
            // ...and exactly at the boundary it does NOT move: "more than expected + 3".
            const auto edge = CC::decide (reading (2.0f, -5.0f));
            check (edge.outcome == CC::Outcome::inRange,
                   "(3) ...and exactly expected + 3 does not move - the spec says MORE than", edge.why);
            // THE MOVE ITSELF, through the profile's own curve: less GR is a HIGHER effective threshold.
            const float from = 0.5f;                       // eff -29.8 on this curve
            const float to = CC::amountNormForLessGr (prof, from, res.correctionDb);
            const auto c = CC::curveOf (prof);
            check (to > from,
                   "(3) the amount norm moves UP the curve, because less gain reduction is a higher threshold",
                   juce::String (from, 3) + " -> " + juce::String (to, 3));
            check (std::abs ((CC::effForNorm (c, to) - CC::effForNorm (c, from)) - res.correctionDb) < 0.1f,
                   "(3) ...by exactly the dB the check asked for, read off amount.curve and not guessed",
                   juce::String (CC::effForNorm (c, from), 1) + " -> " + juce::String (CC::effForNorm (c, to), 1)
                       + " dBFS, wanted +" + juce::String (res.correctionDb, 1));
            check (CC::amountControl (prof) == "Comp Thresh",
                   "(3) ...on the control the PROFILE names, not one this client chose",
                   CC::amountControl (prof));
        }
        // ---- OUTCOME 3: not engaging. Change nothing, report it.
        {
            const auto r = reading (2.0f, -0.2f);
            const auto res = CC::decide (r);
            check (res.outcome == CC::Outcome::notEngaging,
                   "(3) 0.2 dB against an expected 2.0 is NOT ENGAGING: the profile is wrong, so nothing is moved",
                   res.why);
            const auto line = CC::notEngagingLine ("EMO-D5 (s)", "32b7e1d9a0c3", r, res);
            check (line.startsWith ("PROFILE_NOT_ENGAGING") && line.contains ("plugin=\"EMO-D5 (s)\"")
                       && line.contains ("map_fp=32b7e1d9a0c3") && line.contains ("expected_gr_db=2.0")
                       && line.contains ("observed_drop_db=0.20"),
                   "(3) ...and the line names the plugin, the map_fp and the readings, as the spec says",
                   line);
            // ...but 0.2 dB against an expected 0.5 is NOT a report: the spec floors it at 1 dB expected.
            check (CC::decide (reading (0.5f, -0.2f)).outcome == CC::Outcome::inRange,
                   "(3) ...while an expected figure under 1 dB never reports NOT_ENGAGING, because 0.2 of an "
                   "expected 0.5 is not evidence of anything");
        }
        // ---- THE GATES: it does not check early, and it does not check what it cannot.
        {
            check (CC::decide (reading (2.0f, -6.5f, 20.0f, false)).outcome == CC::Outcome::notYet,
                   "(3) it does not check before the dial has settled");
            check (CC::decide (reading (2.0f, -6.5f, 4.0f, true)).outcome == CC::Outcome::notYet,
                   "(3) ...nor before 10 s of loud material, however wrong the reading looks");
            check (CC::decide (reading (std::numeric_limits<float>::quiet_NaN(), -6.5f)).outcome == CC::Outcome::noCheck,
                   "(3) ...and a block with no expected figure is NOT checked at all - that is today's behaviour");
        }
        // ---- THE SUBTRACTIONS, which are what make the drop mean compression.
        {
            // static_gain_db +2: the slot is 2 dB LOUDER for free, so a measured -4.5 dB change is 6.5 of
            // compression. (This leg passed before the sign was fixed by passing -2.0 while its comment said +2 -
            // the assertion and the implementation were wrong together, which is the one way a leg can lie.)
            const auto res = CC::decide (reading (2.0f, -4.5f, 20.0f, true, 2.0f));
            check (std::abs (res.observedDropDb - 6.5f) < 0.01f,
                   "(3) static_gain_db is subtracted, so the drop means COMPRESSION and not a fixed output trim",
                   juce::String (res.observedDropDb, 2) + " dB of drop from a 4.5 dB level change");
            // expected_level_db: an input_drive unit's amount control moves the level itself.
            const auto r2 = CC::decide (reading (2.0f, -6.5f, 20.0f, true, 0.0f, -4.0f));
            check (std::abs (r2.observedDropDb - 2.5f) < 0.01f && r2.outcome == CC::Outcome::inRange,
                   "(3) ...and so is the expected level change, so an input_drive unit is not accused of "
                   "compressing by its own input knob",
                   juce::String (r2.observedDropDb, 2) + " dB of drop from a 6.5 dB level change");
        }
    }

    std::printf ("\n-- (v2) controls_norm, the full map_fp, and a stepped detent (spec v1.1/v1.2) --\n");
    {
        using CC = echojay::CompCheck;
        // ---- ITEM 2: controls_norm lands a RAW NORM and counts as applied.
        // Driven through applyStructuredSettings, which is the function the DIAL calls with a chain block's
        // settings_structured - the block's own road. (setSlotStructuredSettings only STORES them for that pass,
        // and in this rig the pass does not run, so asserting through it would assert the rig and not the code.)
        // AUDelay's "Delay time" is continuous with a wide range, so a norm is unambiguous and nothing resolves it
        // through a map - which is the whole reason the amount position travels as a norm.
        if (h.getNumSlots() >= 1)
        {
            auto normBlock = [] (const juce::var& value)
            {
                auto* pp = new juce::DynamicObject(); pp->setProperty ("Delay time", value);
                auto* w = new juce::DynamicObject(); w->setProperty ("controls_norm", juce::var (pp));
                return juce::var (w);
            };
            const auto report = h.applyStructuredSettings (0, normBlock (0.674), juce::var());
            pumpMs (300);
            float raw = 0.0f, parsed = 0.0f; juce::String text; bool okParse = false;
            const bool read = h.readControlRaw (0, "Delay time", raw, text, okParse, parsed);
            check (read && std::abs (raw - 0.674f) < 0.01f,
                   "(v2) a controls_norm entry lands the RAW NORM 0.674 on the control  (RED as it stood: "
                   "controls_norm was not read at all, so the amount position the server picks off amount.curve "
                   "was never written)",
                   read ? ("norm " + juce::String (raw, 4) + ", reads \"" + text + "\"")
                        : juce::String ("could not read the control back"));
            bool counted = false;
            for (const auto& rr : report)
                if (rr.semantic.containsIgnoreCase ("Delay time") && rr.applied
                    && std::abs (rr.normalized - 0.674f) < 0.01f) counted = true;
            check (counted,
                   "(v2) ...and the apply REPORT counts it as applied, with the norm it wrote, so the dial summary "
                   "names it rather than the write being silent",
                   juce::String ((int) report.size()) + " result(s)");
            // A value that is not a number writes NOTHING and is an honesty entry, never a silent skip.
            const auto badReport = h.applyStructuredSettings (0, normBlock ("loud"), juce::var());
            pumpMs (200);
            float raw2 = 0.0f, p2 = 0.0f; juce::String t2; bool ok2 = false;
            h.readControlRaw (0, "Delay time", raw2, t2, ok2, p2);
            check (std::abs (raw2 - 0.674f) < 0.01f,
                   "(v2) ...while a controls_norm value that is not a number writes nothing and leaves the control "
                   "where it was",
                   "norm still " + juce::String (raw2, 4));
            bool said = false;
            for (const auto& rr : badReport) if (! rr.applied && rr.note.contains ("not a number")) said = true;
            check (said,
                   "(v2) ...and says so in its report, rather than skipping it quietly",
                   juce::String ((int) badReport.size()) + " result(s)");
            // ...and a control the plugin does not have is reported too.
            auto* ghost = new juce::DynamicObject(); ghost->setProperty ("No Such Knob", 0.5);
            auto* gw = new juce::DynamicObject(); gw->setProperty ("controls_norm", juce::var (ghost));
            const auto ghostReport = h.applyStructuredSettings (0, juce::var (gw), juce::var());
            bool named = false;
            for (const auto& rr : ghostReport)
                if (! rr.applied && rr.note.contains ("does not have")) named = true;
            check (named, "(v2) ...and so is a controls_norm entry naming a control that does not exist");
        }

        // ---- ITEM 3: the join key is the FULL 64-hex fingerprint, not the 12-char log form.
        const auto slotFp = h.getSlotIdentity (0).fp;
        check (slotFp.length() == 64,
               "(v2) precondition: the slot's fingerprint is the full 64 hex",
               juce::String (slotFp.length()) + " chars");
        publishProfile (h, slotFp, profileVar (slotFp.substring (0, 12)));   // the LOG form, as a profile might carry
        check (h.slotCompProfile (0).isVoid(),
               "(v2) a profile whose map_fp is the 12-CHAR LOG FORM is refused  (the spec says the fp= in "
               "EJDialSummary is only the first 12 characters and will not match; a 12-char prefix must not pass "
               "as the key, or a profile measured on another binary could dial this one)",
               h.slotCompProfile (0).isVoid() ? juce::String ("refused") : juce::String ("ACCEPTED"));
        publishProfile (h, slotFp, profileVar (juce::String ("f").paddedRight ('f', 64)));
        check (h.slotCompProfile (0).isVoid(),
               "(v2) ...and so is a full-length fingerprint that is simply a different one");
        publishProfile (h, slotFp, profileVar (slotFp));
        check (h.slotCompProfile (0).isObject(),
               "(v2) ...while the slot's OWN full fingerprint is accepted",
               h.slotCompProfile (0).isObject() ? juce::String ("accepted") : juce::String ("refused"));

        // ---- ITEM 4: on a stepped amount control the correction moves to the ADJACENT LISTED DETENT.
        {
            auto stepped = [] (bool isStepped)
            {
                auto* amount = new juce::DynamicObject();
                amount->setProperty ("control", "Comp Thresh");
                amount->setProperty ("stepped", isStepped);
                juce::Array<juce::var> curve;
                // detents at 0.0, 0.25, 0.50, 0.75, 1.00 with 10 dB between them
                for (int i = 0; i < 5; ++i)
                { auto* pt = new juce::DynamicObject();
                  pt->setProperty ("norm", 0.25 * i);
                  pt->setProperty ("eff_threshold_dbfs", -40.0 + 10.0 * i);
                  curve.add (juce::var (pt)); }
                amount->setProperty ("curve", curve);
                auto* o = new juce::DynamicObject();
                o->setProperty ("schema", "ej_comp_profile/1");
                o->setProperty ("topology", "threshold");
                o->setProperty ("amount", juce::var (amount));
                auto* fit = new juce::DynamicObject(); fit->setProperty ("max_error_db", 0.4);
                o->setProperty ("fit", juce::var (fit));
                return juce::var (o);
            };
            // Asking for 4 dB less from the detent at 0.50 (eff -20): the ideal is eff -16, which lies BETWEEN the
            // detents at 0.50 (-20) and 0.75 (-10).
            const float contin = CC::amountNormForLessGr (stepped (false), 0.5f, 4.0f);
            check (contin > 0.5f && contin < 0.75f,
                   "(v2) a CONTINUOUS amount interpolates to the exact position the check asked for",
                   juce::String (contin, 4));
            const float detent = CC::amountNormForLessGr (stepped (true), 0.5f, 4.0f);
            check (std::abs (detent - 0.75f) < 0.0001f,
                   "(v2) ...while a STEPPED one moves to the ADJACENT LISTED DETENT, 0.75, never the in-between "
                   "0.60  (RED as it stood: it interpolated, and a norm between two detents lands on whichever one "
                   "the plugin rounds to - a position nobody chose)",
                   juce::String (detent, 4));
            // ...one step only, even when the ideal lies well past the next detent.
            const float far = CC::amountNormForLessGr (stepped (true), 0.5f, 25.0f);
            check (std::abs (far - 0.75f) < 0.0001f,
                   "(v2) ...and it is ONE step even when the ideal is past it, because one correction is one move",
                   juce::String (far, 4));
            // ...and at the last detent it stays there rather than running off the end.
            const float end = CC::amountNormForLessGr (stepped (true), 1.0f, 4.0f);
            check (std::abs (end - 1.0f) < 0.0001f,
                   "(v2) ...and at the last detent it stays put", juce::String (end, 4));
        }
    }

    std::printf ("\n-- (v3) the curve's threshold field is in_at_gr_dbfs[\"1\"] (spec v1.3) --\n");
    {
        using CC = echojay::CompCheck;
        // v1.3: "eff_threshold_dbfs (optional, informational): identical to in_at_gr_dbfs[\"1\"] by definition. The
        // server never reads it; in_at_gr_dbfs is the only threshold field it uses." The v1.3 example omits
        // eff_threshold_dbfs from every point, so a curve built only from that field is EMPTY and the one
        // correction silently does nothing - which is worse than a wrong move, because nothing says it failed.
        auto curve = [] (bool withEff, bool withGr)
        {
            auto* amount = new juce::DynamicObject();
            amount->setProperty ("control", "Comp Thresh");
            juce::Array<juce::var> pts;
            for (int i = 0; i < 3; ++i)
            {
                auto* pt = new juce::DynamicObject();
                pt->setProperty ("norm", 0.5 * i);
                const double eff = -40.0 + 10.0 * i;
                if (withEff) pt->setProperty ("eff_threshold_dbfs", eff);
                if (withGr)
                {
                    auto* g = new juce::DynamicObject();
                    g->setProperty ("1", eff); g->setProperty ("2", eff + 1.3); g->setProperty ("3", eff + 2.6);
                    pt->setProperty ("in_at_gr_dbfs", juce::var (g));
                }
                pts.add (juce::var (pt));
            }
            amount->setProperty ("curve", pts);
            auto* o = new juce::DynamicObject();
            o->setProperty ("schema", "ej_comp_profile/1");
            o->setProperty ("topology", "threshold");
            o->setProperty ("amount", juce::var (amount));
            return juce::var (o);
        };
        // A v1.3 profile: in_at_gr_dbfs only, no eff_threshold_dbfs anywhere.
        const auto v13 = curve (false, true);
        check ((int) CC::curveOf (v13).size() == 3,
               "(v3) a v1.3 curve with NO eff_threshold_dbfs is read from in_at_gr_dbfs[\"1\"]  (RED as it stood: "
               "the curve came out EMPTY and amountNormForLessGr returned the current norm, so the one correction "
               "did nothing at all and said nothing about it)",
               juce::String ((int) CC::curveOf (v13).size()) + " point(s)");
        const float moved = CC::amountNormForLessGr (v13, 0.5f, 4.0f);
        check (moved > 0.5f,
               "(v3) ...so the correction actually moves on a v1.3 profile",
               juce::String (moved, 4));
        // A v1.2 profile still works: eff_threshold_dbfs only.
        const auto v12 = curve (true, false);
        check ((int) CC::curveOf (v12).size() == 3 && CC::amountNormForLessGr (v12, 0.5f, 4.0f) > 0.5f,
               "(v3) ...and a v1.2 profile with only eff_threshold_dbfs still reads, so the fallback is real",
               juce::String ((int) CC::curveOf (v12).size()) + " point(s)");
        // Both present: in_at_gr_dbfs wins, because the spec says that is the field the threshold comes from.
        const auto both = curve (true, true);
        check ((int) CC::curveOf (both).size() == 3,
               "(v3) ...and with both present it reads, taking in_at_gr_dbfs as the spec directs");
        // A point whose sweep never reached 1 dB (null) cannot anchor the curve.
        {
            auto* amount = new juce::DynamicObject();
            amount->setProperty ("control", "Comp Thresh");
            juce::Array<juce::var> pts;
            for (int i = 0; i < 2; ++i)
            {
                auto* pt = new juce::DynamicObject(); pt->setProperty ("norm", 0.5 * i);
                auto* g = new juce::DynamicObject();
                if (i == 0) g->setProperty ("1", -40.0); else g->setProperty ("1", juce::var());
                pt->setProperty ("in_at_gr_dbfs", juce::var (g));
                pts.add (juce::var (pt));
            }
            amount->setProperty ("curve", pts);
            auto* o = new juce::DynamicObject(); o->setProperty ("amount", juce::var (amount));
            check ((int) CC::curveOf (juce::var (o)).size() == 1,
                   "(v3) ...while a point whose in_at_gr_dbfs[\"1\"] is null is left out - the sweep never reached "
                   "1 dB there, so it cannot anchor anything",
                   juce::String ((int) CC::curveOf (juce::var (o)).size()) + " point(s) of 2");
        }
    }

    std::printf ("\n==== comp_profile_guard: %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
}
} // namespace

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    guardMain();
    return failures == 0 ? 0 : 1;
}
