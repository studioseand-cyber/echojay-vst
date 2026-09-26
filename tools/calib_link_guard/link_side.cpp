// calib_link_guard (21t-d, 25 Sep 2026): A REAL LINK-SIDE STEP of the compressor calibration loop.
//
// The question this answers is the one the ruling asked: does the LINK drive the loop once it owns the rack, or
// does the loop stop at handover? It is answered with a real LinkProcessor against the Link's own archive - not a
// stand-in writing sidecar state, which would only prove that a file can be written.
//
// The fixture: a real LinkProcessor with a compressor in its own chain, a calibration loop left on its sidecar by
// "V2" (the state a lease would have left), and real audio pushed through processBlock so the slot's own tallies
// close real 3 s windows. The assertions are that the Link picks the loop up at the step count V2 left, does not
// judge its first window (two hosts, two tally histories), and then MOVES THE DRIVE and writes the state back.
//
// RED before 21t-d's Link half: LinkProcessor does not compile EJCalibLoop.h at all, so the loop stops dead at
// the handover and the sidecar keeps V2's last state for ever.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "LinkProcessor.h"
#include "LinkShm.h"
#include "EJCalibLoop.h"
#include "EedCompressorProcessor.h"   // force-link the built-in's registrar
#include "EedDeviceRegistry.h"
#include <cstdio>

struct EJCalibLinkTestAccess
{
    static juce::String uid  (LinkProcessor& p) { return p.instanceUid_; }
    // 21t-g item 2: the Link starts the loop from the response's block for a rack IT owns, so the guard drives
    // that entry point rather than planting a sidecar - the point is that this side reads the contract.
    static void startFromBlock (LinkProcessor& p, const juce::var& b) { p.startCalibFromBlock (b); }
    // 21t-g item 6a: the lease is the fact that decides who runs the loop, so the guard sets it.
    static void setLeased (LinkProcessor& p, bool on) { p.rackLeaseActive_ = on; }
    static void clearLoop (LinkProcessor& p) { p.calibLoop_ = echojay::CalibLoop{}; }
    static ChainHost&   host (LinkProcessor& p) { return p.chainHost; }
    static echojay::CalibLoop loop (LinkProcessor& p) { return p.calibLoop_; }
    // 21t-e: the tally is where PEAK must come from - the same window as INT, SHORTMAX and HEARD.
    static echojay::LevelTally::Snapshot tally (LinkProcessor& p) { return p.levelTally_.snapshot(); }
};
using TA = EJCalibLinkTestAccess;

namespace {
int failures = 0;
void check (bool ok, const juce::String& w, const juce::String& d = {})
{ std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(), d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : ""); if (! ok) ++failures; }
void pumpMs (int ms)
{ const double end = juce::Time::getMillisecondCounterHiRes() + ms;
  while (juce::Time::getMillisecondCounterHiRes() < end) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false); } }
}

