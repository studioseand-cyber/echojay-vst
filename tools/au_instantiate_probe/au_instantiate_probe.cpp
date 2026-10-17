// au_instantiate_probe: the smallest possible answer to ONE question —
// does a specific AU instantiate AND initialise (prepareToPlay ->
// AudioUnitInitialize) headless in THIS project's HEADLESS AU host, on THIS
// machine? Ground truth for whether the Link rack-switch crash (16 Sep 2026)
// is bench-reproducible or in-host only. Runs with the REAL home so iLok/
// Softube licensing resolves; writes no EchoJay state.
//
//   au_instantiate_probe "<name>" "<AudioUnit:...identifier...>" <uidHex>
//
// Reports, per plugin: INSTANTIATE ok/fail (the render step was removed 18 Sep 2026; formerly PREPARE+RENDER via a
// few process blocks (alive/silent/passthrough). A hang on a licensing dialog
// is the caller's job to time out and kill — this program never dismisses one.
#include <CoreFoundation/CoreFoundation.h>   // before JUCE: MacTypes Point
#include <AudioToolbox/AudioToolbox.h>       // before JUCE (its AudioBuffer collides after): the raw AU property call in probe_render.h
#include <JuceHeader.h>
#include "probe_render.h"                    // feat/ejmap-cert: --render-test (the whole mode lives there)
#include "probe_write.h"                     // feat/ejmap-cert: --write-test (the whole mode lives there)
#include "probe_sweep.h"                     // feat/ejmap-cert: --sweep, spec section 4 (the whole mode lives there)
#include "probe_pitch.h"                     // feat/ejmap-cert: --sweep-pitch, spec section 5 (tuners), 1 Oct
#include "probe_burst.h"                     // feat/ejmap-cert: --burst, compressor timing PROTOTYPE (roadmap 2.3), 5 Oct
#include "probe_response.h"                  // feat/ejmap-cert: --response, multitone EQ response PROTOTYPE (roadmap 2.2), 5 Oct
#include "probe_material.h"                  // feat/ejmap-cert: --material, generated vocal / drums / mix through the pick (accuracy A2), 6 Oct
#include "probe_tail.h"                      // feat/ejmap-cert: --tail, burst-then-silence for reverb / delay PROTOTYPE (roadmap 2.7), 5 Oct
#include "probe_dynamics.h"                  // feat/ejmap-cert: --hits and --ramp, transient shapers and gates PROTOTYPE (roadmap 2.8), 5 Oct
#include <set>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <iostream>
#include <string>

// THE WARM PROBE (Kathy, 14 Oct): "--serve <name> <identifier> <uidHex>" keeps ONE process for a plugin pass and runs each request
// (one JSON line on stdin: {"argv": [...], "env": {...}}) exactly as a fresh process would - a NEW plugin instance per request,
// created, measured and destroyed, so the 29 Sep fresh-state rule holds per position. What a one-shot run would _Exit with ends the
// request instead: its output, then "\nejprobe-served\t<code>" (printed BEFORE the instance is torn down, so a death in a third-party
// teardown cannot take the answer with it). The process keeps one idle "keeper" instance of the plugin open: on this Mac a bridged
// (x86_64-only) AU costs ~3.8 s per NEW instance because the bridge host process starts again each time, 0.035-0.09 s while one
// instance holds it open (14 Oct timing: CLA-2A, H-Reverb, H-EQ). The keeper is never prepared, written or rendered.
static constexpr const char* kServedSentinel = "ejprobe-served";
static int endRun (int code, bool serving)
{
    std::fflush (stdout);
    if (! serving) std::_Exit (code);   // one-shot: as before - a third-party AU's teardown is not this mode's subject
    std::printf ("\n%s\t%d\n", kServedSentinel, code); std::fflush (stdout);
    return code;
}

