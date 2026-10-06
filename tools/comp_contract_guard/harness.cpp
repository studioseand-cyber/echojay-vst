// comp_contract_guard - the plugin's side of COMP_PROFILE_SPEC_v1, against the SERVER'S OWN OUTPUT.
//
// B generates ~/Desktop/ej_contract/ from the real code path on echojay-saas feat/comp-profiles: "every payload here
// came out of the same functions a live request calls, so these files cannot disagree with the server." This guard
// feeds those files, verbatim, through the functions the plugin really uses - no hand-written fixture in between -
// so a disagreement between the two sides is caught here instead of in a session.
//
// It is deliberately NOT a mock of the server. If the files are absent it says so and passes, because a missing
// contract drop is not a plugin fault; every assertion it DOES make is against bytes the server wrote.
//
//   ej_contract/map_payload_emo.json   what the plugin receives beside a parameter map, profile attached
//   ej_contract/build_request.json     a build request carrying track_level
//   ej_contract/build_reply.json       the reply: EMO-D5 (profile) and NEOLD U2A (none)
//   ej_contract/DONE                   the commit they were generated from

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "ChainHost.h"
#include "EJCompCheck.h"
#include "EJTrackLevel.h"
#include "EedDeviceRegistry.h"
#include "EedGainProcessor.h"
#include <cstdio>
#include <cmath>

// 5 Oct 2026 (Sean's ruling): THIS HARNESS DRIVES ITS OWN WINDOWS, so it opts out of the fresh-window wait
// EXPLICITLY, per loop. In the product an unknown heard-clock means WAIT, because a begin site that forgot to fill
// it would silently bring back the stale reading item 3 closed. A synthetic leg has no clock to supply - it sets
// Window::heardSeconds by hand - so it is the one legitimate caller that must say so out loud. Routed through one
// helper rather than stamped on seventy Config declarations, so the opt-out is auditable in a single place.
static void beginDriven (echojay::CalibLoop& l, echojay::CalibLoop::Config c)
{ c.noFreshWait = true; l.begin (c); }

namespace {
int failures = 0, skipped = 0;
void check (bool ok, const juce::String& w, const juce::String& d = {})
{ std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
void note (const juce::String& w)
{ std::printf ("  note  %s\n", w.toRawUTF8()); }
void pumpMs (int ms)
{ const double end = juce::Time::getMillisecondCounterHiRes() + ms;
  while (juce::Time::getMillisecondCounterHiRes() < end) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, false); } }

juce::File contractDir()
{
    // The operator's own Desktop, where B drops it. Not under the isolated state root: this is an input the
    // harness reads, never state it writes.
    return juce::File::getSpecialLocation (juce::File::userHomeDirectory)
               .getChildFile ("Desktop").getChildFile ("ej_contract");
}

juce::var load (const juce::String& name, bool& okOut)
{
    const auto f = contractDir().getChildFile (name);
    if (! f.existsAsFile()) { okOut = false; return {}; }
    const auto v = juce::JSON::parse (f.loadFileAsString());
    okOut = ! v.isVoid();
    return v;
}

