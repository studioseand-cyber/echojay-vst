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

    std::printf ("\n-- (2b) a profile that cannot be trusted is reported ABSENT, not half-used --\n");
    if (fp.isNotEmpty())
    {
        // "fit.max_error_db: over 1.5 dB means the profile is not trusted and the server treats it as no profile."
        publishProfile (h, fp, profileVar (fp, "threshold", 2.0f));
        check (h.slotCompProfile (0).isVoid(),
               "(2b) a fit worse than 1.5 dB is no profile  (a bad fit must not quietly become a dialled "
               "threshold)");
        // "other: anything else. The server does not auto-set it; it is treated as no profile."
        publishProfile (h, fp, profileVar (fp, "other", 0.4f));
        check (h.slotCompProfile (0).isVoid(), "(2b) topology \"other\" is no profile");
        publishProfile (h, fp, profileVar (fp, "threshold", 0.4f, "ej_comp_profile/2"));
        check (h.slotCompProfile (0).isVoid(), "(2b) a schema this client does not know is no profile");
        publishProfile (h, fp, profileVar (fp, "threshold", 0.4f, "ej_comp_profile/1", false));
        check (h.slotCompProfile (0).isVoid(), "(2b) and a profile with no amount control is no profile");
        // ...while input_drive IS handled, because the spec names it as a topology the client must take.
        publishProfile (h, fp, profileVar (fp, "input_drive", 0.4f));
        check (h.slotCompProfile (0).isObject(),
               "(2b) ...but input_drive is usable: the spec names it alongside threshold");
        publishProfile (h, fp, profileVar (fp, "threshold"));   // leave a good one for what follows
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