int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::String H = argc > 1 ? argv[1] : "/tmp";
    juce::File (H).createDirectory();
    int rc = 0;
    {
        auto l = std::make_unique<LinkProcessor>();
        l->linkName = "Calib Vocal";
        l->markTypedNameAuthoritative();
        l->prepareToPlay (48000.0, 512);
        for (int i = 0; i < 200 && TA::uid (*l).isEmpty(); ++i) pumpMs (5);
        const juce::String uid = TA::uid (*l);
        check (uid.isNotEmpty(), "link side: the Link claimed a registry slot", uid);
        if (uid.isEmpty()) { std::printf ("==== calib_link_guard (link side): RED ====\n"); return 2; }

        // A compressor in the LINK's own chain - the slot the loop will drive.
        { EedCompressorProcessor force; juce::ignoreUnused (force); }
        const auto* comp = BuiltinDeviceRegistry::instance().findByName ("EchoJay Compressor");
        check (comp != nullptr, "link side: the built-in compressor is registered");
        if (comp == nullptr) { std::printf ("==== calib_link_guard (link side): RED ====\n"); return 2; }
        TA::host (*l).insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*comp), 0);
        check (TA::host (*l).getNumSlots() == 1, "link side: the compressor is in the Link's own rack",
               juce::String (TA::host (*l).getNumSlots()));

        int err0 = 0; const auto dir0 = LinkShm::resolveDir (err0);
        // ---- 21t-g item 2: THIS SIDE READS THE BLOCK -------------------------------------------------------
        // A build to a PARKED Link is measured here, by the process that owns the rack, so the calibration block
        // has to start the loop on this side too - with the same defaults (passive unless it says listen) and the
        // same actuator rules as V2, or the behaviour would depend on which process happened to start it.
        {
            // ONE NAMED var FOR THE BLOCK, not a temporary per call. juce::var(DynamicObject*) TAKES A REFERENCE,
            // so a temporary var destroyed at the end of the statement deletes the object - and the second call
            // then resurrected a freed pointer, which crashed inside var's own dynamic_cast. The same temporary-var
            // rule this repo already has for parsed vars, on the writing side of it.
            auto* b = new juce::DynamicObject();
            b->setProperty ("source", "tally");
            b->setProperty ("heard_s", 120);
            b->setProperty ("measure", "short90");
            b->setProperty ("slot", 1);                       // 1-based on the wire
            b->setProperty ("actuator", "threshold");
            {
                juce::Array<juce::var> params; params.add ("Threshold");
                b->setProperty ("param", params);             // the ARRAY form, one entry
            }
            b->setProperty ("sense", "lower_is_harder");
            b->setProperty ("start_db", -18.0);
            b->setProperty ("min_db", -40.0); b->setProperty ("max_db", 0.0);
            { juce::Array<juce::var> band; band.add (2.0); band.add (3.0); b->setProperty ("gr_target_db", band); }
            // (6a) FIRST: WHILE THE RACK IS LEASED, THIS SIDE STARTS NOTHING. V2 hosts those slots and runs the
            // loop; a second loop here would have two hosts stepping one compressor. Proven on the LEASE, not on
            // an empty rack - the rack is full at this point, so "nothing started" cannot be a coincidence.
            TA::clearLoop (*l);
            TA::setLeased (*l, true);
            const juce::var blockVar (b);
            TA::startFromBlock (*l, blockVar);
            check (! TA::loop (*l).active() && TA::host (*l).getNumSlots() > 0,
                   "21t-g (6a). a block arriving while the rack is LEASED starts NO loop on this side - V2 hosts "
                   "those slots (and the rack is not empty, so this is the lease and not a coincidence)",
                   juce::String (TA::host (*l).getNumSlots()) + " slot(s) in the rack, loop "
                   + (TA::loop (*l).active() ? "STARTED" : "not started"));
            TA::setLeased (*l, false);

            TA::startFromBlock (*l, blockVar);
            const auto started = TA::loop (*l);
            check (started.active() && started.mode == echojay::CalibLoop::Mode::Passive,
                   "link side (21t-g). the block starts a PASSIVE loop on the Link's own rack  (RED as it stood: "
                   "only V2 could start one, and only for a rack it could measure)",
                   started.active() ? juce::String ("active, passive") : juce::String ("not started"));
            check (started.actuator == echojay::CalibLoop::Actuator::Threshold
                   && started.params.size() == 1 && started.params[0] == "Threshold"
                   && started.senseSign == -1 && std::abs (started.value - (-18.0f)) < 0.01f,
                   "link side (21t-g). ...with the knob, its sense and its opening value from the block",
                   started.params.joinIntoString (",") + " @ " + juce::String (started.value, 1)
                   + " sense " + juce::String (started.senseSign));
            check (started.card().isEmpty(),
                   "link side (21t-g). ...and it draws no card, because passive asks the user for nothing",
                   started.card().isEmpty() ? juce::String ("(silent)") : started.card());
            // THE FIXTURE'S COMPRESSOR IS A BUILT-IN, and a built-in has no fingerprint and no param map: the
            // loop's named controls are profiled THIRD-PARTY ones. So the opening write is refused by name here
            // rather than dialled - which is the honest outcome, and it must not be a crash (it was: the hosted
            // path's dynamic_cast on a built-in, in a harness that carries the class twice).
            check (TA::host (*l).setSlotControlsToValue (0, juce::StringArray { "Threshold" }, -18.0f) == 0,
                   "link side (21t-g). a named control on a BUILT-IN slot is refused, not written - and not a "
                   "crash on the hosted path's cast");
            const auto rcRead = LinkShm::readRackSidecar (dir0, uid);
            const auto side = echojay::CalibLoop::fromVar (rcRead.calib);
            check (side.mode == echojay::CalibLoop::Mode::Passive
                   && side.actuator == echojay::CalibLoop::Actuator::Threshold,
                   "link side (21t-g). ...and the sidecar says so from the first window, so V2 can close it",
                   juce::String ("valid ") + (rcRead.valid ? "y" : "n") + ", calib "
                   + juce::JSON::toString (rcRead.calib, true).substring (0, 160).replace ("\n", " "));
        }

        // THE STATE V2 LEFT: a loop two steps in, at +2 dB of drive, band 2-3 dB.
        int err = 0; const auto dir = LinkShm::resolveDir (err);
        echojay::CalibLoop left;
        left.begin ("EchoJay Compressor", 0, 2.0f, 3.0f, 2.0f);
        left.steps = 2; left.window = 5; left.lastGr = 0.4f; left.awaitFresh = false;
        auto rcSide = LinkShm::readRackSidecar (dir, uid);
        if (! rcSide.valid) { rcSide.uid = uid; rcSide.valid = true; rcSide.revision = 1; }
        rcSide.calib = left.toVar();
        LinkShm::writeRackSidecar (dir, rcSide);
        check (! LinkShm::readRackSidecar (dir, uid).calib.isVoid(),
               "link side: the sidecar carries the loop V2 left behind");

        // REAL AUDIO through the Link, so the slot's own tallies close real windows.
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        juce::Random rng (99);
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        bool moved = false; int seenSteps = left.steps;
        while (juce::Time::getMillisecondCounterHiRes() - t0 < 14000.0)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < buf.getNumSamples(); ++i)
                    buf.setSample (ch, i, 0.18f * (rng.nextFloat() * 2.0f - 1.0f));
            l->processBlock (buf, midi);
            pumpMs (10);
            const auto now = TA::loop (*l);
            if (now.steps > seenSteps) { moved = true; seenSteps = now.steps; }
        }

        const auto after = TA::loop (*l);
        check (after.active(),
               "link side: the LINK picked the loop up from the sidecar  (RED as it stood: LinkProcessor does not "
               "compile the loop at all, so a handover ended it)", juce::String ((int) after.state));
        check (after.plugin == "EchoJay Compressor" && after.slot == 0,
               "link side: ...the same slot V2 was working on", after.plugin + " slot " + juce::String (after.slot));
        check (after.steps >= 2,
               "link side: ...at the step count V2 left, not from zero  (a handover continues the loop)",
               juce::String (after.steps) + " step(s)");
        check (after.window > left.window,
               "link side: ...and it has judged windows of its own since", juce::String (after.window));
        check (moved || after.steps > 2 || std::abs (after.preDb - 2.0f) > 0.01f,
               "link side: ...and the LINK moved the drive itself",
               juce::String (after.preDb, 1) + " dB after " + juce::String (after.steps) + " step(s)");
        // One more pump before reading the file: the Link's own 30 Hz timer may be mid-write when the loop is
        // sampled in memory, and a one-window skew there would be a race in the GUARD, not a defect.
        pumpMs (250);
        const auto afterSettle = TA::loop (*l);
        {   // 21t-e: PEAK IN THE FRAME IS THE TALLY'S TRUE PEAK, on the same clock as INT/SHORTMAX/HEARD - not
            // meterEngine_'s max-hold, which is only cleared when the RACK changes and on 25 Sep published
            // -0.1 dBTP beside an INT of -17.0 on two members.
            // SETTLE FIRST: the tally's true peak only ever rises, and the guard reads it live while publishes
            // land up to 100 ms apart - so a live comparison is a race, not a measurement. The audio has stopped
            // by here; one publish period is enough for the frame to carry the settled figure.
            pumpMs (400);
            int e3 = 0, fd3 = -1;
            const auto dir3 = LinkShm::resolveDir (e3);
            void* reg3 = LinkShm::openRegistry (dir3, fd3, e3);
            const auto tal = TA::tally (*l);
            LinkMeterFrame pub;
            int slotIdx = -1;
            if (reg3 != nullptr)
                for (int i = 0; i < 64 && slotIdx < 0; ++i)
                { LinkMeterFrame f2; if (LinkShm::readMeterFrame (reg3, i, f2) && f2.heardSeconds > 0.0f) { pub = f2; slotIdx = i; } }
            if (slotIdx >= 0 && tal.truePeakDb > -190.0f)
                check (std::abs (pub.truePeakMax - tal.truePeakDb) < 0.2f,
                       "link side: the published PEAK is the TALLY's true peak, not the meter engine's hold",
                       juce::String (pub.truePeakMax, 1) + " vs tally " + juce::String (tal.truePeakDb, 1));
            else
                std::printf ("  note  no published frame with a tally reading yet - PEAK source not asserted this run\n");
        }
        const auto onDisk = echojay::CalibLoop::fromVar (LinkShm::readRackSidecar (dir, uid).calib);
        // WHAT A HANDOVER NEEDS is the drive, the step count and the band - those must match exactly. The window
        // ORDINAL is a log counter and the Link is still ticking while the file is read, so it is allowed to be
        // one behind; anything more would mean writes are being dropped.
        check (std::abs (onDisk.preDb - afterSettle.preDb) < 0.01f && onDisk.steps == afterSettle.steps
               && std::abs (onDisk.lo - afterSettle.lo) < 0.01f && std::abs (onDisk.hi - afterSettle.hi) < 0.01f,
               "link side: ...and wrote the SAME state back to the sidecar, which is what V2 renders",
               juce::String (onDisk.preDb, 1) + " dB, " + juce::String (onDisk.steps) + " step(s), band "
               + juce::String (onDisk.lo, 1) + "-" + juce::String (onDisk.hi, 1));
        check (afterSettle.window - onDisk.window <= 1 && onDisk.window >= left.window,
               "link side: ...within one window of the loop as it stands, so no write is being dropped",
               juce::String (onDisk.window) + " on disk vs " + juce::String (afterSettle.window) + " in memory");
    }
    std::printf ("==== calib_link_guard (link side): %s ====\n", failures == 0 ? "GREEN" : "RED");
    rc = failures == 0 ? 0 : 1;
    return rc;
}