void guardMain()
{
    std::printf ("== comp_contract_guard: the plugin against the server's own output ==\n");

    const auto dir = contractDir();
    if (! dir.isDirectory())
    {
        note ("~/Desktop/ej_contract does not exist: nothing to check against. A missing contract drop is not a "
              "plugin fault, so this guard passes - and says so rather than inventing a fixture.");
        ++skipped;
        std::printf ("\n==== comp_contract_guard: GREEN (0 failed, %d skipped) ====\n", skipped);
        return;
    }
    const auto doneFile = dir.getChildFile ("DONE");
    if (doneFile.existsAsFile())
        note ("DONE says: " + doneFile.loadFileAsString().trim().substring (0, 120));
    else
        note ("no DONE file yet: these files may still be being written, so a failure here could be a half-written "
              "drop rather than a disagreement.");

    bool okMap = false, okReq = false, okRep = false;
    const auto mapPayload = load ("map_payload_emo.json", okMap);
    const auto request    = load ("build_request.json",   okReq);
    const auto reply      = load ("build_reply.json",     okRep);
    check (okMap && okReq && okRep,
           "the three payloads parse as JSON",
           juce::String (okMap ? "map " : "MAP? ") + (okReq ? "req " : "REQ? ") + (okRep ? "reply" : "REPLY?"));
    if (! (okMap && okRep)) { std::printf ("\n==== comp_contract_guard: RED (%d) ====\n", failures); return; }

    // ---------------------------------------------------------------------------------------------------------
    std::printf ("\n-- the PROFILE, read by the function the plugin really uses --\n");
    juce::var profile;
    juce::String serverFp;
    if (auto* maps = mapPayload.getProperty ("maps", juce::var()).getDynamicObject())
        for (auto& kv : maps->getProperties())
        {
            serverFp = kv.name.toString();
            profile = kv.value.getProperty ("comp_profile", juce::var());
            break;
        }
    check (serverFp.length() == 64,
           "the map is keyed by the FULL 64-hex fingerprint, which is the join key the spec names",
           juce::String (serverFp.length()) + " chars");
    check (profile.isObject(), "the map payload carries a comp_profile");
    const auto info = ChainHost::readCompProfile (profile);
    check (info.usable, "readCompProfile reads it", info.usable ? info.plugin : info.whyNot);
    check (info.mapFp == serverFp,
           "the profile's own plugin.map_fp equals the fp it is published under - so the join the plugin makes on "
           "a real slot would succeed",
           info.mapFp.length() == 64 ? juce::String ("matches, 64 hex") : ("\"" + info.mapFp + "\""));
    check (info.topology == "threshold" || info.topology == "input_drive",
           "its topology is one the client understands", info.topology);

    // The amount curve, through CompCheck - which after v1.3 must read in_at_gr_dbfs["1"].
    const auto curve = echojay::CompCheck::curveOf (profile);
    check (curve.size() >= 2,
           "CompCheck::curveOf reads the server's amount curve  (if this is 0, the curve is being read from a "
           "field the server no longer sends - exactly the v1.3 eff_threshold_dbfs trap)",
           juce::String ((int) curve.size()) + " point(s)");
    check (! echojay::CompCheck::amountControl (profile).isEmpty(),
           "...and the amount control is named", echojay::CompCheck::amountControl (profile));
    {
        bool rises = curve.size() >= 2 && curve.back().eff > curve.front().eff;
        check (rises,
               "...and its effective threshold rises with the norm, so 'less gain reduction' means 'up the curve'",
               curve.size() >= 2 ? (juce::String (curve.front().eff, 1) + " -> " + juce::String (curve.back().eff, 1))
                                 : juce::String ("-"));
    }
    if (auto* po = profile.getDynamicObject())
    {
        check (po->hasProperty ("detector_f"),
               "detector_f is present (v1.4 requires it; the SERVER gates on it, the plugin only passes the two "
               "level figures it needs)",
               po->getProperty ("detector_f").toString());

        // detector_f = 0 IS A VALUE, NOT AN ABSENCE - and B's real EMO-D5 profile carries exactly 0, so this is
        // the live case and not a hypothetical. Spec section 6 makes f the server's own threshold input
        // (L = loud_rms + f x (loud_peak - loud_rms - 3.01)); at f = 0 that is simply the RMS level. The plugin
        // never reads f at all, which is why there is deliberately no plugin-side "profile without detector_f is
        // refused" rule: that is the SERVER's gate (1 Oct ruling - the server is the single gate, and only the
        // map_fp JOIN stays here, because identity is "is this profile for this slot", not "is it good enough").
        // What this leg protects against is someone later adding that rule as a TRUTHINESS test, which would
        // reject every pure-RMS unit in the catalogue while looking perfectly reasonable in review.
        {
            const auto f = po->getProperty ("detector_f");
            check (f.isDouble() || f.isInt() || f.isInt64(),
                   "detector_f is a number on the wire", f.toString());
            check (ChainHost::readCompProfile (profile).usable,
                   "a profile whose detector_f is 0 is USED - zero is pure-RMS detection, not a missing field",
                   "detector_f=" + f.toString());
        }
        check (po->hasProperty ("never_touch"),
               "never_touch is present, so the plugin can honour it");
        check (po->hasProperty ("static_gain_db"),
               "static_gain_db is present - the plugin subtracts it in the section 7 check",
               juce::String (echojay::CompCheck::staticGainOf (profile), 2) + " dB");
    }

    // ---------------------------------------------------------------------------------------------------------
    std::printf ("\n-- the CALIBRATION BLOCKS, through configsFromBlock --\n");
    const auto block = reply.getProperty ("block", juce::var());
    check (block.isObject(), "the reply carries a block");
    std::vector<echojay::CalibLoop::Config> cfgs;
    juce::String why;
    const int chainLen = [&block] { if (auto* a = block.getProperty ("chain", juce::var()).getArray()) return a->size(); return 0; }();
    const int found = echojay::CalibLoop::configsFromBlock (block, chainLen, false, nullptr, cfgs, why);
    check (found >= 1, "configsFromBlock reads the server's calibrations[]",
           juce::String (found) + " block(s) of a " + juce::String (chainLen) + "-slot chain");
    if (why.isNotEmpty()) note ("parser said: " + why);
    bool sawProfiled = false, sawUnprofiled = false;
    for (const auto& c : cfgs)
    {
        const bool hasGr = c.expectedGrDb == c.expectedGrDb;
        if (c.fromProfile && hasGr) sawProfiled = true;
        if (! c.fromProfile && ! hasGr) sawUnprofiled = true;
        note ("slot " + juce::String (c.slot + 1) + ": from_profile=" + (c.fromProfile ? "y" : "n")
              + " expected_gr_db=" + (hasGr ? juce::String (c.expectedGrDb, 1) : juce::String ("(none)"))
              + " expected_level_db="
              + (c.expectedLevelDb == c.expectedLevelDb ? juce::String (c.expectedLevelDb, 1)
                                                        : juce::String ("(none)"))
              + " actuator=" + juce::String (c.actuator == echojay::CalibLoop::Actuator::Drive ? "drive" : "other")
              + " params=" + juce::String (c.params.size()));
    }
    check (sawProfiled,
           "a compressor the server set FROM A PROFILE arrives with from_profile true AND an expected_gr_db  (RED "
           "as it stood: the plugin read those off the chain SLOT object, where the server does not put them, so "
           "on a real reply it would have found nothing and checked nothing)");
    check (sawUnprofiled,
           "...and the compressor with no profile carries neither, which is what keeps it on today's behaviour");
    for (const auto& c : cfgs)
        check (c.params.isEmpty() && c.actuator == echojay::CalibLoop::Actuator::Drive,
               "every block is actuator=drive with no param, so the loop moves EchoJay's own IN/OUT and never a "
               "plugin control (the standing ruling)",
               "slot " + juce::String (c.slot + 1));

    // ---------------------------------------------------------------------------------------------------------
    std::printf ("\n-- controls_norm, applied by the real apply path --\n");
    auto procPtr = std::make_unique<EchoJayProcessor>();
    auto& proc = *procPtr;
    proc.prepareToPlay (48000.0, 512);
    auto& h = proc.getChainHost();
    h.onNeedParamMaps = [] (const juce::StringArray&) {};
    juce::PluginDescription dly;
    dly.name = "AUDelay"; dly.pluginFormatName = "AudioUnit";
    dly.fileOrIdentifier = "AudioUnit:Effects/aufx,dely,appl";
    dly.uniqueId = dly.deprecatedUid = (int) (juce::int64) juce::String ("64607a6d").getHexValue64();
    h.loadPluginAsync (dly, ChainHost::LoadOrigin::User, [] (const juce::String&) {});
    for (int k = 0; k < 40 && h.getNumSlots() < 1; ++k) pumpMs (100);
    check (h.getNumSlots() == 1, "precondition: a real plugin to write norms to",
           juce::String (h.getNumSlots()) + " slot(s)");

    // The server's OWN controls_norm object, taken out of its reply.
    juce::var serverNorms;
    juce::String normSlotName;
    if (auto* arr = block.getProperty ("chain", juce::var()).getArray())
        for (const auto& sv : *arr)
        {
            const auto ss = sv.getProperty ("settings_structured", juce::var());
            if (ss.getProperty ("controls_norm", juce::var()).isObject())
            { serverNorms = ss.getProperty ("controls_norm", juce::var());
              normSlotName = sv.getProperty ("name", juce::var()).toString(); break; }
        }
    check (serverNorms.isObject(),
           "the profiled slot carries controls_norm", normSlotName);
    if (auto* no = serverNorms.getDynamicObject())
    {
        bool allNumeric = true, allInRange = true;
        juce::StringArray names;
        for (auto& kv : no->getProperties())
        {
            names.add (kv.name.toString());
            const bool num = kv.value.isDouble() || kv.value.isInt() || kv.value.isInt64();
            allNumeric = allNumeric && num;
            if (num) { const double d = (double) kv.value; allInRange = allInRange && d >= 0.0 && d <= 1.0; }
        }
        check (allNumeric, "every controls_norm value is a NUMBER, which is what the plugin writes raw",
               names.joinIntoString (", "));
        check (allInRange, "...and every one is inside 0..1");
        check (names.contains (echojay::CompCheck::amountControl (profile)),
               "...and the AMOUNT control appears there and only there - it has no display text to dial, which is "
               "why controls_norm exists",
               "amount is \"" + echojay::CompCheck::amountControl (profile) + "\"");
        // The real apply path, on a plugin that does NOT have these controls: every miss must be an honesty entry.
        auto* wrap = new juce::DynamicObject(); wrap->setProperty ("controls_norm", serverNorms);
        const auto rep = h.applyStructuredSettings (0, juce::var (wrap), juce::var());
        int named = 0;
        for (const auto& r : rep) if (! r.applied && r.note.contains ("does not have")) ++named;
        check (named == names.size(),
               "applying the server's norms to a plugin without those controls names EVERY one it could not write "
               "- no silent skips",
               juce::String (named) + " of " + juce::String (names.size()) + " reported");
        // ...and the same machinery DOES write a norm on a control this plugin has, with the server's own value.
        const double amountNorm = (double) no->getProperty (
            juce::Identifier (echojay::CompCheck::amountControl (profile)));
        auto* one = new juce::DynamicObject(); one->setProperty ("Delay time", amountNorm);
        auto* w2 = new juce::DynamicObject(); w2->setProperty ("controls_norm", juce::var (one));
        h.applyStructuredSettings (0, juce::var (w2), juce::var());
        pumpMs (300);
        float raw = 0.0f, parsed = 0.0f; juce::String text; bool okParse = false;
        h.readControlRaw (0, "Delay time", raw, text, okParse, parsed);
        check (std::abs (raw - (float) amountNorm) < 0.01f,
               "...and the server's own amount norm lands RAW when the control exists",
               juce::String (amountNorm, 4) + " -> " + juce::String (raw, 4));
    }

    // ---------------------------------------------------------------------------------------------------------
    std::printf ("\n-- track_level: what the plugin sends against what the server expects --\n");
    if (auto* tl = request.getProperty ("track_level", juce::var()).getDynamicObject())
    {
        echojay::TrackLevel mine;
        mine.prepare (48000.0);
        // Feed it a tone at the server's own loud_rms figure, so the two numbers are comparable by construction.
        const float wantRms = (float) (double) tl->getProperty ("loud_rms_dbfs");
        std::vector<float> buf ((size_t) 512);
        double phase = 0.0;
        const double inc = 2.0 * juce::MathConstants<double>::pi * 997.0 / 48000.0;
        const float amp = juce::Decibels::decibelsToGain (wantRms) * std::sqrt (2.0f);
        const int total = (int) std::lround (40.0 * 48000.0);
        for (int done = 0; done < total; )
        {
            const int n = juce::jmin (512, total - done);
            for (int i = 0; i < n; ++i) { buf[(size_t) i] = amp * (float) std::sin (phase); phase += inc; }
            mine.push (buf.data(), nullptr, n);
            done += n;
        }
        const auto sent = mine.toVar();
        check (sent.isObject(), "the plugin produces a track_level object");
        if (auto* so = sent.getDynamicObject())
        {
            for (auto field : { "loud_rms_dbfs", "loud_peak_dbfs", "window", "heard_s" })
                check (so->hasProperty (field) && tl->hasProperty (field),
                       juce::String ("both sides carry ") + field);
            check (std::abs ((float) (double) so->getProperty ("loud_rms_dbfs") - wantRms) <= 0.05f,
                   "the plugin's loud_rms_dbfs reproduces the server's figure on a tone at that level, so the "
                   "CONVENTION agrees (plain RMS: a full-scale sine is -3.01, not 0)",
                   juce::String ((double) so->getProperty ("loud_rms_dbfs")) + " vs the server's "
                       + juce::String (wantRms, 2));
            // THIS WAS THE GUARD'S OWN DEFECT, and it is the reason the label was wrong for a whole day.
            // The first cut noted a disagreement as "a MISMATCH for B, not a plugin fault" and, when the two
            // agreed, called check (true, ...) - an assertion that cannot fail, which proves nothing and merely
            // prints. Worse, the note asserted the spec said "400ms_rms_p95"; the spec says "400ms_p95" and has
            // since v1.3, so the guard argued the plugin's case from a premise the contract contradicted and
            // turned a plugin bug into a complaint about the server. A check must be able to fail, and the side
            // it indicts must be read off the CONTRACT, not assumed. So: the label is asserted against the spec's
            // own string, and the server's generated request is asserted against the same string - either side
            // that drifts is named, by name, and neither gets the benefit of the doubt.
            const auto mineWin  = so->getProperty ("window").toString();
            const auto theirWin = tl->getProperty ("window").toString();
            const juce::String specWin ("400ms_p95");   // COMP_PROFILE_SPEC_v1 section 5, verbatim
            check (mineWin == specWin,
                   "the plugin's window label is the spec's string verbatim (section 5)",
                   "plugin sends \"" + mineWin + "\", spec says \"" + specWin + "\"");
            check (theirWin == specWin,
                   "...and so is the server's, in its own generated request - if THIS is the red one it is B's "
                   "side to change, and the plugin is already right",
                   "server sends \"" + theirWin + "\", spec says \"" + specWin + "\"");
            check (mineWin == theirWin,
                   "...so the two agree on the wire",
                   mineWin + " vs " + theirWin);
        }
    }
    else note ("the request carries no track_level: nothing to compare.");

    // ---------------------------------------------------------------------------------------------------------
    std::printf ("\n-- the closing line, from the server's own figures --\n");
    {
        echojay::CalibLoop l;
        echojay::CalibLoop::Config c;
        c.plugin = "EMO-D5 (s)"; c.slot = 1; c.purpose = echojay::CalibLoop::Purpose::buildHold;
        c.dynamicsSlot = true; c.mode = echojay::CalibLoop::Mode::Passive;
        beginDriven (l, c);
        // BOTH flags: the feature is on (which picks section 7's wording at all) and THIS slot has a profile.
        l.profilesFeatureOn = true;
        l.hasProfile = true;
        l.lastGr = -2.0f;                 // the server expected 2 dB; the loop measured it
        l.levelTrimmedDb = -1.5f; l.slotGainDb = -1.5f; l.levelHeld = true;
        const auto line = l.completedLine();
        // 4 Oct 2026 RULING: a MEASURED figure is labelled measured, and "from its profile" is reserved for
        // PREDICTIONS. This leg asserted the old sentence, which said "about 2 dB ... from its profile" about a
        // figure the loop had measured itself - presenting our own reading as the profile's claim, and hiding that
        // it had missed the target. The line now names the plugin, says the reading is MEASURED, and shows what it
        // was aiming for, so a miss is visible in the sentence rather than only in the log.
        check (line.startsWith ("EMO-D5 (s):") && line.contains ("measured"),
               "a profiled compressor's line names the plugin and labels the reading as MEASURED (not \"from its "
               "profile\", which is for predictions)", line);
        check (line.contains ("aimed for"),
               "...and shows the figure it was aiming for, so a miss is visible in the sentence", line);
        check (! line.contains ("from its profile"),
               "...and does NOT claim a measured figure came from the profile", line);
        check (line.contains ("Output -1.5"),
               "...and states the OUT the hold wrote", line);
        // The unprofiled compressor ON A FLAG-ON SESSION: section 7 still governs the wording, so this sets the
        // feature on and the profile off. With the feature OFF it would be letter (q)'s line instead, and
        // level_loop_guard (16) is what asserts that case - the two legs together pin both sides of the flag.
        echojay::CalibLoop l2; beginDriven (l2, c); l2.profilesFeatureOn = true; l2.hasProfile = false;
        l2.levelTrimmedDb = 0.0f; l2.levelResidualDb = 0.0f;
        check (l2.completedLine().contains ("no profile yet"),
               "...while the one with no profile says so", l2.completedLine());
    }

    // ---- controls_norm IS A CONTAINER, AND IT WINS (2 Oct 2026, Sean's 16:33 session) ----------------------
    {
        std::printf ("\n-- controls_norm is never a control name, and never a second write --\n");
        // Two faults from one session, both in the apply path, and both were VISIBLE IN THIS GUARD'S OWN OUTPUT
        // before it asserted them: it printed "controls_norm: manual 0.000 (no mapping for this control on this
        // plugin)" and passed. A guard that prints the defect and stays green is the thing to fix first.
        //   1. the flat pass treated the controls_norm KEY as a parameter name, so Sean's card said "except
        //      controls norm which needs hand-dialing" - naming a control no plugin has;
        //   2. a control carried in BOTH maps was written twice ("Gain: APPLIED 0.336" from controls, then
        //      "Gain: APPLIED 0.330" from controls_norm), so the readback described the value that lost.
        const auto amount = echojay::CompCheck::amountControl (profile);
        auto* norms = new juce::DynamicObject();
        norms->setProperty (juce::Identifier (amount), 0.6683);
        auto* ctrls = new juce::DynamicObject();
        ctrls->setProperty (juce::Identifier (amount), "-19.9");      // the SAME control, as display text
        auto* both = new juce::DynamicObject();
        both->setProperty ("controls",      juce::var (ctrls));
        both->setProperty ("controls_norm", juce::var (norms));
        const auto rep2 = h.applyStructuredSettings (0, juce::var (both), juce::var());
        int named = 0, writes = 0;
        for (const auto& r : rep2)
        {
            if (r.semantic.containsIgnoreCase ("controls_norm")) ++named;
            if (echojay::normalizeControlName (r.semantic)
                    .equalsIgnoreCase (echojay::normalizeControlName (amount)) && r.applied) ++writes;
        }
        check (named == 0,
               "no apply result is named after the controls_norm CONTAINER  (RED as it stood: the key was looked "
               "up as a parameter, missed, and reported to the user as needing their hand)",
               juce::String (named) + " result(s) named controls_norm");
        check (writes <= 1,
               "a control carried in BOTH maps is written ONCE  (RED as it stood: two writes 80 microseconds "
               "apart, and the dial summary described the one that was overwritten)",
               juce::String (writes) + " applied write(s) to \"" + amount + "\"");
    }

    std::printf ("\n==== comp_contract_guard: %s (%d assertion(s) failed, %d skipped) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures, skipped);
}
} // namespace

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    guardMain();
    return failures == 0 ? 0 : 1;
}
