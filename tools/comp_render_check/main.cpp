// comp_render_check - COMP_PROFILE_SPEC_v1 section 8, the ACCEPTANCE check.
//
//   "A profile is accepted when, on the reference vocal clip, an offline render with EchoJay's computed settings
//    gives a measured GR on loud phrases within 1.0 dB of the target."
//
// This loads a real AU in process, applies a given set of controls, renders audio through it, and reports the gain
// reduction on the loud phrases as JSON. It is NOT a profiling sweep - that is EJ Maps' job (spec section 2.1), and
// this tool deliberately cannot produce a profile: it only ever answers "what did this plugin actually do to this
// audio with these settings".
//
// GR ON THE LOUD PHRASES is measured by pairing 400 ms windows of the render against the SAME windows of the input
// and taking the loud ones (those at or above the input's 95th percentile - the same statistic the plugin sends as
// track_level, for the same reason). STATIC GAIN is measured, not assumed: the same signal is rendered 40 dB
// quieter, well below any threshold, and the output-minus-input there is the fixed offset. It is subtracted, so the
// number reported is compression and not a makeup knob.
//
// Usage:
//   comp_render_check --name "EMO-D5 (s)" --set "Comp=On" --set "Comp Thresh=-20" --set "Comp Ratio=4"
//                     [--wav <in.wav>] [--sr 48000] [--seconds 20] [--json <out.json>] [--list]
//
// A control is given as NAME=VALUE. VALUE is matched against the parameter's own positions by text first (so
// "On", "4.00" and "-20" all work the way the panel reads), then as a number against its range, then as a raw
// normalised 0..1 if it is written as "norm:0.42". Every control's landed text is reported, so a value that did
// not take is visible rather than assumed.

#include <JuceHeader.h>
#include <cstdio>
#include <cmath>
#include <vector>
#include <map>

