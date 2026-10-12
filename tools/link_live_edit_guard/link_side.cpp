// link_live_edit_guard / LINK SIDE (11 Oct 2026). Sean's leg, verbatim: "with a Link addressed, build a chain
// with a +12 dB gain slot from the V2 and assert the Link's rendered output changes without a rack switch, then
// change a param and assert it changes again."
//
// WHY THIS SHAPE. The in-host report was that a V2 edit to an addressed Link lands only when the V2 switches
// away. Every instrument that could have settled it was unreliable: the Link ACKED the build "ok" within two
// seconds (so the ack proves nothing), and the shared log both rotated inside half an hour and could not say
// which process wrote a line. So this guard asks the only question that cannot be faked - DOES THE AUDIO COMING
// OUT OF THE LINK CHANGE - and it asks it while the V2 still holds the rack. A state-only check (slot counts,
// sidecar contents) would have passed on the broken build, because the V2's mirror was perfectly correct; it was
// the Link's own rendering that never moved.
//
// RENDERED, not reported: a fixed sine goes in, the RMS of what processBlock leaves in the buffer comes out, and
// the +12 dB slot has to show up in that number. The rack is never switched - the V2 side holds its session open
// across both assertions and asserts its own session is still live at the end.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "LinkProcessor.h"
#include "ChainHost.h"
#include "LinkShm.h"
#include "EedDeviceRegistry.h"
#include <cstdio>
#include <cmath>
#include <memory>
// The header already declares this struct a friend, so the guard reads the SHIPPED LinkProcessor's own rack
// rather than a stand-in. getNumSlots() on the Link's real ChainHost is the only thing that can answer "did the
// Link actually take the edit".
struct EchoJayLinkSyncTestAccess
{
    static juce::String uid (LinkProcessor& p) { return p.instanceUid_; }
    static int slots (LinkProcessor& p) { return p.chainHost.getNumSlots(); }
    static int numPlugins (LinkProcessor& p) { return p.chainHost.getNumPlugins(); }
};
using TA = EchoJayLinkSyncTestAccess;
namespace {
int failures = 0;
void check (bool ok, const juce::String& w, const juce::String& d = {})
{ std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
void pumpMs (int ms)
{ const double end = juce::Time::getMillisecondCounterHiRes() + ms;
  while (juce::Time::getMillisecondCounterHiRes() < end) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false); } }
bool waitFile (const juce::File& f, int ms)
{ const double end = juce::Time::getMillisecondCounterHiRes() + ms;
  while (juce::Time::getMillisecondCounterHiRes() < end) { if (f.existsAsFile()) return true; pumpMs (50); } return f.existsAsFile(); }

