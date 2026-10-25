// shadow_live_repro (24 Oct 2026): ONE question, asked on the bench, about a plugin the rig is
// normally forbidden to load.
//
// WHY THIS EXISTS. His 12:14 crash (AUHostingServiceXPC_arrow-2026-10-24-121523.ips) died on the
// AUOOPRenderingServer thread, nineteen seconds after the live view started:
//
//     cl1bST_VST_AU_Protect  (the Tube-Tech's own code, SIGSEGV at 0x0)
//     AudioToolboxCore       AudioUnitRender
//     EchoJay V2             juce::AudioUnitPluginInstanceHeadless::processAudio
//     EchoJay V2             EchoJayProcessor::processBlock
//
// That is the live view's copy being rendered by EJShadowView::pump. Nothing was in flight on the
// message thread this time (thread 0 was in LinkShm::resolveDir), so the 08:58 rule - no render
// while we are inside a plugin call - could not have stopped it, and neither could the editor
// ordering: the copy was the one with the editor on it, which is what that fix was for.
//
// So the question is what is DIFFERENT about our render, and the honest way to answer it is to vary
// one thing at a time against the real plugin. Sean granted a one-off supervised exception to the
// rig rule for exactly this (the rig never loads iLok or PACE plugins unsupervised), so every leg
// here is one process, named, bounded and reported, and the runner prints what the operating system
// did to it.
//
// WHAT THIS IS NOT. It cannot render the copy from inside another AU's render callback, which is
// where the product's pump actually runs (Logic's render thread, nested inside JuceAU::Render). If
// every leg below survives, that nesting is the next suspect and the report says so rather than
// declaring the plugin innocent.
//
//   shadow_live_repro <leg> [seconds] ["<name>"] ["<AudioUnit:identifier>"] [uidHex]
//
// Legs, each a separate process:
//   product    what the V2 does now: plain stereo asked for (every other bus DISABLED),
//              prepareToPlay(48000, 512), the copy's own editor created and shown, rendered from a
//              background thread at real time.
//   noeditor   the same copy, rendered the same way, with NO editor. Isolates the open interface.
//   keyfed     the plugin's OWN layout kept and every channel fed, which is what ChainHost does for
//              this plugin ("input bus 1 (sidechain) fed from main"). Isolates the disabled key bus.
//   bigblock   the product shape, then ONE block larger than the prepared block. Isolates
//              kAudioUnitProperty_MaximumFramesPerSlice.
//   playhead   the product shape with a playhead that supplies host time and a sample position.
//              Isolates the missing playhead (our copy has none, so every block says host time 0).
//   selfkey    THE FIX, exactly: the layout left alone, the pair in channels 0/1, every enabled
//              non-main input bus self-keyed from it by echojay::selfKeyCopyPlan - the same call
//              EJShadowView::start makes. This is the leg that has to survive.
//   selfkey_big  the fix, then ONE block larger than the prepared block, so the maximum-frames
//              question is asked again without the crashing layout confusing the answer.
//
// ---- HIS 14:27 CRASH, 24 Oct: THE TEARDOWN, NOT THE RENDER ---------------------------------------
// The fourth live-view crash died on the MAIN thread with no EchoJay frame on it at all: Softube's
// own code, under CoreFoundation's notification callout, reading null+0x17. The condition was laid
// 108 s earlier, when the V2's window closed: the live copy was DISPOSED (ShadowView::stop ended in
// dying.reset()) while its Cocoa view was still parented in the panel, so the AU was freed first
// and its editor torn down afterwards against freed memory. Sean's ruling of 6 Oct already forbids
// the first half of that - *"a hosted third-party AU is never disposed mid-session (the 18:32
// Softube observer crash)"* - and the borrow pool has obeyed it since; the live view never did.
//
//   teardown       the product's ordering: a copy rendering with its editor open, then the INSTANCE
//                  freed while the editor still lives, then the editor, then a SECOND instance with
//                  its own view while window notifications fly. This is the leg that has to crash.
//   persist        the mechanism his 22:08 settings were lost to: an older state applied over a
//                  newer change destroys it. No licence needed, so it runs unsupervised.
//   control        the whole of his 25 Oct instruction, on two LIVE instances: a knob moved in the
//                  live view goes to the plugin on the LINK, the V2's copy never sees it, and the
//                  close-apply is then asked - through the product's own BorrowCommit::classify -
//                  whether to write that copy back. Run twice, for a picker-added slot and for a
//                  slot from the Link's own rack. The old rule wipes the change; the new one keeps
//                  it. No licence needed, so it runs unsupervised on AUDelay.
//   followparams   FOLLOW layer 2's three open questions, on the real plugin: does the guest view
//                  REDRAW when the host writes a parameter (his eye answers that one), what a full
//                  getValue() sweep costs, and whether writing while the copy renders is safe.
//   teardown_safe  the fix's ordering: the EDITOR first, then the instance KEPT (never disposed),
//                  then the same second instance and the same notifications. Has to survive.
//
// Exit 0 = it survived the whole duration. Any signal = the leg reproduced a crash, and the runner
// reports which leg and how many seconds in.
#include <CoreFoundation/CoreFoundation.h>   // before JUCE: MacTypes Point
#include <JuceHeader.h>
#include "EJShadowChannels.h"   // the FIX's own rule, so the leg and the product share one author
#include "EJShadowValues.h"     // the drag leg: shadowApplyChangedValues, the product's own apply loop
#include "LinkShm.h"            // the control leg asks the PRODUCT's own commit rule, not a copy of it
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cmath>              // std::fmod, the followparams sweep

namespace
{
// The defaults are his crash's plugin and the rate his session ran at.
constexpr double kRate       = 48000.0;
constexpr int    kBlock      = 512;
constexpr int    kBigBlock   = 1024;
constexpr int    kBufFrames  = 8192;      // EJShadowView::kBufFrames, the same capacity
constexpr float  kFeedPeak   = 0.126f;    // about -18 dBFS, so a compressor actually works

void say (const juce::String& leg, const juce::String& line)
{
    std::printf ("[%s] %s\n", leg.toRawUTF8(), line.toRawUTF8());
    std::fflush (stdout);
}

/** A playhead that answers, for the one leg that asks whether the missing one matters.

    The product's copy has NO playhead, so AudioUnitPluginInstanceHeadless::processAudio clears
    kAudioTimeStampHostTimeValid and renders every block with a host time of zero. This supplies a
    monotonic host time and sample position instead, which is what a host would. */
struct TickingPlayHead final : public juce::AudioPlayHead
{
    std::atomic<juce::int64> samples { 0 };

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        const auto s = samples.load (std::memory_order_relaxed);
        p.setTimeInSamples (s);
        p.setTimeInSeconds ((double) s / kRate);
        p.setHostTimeNs ((juce::uint64) (juce::Time::getHighResolutionTicks()
                                         * (1.0e9 / (double) juce::Time::getHighResolutionTicksPerSecond())));
        p.setBpm (120.0);
        p.setIsPlaying (true);
        return p;
    }
};