namespace {

constexpr double kWindowMs = 400.0;

struct Opt
{
    juce::String name, wav, jsonOut, id;
    juce::StringArray sets;
    double sr = 48000.0;
    double seconds = 20.0;
    bool  tone = false;          // spec section 8: the 997 Hz tone check
    float toneDbfs = -18.0f;     // L, as sine RMS dBFS
};

Opt parseArgs (int argc, char** argv)
{
    Opt o;
    for (int i = 1; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        auto next = [&] () -> juce::String { return (i + 1 < argc) ? juce::String (argv[++i]) : juce::String(); };
        if (a == "--name")         o.name = next();
        else if (a == "--set")     o.sets.add (next());
        else if (a == "--wav")     o.wav = next();
        else if (a == "--json")    o.jsonOut = next();
        else if (a == "--sr")      o.sr = next().getDoubleValue();
        else if (a == "--seconds") o.seconds = next().getDoubleValue();
        else if (a == "--id")      o.id = next();
        else if (a == "--tone")   { o.tone = true; o.toneDbfs = next().getFloatValue(); }
    }
    return o;
}

/** A SPEECH-LIKE TEST SIGNAL, because the repo has no vocal reference clip yet (spec section 8 leaves
    refs/vocal_ref_01.wav for Sean to choose). What matters for a compressor measurement is the ENVELOPE - loud
    phrases separated by quiet gaps, with a crest factor like a voice - so this is a 160 Hz sawtooth-ish glottal
    pulse train through a slow formant filter, amplitude-modulated into ~1.4 s phrases with 0.5 s gaps, at about
    12 dB crest. It is not a voice, and the handoff says so; it is a repeatable envelope with known levels, which
    is what an acceptance number needs to be comparable from run to run. */
void makeSpeechLike (juce::AudioBuffer<float>& buf, double sr, double seconds, float phraseRmsDbfs)
{
    const int n = (int) std::lround (sr * seconds);
    buf.setSize (1, n);
    buf.clear();
    auto* d = buf.getWritePointer (0);
    double phase = 0.0, f0 = 160.0;
    float lp1 = 0.0f, lp2 = 0.0f;
    juce::Random rng (20261001);
    for (int i = 0; i < n; ++i)
    {
        const double t = (double) i / sr;
        // phrases: 1.4 s on, 0.5 s off, with a soft attack and release so nothing clicks
        const double cyc = std::fmod (t, 1.9);
        double env = 0.0;
        if (cyc < 1.4)
        {
            const double a = juce::jmin (1.0, cyc / 0.08);
            const double r = juce::jmin (1.0, (1.4 - cyc) / 0.12);
            env = a * r;
            env *= 0.75 + 0.25 * std::sin (2.0 * juce::MathConstants<double>::pi * 2.3 * t);   // syllables
        }
        // a glottal-ish pulse: a sawtooth with a little jitter, through two one-poles for a formant-ish tilt
        f0 = 160.0 + 6.0 * std::sin (2.0 * juce::MathConstants<double>::pi * 0.7 * t);
        phase += f0 / sr;
        if (phase >= 1.0) phase -= 1.0;
        const float saw = (float) (2.0 * phase - 1.0);
        const float breath = rng.nextFloat() * 0.06f - 0.03f;
        lp1 += 0.22f * ((saw + breath) - lp1);
        lp2 += 0.45f * (lp1 - lp2);
        d[i] = (float) env * lp2;
    }
    // Normalise so the LOUD PHRASES sit at the wanted RMS: measure the rms of the samples above a tenth of peak.
    double sum = 0.0; int cnt = 0; float peak = 0.0f;
    for (int i = 0; i < n; ++i) peak = juce::jmax (peak, std::abs (d[i]));
    for (int i = 0; i < n; ++i)
        if (std::abs (d[i]) > peak * 0.1f) { sum += (double) d[i] * d[i]; ++cnt; }
    const float loudRms = cnt > 0 ? (float) std::sqrt (sum / (double) cnt) : 1.0f;
    const float want = juce::Decibels::decibelsToGain (phraseRmsDbfs);
    buf.applyGain (loudRms > 0.0f ? want / loudRms : 1.0f);
}

/** A 997 Hz SINE AT AN EXACT RMS dBFS (spec section 8): "a 997 Hz tone at L through the same settings lands within
    0.5 dB of g. The tone check catches calculation errors the 1 dB vocal tolerance would hide."

    997 Hz, not 1000: section 4 says it is "off the 1000 Hz default crossover some multiband compressors use". The
    level is stated as RMS, on the same convention as everything else here - a sine of amplitude a has RMS a/sqrt(2),
    so the amplitude for a wanted RMS in dBFS is 10^(db/20) * sqrt(2). A full-scale sine is therefore -3.01 dBFS RMS,
    and asking for anything above that is impossible: it is clamped and said out loud rather than silently clipped.

    It opens with a 1 s fade so a compressor's attack is not measured against a step, and the measurement windows
    that matter are well past it. */
void makeTone (juce::AudioBuffer<float>& buf, double sr, double seconds, float rmsDbfs, bool& clampedOut)
{
    const float maxRms = -3.0103f;
    clampedOut = rmsDbfs > maxRms;
    const float use = juce::jmin (rmsDbfs, maxRms);
    const int n = (int) std::lround (sr * seconds);
    buf.setSize (1, n);
    const float amp = juce::Decibels::decibelsToGain (use) * std::sqrt (2.0f);
    const double inc = 2.0 * juce::MathConstants<double>::pi * 997.0 / sr;
    double phase = 0.0;
    auto* d = buf.getWritePointer (0);
    const int fade = (int) std::lround (sr);
    for (int i = 0; i < n; ++i)
    {
        const float env = i < fade ? (float) i / (float) fade : 1.0f;
        d[i] = amp * env * (float) std::sin (phase);
        phase += inc;
    }
}

std::vector<float> windowRmsDb (const juce::AudioBuffer<float>& b, double sr)
{
    std::vector<float> out;
    const int w = juce::jmax (1, (int) std::lround (sr * kWindowMs * 0.001));
    const auto* d = b.getReadPointer (0);
    for (int start = 0; start + w <= b.getNumSamples(); start += w)
    {
        double s = 0.0;
        for (int i = 0; i < w; ++i) { const double v = d[start + i]; s += v * v; }
        const float rms = (float) std::sqrt (s / (double) w);
        out.push_back (rms > 0.0f ? juce::Decibels::gainToDecibels (rms, -120.0f) : -120.0f);
    }
    return out;
}

float percentile (std::vector<float> v, float p)
{
    if (v.empty()) return -120.0f;
    std::sort (v.begin(), v.end());
    const int idx = juce::jlimit (0, (int) v.size() - 1, (int) std::lround (p * (double) (v.size() - 1)));
    return v[(size_t) idx];
}

/** Render `in` through `plug`, in blocks, returning the output. The plugin is prepared fresh and its latency is
    compensated by dropping its reported samples from the head, so the windows of input and output line up. */
juce::AudioBuffer<float> render (juce::AudioPluginInstance& plug, const juce::AudioBuffer<float>& in, double sr)
{
    const int block = 512;
    plug.setPlayConfigDetails (1, 1, sr, block);
    plug.prepareToPlay (sr, block);
    const int latency = juce::jmax (0, plug.getLatencySamples());
    juce::AudioBuffer<float> work (juce::jmax (1, plug.getTotalNumOutputChannels()), block);
    juce::AudioBuffer<float> out (1, in.getNumSamples() + latency);
    out.clear();
    juce::MidiBuffer midi;
    for (int pos = 0; pos < out.getNumSamples(); pos += block)
    {
        const int n = juce::jmin (block, out.getNumSamples() - pos);
        work.clear();
        for (int ch = 0; ch < work.getNumChannels(); ++ch)
            for (int i = 0; i < n; ++i)
                work.setSample (ch, i, (pos + i) < in.getNumSamples() ? in.getSample (0, pos + i) : 0.0f);
        juce::AudioBuffer<float> slice (work.getArrayOfWritePointers(), work.getNumChannels(), n);
        plug.processBlock (slice, midi);
        for (int i = 0; i < n; ++i) out.setSample (0, pos + i, slice.getSample (0, i));
    }
    plug.releaseResources();
    if (latency > 0)
    {
        juce::AudioBuffer<float> aligned (1, in.getNumSamples());
        aligned.clear();
        for (int i = 0; i < in.getNumSamples(); ++i)
            aligned.setSample (0, i, out.getSample (0, juce::jmin (out.getNumSamples() - 1, i + latency)));
        return aligned;
    }
    juce::AudioBuffer<float> trimmed (1, in.getNumSamples());
    for (int i = 0; i < in.getNumSamples(); ++i) trimmed.setSample (0, i, out.getSample (0, i));
    return trimmed;
}

struct Applied { juce::String control, asked, landed; bool ok = false; float norm = 0.0f; };

/** Apply NAME=VALUE against the plugin's own parameter list: by position TEXT first, then as a number in range,
    then as "norm:0.42". Reports what landed, read back from the parameter after a settle. */
Applied applyOne (juce::AudioPluginInstance& plug, const juce::String& spec)
{
    Applied a;
    a.control = spec.upToFirstOccurrenceOf ("=", false, false).trim();
    a.asked   = spec.fromFirstOccurrenceOf ("=", false, false).trim();
    juce::AudioProcessorParameter* found = nullptr;
    for (auto* p : plug.getParameters())
        if (p != nullptr && p->getName (128).trim().equalsIgnoreCase (a.control)) { found = p; break; }
    if (found == nullptr)
    {
        // SAY WHAT DOES EXIST. A tool that reports "(no such control)" and stops makes the next person guess at
        // the spelling; the profile's control names have to match the plugin's exactly (spec section 3), so the
        // list is the useful half of the answer.
        juce::StringArray names;
        for (auto* p : plug.getParameters())
            if (p != nullptr && p->getName (128).trim().isNotEmpty()) names.add (p->getName (128).trim());
        a.landed = "(no such control; this plugin has: " + names.joinIntoString (", ") + ")";
        return a;
    }

    auto settle = [&] { for (int i = 0; i < 6; ++i) { juce::Timer::callPendingTimersSynchronously();
                                                     CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.02, false); } };
    if (a.asked.startsWithIgnoreCase ("norm:"))
    {
        a.norm = juce::jlimit (0.0f, 1.0f, a.asked.fromFirstOccurrenceOf (":", false, false).getFloatValue());
        found->setValueNotifyingHost (a.norm); settle();
        a.landed = found->getCurrentValueAsText().trim(); a.ok = true; return a;
    }
    // By the text the panel shows, across the parameter's own steps - this is how "On" and "4.00" land exactly.
    const int steps = found->getNumSteps();
    const int probes = (steps > 0 && steps < 512) ? steps : 201;
    auto flat = [] (const juce::String& x) { return x.toLowerCase().removeCharacters (" \t").trim(); };
    for (int i = 0; i < probes; ++i)
    {
        const float nrm = probes <= 1 ? 0.0f : (float) i / (float) (probes - 1);
        found->setValueNotifyingHost (nrm); settle();
        const auto txt = found->getCurrentValueAsText().trim();
        if (flat (txt) == flat (a.asked) || flat (txt) == flat (a.asked + " dB")
            || flat (txt).upToFirstOccurrenceOf ("db", false, false) == flat (a.asked))
        { a.norm = nrm; a.landed = txt; a.ok = true; return a; }
    }
    // Nearest by NUMBER, for a continuous control whose text carries units.
    const float wanted = a.asked.getFloatValue();
    float bestNorm = 0.0f, bestErr = 1.0e9f; juce::String bestTxt;
    for (int i = 0; i < 201; ++i)
    {
        const float nrm = (float) i / 200.0f;
        found->setValueNotifyingHost (nrm); settle();
        const auto txt = found->getCurrentValueAsText().trim();
        const float got = txt.retainCharacters ("-0123456789.").getFloatValue();
        const float err = std::abs (got - wanted);
        if (err < bestErr) { bestErr = err; bestNorm = nrm; bestTxt = txt; }
    }
    found->setValueNotifyingHost (bestNorm); settle();
    a.norm = bestNorm; a.landed = bestTxt; a.ok = bestErr <= 1.0f;
    return a;
}

