/*
    EedReverbProcessor.cpp  —  see EedReverbProcessor.h.
*/

#include "EedReverbProcessor.h"
#include "EedReverbEditor.h"
#include "EedDeviceRegistry.h"

#include <cmath>

// ---------------------------------------------------------------------------
// the dialable contract
// ---------------------------------------------------------------------------
// Ranges are the SAME numbers ReverbEngine clamps to. The descriptions are
// written for a model choosing between them, so each says what the knob is FOR
// rather than what it is called: "size" and "decay_s" both make a reverb longer,
// and the difference between them is the whole reason a model needs prose.
const echojay::ParamSchema& EedReverbProcessor::schema()
{
    using E = echojay::ReverbEngine;

    static const echojay::ParamSchema s ({
        { kSize, "%",
          (double) E::kMinSizePct, (double) E::kMaxSizePct, 55.0,
          "how big the space is; changes the spacing of the reflections, "
          "so small is a room and large is a hall even at the same decay", false },

        { kDecayS, "s",
          (double) E::kMinDecaySec, (double) E::kMaxDecaySec, 2.0,
          "how long the tail takes to fall 60 dB; this is length, "
          "size is character", false },

        { kPredelayMs, "ms",
          (double) E::kMinPredelayMs, (double) E::kMaxPredelayMs, 20.0,
          "silence between the dry signal and the reverb; raise it to keep "
          "a vocal or snare in front of its own tail", false },

        { kDamping, "%",
          (double) E::kMinDampingPct, (double) E::kMaxDampingPct, 45.0,
          "how fast the highs die inside the tail; 0 is a tiled room, "
          "100 is a heavily furnished one", false },

        { kLowCutHz, "Hz",
          (double) E::kMinLowCutHz, (double) E::kMaxLowCutHz, 80.0,
          "high-pass inside the tail; raise it to keep the reverb out of the "
          "bass instead of turning the reverb down", false },

        { kWidth, "%",
          (double) E::kMinWidthPct, (double) E::kMaxWidthPct, 100.0,
          "stereo spread of the tail; 0 collapses it to mono, "
          "100 is the network's natural stereo", false },

        { kEarlyLate, "%",
          (double) E::kMinEarlyLatePct, (double) E::kMaxEarlyLatePct, 70.0,
          "balance between early reflections and the late tail; low reads as "
          "a nearby surface, high as a distant wash", false },

        { kModDepth, "%",
          (double) E::kMinModDepthPct, (double) E::kMaxModDepthPct, 25.0,
          "movement inside the tail; a little breaks up the metallic ring "
          "of a static network, a lot is a chorused shimmer", false },

        { kMix, "%",
          (double) E::kMinMixPct, (double) E::kMaxMixPct, 30.0,
          "wet/dry balance; 0 is bypass-clean dry, 100 is tail only", false },

        // ---- the depth pass (DEVICE_DEPTH_PLAN.md, Time) -------------------
        // THE marquee control, and the one a model should set FIRST: it decides
        // what kind of space this is, and every other knob then shapes that space
        // rather than defining it. Each description says what the algorithm is
        // FOR, because "put it in a room" and "put it on a plate" are the same
        // sentence with different answers.
        { kAlgorithm, "",
          0.0, (double) (echojay::kNumReverbAlgorithms - 1),
          (double) (int) echojay::ReverbAlgorithm::Hall,
          "the kind of space, which reshapes the reflection pattern, density, "
          "brightness and early/late balance: room is small and tight with audible "
          "walls, hall is big and smooth, plate is bright and very diffuse with NO "
          "distinct early reflections (the classic vocal reverb), spring is short "
          "and resonant with a boingy dispersed character, ambience is a very short "
          "subtle space that is mostly early reflections. decay_s still sets the "
          "length in all five, so switching changes character and not time",
          false, { "room", "hall", "plate", "spring", "ambience" } },

        { kDiffusion, "%",
          (double) E::kMinDiffusionPct, (double) E::kMaxDiffusionPct,
          (double) echojay::kReverbDiffusionUnityPct,
          "density of the tail: low leaves individual reflections audible (grainier, "
          "more like a real room), high smears them into a smooth wash. Raise it for "
          "vocals, lower it for drums where you want the reflections to be heard",
          false },

        { kDuck, "%",
          (double) E::kMinDuckPct, (double) E::kMaxDuckPct, 0.0,
          "how much the tail ducks under the DRY signal, so the reverb ebbs while "
          "the source plays and swells in the gaps. This is how a big reverb stays "
          "out of the way of a vocal; 0 is off, 40-70 is the usual range", false },
    });
    return s;
}