/** The render loop, on its own thread, at real time - the product renders this copy from the host's
    audio thread, so a leg that rendered as fast as it could would be testing something else. */
struct Renderer final : public juce::Thread
{
    Renderer (juce::AudioPluginInstance& i, const juce::String& legName, int channels,
              int blockFrames, int bigAfterBlocks, TickingPlayHead* ph,
              std::vector<echojay::ShadowChannelCopy> keyPlan = {})
        : juce::Thread ("repro render"), inst (i), leg (legName), block (blockFrames),
          bigAfter (bigAfterBlocks), playHead (ph), plan (std::move (keyPlan))
    {
        // Sized exactly as EJShadowView::start sizes it: from the copy's own channel counts, never
        // narrower than a stereo pair, and the capacity the pump uses.
        buf.setSize (channels, kBufFrames);
        buf.clear();
    }

    void run() override
    {
        juce::Random rng (1234);
        juce::MidiBuffer noMidi;
        const double perBlockMs = 1000.0 * (double) block / kRate;
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        double nextDue = t0;

        while (! threadShouldExit())
        {
            const int n = (bigAfter > 0 && blocks.load() == bigAfter) ? kBigBlock : block;
            if (n == kBigBlock)
                say (leg, "the next block is " + juce::String (n) + " frames, and the copy was "
                          "prepared for " + juce::String (block)
                        + " - this is the maximum-frames-per-slice leg");

            // The ring carries one stereo pair; every channel beyond it is silence, never
            // leftovers. The same rule as pump(), for the same reason.
            for (int ch = 0; ch < buf.getNumChannels(); ++ch)
                if (ch < 2)
                    for (int i = 0; i < n; ++i)
                        buf.setSample (ch, i, kFeedPeak * (rng.nextFloat() * 2.0f - 1.0f));
                else
                    buf.clear (ch, 0, n);

            // The fix's own copy, in the fix's own order: the pair, silence everywhere else, then
            // every planned key channel fed from the pair.
            for (const auto& c : plan)
                buf.copyFrom (c.destChannel, 0, buf, c.srcChannel, 0, n);

            juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), buf.getNumChannels(), 0, n);
            inst.processBlock (view, noMidi);
            if (playHead != nullptr) playHead->samples.fetch_add (n);
            blocks.fetch_add (1);
            framesDone.fetch_add (n);

            nextDue += perBlockMs;
            const auto now = juce::Time::getMillisecondCounterHiRes();
            if (nextDue > now) juce::Thread::sleep ((int) (nextDue - now));
            else               nextDue = now;        // never run a backlog faster than real time
        }
    }

    juce::AudioPluginInstance& inst;
    juce::String leg;
    juce::AudioBuffer<float> buf;
    int block, bigAfter;
    TickingPlayHead* playHead;
    std::vector<echojay::ShadowChannelCopy> plan;
    std::atomic<int> blocks { 0 };
    std::atomic<juce::int64> framesDone { 0 };
};

/** A never-freed store, the same discipline ChainHost::keepHostedNodeForever uses and for the same
    reason: a function-local static VECTOR would be destroyed during process exit and would dispose
    the plugin there instead, which is the UAD-2-shaped crash under __cxa_finalize_ranges already on
    file. A static POINTER has a trivial destructor, so what it holds outlives the process. */
void keepForeverOnTheBench (std::unique_ptr<juce::AudioPluginInstance> inst)
{
    static std::vector<std::unique_ptr<juce::AudioPluginInstance>>* const store
        = new std::vector<std::unique_ptr<juce::AudioPluginInstance>>();
    store->push_back (std::move (inst));
}

struct EditorWindow final : public juce::DocumentWindow
{
    EditorWindow (juce::AudioProcessorEditor* ed)
        : juce::DocumentWindow ("live copy", juce::Colours::black, 0)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (ed, true);
        centreWithSize (ed->getWidth(), ed->getHeight());
        setVisible (true);
    }
    void closeButtonPressed() override {}
};
/** Instantiate one, synchronously, with the same bound and the same refusals as main's. */
std::unique_ptr<juce::AudioPluginInstance> makeOne (juce::AudioPluginFormatManager& fm,
                                                   const juce::PluginDescription& d,
                                                   const juce::String& leg,
                                                   const juce::String& which)
{
    std::unique_ptr<juce::AudioPluginInstance> inst;
    juce::String err;
    bool done = false;
    fm.createPluginInstanceAsync (d, kRate, kBlock,
        [&] (std::unique_ptr<juce::AudioPluginInstance> p, const juce::String& e)
        { inst = std::move (p); err = e; done = true; });

    const double t0 = juce::Time::getMillisecondCounterHiRes();
    while (! done && juce::Time::getMillisecondCounterHiRes() - t0 < 120000.0)
    {
        juce::Timer::callPendingTimersSynchronously();
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.05, false);
    }
    if (! done)          { say (leg, "REFUSED: no callback in 120 s for " + which + " (a licence prompt?)"); return {}; }
    if (inst == nullptr) { say (leg, "REFUSED: " + which + ": " + err.replace ("\n", " ")); return {}; }
    say (leg, which + " instantiated: " + juce::String (inst->getTotalNumInputChannels()) + " in / "
            + juce::String (inst->getTotalNumOutputChannels()) + " out");
    return inst;
}

/** Run the message loop for `ms`, and MOVE AND RESIZE the window while doing it.

    This is the 14:27:44 trigger, and it is not decoration: the 18:32 crash on file was posted by
    *"the NLS Buss popout's window frame change"*, and this one came in through ViewBridge and
    AppKit while a window was being laid out. A stale observer is only a crash when something
    posts to it, so the leg has to post. */
void churnWindows (const juce::String& leg, juce::DocumentWindow* w, int ms)
{
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    int n = 0;
    while (juce::Time::getMillisecondCounterHiRes() - t0 < (double) ms)
    {
        juce::Timer::callPendingTimersSynchronously();
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false);
        if (w != nullptr && (n % 5) == 0)
        {
            const auto b = w->getBounds();
            w->setBounds (b.translated ((n % 10) == 0 ? 7 : -7, (n % 10) == 0 ? 5 : -5));
        }
        // A window of our own, opened and closed, so NSWindow notifications fly even when the
        // plugin's own view is quiet.
        if ((n % 25) == 0)
        {
            juce::DocumentWindow extra ("churn", juce::Colours::darkgrey, 0);
            extra.centreWithSize (220, 120);
            extra.setVisible (true);
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false);
            extra.setVisible (false);
        }
        ++n;
    }
}