int run (const Opt& o)
{
    // juce_audio_processors_headless DELETES addDefaultFormats(); this repo's own helper is what the other
    // real-plugin guards use (stepped_position_guard does the same).
    // ---- NEVER A FULL SCAN (1 Oct 2026 ruling, after this tool caused real harm) --------------------------
    // The first version of this tool walked every AU on the machine to resolve a name. On Sean's Mac that drove
    // iLok/PACE authorisation prompts and crashes, and the process was killed twice (exit 144) - because resolving
    // a name means asking each component what it contains, and for licence-bound plugins that means loading them.
    // So there is no scan and no --list any more: the component is named outright with --id, exactly one
    // AudioComponent is asked about itself, and nothing else on the machine is touched.
    //
    //   --id "AudioUnit:Effects/aufx,dcmp,appl"
    //
    // AND IT REFUSES A LICENCE-BOUND PLUGIN ON THIS MACHINE. Waves (ksWV), UAD and the rest sit behind PACE; an
    // unsigned binary cannot load them, the attempt prompts for an iLok that is on another Mac, and the prompt
    // takes the host down with it. EJ Maps has to solve the same wall (spec section 4), so that is where those
    // plugins belong - not here.
    juce::AudioPluginFormatManager fm;
    juce::addDefaultFormatsToManager (fm);
    if (o.id.isEmpty())
    { std::printf ("{ \"error\": \"--id is required: this tool never scans\" }\n"); return 2; }
    {
        static const char* const kLicenceBound[] = { "ksWV", "uadx", "UAD", "Wave", "iLok", "PACE", "Slte", "SSLB" };
        for (auto* code : kLicenceBound)
            if (o.id.contains (code))
            { std::printf ("{ \"error\": \"refusing a licence-bound plugin on this machine\", \"id\": \"%s\","
                           " \"why\": \"PACE/iLok: an unsigned binary cannot load it, and the authorisation"
                           " prompt crashes the host. EJ Maps owns these (spec section 4).\" }\n",
                           o.id.toRawUTF8()); return 5; }
    }
    juce::OwnedArray<juce::PluginDescription> found;
    for (auto* f : fm.getFormats())
    {
        if (f == nullptr || ! f->getName().containsIgnoreCase ("AudioUnit")) continue;
        f->findAllTypesForFile (found, o.id);      // ONE component, the one named. No search path is walked.
    }
    if (found.isEmpty())
    { std::printf ("{ \"error\": \"nothing at --id\", \"id\": \"%s\" }\n", o.id.toRawUTF8()); return 2; }
    const juce::PluginDescription* pick = found[0];
    if (o.name.isNotEmpty())
        for (auto* d : found) if (d->name.equalsIgnoreCase (o.name)) { pick = d; break; }

    juce::String err;
    std::unique_ptr<juce::AudioPluginInstance> plug (fm.createPluginInstance (*pick, o.sr, 512, err));
    if (plug == nullptr)
    { std::printf ("{ \"error\": \"could not instantiate\", \"plugin\": \"%s\", \"why\": \"%s\" }\n",
                   pick->name.toRawUTF8(), err.toRawUTF8()); return 3; }

    std::vector<Applied> applied;
    for (const auto& sp : o.sets) applied.push_back (applyOne (*plug, sp));

    juce::AudioBuffer<float> in;
    juce::String signalName;
    bool toneClamped = false;
    if (o.tone)
    {
        makeTone (in, o.sr, juce::jmax (6.0, o.seconds), o.toneDbfs, toneClamped);
        signalName = "tone:997hz_" + juce::String (juce::jmin (o.toneDbfs, -3.0103f), 2) + "dbfs_rms";
    }
    else if (o.wav.isNotEmpty() && juce::File (o.wav).existsAsFile())
    {
        juce::AudioFormatManager afm; afm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> rd (afm.createReaderFor (juce::File (o.wav)));
        if (rd == nullptr)
        { std::printf ("{ \"error\": \"could not read --wav\", \"wav\": \"%s\" }\n", o.wav.toRawUTF8()); return 4; }
        in.setSize (1, (int) rd->lengthInSamples);
        rd->read (&in, 0, (int) rd->lengthInSamples, 0, true, rd->numChannels > 1);
        signalName = "wav:" + juce::File (o.wav).getFileName();
    }
    else
    {
        makeSpeechLike (in, o.sr, o.seconds, -18.0f);
        signalName = "generated:speech_like_160hz_phrases";
    }

    const auto wet  = render (*plug, in, o.sr);
    // STATIC GAIN, measured: the same signal 40 dB down is well below any threshold, so what comes back is the
    // plugin's fixed offset and nothing else.
    juce::AudioBuffer<float> quiet (1, in.getNumSamples());
    quiet.makeCopyOf (in); quiet.applyGain (juce::Decibels::decibelsToGain (-40.0f));
    const auto quietWet = render (*plug, quiet, o.sr);

    const auto dryDb = windowRmsDb (in, o.sr);
    const auto wetDb = windowRmsDb (wet, o.sr);
    const auto qDry  = windowRmsDb (quiet, o.sr);
    const auto qWet  = windowRmsDb (quietWet, o.sr);
    const int n = (int) juce::jmin (dryDb.size(), wetDb.size());
    const float loudFloor = percentile (dryDb, 0.95f);

    double sumDry = 0.0, sumWet = 0.0; int loudWindows = 0;
    for (int i = 0; i < n; ++i)
        if (dryDb[(size_t) i] >= loudFloor - 0.01f)
        { sumDry += dryDb[(size_t) i]; sumWet += wetDb[(size_t) i]; ++loudWindows; }
    const float loudDry = loudWindows > 0 ? (float) (sumDry / loudWindows) : -120.0f;
    const float loudWet = loudWindows > 0 ? (float) (sumWet / loudWindows) : -120.0f;

    double sqd = 0.0, sqw = 0.0; int qn = 0;
    const float qFloor = percentile (qDry, 0.95f);
    for (int i = 0; i < (int) juce::jmin (qDry.size(), qWet.size()); ++i)
        if (qDry[(size_t) i] >= qFloor - 0.01f) { sqd += qDry[(size_t) i]; sqw += qWet[(size_t) i]; ++qn; }
    const float staticGain = qn > 0 ? (float) ((sqw - sqd) / qn) : 0.0f;

    const float grLoud = (loudDry - loudWet) + staticGain;

    juce::String j;
    j << "{\n";
    j << "  \"tool\": \"comp_render_check/1\",\n";
    j << "  \"plugin\": \"" << pick->name << "\",\n";
    j << "  \"manufacturer\": \"" << pick->manufacturerName << "\",\n";
    j << "  \"plugin_id\": \"" << pick->fileOrIdentifier << "\",\n";
    j << "  \"version\": \"" << pick->version << "\",\n";
    j << "  \"sample_rate\": " << juce::String (o.sr, 0) << ",\n";
    j << "  \"signal\": \"" << signalName << "\",\n";
    j << "  \"seconds\": " << juce::String (in.getNumSamples() / o.sr, 2) << ",\n";
    j << "  \"window\": \"400ms_rms_p95\",\n";
    j << "  \"controls\": [\n";
    for (size_t i = 0; i < applied.size(); ++i)
        j << "    " << (i ? "," : "") << "{ \"control\": \"" << applied[i].control << "\", \"asked\": \""
          << applied[i].asked << "\", \"landed\": \"" << applied[i].landed << "\", \"norm\": "
          << juce::String (applied[i].norm, 4) << ", \"ok\": " << (applied[i].ok ? "true" : "false") << " }\n";
    j << "  ],\n";
    j << "  \"loud_windows\": " << loudWindows << ",\n";
    j << "  \"loud_rms_in_dbfs\": " << juce::String (loudDry, 2) << ",\n";
    j << "  \"loud_rms_out_dbfs\": " << juce::String (loudWet, 2) << ",\n";
    j << "  \"static_gain_db\": " << juce::String (staticGain, 2) << ",\n";
    j << "  \"gr_loud_db\": " << juce::String (grLoud, 2) << "\n";
    if (o.tone)
    {
        // ON A TONE every window is the same window, so "the loud phrases" is the tone itself and gr_loud_db IS the
        // tone's gain reduction. Reported under its own name so a tone run cannot be mistaken for a vocal one.
        j << "  ,\"tone_check\": {\n";
        j << "     \"requested_rms_dbfs\": " << juce::String (o.toneDbfs, 2) << ",\n";
        j << "     \"rendered_rms_dbfs\": " << juce::String (juce::jmin (o.toneDbfs, -3.0103f), 2) << ",\n";
        j << "     \"clamped_to_full_scale\": " << (toneClamped ? "true" : "false") << ",\n";
        j << "     \"gr_db\": " << juce::String (grLoud, 2) << ",\n";
        j << "     \"tolerance_db\": 0.5,\n";
        j << "     \"note\": \"spec section 8: a 997 Hz tone at L must land within 0.5 dB of the target g\"\n";
        j << "  }\n";
    }
    j << "}\n";
    std::printf ("%s", j.toRawUTF8());
    if (o.jsonOut.isNotEmpty()) juce::File (o.jsonOut).replaceWithText (j);
    return 0;
}

} // namespace

int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    const auto o = parseArgs (argc, argv);
    if (o.id.isEmpty())
    {
        std::printf ("comp_render_check - COMP_PROFILE_SPEC_v1 section 8 acceptance check\n"
                     "  --id \"AudioUnit:Effects/aufx,dcmp,appl\" --set \"Compression Threshold=-30\"\n"
                     "  [--tone <dBFS>]  a 997 Hz sine at that RMS instead of the vocal-shaped signal, for the\n"
                     "                   section 8 tone check: GR on it must be within 0.5 dB of the target.\n"
                     "  [--name \"...\"] [--wav in.wav] [--sr 48000] [--seconds 20] [--json out.json]\n"
                     "\n"
                     "  --id is REQUIRED and names ONE AudioComponent. This tool never scans: resolving a name by\n"
                     "  scanning asks every component on the machine what it contains, which for licence-bound\n"
                     "  plugins means loading them - on 1 Oct 2026 that drove iLok prompts and crashes on Sean's\n"
                     "  Mac and the process was killed twice. Licence-bound ids (Waves/UAD/PACE) are REFUSED here;\n"
                     "  EJ Maps owns those (spec section 4).\n");
        return 1;
    }
    return run (o);
}
