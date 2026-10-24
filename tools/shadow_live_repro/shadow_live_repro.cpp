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