/** ---- THE MECHANISM HIS SETTINGS WERE LOST TO, ON A REAL PLUGIN (25 Oct 2026) --------------

    His 22:08 loss: the borrow session's close-apply committed its UNSEEDED mirror's state - plain
    defaults - onto the Link, over a Threshold he had just heard move. The fix is a rule about when
    a state may be written (seeded from the Link, or born in this V2 session; never neither), and
    its guards are pure-function legs.

    What a guard CANNOT show is that the premise is real: that applying an older state over a newer
    change actually destroys the change on a real hosted plugin, rather than being merged, ignored
    or partially applied. That is a claim about somebody else's code, and this leg asks it.

    NO PACE, NO iLOK, so it runs unsupervised overnight: the default here is Apple's AUDelay, which
    is the one Apple-made effect in this machine's scanned list. The CL 1B's own proof waits for
    Sean in the morning, under the supervised exception.

    No editor is needed or asked for, so a plugin with no custom view is a valid subject.

      1. read state A                      - what the plugin says it is
      2. move a parameter, read it back     - a change we know landed
      3. read state B, assert B != A        - the plugin really serialises the change
      4. re-apply A, assert the knob WENT BACK   <- the loss, reproduced
      5. re-apply B, assert the knob returns     <- and it is a true round trip, not a reset        */
int runPersistLeg (const juce::String& leg, juce::AudioPluginFormatManager& fm,
                   const juce::PluginDescription& d, int seconds)
{
    auto inst = makeOne (fm, d, leg, "the subject");
    if (inst == nullptr) return 3;

    const auto plan = echojay::selfKeyCopyPlan (echojay::shadowInputBusesOf (*inst));
    inst->prepareToPlay (kRate, kBlock);

    auto& params = inst->getParameters();
    if (params.isEmpty()) { say (leg, "REFUSED: this plugin reports no parameters"); return 3; }

    // A parameter that is not already at an end, so a move in either direction is visible.
    int which = -1;
    for (int i = 0; i < params.size(); ++i)
        if (params[i]->getName (64).isNotEmpty()) { which = i; break; }
    auto* prm = params[which];
    say (leg, juce::String (params.size()) + " parameter(s); driving " + juce::String (which)
            + " \"" + prm->getName (64) + "\", now " + juce::String (prm->getValue(), 4));

    const int channels = juce::jmax (2, inst->getTotalNumInputChannels(),
                                        inst->getTotalNumOutputChannels());
    Renderer r (*inst, leg, channels, kBlock, 0, nullptr, plan);
    r.startThread (juce::Thread::Priority::highest);
    say (leg, "rendering " + juce::String (channels) + " channel(s) - every state call below happens "
              "with the plugin live, which is the product's condition");

    auto settle = [&] (int ms)
    {
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        while (juce::Time::getMillisecondCounterHiRes() - t0 < (double) ms)
        {
            juce::Timer::callPendingTimersSynchronously();
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false);
        }
    };

    juce::MemoryBlock A, B;
    const float v0 = prm->getValue();
    inst->getStateInformation (A);
    settle (200);
    say (leg, "state A is " + juce::String ((juce::int64) A.getSize()) + " bytes at value "
            + juce::String (v0, 4));

    const float v1 = v0 < 0.5f ? 0.80f : 0.20f;
    prm->setValueNotifyingHost (v1);
    settle (300);
    const float back1 = prm->getValue();
    const bool moved = std::abs (back1 - v0) > 0.05f;
    say (leg, juce::String (moved ? "PASS" : "FAIL") + ": the parameter MOVED to "
            + juce::String (back1, 4) + " (asked " + juce::String (v1, 2) + ")");

    inst->getStateInformation (B);
    settle (200);
    const bool differs = B.getSize() != A.getSize()
                      || std::memcmp (A.getData(), B.getData(), A.getSize()) != 0;
    say (leg, juce::String (differs ? "PASS" : "FAIL")
            + ": state B (" + juce::String ((juce::int64) B.getSize()) + " bytes) DIFFERS from A, so "
              "this plugin really serialises the change - without that, a stale commit could not "
              "destroy anything and the rule would be guarding nothing");

    // ---- THE LOSS, REPRODUCED -------------------------------------------------------------
    inst->setStateInformation (A.getData(), (int) A.getSize());
    settle (400);
    const float afterStale = prm->getValue();
    const bool lost = std::abs (afterStale - v0) < 0.05f && std::abs (afterStale - back1) > 0.05f;
    say (leg, juce::String (lost ? "PASS" : "FAIL")
            + ": applying the OLDER state A put the parameter back to " + juce::String (afterStale, 4)
            + " - the change was DESTROYED, which is exactly what the borrow commit did to his "
              "Threshold at 22:09:05. The premise of the rule is real on a live plugin");

    inst->setStateInformation (B.getData(), (int) B.getSize());
    settle (400);
    const float afterNewer = prm->getValue();
    const bool restored = std::abs (afterNewer - back1) < 0.05f;
    say (leg, juce::String (restored ? "PASS" : "FAIL")
            + ": and re-applying the NEWER state B brought it back to " + juce::String (afterNewer, 4)
            + " - a true round trip, so the loss above was the stale WRITE and not a reset");

    if (seconds > 4) settle ((seconds - 4) * 1000);
    r.stopThread (2000);
    inst->releaseResources();
    say (leg, "SURVIVED, " + juce::String (r.blocks.load()) + " blocks rendered under "
            + juce::String (4) + " state calls");
    keepForeverOnTheBench (std::move (inst));
    return (moved && differs && lost && restored) ? 0 : 4;
}

/** ---- FOLLOW LAYER 2'S THREE OPEN QUESTIONS, ASKED ON THE REAL PLUGIN (24 Oct 2026) ---------

    Layer 1 shipped and passed: the copy opens on the Link's real settings. Layer 2 is the
    continuous mirror - the Link publishes the tapped slot's parameter values at 30 Hz and the V2
    writes the changed ones into the copy. Three things about that are CLAIMS until this plugin
    answers them, and the design note says so in as many words:

      1. DOES A GUEST AU'S OWN VIEW REDRAW when the host writes its parameter? If it does not, the
         mirror still fixes the copy's audio and its meters, and the knobs in the picture stay
         where they were - which would be a different promise to Sean and he must hear it before
         the round, not after.
      2. WHAT DOES A FULL PARAMETER SWEEP COST? The Link would call getValue() for every parameter
         of the tapped slot, thirty times a second, on its message thread. Nobody has ever measured
         it on a real plugin. A number here sizes the publish; a guess here is how you ship a stall.
      3. IS WRITING A PARAMETER WHILE THE COPY RENDERS SAFE on this plugin? The product writes on
         the message thread while the audio thread is inside the same instance. That is the shape
         of the 08:58 crash (a plugin call with a render underneath it), and if it bites here then
         layer 2 needs the PluginCallWindow around its writes - a design change, not a tweak.

    SO THE EYE IS PART OF THE INSTRUMENT, and that is why this leg asks for him. Questions 2 and 3
    this harness answers by itself and asserts; question 1 is a picture and only he can see it, so
    the leg names the parameter it is driving, drives it slowly and visibly, and says what to look
    for. EJ_FOLLOW_PARAM=<index> picks a different one without a rebuild. */
