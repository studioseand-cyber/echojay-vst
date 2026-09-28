// probe_write.h - EchoJayProbe's WRITE-VERIFY MODE (feat/ejmap-cert, step 4 of the certification plan).
//
//   EchoJayProbe "<name>" "<identifier>" <uidHex> --write-test <index> <fromNorm> <toNorm> <arm>
//
// Two jobs.
//
// 1. THE THREE-STEP WRITE VERIFY, as reusable functions, because spec section 4.3 sets a control and then renders,
//    which assumes the write landed first. On a bridged AU that is false (DEFECT_BRIDGED_READBACK, 31 Jul and
//    10 Aug: a read in the same call stack as the write returns the OLD value, display and getValue alike). So:
//      (1) confirmProperty - pump the message loop until getValue() matches, bounded 500 ms. On timeout the caller
//          records write_unlanded and SKIPS that position rather than render it.
//      (2) stableText - only where display text is the evidence: read until two reads 20 ms of pumping apart agree,
//          bounded 300 ms.
//      (3) the re-read after the render - getValue() and the text again after the hold. If either moved, the write
//          landed DURING the render, and the position is discarded and re-rendered. This is what catches the
//          self-consistent one-behind table.
//
// 2. MEASUREMENT 2, which the procedure does not depend on but the record needs. 2 Aug (M9 Task 0-B) measured that a
//    bridged write lands with message-loop pumping and no rendering. The 10 Aug note says bridge parameter traffic
//    flushes on RENDER cycles. Both cannot be the whole story. Each ARM isolates one mechanism, in its own fresh
//    process:
//      ref    - set <to> first (confirmed), then render: the level the plugin makes when the write HAS landed
//      pump   - write <to>, then ONLY pump the message loop for 500 ms (no render), then render with no pumping
//      render - write <to>, then ONLY render (no pumping at all)
//      sleep  - write <to>, then wait 500 ms doing neither, then render with no pumping
//      verify - write <to> through the three-step procedure above, with the 1.5 s hold of spec 4.3
//    Every arm first sets <from> (confirmed), renders a 1 s baseline, and prints the IN-STACK read taken
//    immediately after the write under test. After the write, "blk" lines give per-block output level (channel 0)
//    beside getValue() and the display text, so the render plane and the property plane are read side by side.
//
// MEASUREMENT 2, RESULT (28 Sep 2026, bridged API-2500 (m) Thresh 0.2 <-> 0.9 at -12 dBFS, 5.1 dB apart in the audio;
// 30+ fresh-process runs, both directions). WALL TIME IS THE VARIABLE, not pumping and not rendering.
//   - The in-stack read (getValue + text right after the write) takes 31-40 ms on the bridge and returns the OLD
//     value, every run.
//   - With NO pumping at all - rendering or sleeping, it makes no difference - the write lands 34-69 ms after it
//     was made (median 46). In 3 runs it had not landed when the log ended, at 44-58 ms.
//   - With pumping it lands at about the same total time: the pump arm's 3.2 / 5.9 ms are counted from the end of
//     the ~35 ms in-stack read, so about 38-41 ms after the write. Setup confirms took 49-70 ms. So pumping is
//     neither required nor measurably faster here.
//   - Audio and property land TOGETHER: the same block in 22 of 26 runs, and in the other 4 getValue shows the new
//     value one block before the audio does. The property never lags the audio, so a confirmed getValue means the
//     next rendered block reflects the write.
// So both earlier findings are elapsed time seen from different hosts. 2 Aug's "pump 0 = never lands" rendered at
// once, offline, with only milliseconds elapsed, and its >=50 ms of pumping supplied the time. 10 Aug's "flushes on
// render cycles" was a DAW, where render cycles run at realtime (10.7 ms per 512-sample block) and so supply the
// time. Offline, 46 blocks can take under 50 ms, so "N render cycles later" is no promise at all. What the
// procedure needs is the confirm: never assume a write has landed after some number of blocks or milliseconds.
// Measured cost of the verify: 8-12 ms of confirm after the in-stack read (43-47 ms after the write), 21-22 ms for a
// stable text (2 reads), and the hold level within 0.0002 dB of the reference. The post-render re-read was steady in
// both directions.
//
// It MEASURES; it does not decide (D2). Which block the audio moved in, and which mechanism carried the write, are
// derived from these lines outside the signed binary.
#pragma once

#include <CoreFoundation/CoreFoundation.h>   // CFRunLoopRunInMode; before JUCE, as the probe requires (MacTypes Point)
#include "probe_render.h"
#include <chrono>
#include <thread>

namespace ejprobe
{

// ONE PUMP SLICE: the same primitive the probe's other modes use to let an AU's parameter traffic and text settle.
inline void pumpSlice (double seconds = 0.002)
{
    juce::Timer::callPendingTimersSynchronously();
    CFRunLoopRunInMode (kCFRunLoopDefaultMode, seconds, false);
}

// A phase-continuous tone through the plugin, block by block, on the MAIN input bus only (sidechains stay silent,
// per the SIDECHAIN POLICY in probe_render.h).
struct ToneRenderer
{
    juce::AudioPluginInstance& p;
    juce::AudioBuffer<float> io;
    juce::MidiBuffer midi;
    double phase = 0.0, step = 0.0, amp = 0.0;
    int mainIn = 0, block = 512;