static int runRequest (int argc, char** argv, bool serving)
{
    ejprobe::sidechainPolicy() = ejprobe::kSidechainDefault;   // per request: a policy is an argument, never left from the last one
    if (! ejprobe::takeSidechainPolicyArg (argc, argv)) { std::printf ("refused sidechain= must be unconnected, silent or self\n"); return endRun (3, serving); }
    juce::AudioPluginFormatManager fm;
    juce::addDefaultFormatsToManager (fm);   // the HEADLESS module's registration (as ChainHost)

    juce::PluginDescription d;
    d.pluginFormatName = "AudioUnit";
    d.name             = (argc >= 2) ? juce::String::fromUTF8 (argv[1]) : juce::String();
    d.fileOrIdentifier = (argc >= 3) ? juce::String::fromUTF8 (argv[2]) : juce::String();
    // 21 Sep 2026 (owed from the item-5 scan): the FORMAT comes from the file argument - a ".vst3" bundle path (or any path that is
    // not an "AudioUnit:" identifier) is a VST3; the probe used to hard-code AudioUnit and refused every VST3 with
    // "No compatible plug-in format exists for this plug-in".
    if (d.fileOrIdentifier.endsWithIgnoreCase (".vst3") || (d.fileOrIdentifier.startsWithChar ('/') && ! d.fileOrIdentifier.startsWith ("AudioUnit:")))
        d.pluginFormatName = "VST3";
    if (argc >= 4) d.uniqueId = d.deprecatedUid = (int) (juce::int64) juce::String (argv[3]).getHexValue64();

   #if defined(__x86_64__)
    const char* probeArch = "x86_64";
   #elif defined(__arm64__) || defined(__aarch64__)
    const char* probeArch = "arm64";
   #else
    const char* probeArch = "unknown";
   #endif
    std::printf ("probe: \"%s\" | %s | uid=%s | arch=%s | format=%s\n",
                 d.name.toRawUTF8(), d.fileOrIdentifier.toRawUTF8(),
                 argc >= 4 ? argv[3] : "(none)", probeArch, d.pluginFormatName.toRawUTF8());
    // 18 Sep 2026: argv[4] = a marker file the HOST polls. Touched the moment the instance exists, so a
    // crash or stall in the render check below (AMEK Mastering Compressor segfaults there, exit 139) can
    // never read as "hangs on load".
    // 21 Sep 2026 (ruling): "--list-params" as the 4th argument = LIST MODE. Prints one line per parameter
    // "index<TAB>name<TAB>label<TAB>numSteps<TAB>isDiscrete", never creates an editor, never touches the marker or any
    // EchoJay state file; wall-clock bound 30 s (EJ_PROBE_LIST_BOUND_MS overrides, for the guard); exit 0 on success, exit 3
    // on refuse/timeout with one line "refused <reason>". The signed probe is the only process allowed to list a PACE-wrapped plugin.
    // 21 Sep 2026 (round (c)): "--list-steps <index>" = STEP MODE - for one stepped parameter, walk its detents and print
    // "step<TAB>i<TAB>norm<TAB>text" (the panel text read after the AU settled on the run loop), the map builder's source for
    // `positions`. Same rules as list mode: no editor, no marker, no state file, exit 0 / exit 3 "refused <reason>".
    const bool listParams = argc >= 5 && juce::String (argv[4]) == "--list-params";
    const bool listSteps  = argc >= 6 && juce::String (argv[4]) == "--list-steps";
    // 21o item 5 (23 Sep 2026): "--sample-text <index>" = SAMPLED-TEXT MODE. A plugin may hold a stepped control while
    // telling the host it is continuous - Auto-Tune Pro reports numSteps 2147483647 and isDiscrete 0 for Scale, Key and
    // Detune, so --list-steps refuses them and the map can only carry a bare numeric range. This mode sweeps the
    // NORMALISED range at 1/512 and records every value at which the DISPLAY TEXT changes; a sweep that yields a finite
    // set of distinct texts (2..128) is a stepped control with those names, whatever numSteps claims. Prints one line
    // per run: "pos<TAB>n<TAB>normCentre<TAB>normLo<TAB>normHi<TAB>text", then "distinct<TAB>N". Same rules as the other
    // list modes: no editor, no marker, no state file, exit 0 / exit 3 "refused <reason>".
    const bool sampleText = argc >= 6 && juce::String (argv[4]) == "--sample-text";
    // 21r item 2(b) (24 Sep 2026): "--sample-stepped" = EVERY stepped control in ONE instantiation. The client runs
    // this once per identity the first time a plugin is racked, in the background, and the names ride the fp map.
    // One launch, not one per control: instantiating a UAD plugin costs ~5 s, sweeping a control costs ~2.
    const bool sampleAll = argc >= 5 && juce::String (argv[4]) == "--sample-stepped";
    // 21s-b (24 Sep 2026): "--text-at <index>" = THREE READS, at normalised 0.0, 0.5 and 1.0. A CONTINUOUS control
    // is exactly what --sample-text refuses (it is not a named control), so a fixture had a unit and no range.
    // This is the smallest honest answer: what the plugin PRINTS at the two ends and the middle, which is where a
    // numeric range comes from when the text parses as a number.
    const bool textAt = argc >= 6 && juce::String (argv[4]) == "--text-at";
    // feat/ejmap-cert (28 Sep 2026): "--render-test" = RENDER MODE, EJ Map certification's instrument. Same rules as
    // the other list modes (no editor, no marker, no state file, exit 0 / exit 3 "refused <reason>"); see probe_render.h.
    const bool renderTest = argc >= 5 && juce::String (argv[4]) == "--render-test";
    const bool writeTest  = argc >= 9 && juce::String (argv[4]) == "--write-test";   // <index> <from> <to> <arm>
    const bool sweep      = argc >= 5 && juce::String (argv[4]) == "--sweep";        // key=value arguments, probe_sweep.h
    const bool textAtNorms = argc >= 7 && juce::String (argv[4]) == "--text-at-norms"; // <index> <n0,n1,...>, probe_sweep.h
    const bool textAtNormsMulti = argc >= 6 && juce::String (argv[4]) == "--text-at-norms-multi"; // "<i>:<n,..>;<j>:<n,..>" (14 Oct), probe_sweep.h
    const bool sweepPitch = argc >= 5 && juce::String (argv[4]) == "--sweep-pitch";   // key=value arguments, probe_pitch.h
    const bool burst      = argc >= 5 && juce::String (argv[4]) == "--burst";        // key=value arguments, probe_burst.h (PROTOTYPE)
    const bool response   = argc >= 5 && juce::String (argv[4]) == "--response";     // key=value arguments, probe_response.h (PROTOTYPE)
    const bool tail       = argc >= 5 && juce::String (argv[4]) == "--tail";         // key=value arguments, probe_tail.h (PROTOTYPE)
    const bool hits       = argc >= 5 && juce::String (argv[4]) == "--hits";         // key=value arguments, probe_dynamics.h (PROTOTYPE)
    const bool ramp       = argc >= 5 && juce::String (argv[4]) == "--ramp";         // key=value arguments, probe_dynamics.h (PROTOTYPE)
    const bool materialM  = argc >= 5 && juce::String (argv[4]) == "--material";     // key=value arguments, probe_material.h (PROTOTYPE, A2)
    // 12 Oct (Kathy, custom defaults): "--factory-preset [default|N|open]" = the AU's OWN factory preset, no audio ("open": the opening block only). Prints the opening block
    // (param lines), the unit's factory presets (programs / program lines), then loads preset N (default: the first, 0) and prints
    // its block as "fparam" lines; "factory none" when the unit publishes no factory preset
    const bool factoryPreset = argc >= 5 && juce::String (argv[4]) == "--factory-preset";
    const bool listMode = listParams || listSteps || sampleText || sampleAll || textAt || renderTest || writeTest || sweep || textAtNorms || textAtNormsMulti || sweepPitch || burst || response || tail || hits || ramp || materialM || factoryPreset;
    const juce::File marker = (argc >= 5 && ! listMode) ? juce::File (juce::String::fromUTF8 (argv[4])) : juce::File();
    std::fflush (stdout);

    // 22 Sep 2026 (ruling 5): a VST3 is created from the description the FORMAT finds in the bundle (findAllTypesForFile), not
    // from the bare name + uid 0 the entries list carries - JUCE matches the class by name/uid and answered "Unable to load
    // VST-3 plug-in file" for every VST3 (0 of 66 scanned) while the leg l6 only checked the format decision.
    if (d.pluginFormatName == "VST3")
    {
        juce::OwnedArray<juce::PluginDescription> found;
        for (auto* f : fm.getFormats()) if (f->getName() == "VST3") f->findAllTypesForFile (found, d.fileOrIdentifier);
        const juce::PluginDescription* pick = nullptr;
        for (auto* c : found) if (c->name.trim().equalsIgnoreCase (d.name.trim())) { pick = c; break; }
        if (pick == nullptr && found.size() > 0) pick = found[0];
        if (pick != nullptr) { const auto keepName = d.name; d = *pick; std::printf ("vst3 class: \"%s\" (asked \"%s\", %d class(es) in the bundle)\tversion\t%s\n", d.name.toRawUTF8(), keepName.toRawUTF8(), found.size(), d.version.toRawUTF8()); }   // 17 Oct: the installed version (the read-back sweep's VST3 record)
        else std::printf ("vst3 class: none found in the bundle\n");
        std::fflush (stdout);
    }
    std::unique_ptr<juce::AudioPluginInstance> inst;
    juce::String err;
    bool done = false;
    fm.createPluginInstanceAsync (d, 48000.0, 512,
        [&] (std::unique_ptr<juce::AudioPluginInstance> p, const juce::String& e)
        { inst = std::move (p); err = e; done = true; });
    // the wall-clock bound: 30 s; EJ_PROBE_LIST_BOUND_MS overrides in list mode (the guard forces it; 0 = refuse at once, the
    // forced-timeout fixture). The run loop is pumped in slices no longer than the bound.
    const int boundMs = listMode && std::getenv ("EJ_PROBE_LIST_BOUND_MS") != nullptr ? juce::jmax (0, atoi (std::getenv ("EJ_PROBE_LIST_BOUND_MS"))) : 30000;
    const double tStart = juce::Time::getMillisecondCounterHiRes();
    const double slice = juce::jlimit (0.001, 0.05, boundMs / 1000.0);
    while (! done && boundMs > 0 && juce::Time::getMillisecondCounterHiRes() - tStart < (double) boundMs)
    {
        juce::Timer::callPendingTimersSynchronously();
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, slice, false);
    }
    if (listMode)
    {
        if (! done)          { std::printf ("refused timeout after %d ms\n", boundMs); return endRun (3, serving); }
        if (inst == nullptr) { std::printf ("refused %s\n", err.replace ("\n", " ").toRawUTF8()); return endRun (3, serving); }
        // THE PRESET (Kathy's NEXT BUILD item E, channel strips, 7 Oct): EJ_PROBE_PRESET="i:norm,i:norm" is written before any mode runs -
        // a strip's section engage switch (Rule 1) rides under every probe process of that section; each write is confirmed and printed
        // (`preset`), so a trace always says what was in force. Absent: nothing changes.
        if (const char* preset = std::getenv ("EJ_PROBE_PRESET"); preset != nullptr && *preset != 0)
        {
            auto ps0 = inst->getParameters();
            for (auto& t : juce::StringArray::fromTokens (juce::String::fromUTF8 (preset), ",", ""))
            {
                const int idx = t.upToFirstOccurrenceOf (":", false, false).getIntValue(); const float norm = (float) t.fromFirstOccurrenceOf (":", false, false).getDoubleValue();
                if (! juce::isPositiveAndBelow (idx, ps0.size()) || ps0[idx] == nullptr) { std::printf ("refused preset: no parameter %d\n", idx); return endRun (3, serving); }
                ps0[idx]->setValueNotifyingHost (norm);
                const auto l = ejprobe::landWrite (*ps0[idx], norm, nullptr);
                std::printf ("preset\t%d\t%.6f\tgetValue\t%.6f\tlanded_by\t%s\ttext\t%s\n", idx, norm, l.read, l.ms >= 0 ? l.by : "unlanded", ps0[idx]->getCurrentValueAsText().replace ("\t", " ").toRawUTF8());
            }
        }
        // AN EMPTY LIST AT CREATION IS RE-READ AFTER PREPARE AND A FIRST RENDER (Kathy's NEXT BUILD item C, 6-7 Oct: Soundtoys and 2C
        // listed NOTHING - Decapitator's trace is the header line alone - yet they automate in Logic). JUCE reads the AU's
        // kAudioUnitProperty_ParameterList at creation, BEFORE AudioUnitInitialize; a unit that publishes its parameters only once
        // initialised shows an empty list until prepareToPlay re-reads it (JUCE: `if (! haveParameterList) refreshParameterList()`
        // after AudioUnitInitialize), and some publish only after a first render. So: count at creation; if zero, prepare and count;
        // still zero, render one silent block, refresh and count. A plugin with a list at creation is untouched (no extra line).
        if (inst->getParameters().isEmpty() && ! renderTest)
        {
            const int n0 = inst->getParameters().size();
            ejprobe::configureAndPrepare (*inst, {});
            inst->refreshParameterList();
            const int n1 = inst->getParameters().size();
            int n2 = n1;
            if (n1 == 0)
            {
                juce::AudioBuffer<float> io (juce::jmax (2, inst->getTotalNumInputChannels(), inst->getTotalNumOutputChannels()), 512); io.clear(); juce::MidiBuffer midi;
                inst->processBlock (io, midi);
                const double t1 = juce::Time::getMillisecondCounterHiRes();
                while (juce::Time::getMillisecondCounterHiRes() - t1 < 300.0) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false); }
                inst->refreshParameterList();
                n2 = inst->getParameters().size();
            }
            std::printf ("\nparamcount\tat_create\t%d\tafter_prepare\t%d\tafter_render\t%d\n", n0, n1, n2);
        }
        const auto& ps = inst->getParameters();
        const auto clean = [] (juce::String t) { return t.replace ("\t", " ").replace ("\n", " ").replace ("\r", " "); };
        std::printf ("\n");   // 21 Sep 2026: row 0 starts a line of its own - WaveShell-AU writes a banner to stdout with no trailing newline
        if (renderTest) { ejprobe::runRenderTest (*inst); return endRun (0, serving); }
        if (factoryPreset)
        {
            auto block = [&] (const char* tag) { const auto& pp = inst->getParameters(); for (int i = 0; i < pp.size(); ++i) if (auto* q = pp[i])
                std::printf ("%s\t%d\t%.6f\t%s\t%s\n", tag, i, q->getValue(), clean (q->getName (128)).toRawUTF8(), clean (q->getCurrentValueAsText()).toRawUTF8()); };
            block ("param");
            const int n = inst->getNumPrograms(), cur = inst->getCurrentProgram();
            std::printf ("programs\t%d\tcurrent_at_open\t%d\n", n, cur);
            for (int k = 0; k < juce::jmin (n, 64); ++k) std::printf ("program\t%d\t%s\n", k, clean (inst->getProgramName (k)).toRawUTF8());
            const juce::String which = argc >= 6 ? juce::String (argv[5]) : juce::String ("default");
            if (which == "open") { return endRun (0, serving); }   // "open": the opening block alone (the custom-default check / read-back)
            if (n <= 0) { std::printf ("factory\tnone\tthe unit publishes no factory preset\n"); return endRun (0, serving); }
            const int k = which == "default" ? 0 : juce::jlimit (0, n - 1, which.getIntValue());
            inst->setCurrentProgram (k);
            const double t1 = juce::Time::getMillisecondCounterHiRes();   // let the unit apply it (some do on the run loop)
            while (juce::Time::getMillisecondCounterHiRes() - t1 < 400.0) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false); }
            std::printf ("factory\t%d\t%s\n", k, clean (inst->getProgramName (k)).toRawUTF8());
            block ("fparam");
            return endRun (0, serving);
        }
        if (textAtNormsMulti)
        {
            ejprobe::configureAndPrepare (*inst, {});
            ejprobe::runTextAtNormsMulti (*inst, juce::String::fromUTF8 (argv[5]));
            return endRun (0, serving);
        }
        if (textAtNorms)
        {
            std::vector<float> ns;
            for (auto& t : juce::StringArray::fromTokens (juce::String::fromUTF8 (argv[6]), ",", "")) ns.push_back ((float) t.getDoubleValue());
            ejprobe::configureAndPrepare (*inst, {});
            ejprobe::runTextAtNorms (*inst, atoi (argv[5]), ns);
            return endRun (0, serving);
        }
        if (response)
        {
            ejprobe::ResponseSpec spec; juce::String why;
            if (! ejprobe::parseResponseArgs (argc, argv, 5, spec, why)) { std::printf ("refused %s\n", why.toRawUTF8()); return endRun (3, serving); }
            ejprobe::runResponse (*inst, spec);
            return endRun (0, serving);
        }
        if (hits)
        {
            ejprobe::HitsSpec spec; juce::String why;
            if (! ejprobe::parseHitsArgs (argc, argv, 5, spec, why)) { std::printf ("refused %s\n", why.toRawUTF8()); return endRun (3, serving); }
            ejprobe::runHits (*inst, spec);
            return endRun (0, serving);
        }
        if (ramp)
        {
            ejprobe::RampSpec spec; juce::String why;
            if (! ejprobe::parseRampArgs (argc, argv, 5, spec, why)) { std::printf ("refused %s\n", why.toRawUTF8()); return endRun (3, serving); }
            ejprobe::runRamp (*inst, spec);
            return endRun (0, serving);
        }
        if (tail)
        {
            ejprobe::TailSpec spec; juce::String why;
            if (! ejprobe::parseTailArgs (argc, argv, 5, spec, why)) { std::printf ("refused %s\n", why.toRawUTF8()); return endRun (3, serving); }
            ejprobe::runTail (*inst, spec);
            return endRun (0, serving);
        }
        if (materialM)
        {
            ejprobe::MaterialSpec spec; juce::String why;
            if (! ejprobe::parseMaterialArgs (argc, argv, 5, spec, why)) { std::printf ("refused %s\n", why.toRawUTF8()); return endRun (3, serving); }
            ejprobe::runMaterial (*inst, spec);
            return endRun (0, serving);
        }
        if (burst)
        {
            ejprobe::BurstSpec spec; juce::String why;
            if (! ejprobe::parseBurstArgs (argc, argv, 5, spec, why)) { std::printf ("refused %s\n", why.toRawUTF8()); return endRun (3, serving); }
            ejprobe::runBurst (*inst, spec);
            return endRun (0, serving);
        }
        if (sweepPitch)
        {
            ejprobe::PitchSpec spec; juce::String why;
            if (! ejprobe::parsePitchArgs (argc, argv, 5, spec, why)) { std::printf ("refused %s\n", why.toRawUTF8()); return endRun (3, serving); }
            ejprobe::runPitchSweep (*inst, spec);
            return endRun (0, serving);
        }
        if (sweep)
        {
            ejprobe::SweepSpec spec; juce::String why;
            if (! ejprobe::parseSweepArgs (argc, argv, 5, spec, why)) { std::printf ("refused %s\n", why.toRawUTF8()); return endRun (3, serving); }
            ejprobe::runSweep (*inst, spec);
            return endRun (0, serving);
        }
        if (writeTest)
        {
            ejprobe::runWriteTest (*inst, atoi (argv[5]), (float) atof (argv[6]), (float) atof (argv[7]),
                                   juce::String::fromUTF8 (argv[8]));
            return endRun (0, serving);
        }
        // ONE sweep, shared by --sample-text (one control) and --sample-stepped (every control). Returns the number
        // of distinct texts, or 0 when the control is not a named one. THE TEXT IS READ AT 256 CHARACTERS
        // (ruling of 24 Sep 2026: at least 64, so a name is never truncated BY US - when it still comes back
        // short, as UAD's 8-character "CHROMATI" does, that is the plugin's own display width and the profile
        // keeps the canonical name beside it).
        struct Run { juce::String text; float lo = 0.0f, hi = 0.0f; };
        auto sweep = [&clean] (juce::AudioProcessorParameter* q, std::vector<Run>& runs) -> int
        {
            constexpr int kSamples = 513;        // 1/512 spacing, endpoints included
            constexpr int kMaxTexts = 128;
            const float before = q->getValue();
            runs.clear();
            for (int i = 0; i < kSamples; ++i)
            {
                const float norm = (float) i / (float) (kSamples - 1);
                q->setValueNotifyingHost (norm);
                for (int k = 0; k < 3; ++k) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.004, false); }
                const auto t = clean (q->getText (norm, 256).trim());
                if (! runs.empty() && runs.back().text == t) runs.back().hi = norm;
                else                                        runs.push_back ({ t, norm, norm });
                if ((int) runs.size() > kMaxTexts + 1) break;   // a continuous read-out: not a stepped control
            }
            q->setValueNotifyingHost (before);
            std::set<juce::String> distinct;
            for (const auto& r : runs) distinct.insert (r.text);
            return (distinct.size() < 2 || distinct.size() > (size_t) kMaxTexts) ? 0 : (int) distinct.size();
        };
        auto printRuns = [] (const std::vector<Run>& runs, int distinct)
        {
            int n = 0;
            for (const auto& r : runs)
                std::printf ("pos\t%d\t%.6f\t%.6f\t%.6f\t%s\n", ++n, (r.lo + r.hi) * 0.5f, r.lo, r.hi, r.text.toRawUTF8());
            std::printf ("distinct\t%d\n", distinct);
        };
        if (textAt)
        {
            // "--text-at all" walks every settable control in ONE instantiation - 55 controls across the tuner
            // set is 55 launches otherwise, and a UAD instantiation is five seconds.
            // 5 Oct 2026 (ruling 3, Saturn 2's 951 parameters): "--text-at 3,7,12" samples ONLY those indices, in one
            // instantiation - the controls a mode needs, not the whole unit. "all" and a single index are unchanged.
            const bool everyone = juce::String (argv[5]) == "all";
            std::vector<int> wanted; if (! everyone) for (const auto& t : juce::StringArray::fromTokens (juce::String (argv[5]), ",", "")) if (t.trim().isNotEmpty()) wanted.push_back (t.trim().getIntValue());
            if (! everyone && wanted.empty()) { std::printf ("refused no parameter index given\n"); return endRun (3, serving); }
            if (! everyone) for (int w : wanted) if (w < 0 || w >= ps.size()) { std::printf ("refused no parameter at index %d (%d parameters)\n", w, ps.size()); return endRun (3, serving); }
            for (int pi = 0; pi < ps.size(); ++pi)
            {
                if (! everyone && std::find (wanted.begin(), wanted.end(), pi) == wanted.end()) continue;
                auto* q = ps[pi]; if (q == nullptr) continue;
                if (everyone && (! q->isAutomatable() || q->isMetaParameter())) continue;
                const float before = q->getValue();
                std::printf ("param\t%d\t%s\tunit\t%s\n", pi, clean (q->getName (128)).toRawUTF8(), clean (q->getLabel()).toRawUTF8());
                // 21t (25 Sep 2026): THE VALUE THE PLUGIN CAME UP WITH. The compressor profiling pass asked for
                // "the default position on instantiate" and no mode printed it - this one restored it silently
                // and said nothing. Printed BEFORE the sweep moves anything, so it is the instantiate value and
                // not a read-back of our own write. getDefaultValue() rides beside it: what the plugin calls its
                // default is not always what it opens at, and conflating the two would be a guess.
                std::printf ("def\t%.6f\t%s\tdefault\t%.6f\n", before,
                             clean (q->getText (before, 256).trim()).toRawUTF8(), q->getDefaultValue());
                for (float nrm : { 0.0f, 0.5f, 1.0f })
                {
                    q->setValueNotifyingHost (nrm);
                    for (int k = 0; k < 6; ++k) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false); }
                    std::printf ("at\t%.3f\t%s\n", nrm, clean (q->getText (nrm, 256).trim()).toRawUTF8());
                }
                q->setValueNotifyingHost (before);   // put it back: this mode reads, it does not set
            }
            return endRun (0, serving);
        }
        if (sampleText)
        {
            const int idx = atoi (argv[5]);
            auto* q = idx >= 0 && idx < ps.size() ? ps[idx] : nullptr;
            if (q == nullptr) { std::printf ("refused no parameter at index %d (%d parameters)\n", idx, ps.size()); return endRun (3, serving); }
            std::vector<Run> runs;
            const int distinct = sweep (q, runs);
            if (distinct == 0) { std::printf ("refused not a named control (over 513 samples)\n"); return endRun (3, serving); }
            printRuns (runs, distinct);
            return endRun (0, serving);
        }
        if (sampleAll)
        {
            // 21r item 2(b): every control in ONE instantiation - the client runs this once per identity, in the
            // background, the first time a plugin is racked. A control that is not a named one prints a skip line,
            // so "sampled, nothing there" is distinguishable from "never sampled".
            std::vector<Run> runs;
            int sampled = 0;
            for (int i = 0; i < ps.size(); ++i)
            {
                auto* q = ps[i]; if (q == nullptr) continue;
                if (! q->isAutomatable() || q->isMetaParameter())
                { std::printf ("skip\t%d\t%s\tnot a settable control\n", i, clean (q->getName (128)).toRawUTF8()); continue; }
                const int distinct = sweep (q, runs);
                if (distinct == 0)
                { std::printf ("skip\t%d\t%s\tcontinuous\n", i, clean (q->getName (128)).toRawUTF8()); continue; }
                std::printf ("param\t%d\t%s\n", i, clean (q->getName (128)).toRawUTF8());
                printRuns (runs, distinct);
                ++sampled;
            }
            std::printf ("sampled\t%d\tof\t%d\n", sampled, ps.size());
            return endRun (0, serving);
        }
        if (listSteps)
        {
            const int idx = atoi (argv[5]);
            auto* q = idx >= 0 && idx < ps.size() ? ps[idx] : nullptr;
            if (q == nullptr) { std::printf ("refused no parameter at index %d (%d parameters)\n", idx, ps.size()); return endRun (3, serving); }
            const int n = q->getNumSteps();
            if (! q->isDiscrete() || n < 2 || n > 64) { std::printf ("refused not a stepped control (numSteps %d, discrete %d)\n", n, q->isDiscrete() ? 1 : 0); return endRun (3, serving); }
            for (int i = 0; i < n; ++i)
            {
                const float norm = (float) i / (float) (n - 1);
                q->setValueNotifyingHost (norm);
                for (int k = 0; k < 8; ++k) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false); }   // the AU's text settles on the run loop
                std::printf ("step\t%d\t%.6f\t%s\n", i + 1, norm, clean (q->getCurrentValueAsText().trim()).toRawUTF8());
            }
            return endRun (0, serving);
        }
        for (int i = 0; i < ps.size(); ++i)
        {
            auto* q = ps[i]; if (q == nullptr) continue;
            // 21o item 6: two more columns, additive - automatable and meta. On an AudioUnit JUCE sets automatable from
            // the AU's own flags ((flags & kAudioUnitParameterFlag_NonRealTime) == 0), which is exactly the line between a
            // settable control and a read-out: the PuigChild 660's six controls are Readable+Writable, its 69 LED / VU
            // rows are "Not Real Time, Readable" and nothing else (auval, 23 Sep).
            std::printf ("%d\t%s\t%s\t%d\t%d\t%d\t%d\n", i, clean (q->getName (128)).toRawUTF8(), clean (q->getLabel()).toRawUTF8(), q->getNumSteps(), q->isDiscrete() ? 1 : 0,
                         q->isAutomatable() ? 1 : 0, q->isMetaParameter() ? 1 : 0);
        }
        return endRun (0, serving);   // a third-party AU's teardown is not this mode's subject
    }
    if (! done)          { std::printf ("RESULT: TIMED OUT creating instance\n"); return serving ? endRun (2, true) : 2; }
    if (inst == nullptr) { std::printf ("INSTANTIATE: FAIL (%s)\nRESULT: FAIL\n", err.toRawUTF8()); return serving ? endRun (1, true) : 1; }
    std::printf ("INSTANTIATE: OK  name=\"%s\" latency=%d\n",
                 inst->getName().toRawUTF8(), inst->getLatencySamples());
    std::fflush (stdout);
    if (marker != juce::File()) marker.create();

    // 18 Sep 2026: the probe's job is LOAD hangs. It used to prepare and render 40 blocks after this point; AMEK
    // Mastering Compressor segfaulted there (renderGetInput inside the headless AU host's input callback, exit 139,
    // both architectures) and in-host that step stalled to the bound and read as "hangs on load". There is no stated
    // reason to render: the verdict is the instance. Stop here.
    inst.reset();
    std::printf ("RESULT: OK\n");
    return serving ? endRun (0, true) : 0;
}