int runFollowParamsLeg (const juce::String& leg, juce::AudioPluginFormatManager& fm,
                        const juce::PluginDescription& d, int seconds)
{
    auto inst = makeOne (fm, d, leg, "the live copy");
    if (inst == nullptr) return 3;

    // THE FIX'S SHAPE, always: the plugin's own layout with every enabled non-main input bus
    // self-keyed from the main pair. A narrowed layout is his 12:14 crash and must never be the
    // ground a NEW question is asked on.
    const auto plan = echojay::selfKeyCopyPlan (echojay::shadowInputBusesOf (*inst));
    inst->prepareToPlay (kRate, kBlock);
    say (leg, "prepared at " + juce::String ((int) kRate) + " Hz, " + juce::String (kBlock)
            + " frames, own layout, " + juce::String ((int) plan.size()) + " self-key copy(ies)");

    // ---- QUESTION 2: WHAT A FULL SWEEP COSTS ---------------------------------------------------
    auto& params = inst->getParameters();
    const int n = params.size();
    if (n == 0) { say (leg, "REFUSED: this instance reports no parameters at all"); return 3; }

    const int sweeps = 200;
    const auto s0 = juce::Time::getMillisecondCounterHiRes();
    double sink = 0.0;
    for (int k = 0; k < sweeps; ++k)
        for (auto* prm : params) sink += (double) prm->getValue();
    const auto sweepMs = (juce::Time::getMillisecondCounterHiRes() - s0) / (double) sweeps;
    say (leg, juce::String (n) + " parameters; ONE FULL SWEEP of getValue() = "
            + juce::String (sweepMs, 3) + " ms"
            + " -> at 30 Hz that is " + juce::String (sweepMs * 30.0, 2)
            + " ms of every 1000 ms on the Link's message thread ("
            + juce::String (sweepMs * 30.0 / 10.0, 2) + "% of it)"
            + (sink > 1.0e9 ? " " : ""));            // sink is only here to defeat the optimiser

    // ---- WHICH PARAMETER DOES HE WATCH? --------------------------------------------------------
    // A name he will recognise on the plugin's own panel, preferred in this order; anything else
    // falls back to the first parameter with a name. An index in EJ_FOLLOW_PARAM overrides.
    const char* preferred[] = { "Input Gain", "Output Gain", "Threshold", "Ratio", "Attack",
                                "Release", "Gain", "Input", "Output" };
    int which = -1;
    if (auto* env = std::getenv ("EJ_FOLLOW_PARAM"))
    {
        which = juce::jlimit (0, n - 1, juce::String::fromUTF8 (env).getIntValue());
        say (leg, "EJ_FOLLOW_PARAM picked parameter " + juce::String (which));
    }
    for (const auto* want : preferred)
    {
        if (which >= 0) break;
        for (int i = 0; i < n; ++i)
            if (params[i]->getName (64).equalsIgnoreCase (juce::String::fromUTF8 (want)))
            { which = i; break; }
    }
    for (int i = 0; which < 0 && i < n; ++i)
        if (params[i]->getName (64).isNotEmpty()) which = i;
    if (which < 0) which = 0;

    juce::String firstFew;
    for (int i = 0; i < juce::jmin (n, 16); ++i)
        firstFew << (i == 0 ? "" : ", ") << i << "=" << params[i]->getName (24);
    say (leg, "its parameters: " + firstFew + (n > 16 ? ", ..." : ""));

    auto* driven = params[which];
    const auto drivenName = driven->getName (64);
    const float startValue = driven->getValue();
    say (leg, "DRIVING parameter " + juce::String (which) + " \"" + drivenName
            + "\", which reads \"" + driven->getCurrentValueAsText() + "\" ("
            + juce::String (startValue, 4) + " normalised) right now");

    // ---- THE EDITOR, AND THE RENDER UNDERNEATH IT ----------------------------------------------
    std::unique_ptr<EditorWindow> win;
    if (inst->hasEditor())
        if (auto* ed = inst->createEditorIfNeeded())
        {
            win = std::make_unique<EditorWindow> (ed);
            say (leg, "the copy's own editor is open (" + juce::String (ed->getWidth()) + "x"
                    + juce::String (ed->getHeight()) + ")");
        }
    if (win == nullptr) { say (leg, "REFUSED: no editor, so question 1 cannot be asked"); return 3; }

    const int channels = juce::jmax (2, inst->getTotalNumInputChannels(),
                                        inst->getTotalNumOutputChannels());
    Renderer r (*inst, leg, channels, kBlock, 0, nullptr, plan);
    r.startThread (juce::Thread::Priority::highest);
    say (leg, "rendering " + juce::String (channels) + " channel(s) at real time, as the product does");

    say (leg, "");
    say (leg, "#### WATCH THE PLUGIN'S WINDOW. \"" + drivenName + "\" is being swept slowly from "
              "end to end by the HOST, exactly as the Link's values would drive it. Does that "
              "control MOVE on the plugin's own panel?  (A still control means layer 2 mirrors the "
              "audio and the meters but not the picture - which is a promise I would have to "
              "correct before building it.)");
    say (leg, "");

    // ---- THE SWEEP: THE MESSAGE THREAD WRITES WHILE THE RENDER THREAD IS INSIDE THE PLUGIN -----
    // 10 writes a second, a triangle from end to end, so it is unmistakable to the eye and slow
    // enough that a lazy repaint still has time to show.
    const auto tRun = juce::Time::getMillisecondCounterHiRes();
    const double periodMs = (double) seconds * 1000.0 / 2.0;       // one up-and-down over the leg
    int    writes = 0, lastSaid = -1;
    double nextWriteAt = tRun;
    float  minSeen = 2.0f, maxSeen = -1.0f;

    while (juce::Time::getMillisecondCounterHiRes() - tRun < seconds * 1000.0)
    {
        juce::Timer::callPendingTimersSynchronously();
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false);

        const auto now = juce::Time::getMillisecondCounterHiRes();
        if (now >= nextWriteAt)
        {
            nextWriteAt = now + 100.0;
            const double phase = std::fmod (now - tRun, periodMs) / periodMs;        // 0..1
            const float  want  = (float) (phase < 0.5 ? phase * 2.0 : 2.0 - phase * 2.0);
            driven->setValueNotifyingHost (want);
            ++writes;
            const float back = driven->getValue();
            minSeen = juce::jmin (minSeen, back);
            maxSeen = juce::jmax (maxSeen, back);
        }

        const int sec = (int) ((now - tRun) / 1000.0);
        if (sec != lastSaid)
        {
            lastSaid = sec;
            say (leg, "t=" + juce::String (sec) + "s blocks=" + juce::String (r.blocks.load())
                    + " writes=" + juce::String (writes)
                    + " \"" + drivenName + "\"=" + juce::String (driven->getValue(), 4)
                    + " (\"" + driven->getCurrentValueAsText() + "\")");
        }
    }

    r.stopThread (2000);

    // ---- WHAT THIS HARNESS CAN ANSWER BY ITSELF ------------------------------------------------
    const bool landed = (maxSeen - minSeen) > 0.25f;
    say (leg, juce::String (landed ? "PASS" : "FAIL")
            + ": the host's writes LANDED on the plugin - \"" + drivenName + "\" read back across "
            + juce::String (minSeen, 3) + " to " + juce::String (maxSeen, 3)
            + " over " + juce::String (writes) + " writes"
            + (landed ? "" : " (a parameter that does not move cannot be mirrored, and layer 2 "
                             "would have to say so for this plugin)"));
    say (leg, "PASS: writing a parameter on the message thread while the copy renders did not take "
              "this process down (" + juce::String (r.blocks.load()) + " blocks rendered under it)");

    // THE SAFE TEARDOWN, which is now the house's: the EDITOR first, then the instance is KEPT.
    win.reset();
    inst->releaseResources();
    say (leg, "SURVIVED " + juce::String (seconds) + " s");
    keepForeverOnTheBench (std::move (inst));
    return landed ? 0 : 4;
}