    ToneRenderer (juce::AudioPluginInstance& proc, const RenderSpec& s)
        : p (proc), io (juce::jmax (2, proc.getTotalNumInputChannels(), proc.getTotalNumOutputChannels()), s.block)
    {
        step = juce::MathConstants<double>::twoPi * s.hz / s.sampleRate;
        amp = std::pow (10.0, s.dbfs / 20.0);
        mainIn = mainInputChannels (proc);
        block = s.block;
    }

    // Renders one block; returns the mean square of output channel 0 over it.
    double renderBlock (bool tone)
    {
        io.clear();
        if (tone)
            for (int n = 0; n < block; ++n)
            {
                const float v = (float) (amp * std::sin (phase));
                for (int ch = 0; ch < mainIn; ++ch) io.setSample (ch, n, v);
                phase += step;
                if (phase > juce::MathConstants<double>::twoPi) phase -= juce::MathConstants<double>::twoPi;
            }
        midi.clear();
        p.processBlock (io, midi);
        double ss = 0.0;
        if (p.getTotalNumOutputChannels() > 0)
        {
            const float* d = io.getReadPointer (0);
            for (int n = 0; n < block; ++n) ss += (double) d[n] * d[n];
        }
        return ss / block;
    }

    // Renders n tone blocks; returns the level in dBFS (RMS) over the LAST `tail` of them.
    double renderToneLevel (int n, int tail)
    {
        double acc = 0.0; int counted = 0;
        for (int k = 0; k < n; ++k)
        {
            const double ms = renderBlock (true);
            if (k >= n - tail) { acc += ms; ++counted; }
        }
        return toDb (counted > 0 ? std::sqrt (acc / counted) : 0.0);
    }
};

// (1) PROPERTY CONFIRM. Pumps at least once (a fast-property plugin would otherwise confirm before the run loop was
// ever serviced, and its DSP would never see the write), then polls getValue(). Returns ms, or -1 on timeout.
inline double confirmProperty (juce::AudioProcessorParameter& q, float want, float& lastRead,
                               int boundMs = 500, float tol = 0.005f)
{
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    do
    {
        pumpSlice();
        lastRead = q.getValue();
        if (std::abs (lastRead - want) <= tol)
            return juce::Time::getMillisecondCounterHiRes() - t0;
    }
    while (juce::Time::getMillisecondCounterHiRes() - t0 < boundMs);
    return -1.0;
}

// (2) DISPLAY STABLE-READ. Two reads 20 ms of pumping apart must agree. Returns ms, or -1 on timeout.
inline double stableText (juce::AudioProcessorParameter& q, juce::String& text, int& reads,
                          int boundMs = 300, int spacingMs = 20)
{
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    juce::String prev = q.getCurrentValueAsText();
    reads = 1;
    while (juce::Time::getMillisecondCounterHiRes() - t0 < boundMs)
    {
        const double s0 = juce::Time::getMillisecondCounterHiRes();
        while (juce::Time::getMillisecondCounterHiRes() - s0 < spacingMs) pumpSlice();
        const juce::String now = q.getCurrentValueAsText();
        ++reads;
        if (now == prev) { text = now; return juce::Time::getMillisecondCounterHiRes() - t0; }
        prev = now;
    }
    text = prev;
    return -1.0;
}

// t_ms is WALL time since the write under test. It matters because an offline render runs far faster than realtime
// (~200x on bridged API-2500), so "46 blocks later" can be only a few milliseconds later.
inline void printBlockLog (ToneRenderer& r, juce::AudioProcessorParameter& q, int blocks, double tWrite)
{
    for (int k = 0; k < blocks; ++k)
    {
        const double ms = r.renderBlock (true);
        std::printf ("blk\t%d\tt_ms\t%.3f\trms_dbfs\t%.4f\tval\t%.6f\ttext\t%s\n", k,
                     juce::Time::getMillisecondCounterHiRes() - tWrite, toDb (std::sqrt (ms)), q.getValue(),
                     clean (q.getCurrentValueAsText()).toRawUTF8());
    }
}

inline void runWriteTest (juce::AudioPluginInstance& p, int index, float fromN, float toN, const juce::String& arm,
                          const RenderSpec& s = {})
{
    auto ps = p.getParameters();
    if (! juce::isPositiveAndBelow (index, ps.size()) || ps[index] == nullptr)
    { std::printf ("refused no parameter at index %d (%d parameters)\n", index, ps.size()); return; }
    // sleepN = wait N ms doing neither, then render with no pumping ("sleep" alone is 500 ms). It separates WALL
    // TIME from pumping and from rendering, which the other arms cannot.
    const bool isSleep = arm.startsWith ("sleep") && (arm.length() == 5 || arm.substring (5).containsOnly ("0123456789"));
    const int sleepMs = isSleep ? (arm.length() == 5 ? 500 : arm.substring (5).getIntValue()) : 0;
    if (! isSleep && ! juce::StringArray { "ref", "pump", "render", "verify" }.contains (arm))
    { std::printf ("refused unknown arm '%s' (ref pump render sleep sleepN verify)\n", arm.toRawUTF8()); return; }
    auto& q = *ps[index];
    std::printf ("write\tproto\t1\tindex\t%d\tname\t%s\tfrom\t%.6f\tto\t%.6f\tarm\t%s\n", index,
                 clean (q.getName (128)).toRawUTF8(), fromN, toN, arm.toRawUTF8());

    configureAndPrepare (p, s);
    ToneRenderer r (p, s);
    const int blocksPerSecond = (int) (s.sampleRate / s.block);          // 93 at 48 k / 512

    // SETUP: the starting value, confirmed, before any audio.
    stage ("setup");
    const float initial = arm == "ref" ? toN : fromN;
    q.setValueNotifyingHost (initial);
    float read = 0.0f;
    const double setupMs = confirmProperty (q, initial, read);
    std::printf ("setup\tvalue\t%.6f\tlanded_ms\t%.1f\tgetValue\t%.6f\ttext\t%s\n", initial, setupMs, read,
                 clean (q.getCurrentValueAsText()).toRawUTF8());
    if (setupMs < 0) { std::printf ("setup_unlanded\t%.6f\n", initial); return; }

    stage ("baseline");
    for (int k = 0; k < blocksPerSecond / 2; ++k) r.renderBlock (false);   // 0.5 s of silence
    std::printf ("baseline\trms_dbfs\t%.4f\n", r.renderToneLevel (blocksPerSecond, blocksPerSecond / 2));
    if (arm == "ref") { stage ("done"); return; }

    // THE WRITE UNDER TEST, and the in-stack read taken before anything else can run.
    stage ("write");
    const double tWrite = juce::Time::getMillisecondCounterHiRes();
    q.setValueNotifyingHost (toN);
    std::printf ("instack\tval\t%.6f\ttext\t%s\tt_ms\t%.3f\n", q.getValue(), clean (q.getCurrentValueAsText()).toRawUTF8(),
                 juce::Time::getMillisecondCounterHiRes() - tWrite);

    const int postBlocks = blocksPerSecond / 2;                              // ~0.5 s of audio logged after the write
    if (arm == "pump")
    {
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        double landed = -1.0; float v = q.getValue();
        while (juce::Time::getMillisecondCounterHiRes() - t0 < 500.0)
        {
            pumpSlice();
            v = q.getValue();
            if (landed < 0 && std::abs (v - toN) <= 0.005f) landed = juce::Time::getMillisecondCounterHiRes() - t0;
        }
        std::printf ("pump\tlanded_ms\t%.1f\tval_after_500ms\t%.6f\ttext\t%s\n", landed, v,
                     clean (q.getCurrentValueAsText()).toRawUTF8());
        printBlockLog (r, q, postBlocks, tWrite);
    }
    else if (arm == "render")
    {
        printBlockLog (r, q, postBlocks, tWrite);
    }
    else if (isSleep)
    {
        std::this_thread::sleep_for (std::chrono::milliseconds (sleepMs));
        std::printf ("sleep\tms\t%d\tval_after\t%.6f\ttext\t%s\n", sleepMs, q.getValue(),
                     clean (q.getCurrentValueAsText()).toRawUTF8());
        printBlockLog (r, q, postBlocks, tWrite);
    }
    else // verify: the three-step procedure, with spec 4.3's 1.5 s hold measured over its last 0.75 s
    {
        const double ms = confirmProperty (q, toN, read);
        std::printf ("verify\tproperty_ms\t%.1f\tgetValue\t%.6f\n", ms, read);
        if (ms < 0) { std::printf ("verify\twrite_unlanded\n"); stage ("done"); return; }
        juce::String text; int reads = 0;
        const double tms = stableText (q, text, reads);
        std::printf ("verify\ttext_stable_ms\t%.1f\treads\t%d\ttext\t%s\n", tms, reads, clean (text).toRawUTF8());
        const float preVal = q.getValue();
        const juce::String preText = q.getCurrentValueAsText();
        const int hold = (int) (1.5 * blocksPerSecond), tail = (int) (0.75 * blocksPerSecond);
        std::printf ("verify\thold_rms_dbfs\t%.4f\n", r.renderToneLevel (hold, tail));
        const float postVal = q.getValue();
        const juce::String postText = q.getCurrentValueAsText();
        const bool moved = std::abs (postVal - preVal) > 0.005f || postText != preText;
        std::printf ("verify\treread\t%s\tpre\t%.6f\t%s\tpost\t%.6f\t%s\n", moved ? "MOVED" : "steady",
                     preVal, clean (preText).toRawUTF8(), postVal, clean (postText).toRawUTF8());
    }
    stage ("done");
}

} // namespace ejprobe