bool EedReverbProcessor::setParamValue (const juce::String& id, double value)
{
    // `value` arrives ALREADY clamped to the schema range — never re-clamp here.
    if (id == kSize)       { engine_.setSizePct ((float) value);      return true; }
    if (id == kDecayS)     { engine_.setDecaySeconds ((float) value); return true; }
    if (id == kPredelayMs) { engine_.setPredelayMs ((float) value);   return true; }
    if (id == kDamping)    { engine_.setDampingPct ((float) value);   return true; }
    if (id == kLowCutHz)   { engine_.setLowCutHz ((float) value);     return true; }
    if (id == kWidth)      { engine_.setWidthPct ((float) value);     return true; }
    if (id == kEarlyLate)  { engine_.setEarlyLatePct ((float) value); return true; }
    if (id == kModDepth)   { engine_.setModDepthPct ((float) value);  return true; }
    if (id == kMix)        { engine_.setMixPct ((float) value);       return true; }
    if (id == kDiffusion)  { engine_.setDiffusionPct ((float) value); return true; }
    if (id == kDuck)       { engine_.setDuckPct ((float) value);      return true; }

    if (id == kAlgorithm)
    {
        engine_.setAlgorithm (
            echojay::reverbAlgorithmFromIndex ((int) std::lround (value)));
        return true;
    }
    return false;
}

double EedReverbProcessor::getParamValue (const juce::String& id) const
{
    if (id == kSize)       return (double) engine_.getSizePct();
    if (id == kDecayS)     return (double) engine_.getDecaySeconds();
    if (id == kPredelayMs) return (double) engine_.getPredelayMs();
    if (id == kDamping)    return (double) engine_.getDampingPct();
    if (id == kLowCutHz)   return (double) engine_.getLowCutHz();
    if (id == kWidth)      return (double) engine_.getWidthPct();
    if (id == kEarlyLate)  return (double) engine_.getEarlyLatePct();
    if (id == kModDepth)   return (double) engine_.getModDepthPct();
    if (id == kMix)        return (double) engine_.getMixPct();
    if (id == kAlgorithm)  return (double) (int) engine_.getAlgorithm();
    if (id == kDiffusion)  return (double) engine_.getDiffusionPct();
    if (id == kDuck)       return (double) engine_.getDuckPct();
    return 0.0;
}

// ---------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------
void EedReverbProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine_.prepare (sampleRate, samplesPerBlock);
    // Empty the network and snap the smoothers: a restored session starts AT
    // its settings, and the previous take's tail does not survive a stop.
    engine_.reset();
}

void EedReverbProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // Clear any output channel the host gave us that has no input behind it,
    // otherwise it carries whatever was left in the buffer.
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    if (isBypassed()) return;

    const int numCh = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels());
    if (numCh <= 0) return;

    float* l = buffer.getWritePointer (0);
    float* r = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

    engine_.process (l, r, buffer.getNumSamples());
}

double EedReverbProcessor::getTailLengthSeconds() const
{
    // The predelay is part of the tail's wall-clock length, and the decay is
    // measured to -60 dB — a little past that is still audible under a fade, so
    // report a touch more than the nominal rather than exactly it.
    const double pre = (double) engine_.getPredelayMs() * 0.001;
    return pre + engine_.effectiveDecaySeconds() * 1.2;
}

juce::AudioProcessorEditor* EedReverbProcessor::createEditor()
{
    return new EedReverbEditor (*this);
}

// ---------------------------------------------------------------------------
// registration — the ENTIRE integration of this device
// ---------------------------------------------------------------------------
namespace
{
    BuiltinDevice makeReverbDevice()
    {
        BuiltinDevice d;
        d.name            = "EchoJay Reverb";
        d.category        = "Time";
        d.descriptiveName = "EchoJay algorithmic reverb (built in)";

        // ASCII ONLY: juce::String's const char* constructor reads its input as
        // ASCII, so a UTF-8 dash here ships mojibake into the AI prompt.
        d.summary         = "Feedback-delay-network reverb with five algorithms (room, "
                            "hall, plate, spring, ambience), early reflections, predelay, "
                            "damping, diffusion, width and ducking from the dry signal. "
                            "Reach for it to put a source in a space; set algorithm first "
                            "for the KIND of space, then decay_s for how long it rings "
                            "and duck to keep it out of the way of a vocal. No latency.";
        d.identifier      = "echojay:builtin:reverb";
        d.uid             = 0x456A5256;   // 'EjRV' - frozen once shipped
        d.aliases         = { "EchoJayReverb", "EchoJay Room", "EchoJay Hall",
                              "EchoJay Verb" };
        d.schema          = EedReverbProcessor::schema();
        d.create          = [] { return std::make_unique<EedReverbProcessor>(); };
        return d;
    }

    // File-scope static: this line, and nothing else, integrates the device.
    const BuiltinDeviceRegistrar reverbRegistrar { makeReverbDevice() };
}