/** HIS 25 OCT INSTRUCTION, ON TWO LIVE INSTANCES OF A REAL PLUGIN.

        *"move a knob in the live view, close the V2, reopen the Link, the change is there; the same
        for a picker-added slot and for a slot from the Link's own rack."*

    Two instances, because that is what the product has and it is the whole of the fault:

      * L stands for the plugin on the LINK - the one he hears, the one control reaches.
      * V stands for the V2's BORROWED MIRROR of that slot - the instance whose state the
        close-apply commits. It is NOT the live view's copy: the live view has a third instance,
        seeded from the Link, and control never writes to either of them. That is why committing V
        wiped a Threshold he had just heard move.

    The commit decision is not modelled here: the leg calls the product's own
    LinkShm::BorrowCommit::classify, so a leg that passes while the shipped rule changes is not
    possible. The old rule is called with its own old arguments, which is the known-bad direction. */
// ---- THE DRAG: A SMOOTH, ONE-DIRECTION SEQUENCE IN THE COPY (his item 1a, 26 Oct 2026) -----
//
//  *"turning a knob by hand isn't smooth, it jumps."*  And his own reading: the gesture goes to the
//  Link, the Link republishes at 30 Hz, and the 20 Hz follow tick writes the Link's slightly older
//  value back into the copy mid-drag.
//
//  This drives that exact race on two LIVE, RENDERING instances of a non-PACE AU. V is the copy his
//  hand is on; L is the plugin on the Link. Each step of the drag moves V and sends the value to L,
//  as the ring does; between steps a FOLLOW tick applies L's value AS PUBLISHED TWO FRAMES AGO back
//  into V, which is what the real 30 Hz publish against a 20 Hz read amounts to.
//
//  Run twice on the same inputs: once with the product's hold (echojay::followMayWriteParam through
//  shadowApplyChangedValues' heldByGesture) and once without it. The verdict is the SHAPE of the
//  copy's value sequence - monotonic with the hold, and stepping backwards without it. The second
//  half is the known-bad, so this leg cannot pass by accident.
int runDragLeg (const juce::String& leg, juce::AudioPluginFormatManager& fm,
                const juce::PluginDescription& d, int seconds)
{
    juce::ignoreUnused (seconds);
    auto L = makeOne (fm, d, leg, "L: the plugin on the Link (the one he hears)");
    if (L == nullptr) return 3;
    auto V = makeOne (fm, d, leg, "V: the V2's live-view copy (the one his hand is on)");
    if (V == nullptr) return 3;
    L->prepareToPlay (kRate, kBlock);
    V->prepareToPlay (kRate, kBlock);
    auto& pL = L->getParameters();
    auto& pV = V->getParameters();
    if (pL.isEmpty() || pV.isEmpty()) { say (leg, "REFUSED: this plugin reports no parameters"); return 3; }
    int which = -1;
    for (int i = 0; i < pV.size(); ++i)
        if (pV[i]->getName (64).isNotEmpty()) { which = i; break; }
    say (leg, juce::String (pV.size()) + " parameter(s); dragging " + juce::String (which) + " \""
            + pV[which]->getName (64) + "\"");

    const int chL = juce::jmax (2, L->getTotalNumInputChannels(), L->getTotalNumOutputChannels());
    const int chV = juce::jmax (2, V->getTotalNumInputChannels(), V->getTotalNumOutputChannels());
    Renderer rL (*L, leg, chL, kBlock, 0, nullptr,
                 echojay::selfKeyCopyPlan (echojay::shadowInputBusesOf (*L)));
    Renderer rV (*V, leg, chV, kBlock, 0, nullptr,
                 echojay::selfKeyCopyPlan (echojay::shadowInputBusesOf (*V)));
    rL.startThread (juce::Thread::Priority::highest);
    rV.startThread (juce::Thread::Priority::highest);
    say (leg, "both instances are RENDERING - the drag below happens on live plugins");

    auto settle = [&] (int ms)
    {
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        while (juce::Time::getMillisecondCounterHiRes() - t0 < (double) ms)
        { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false); }
    };

    // ONE DRAG, MODELLED ONCE AND RUN TWICE. 40 steps from 0.05 to 0.95, a follow tick after every
    // step, and the frame it carries is the Link's value from two publishes ago.
    auto oneRun = [&] (bool withHold, std::vector<float>& seen) -> void
    {
        pV[which]->setValueNotifyingHost (0.05f);
        pL[which]->setValueNotifyingHost (0.05f);
        settle (120);
        std::vector<float> lastApplied;
        std::vector<float> lag;                       // the Link's published history
        seen.clear();
        for (int step = 0; step < 40; ++step)
        {
            const float hand = 0.05f + 0.90f * ((float) step / 39.0f);
            pV[which]->setValueNotifyingHost (hand);              // his hand, on the copy
            pL[which]->setValueNotifyingHost (hand);              // ...over the ring, to the Link
            settle (12);
            lag.push_back (pL[which]->getValue());                // the Link publishes
            // THE FOLLOW TICK, carrying a frame composed two publishes ago.
            const int from = juce::jmax (0, (int) lag.size() - 3);
            std::vector<float> frame ((size_t) pV.size(), 0.0f);
            for (int i = 0; i < pV.size(); ++i) frame[(size_t) i] = pV[i]->getValue();
            frame[(size_t) which] = lag[(size_t) from];
            if (withHold)
                echojay::shadowApplyChangedValues (*V, frame.data(), (int) frame.size(), lastApplied,
                                                   [which] (int i) { return i == which; });
            else
                echojay::shadowApplyChangedValues (*V, frame.data(), (int) frame.size(), lastApplied);
            settle (6);
            seen.push_back (pV[which]->getValue());
        }
    };

    std::vector<float> withHold, without;
    say (leg, "---- the drag WITH the product's hold ----");
    oneRun (true, withHold);
    say (leg, "---- the same drag WITHOUT it (the known-bad) ----");
    oneRun (false, without);

    auto backsteps = [] (const std::vector<float>& v)
    {
        int n = 0;
        for (size_t i = 1; i < v.size(); ++i) if (v[i] < v[i - 1] - 1.0e-4f) ++n;
        return n;
    };
    auto span = [] (const std::vector<float>& v)
    { return v.empty() ? 0.0f : v.back() - v.front(); };

    const int backHold = backsteps (withHold), backNone = backsteps (without);
    int passes = 0, legs = 0;
    auto one = [&] (bool ok, const juce::String& what, const juce::String& detail)
    { ++legs; if (ok) ++passes; say (leg, juce::String (ok ? "PASS" : "FAIL") + ": " + what + "  [" + detail + "]"); };

    one (backHold == 0,
         "with the hold, the copy's value sequence NEVER steps backwards during the drag - which is "
         "what \"smooth\" is, measured rather than felt",
         juce::String (backHold) + " backward step(s) in " + juce::String ((int) withHold.size()));
    one (span (withHold) > 0.5f,
         "...and it actually travelled, so the hold is not just freezing the knob",
         "span " + juce::String (span (withHold), 3));
    one (backNone > 0,
         "WITHOUT the hold the same drag DOES step backwards - the known-bad, so this leg cannot "
         "pass by accident and his \"it jumps\" is reproduced here rather than argued about",
         juce::String (backNone) + " backward step(s) in " + juce::String ((int) without.size()));

    rL.signalThreadShouldExit(); rV.signalThreadShouldExit();
    rL.waitForThreadToExit (4000); rV.waitForThreadToExit (4000);
    say (leg, "==== " + juce::String (passes) + "/" + juce::String (legs) + " ====");
    return passes == legs ? 0 : 1;
}