// A FIXED sine in, the RMS of what the Link leaves in the buffer out. Deterministic on purpose: the whole leg is
// a comparison of three of these numbers, so the input may not vary between them.
double renderRmsDb (LinkProcessor& l, double seconds)
{
    juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
    const int blocks = juce::jmax (1, (int) (seconds * 48000.0 / 512.0));
    static double phase = 0.0;
    const double inc = 2.0 * juce::MathConstants<double>::pi * 220.0 / 48000.0;
    // The first half settles the Link's gain smoothing; only the second half is measured, or a ramp would be
    // read as a level.
    double sum = 0.0; long long n = 0;
    for (int b = 0; b < blocks; ++b)
    {
        for (int i = 0; i < 512; ++i)
        {
            const float s = (float) (std::sin (phase) * 0.1);
            phase += inc; if (phase > 2.0e6) phase = std::fmod (phase, 2.0 * juce::MathConstants<double>::pi);
            buf.setSample (0, i, s); buf.setSample (1, i, s);
        }
        l.processBlock (buf, midi);
        if (b >= blocks / 2)
            for (int ch = 0; ch < 2; ++ch)
            { const auto* d = buf.getReadPointer (ch); for (int i = 0; i < 512; ++i) { sum += (double) d[i] * d[i]; ++n; } }
        if ((b % 20) == 19) pumpMs (1);   // the 30 Hz timer consumes chain-cmds and publishes
    }
    const double rms = n > 0 ? std::sqrt (sum / (double) n) : 0.0;
    return 20.0 * std::log10 (juce::jmax (1.0e-9, rms));
}
// Keep draining the message thread, with audio running, until a predicate holds or the bound expires. The
// predicate is checked on the LINK's own rack, never on a file somebody else wrote.
template <typename Fn>
bool pumpUntil (LinkProcessor& l, Fn&& done, int ms)
{
    const double end = juce::Time::getMillisecondCounterHiRes() + ms;
    while (juce::Time::getMillisecondCounterHiRes() < end)
    {
        if (done()) return true;
        renderRmsDb (l, 0.1);
    }
    return done();
}
}
int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::String H = argc > 1 ? argv[1] : "/tmp";
    juce::File (H).createDirectory();
    int rc = 2;
    {
        auto link = std::make_unique<LinkProcessor>();
        link->linkName = "LiveEdit";
        link->markTypedNameAuthoritative();
        link->prepareToPlay (48000.0, 512);
        juce::String uid;
        for (int k = 0; k < 400 && uid.isEmpty(); ++k) { uid = TA::uid (*link); if (uid.isEmpty()) pumpMs (10); }
        if (uid.isEmpty()) { std::printf ("link side: the Link never claimed a uid\n"); return 2; }
        std::printf ("link side: uid=%s\n", uid.toRawUTF8());

        // ---- A HARNESS PRECONDITION, SAID OUT LOUD ----------------------------------------------------
        // An `add` resolves its name through ChainHost::resolveByName, which asks BuiltinDeviceRegistry. The
        // registry is filled by static initialisers in the device translation units, and a harness linking a
        // static archive only pulls the objects it references - so an empty registry here would abort every add
        // with "not in the loadable plugin list" and this guard would report a product failure that is really
        // its own link line. It refuses instead. (First written after exactly that: the leg went RED on an
        // aborted add and the abort was mine.)
        const int nBuiltins = (int) BuiltinDeviceRegistry::instance().descriptions().size();
        const bool haveLevel = BuiltinDeviceRegistry::instance().findByName ("EchoJay EQ") != nullptr;
        juce::StringArray devNames;
        for (const auto& d : BuiltinDeviceRegistry::instance().descriptions()) devNames.add (d.name);
        std::printf ("    builtin registry: %d device(s) [%s], \"EchoJay EQ\" %s\n",
                     nBuiltins, devNames.joinIntoString (", ").toRawUTF8(), haveLevel ? "present" : "ABSENT");
        if (! haveLevel)
        {
            std::printf ("link side: HARNESS PRECONDITION FAILED - this binary has no built-in device registry, so no\n"
                         "           add of a built-in can resolve. That is this harness's link line, NOT the product.\n");
            return 2;
        }

        // ---- BASELINE: an empty rack, rendered ----
        const double dbEmpty = renderRmsDb (*link, 1.0);
        std::printf ("    rendered RMS, empty rack:      %+.2f dBFS  (%d slot(s))\n", dbEmpty, TA::slots (*link));

        // ---- A PLUGIN THIS LINK CAN ONLY KNOW FROM THE SHARED CACHE -----------------------------------
        // Built-ins resolve from the compiled registry and always worked, which is why a leg built only on
        // them passes on the broken code - it did. Everything else reaches a Link through chain_entries.xml,
        // written by whichever host last scanned. One row is laid down here so the next assertion can ask the
        // question that matters: when a v:2 edit arrives, is it judged against this list or against nothing?
        {
            juce::PluginDescription d;
            d.name = "Guard Third Party"; d.pluginFormatName = "AudioUnit";
            d.manufacturerName = "EchoJay Guard"; d.fileOrIdentifier = "echojay:guard:thirdparty";
            d.uniqueId = 0x47554131; d.numInputChannels = 2; d.numOutputChannels = 2;
            auto root = std::make_unique<juce::XmlElement> ("CHAIN_ENTRIES");
            root->setAttribute ("scannedAt", juce::String (juce::Time::getCurrentTime().toMilliseconds()));
            if (auto x = d.createXml()) root->addChildElement (x.release());
            const auto ec = ChainHost::getEntriesCacheFile();
            ec.getParentDirectory().createDirectory();
            ec.replaceWithText (root->toString());
            std::printf ("    laid down a shared entries cache at %s (%d byte(s))\n",
                         ec.getFullPathName().toRawUTF8(), (int) ec.getSize());
        }
        std::printf ("    loadable entries the Link can see BEFORE any command: %d\n", TA::numPlugins (*link));

        auto* ro = new juce::DynamicObject();
        ro->setProperty ("uid", uid);
        juce::File (H + "/link_ready.json").replaceWithText (juce::JSON::toString (juce::var (ro), true));

        // ---- LEG 1: the build arrives while the V2 still holds the rack ----
        if (! waitFile (juce::File (H + "/v2_built.json"), 90000))
        { std::printf ("link side: the v2 side never reported its build\n"); return 2; }
        const bool gotSlots = pumpUntil (*link, [&] { return TA::slots (*link) > 0; }, 20000);
        const double dbBuilt = renderRmsDb (*link, 1.5);
        std::printf ("    rendered RMS, after the build: %+.2f dBFS  (%d slot(s))\n", dbBuilt, TA::slots (*link));
        check (gotSlots, "the Link's OWN rack holds the built slot(s) with NO rack switch - the V2 is still addressing it",
               juce::String (TA::slots (*link)) + " slot(s)");
        check (dbBuilt - dbEmpty >= 6.0,
               "the Link's RENDERED output rose with the +12 dB gain slot, with no rack switch",
               juce::String (dbBuilt - dbEmpty, 2) + " dB (want >= +6)");
        // THE THIRD-PARTY LEG. The ops above are built-ins and would land either way; this asks whether the
        // edit path consulted the shared plugin list at all. 0 here means every non-built-in name in a V2 edit
        // is refused as "not in the loadable plugin list", the batch aborts, the sender keeps it pending, and
        // the rack only changes when the V2 switches away - which is the whole of the in-host report.
        const int seen = TA::numPlugins (*link);
        std::printf ("    loadable entries the Link can see AFTER the command: %d\n", seen);
        check (seen > 0,
               "a v:2 edit is judged against the SHARED plugin list, so a third-party name can resolve on the Link",
               juce::String (seen) + " loadable entr(ies)");

        auto* so = new juce::DynamicObject(); so->setProperty ("dbEmpty", dbEmpty); so->setProperty ("dbBuilt", dbBuilt);
        juce::File (H + "/link_built_seen.json").replaceWithText (juce::JSON::toString (juce::var (so), true));

        // ---- LEG 2: a param change on the same addressed rack ----
        if (! waitFile (juce::File (H + "/v2_param.json"), 90000))
        { std::printf ("link side: the v2 side never reported its param change\n"); return 2; }
        const bool moved = pumpUntil (*link, [&]
        {
            const double d = renderRmsDb (*link, 0.3);
            return std::abs (d - dbBuilt) >= 6.0;
        }, 20000);
        const double dbParam = renderRmsDb (*link, 1.5);
        std::printf ("    rendered RMS, after the param: %+.2f dBFS  (%d slot(s))\n", dbParam, TA::slots (*link));
        check (moved || std::abs (dbParam - dbBuilt) >= 6.0,
               "a PARAM change from the V2 moved the Link's rendered output again, still with no rack switch",
               juce::String (dbParam - dbBuilt, 2) + " dB (want |d| >= 6)");

        auto* vo = new juce::DynamicObject();
        vo->setProperty ("dbEmpty", dbEmpty); vo->setProperty ("dbBuilt", dbBuilt); vo->setProperty ("dbParam", dbParam);
        vo->setProperty ("failures", failures);
        juce::File (H + "/link_done.json").replaceWithText (juce::JSON::toString (juce::var (vo), true));
        std::printf ("\n==== link_live_edit_guard (link side): %s (%d assertion(s) failed) ====\n",
                     failures == 0 ? "GREEN" : "RED", failures);
        rc = failures == 0 ? 0 : 1;
    }
    return rc;
}