static int serveLoop (int argc, char** argv)
{
    if (argc < 5) { std::printf ("refused --serve needs <name> <identifier> <uidHex>\n"); std::fflush (stdout); return 3; }
    std::signal (SIGPIPE, SIG_IGN);
    juce::AudioPluginFormatManager fm; juce::addDefaultFormatsToManager (fm);
    std::unique_ptr<juce::AudioPluginInstance> keeper;   // THE KEEPER: holds the plugin's library / bridge host open; never prepared or touched
    {
        juce::PluginDescription d; d.pluginFormatName = "AudioUnit"; d.name = juce::String::fromUTF8 (argv[2]); d.fileOrIdentifier = juce::String::fromUTF8 (argv[3]);
        d.uniqueId = d.deprecatedUid = (int) (juce::int64) juce::String (argv[4]).getHexValue64();
        if (d.fileOrIdentifier.startsWith ("AudioUnit:"))
        {
            bool done = false; juce::String err;
            fm.createPluginInstanceAsync (d, 48000.0, 512, [&] (std::unique_ptr<juce::AudioPluginInstance> p, const juce::String& e) { keeper = std::move (p); err = e; done = true; });
            const double t0 = juce::Time::getMillisecondCounterHiRes();
            while (! done && juce::Time::getMillisecondCounterHiRes() - t0 < 30000.0) { juce::Timer::callPendingTimersSynchronously(); CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.05, false); }
        }
        std::printf ("\nejprobe-ready\tkeeper\t%s\n", keeper != nullptr ? "held" : "none"); std::fflush (stdout);
    }
    std::string line;
    while (std::getline (std::cin, line))
    {
        const auto req = juce::JSON::parse (juce::String::fromUTF8 (line.c_str()));
        const auto* av = req.getProperty ("argv", {}).getArray();
        if (av == nullptr || av->isEmpty()) { std::printf ("refused a request without argv\n"); endRun (3, true); continue; }
        static const char* kEnv[] { "EJ_PROBE_PRESET", "EJ_PROBE_LIST_BOUND_MS" };   // the environment a one-shot probe would have inherited at this call
        const auto env = req.getProperty ("env", {});
        for (const char* k : kEnv) { const auto v = env.getProperty (juce::Identifier (k), {}); if (v.isString()) setenv (k, v.toString().toRawUTF8(), 1); else unsetenv (k); }
        std::vector<std::string> store; for (const auto& a : *av) store.push_back (a.toString().toStdString());
        std::vector<char*> args; for (auto& a : store) args.push_back (a.data()); args.push_back (nullptr);
        runRequest ((int) store.size(), args.data(), true);
    }
    std::fflush (stdout);
    std::_Exit (0);   // stdin closed: the caller is done (or gone); the keeper's teardown is not this mode's subject
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    if (argc >= 2 && juce::String (argv[1]) == "--pitch-selftest") { const int rc = ejprobe::runPitchSelfTest(); std::fflush (stdout); std::_Exit (rc); }
    if (argc >= 2 && juce::String (argv[1]) == "--serve") return serveLoop (argc, argv);
    return runRequest (argc, argv, false);
}