int runControlLeg (const juce::String& leg, juce::AudioPluginFormatManager& fm,
                   const juce::PluginDescription& d, int seconds)
{
    using BC = LinkShm::BorrowCommit;
    using A  = BC::Action;

    auto L = makeOne (fm, d, leg, "L: the plugin on the Link (the one he hears)");
    if (L == nullptr) return 3;
    auto V = makeOne (fm, d, leg, "V: the V2's borrowed mirror of that slot");
    if (V == nullptr) return 3;

    L->prepareToPlay (kRate, kBlock);
    V->prepareToPlay (kRate, kBlock);

    auto& pL = L->getParameters();
    auto& pV = V->getParameters();
    if (pL.isEmpty() || pV.isEmpty()) { say (leg, "REFUSED: this plugin reports no parameters"); return 3; }

    int which = -1;
    for (int i = 0; i < pL.size(); ++i)
        if (pL[i]->getName (64).isNotEmpty()) { which = i; break; }
    say (leg, juce::String (pL.size()) + " parameter(s); driving " + juce::String (which) + " \""
            + pL[which]->getName (64) + "\"");

    const int chL = juce::jmax (2, L->getTotalNumInputChannels(), L->getTotalNumOutputChannels());
    const int chV = juce::jmax (2, V->getTotalNumInputChannels(), V->getTotalNumOutputChannels());
    Renderer rL (*L, leg, chL, kBlock, 0, nullptr,
                 echojay::selfKeyCopyPlan (echojay::shadowInputBusesOf (*L)));
    Renderer rV (*V, leg, chV, kBlock, 0, nullptr,
                 echojay::selfKeyCopyPlan (echojay::shadowInputBusesOf (*V)));
    rL.startThread (juce::Thread::Priority::highest);
    rV.startThread (juce::Thread::Priority::highest);
    say (leg, "both instances are RENDERING - every state call below happens on a live plugin, "
              "which is the product's condition and the only one worth a verdict");

    auto settle = [&] (int ms)
    {
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        while (juce::Time::getMillisecondCounterHiRes() - t0 < (double) ms)
        {
            juce::Timer::callPendingTimersSynchronously();
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false);
        }
    };

    int passes = 0, legs = 0;
    auto one = [&] (const char* what, bool seeded, bool bornHere)
    {
        ++legs;
        say (leg, juce::String ("---- ") + what + " ----");

        // THE SLOT'S STARTING POINT. A born-here slot reached the Link as a Create seeded from the
        // copy, so both sides start equal; a slot from the Link's own rack starts from the LINK's
        // state, and the mirror is seeded with it. Either way the two agree before he touches it.
        juce::MemoryBlock seed;
        if (bornHere) { V->getStateInformation (seed); L->setStateInformation (seed.getData(), (int) seed.getSize()); }
        else          { L->getStateInformation (seed); V->setStateInformation (seed.getData(), (int) seed.getSize()); }
        settle (300);
        const float v0 = pL[which]->getValue();
        say (leg, juce::String (bornHere ? "a picker ADD: L was created from the copy's state ("
                                         : "from the LINK's own rack: the mirror was seeded from L (")
                + juce::String ((juce::int64) seed.getSize()) + " bytes), both at "
                + juce::String (v0, 4) + ")");

        // HIS GESTURE, through the live view: it goes to the plugin on the LINK and nowhere else.
        const float v1 = v0 < 0.5f ? 0.80f : 0.20f;
        pL[which]->setValueNotifyingHost (v1);
        settle (300);
        const float onLink = pL[which]->getValue();
        const float onCopy = pV[which]->getValue();
        const bool landed = std::abs (onLink - v0) > 0.05f;
        const bool copyBlind = std::abs (onCopy - v0) < 0.05f;
        say (leg, juce::String (landed && copyBlind ? "PASS" : "FAIL")
                + ": his knob reached the LINK (now " + juce::String (onLink, 4)
                + ") and the borrowed mirror never saw it (still " + juce::String (onCopy, 4)
                + ") - which is the design, and the whole reason committing the mirror is a loss");

        // ---- THE CLOSE, THE OLD WAY: the known-bad direction, on a live plugin ----------------
        juce::MemoryBlock copyState;
        V->getStateInformation (copyState);
        const auto oldAction = BC::classify (/*withheld*/ false, /*edited*/ true, seeded, bornHere);
        bool wiped = false;
        if (oldAction == A::Commit)
        {
            L->setStateInformation (copyState.getData(), (int) copyState.getSize());
            settle (400);
            wiped = std::abs (pL[which]->getValue() - v0) < 0.05f;
        }
        say (leg, juce::String (oldAction == A::Commit && wiped ? "PASS" : "FAIL")
                + ": yesterday's rule said COMMIT and writing the mirror over the Link put it back "
                  "to " + juce::String (pL[which]->getValue(), 4) + " - the change he made and heard "
                  "DESTROYED, which is his 22:08 loss reproduced on a live plugin");

        // ---- AND THE CLOSE, THE RULE THAT SHIPS ----------------------------------------------
        pL[which]->setValueNotifyingHost (v1);          // he makes it again
        settle (300);
        const auto nowAction = BC::classify (/*withheld*/ false, /*edited*/ true, seeded, bornHere,
                                             /*linkNewer*/ true);
        if (nowAction == A::Commit)
            L->setStateInformation (copyState.getData(), (int) copyState.getSize());
        settle (400);
        const float kept = pL[which]->getValue();
        const bool ok = nowAction == A::LeaveLinkNewer && std::abs (kept - v1) < 0.05f;
        say (leg, juce::String (ok ? "PASS" : "FAIL")
                + ": the shipped rule LEAVES it (LEAVE-LINK-NEWER) and the Link still reads "
                + juce::String (kept, 4) + " after the close - the change survives leaving the rack, "
                  "which is what he asked for");
        if (ok && landed && copyBlind && wiped) ++passes;
    };

    // His two guards, in his order.
    one ("(a) a PICKER-ADDED slot - what his lost CL 1B was at 22:08:49", /*seeded*/ false, /*bornHere*/ true);
    one ("(b) a slot from the LINK'S OWN rack", /*seeded*/ true, /*bornHere*/ false);

    if (seconds > 8) settle ((seconds - 8) * 1000);
    rL.stopThread (2000);
    rV.stopThread (2000);
    L->releaseResources();
    V->releaseResources();
    say (leg, "SURVIVED, " + juce::String (rL.blocks.load()) + "+" + juce::String (rV.blocks.load())
            + " blocks rendered, " + juce::String (passes) + "/" + juce::String (legs)
            + " leg(s) green");
    keepForeverOnTheBench (std::move (L));
    keepForeverOnTheBench (std::move (V));
    return passes == legs ? 0 : 4;
}

/** HIS 14:27 CRASH: the ordering of a teardown, with the real plugin.

    One leg, two orderings, chosen by name. Everything else is held identical: the same layout (the
    12:14 fix's, so a render crash cannot be mistaken for this one), the same render, the same
    editor, the same second instance, the same window churn. */
int runTeardownLeg (const juce::String& leg, juce::AudioPluginFormatManager& fm,
                    const juce::PluginDescription& d, int seconds)
{
    const bool fixed = (leg == "teardown_safe");
    say (leg, fixed ? "THE FIX'S ORDERING: the editor goes first, then the instance is KEPT"
                    : "THE PRODUCT'S ORDERING: the instance is freed while its editor still lives");

    auto a = makeOne (fm, d, leg, "copy A");
    if (a == nullptr) return 3;

    const auto plan = echojay::selfKeyCopyPlan (echojay::shadowInputBusesOf (*a));
    const int channels = juce::jmax (2, a->getTotalNumInputChannels(), a->getTotalNumOutputChannels());
    a->prepareToPlay (kRate, kBlock);

    std::unique_ptr<EditorWindow> winA;
    if (a->hasEditor())
        if (auto* ed = a->createEditorIfNeeded())
        {
            winA = std::make_unique<EditorWindow> (ed);
            say (leg, "copy A's own editor is open (" + juce::String (ed->getWidth()) + "x"
                    + juce::String (ed->getHeight()) + ") - this is what was on his screen at 14:24:35");
        }

    {
        Renderer r (*a, leg, channels, kBlock, 0, nullptr, plan);
        r.startThread (juce::Thread::Priority::highest);
        say (leg, "copy A is rendering; running for 4 s, as the live view was");
        churnWindows (leg, winA.get(), 4000);
        r.stopThread (2000);
        say (leg, "copy A has stopped rendering after " + juce::String (r.blocks.load()) + " blocks");
    }
    a->releaseResources();

    // ---- THE ONE THING THIS LEG VARIES ---------------------------------------------------------
    if (fixed)
    {
        winA.reset();                       // THE EDITOR FIRST, while its processor is still alive
        say (leg, "copy A's editor is destroyed, and its processor was alive the whole time");
        keepForeverOnTheBench (std::move (a));
        say (leg, "copy A's INSTANCE IS KEPT - never disposed, which is Sean's ruling of 6 Oct");
    }
    else
    {
        a.reset();                          // the AU is freed with its editor still parented
        say (leg, "copy A's INSTANCE IS FREED, with its editor still alive - what stop() does today");
        winA.reset();                       // ...and now the editor is torn down against freed memory
        say (leg, "copy A's editor is destroyed, after its processor - the inversion");
    }

    // ---- 14:26:00 ONWARDS: ANOTHER INSTANCE OF THE SAME PLUGIN, WITH ITS OWN VIEW ---------------
    auto b = makeOne (fm, d, leg, "copy B (the Link's own, in his session)");
    if (b == nullptr) return 3;
    b->prepareToPlay (kRate, kBlock);
    std::unique_ptr<EditorWindow> winB;
    if (b->hasEditor())
        if (auto* ed = b->createEditorIfNeeded())
        {
            winB = std::make_unique<EditorWindow> (ed);
            say (leg, "copy B's editor is open - as the Link's own was at 14:26:00");
        }

    const int channelsB = juce::jmax (2, b->getTotalNumInputChannels(), b->getTotalNumOutputChannels());
    Renderer rb (*b, leg, channelsB, kBlock, 0,
                 nullptr, echojay::selfKeyCopyPlan (echojay::shadowInputBusesOf (*b)));
    rb.startThread (juce::Thread::Priority::highest);

    const int left = juce::jmax (4, seconds - 10);
    say (leg, "copy B is rendering with its view open; posting window notifications for "
            + juce::String (left) + " s");
    churnWindows (leg, winB.get(), left * 1000);

    rb.stopThread (2000);
    winB.reset();
    b->releaseResources();
    say (leg, "SURVIVED " + juce::String (seconds) + " s, " + juce::String (rb.blocks.load())
            + " blocks on copy B");
    keepForeverOnTheBench (std::move (b));   // the bench obeys the rule too, on the way out
    return 0;
}

} // namespace

int main (int argc, char** argv)
{
    const juce::String leg = argc >= 2 ? juce::String::fromUTF8 (argv[1]) : juce::String ("product");
    const int seconds      = argc >= 3 ? juce::jlimit (2, 600, atoi (argv[2])) : 60;

    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::AudioPluginFormatManager fm;
    // The EDITOR-CAPABLE registration, which is what ChainHost uses (juce::AudioUnitPluginFormat
    // derives from the headless one and overrides createPluginInstance). The live copy comes from
    // ChainHost::asyncCreatePlugin, so it is an editor-capable instance and this must match it.
    juce::addDefaultFormatsToManager (fm);

    juce::PluginDescription d;
    d.pluginFormatName = "AudioUnit";
    d.name             = argc >= 4 ? juce::String::fromUTF8 (argv[3]) : juce::String ("Tube-Tech CL 1B");
    d.fileOrIdentifier = argc >= 5 ? juce::String::fromUTF8 (argv[4])
                                   : juce::String ("AudioUnit:Effects/aufx,clST,SfTb");
    if (argc >= 6) d.uniqueId = d.deprecatedUid = (int) (juce::int64) juce::String (argv[5]).getHexValue64();

    say (leg, "\"" + d.name + "\" | " + d.fileOrIdentifier + " | " + juce::String (seconds) + " s");

    // HIS 14:27 CRASH is about an ORDERING, not a render, so its legs own the whole run.
    if (leg == "teardown" || leg == "teardown_safe") return runTeardownLeg (leg, fm, d, seconds);
    // FOLLOW layer 2's three questions own their whole run too: the sweep timing has to be taken
    // before a render starts competing for the thread, and the sweep he WATCHES has to be the only
    // thing moving on the panel.
    if (leg == "followparams") return runFollowParamsLeg (leg, fm, d, seconds);
    // 25 Oct: the stale-state loss, on a plugin with no licence, so it runs unsupervised.
    if (leg == "persist") return runPersistLeg (leg, fm, d, seconds);
    // 25 Oct: control, end to end, through the product's own commit rule. Unsupervised too.
    if (leg == "control") return runControlLeg (leg, fm, d, seconds);
    if (leg == "drag")    return runDragLeg (leg, fm, d, seconds);

    std::unique_ptr<juce::AudioPluginInstance> inst;
    juce::String err;
    bool done = false;
    fm.createPluginInstanceAsync (d, kRate, kBlock,
        [&] (std::unique_ptr<juce::AudioPluginInstance> p, const juce::String& e)
        { inst = std::move (p); err = e; done = true; });

    // Bounded, and the bound is generous because a licence check may put a window in front of Sean.
    const double tStart = juce::Time::getMillisecondCounterHiRes();
    while (! done && juce::Time::getMillisecondCounterHiRes() - tStart < 120000.0)
    {
        juce::Timer::callPendingTimersSynchronously();
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.05, false);
    }
    if (! done)          { say (leg, "REFUSED: no callback in 120 s (a licence prompt?)"); return 3; }
    if (inst == nullptr) { say (leg, "REFUSED: " + err.replace ("\n", " ")); return 3; }
    say (leg, "instantiated: " + juce::String (inst->getTotalNumInputChannels()) + " in / "
            + juce::String (inst->getTotalNumOutputChannels()) + " out, "
            + juce::String (inst->getBusCount (true)) + " input bus(es)");

    // ---- THE ONE THING EACH LEG VARIES ---------------------------------------------------------
    const bool ownLayout = (leg == "keyfed" || leg.startsWith ("selfkey"));
    if (! ownLayout)
    {
        // What PluginProcessor::shadowBegin does today: ask for a plain stereo pair and DISABLE
        // every other bus, because the shadow ring carries one stereo pair and nothing else.
        auto want = inst->getBusesLayout();
        for (int i = 0; i < want.inputBuses.size(); ++i)
            want.inputBuses.getReference (i) = (i == 0 ? juce::AudioChannelSet::stereo()
                                                      : juce::AudioChannelSet::disabled());
        for (int i = 0; i < want.outputBuses.size(); ++i)
            want.outputBuses.getReference (i) = (i == 0 ? juce::AudioChannelSet::stereo()
                                                        : juce::AudioChannelSet::disabled());
        const bool took = inst->setBusesLayout (want);
        say (leg, juce::String (took ? "asked for a plain stereo pair and it took"
                                     : "asked for a plain stereo pair and it REFUSED")
                + ": " + juce::String (inst->getTotalNumInputChannels()) + " in / "
                + juce::String (inst->getTotalNumOutputChannels()) + " out");
    }
    else
    {
        say (leg, "keeping the plugin's OWN layout - the shape ChainHost gives a rack slot "
                  "(\"input bus 1 (sidechain) fed from main\")");
    }

    std::unique_ptr<TickingPlayHead> ph;
    if (leg == "playhead")
    {
        ph = std::make_unique<TickingPlayHead>();
        inst->setPlayHead (ph.get());
        say (leg, "a playhead is installed, so every block carries a host time and a sample position");
    }

    inst->prepareToPlay (kRate, kBlock);
    say (leg, "prepared at " + juce::String ((int) kRate) + " Hz, " + juce::String (kBlock) + " frames");

    std::unique_ptr<EditorWindow> win;
    if (leg != "noeditor")
    {
        if (inst->hasEditor())
        {
            if (auto* ed = inst->createEditorIfNeeded())
            {
                win = std::make_unique<EditorWindow> (ed);
                say (leg, "the copy's own editor is open ("
                        + juce::String (ed->getWidth()) + "x" + juce::String (ed->getHeight()) + ")");
            }
            else say (leg, "hasEditor() is true but createEditorIfNeeded returned nothing");
        }
        else say (leg, "this instance reports no editor");
    }
    else say (leg, "no editor, by design");

    const int channels = juce::jmax (2, inst->getTotalNumInputChannels(),
                                        inst->getTotalNumOutputChannels());

    std::vector<echojay::ShadowChannelCopy> plan;
    if (leg.startsWith ("selfkey"))
    {
        plan = echojay::selfKeyCopyPlan (echojay::shadowInputBusesOf (*inst));
        juce::String says;
        for (const auto& c : plan)
            says << (says.isEmpty() ? "" : ", ") << "ch" << c.destChannel << " <- ch" << c.srcChannel;
        say (leg, "the self-key plan is " + juce::String ((int) plan.size()) + " copy(ies)"
                + (says.isEmpty() ? juce::String (" - this plugin has no key bus") : ": " + says));
        if (echojay::highestPlannedChannel (plan) >= channels)
        { say (leg, "REFUSED: the plan does not fit a " + juce::String (channels) + "-channel buffer"); return 3; }
    }

    const bool bigLeg = (leg == "bigblock" || leg == "selfkey_big");
    Renderer r (*inst, leg, channels, kBlock, bigLeg ? 100 : 0, ph.get(), plan);
    r.startThread (juce::Thread::Priority::highest);
    say (leg, "rendering " + juce::String (channels) + " channel(s) at real time");

    // The message thread does what it does in the product: runs the loop, so the editor's own timers
    // and the plugin's interface are alive while the render thread is inside the plugin.
    const auto tRun = juce::Time::getMillisecondCounterHiRes();
    int lastSaid = -1;
    while (juce::Time::getMillisecondCounterHiRes() - tRun < seconds * 1000.0)
    {
        juce::Timer::callPendingTimersSynchronously();
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false);
        const int s = (int) ((juce::Time::getMillisecondCounterHiRes() - tRun) / 1000.0);
        if (s != lastSaid)
        {
            lastSaid = s;
            say (leg, "t=" + juce::String (s) + "s blocks=" + juce::String (r.blocks.load())
                    + " frames=" + juce::String (r.framesDone.load()));
        }
    }

    r.stopThread (2000);
    win.reset();
    inst->releaseResources();
    say (leg, "SURVIVED " + juce::String (seconds) + " s, " + juce::String (r.blocks.load()) + " blocks");
    inst.reset();
    return 0;
}
