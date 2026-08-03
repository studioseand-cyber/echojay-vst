/*
    EedCompressorProcessor.cpp  —  see EedCompressorProcessor.h.
*/

#include "EedCompressorProcessor.h"
#include "EedCompressorEditor.h"
#include "EedDeviceRegistry.h"

EedCompressorProcessor::EedCompressorProcessor()
{
    core_.setMode (echojay::DynamicsMode::Compress);
    core_.setRmsWindowMs (10.0);

    // Sized before the first prepare, never after: the lookahead ring is
    // allocated in prepareToPlay for this maximum and only ever moves its read
    // pointer afterwards.
    core_.setMaxLookaheadMs (kMaxLookaheadMs);

    // Everything else, including the detector and the character mode, comes from
    // the schema's defaults — one source of truth for what a fresh device is.
    resetParamsToDefaults();
}

// ---------------------------------------------------------------------------
// the dialable contract
// ---------------------------------------------------------------------------
const echojay::ParamSchema& EedCompressorProcessor::schema()
{
    // Descriptions are written for the MODEL, not for a manual: each one says
    // what turning the knob does to the sound, because that is what a request
    // like "make the vocal sit better" has to be translated against.
    static const echojay::ParamSchema s ({
        { kThresholdDb, "dB", -60.0, 0.0, -18.0,
          "level where compression starts; lower means more of the signal is "
          "compressed", false },

        { kRatio, "", 1.0, 20.0, 4.0,
          "how hard it compresses above the threshold; 2 is gentle glue, 4 is "
          "control, 10+ is limiting", false },

        { kAttackMs, "ms", 0.1, 200.0, 10.0,
          "how fast it clamps down; short keeps transients in check, long lets "
          "them through and sounds punchier", false },

        { kReleaseMs, "ms", 5.0, 2000.0, 120.0,
          "how fast it lets go; short is more audible pumping, long is smoother "
          "level riding", false },

        { kKneeDb, "dB", 0.0, 24.0, 6.0,
          "width of the soft transition around the threshold; 0 is a hard corner, "
          "wide is gradual and transparent", false },

        { kMakeupDb, "dB", -12.0, 24.0, 0.0,
          "output gain to put back what the compression took off", false },

        { kMix, "%", 0.0, 100.0, 100.0,
          "blend of compressed against dry; below 100 is parallel compression, "
          "which adds density without losing transients", false },

        // ---- the depth pass ------------------------------------------------
        // Written to be READ BY THE MODEL: each choice says what the mode does to
        // the sound and what material it is for, because "make the drums punchy"
        // has to be translatable against this text alone.
        { kMode, "", 0.0, (double) (echojay::kCharacterCount - 1), 0.0,
          "compressor character, which reshapes attack, release and knee and adds "
          "gentle drive as it works: clean is a transparent VCA (the reference), "
          "glue is a slow soft bus compressor for making a mix cohere, punch is a "
          "fast aggressive FET for drums and bass, smooth is an opto with a "
          "programme-dependent release for vocals and anything delicate",
          false, { "clean", "glue", "punch", "smooth" } },

        { kDetector, "", 0.0, 1.0, 1.0,
          "what the detector measures: rms follows loudness and sounds like a "
          "fader being ridden (right for most material), peak catches every "
          "transient and is tighter on drums and bass",
          false, { "peak", "rms" } },

        { kScHpfHz, "Hz", 0.0, 500.0, 0.0,
          "high-pass on the DETECTOR only, never on the audio: bass no longer "
          "drives the compression, so a loud kick stops pumping the whole mix. "
          "80-150 on a full mix bus; 0 is off", false },

        { kLookaheadMs, "ms", 0.0, kMaxLookaheadMs, 0.0,
          "how far ahead it looks, so a transient is already under control when it "
          "arrives; this is added latency, reported to the host. 0 is zero-latency "
          "and the right choice while a chain is monitored live", false },

        { kAutoRelease, "", 0.0, 1.0, 0.0,
          "release follows the programme: quick after a short transient, slower "
          "after a sustained passage that pulled deep. Use it when one release "
          "time cannot suit both", true },

        { kStereoLink, "%", 0.0, 100.0, 100.0,
          "100 is fully linked - one gain for both channels, so the stereo image "
          "cannot wander. Lower it only for two unrelated mono sources sharing a "
          "stereo track; on a mix, less than 100 moves the image", false },

        { kRangeDb, "dB", 0.0, 40.0, 0.0,
          "the most gain reduction it is allowed to apply; 0 is unlimited. Use it "
          "to cap how far a loud passage can be pulled down without softening the "
          "ratio", false },
    });
    return s;
}

bool EedCompressorProcessor::setParamValue (const juce::String& id, double value)
{
    if (id == kThresholdDb) { core_.setThresholdDb ((float) value); return true; }
    if (id == kRatio)       { core_.setRatio       ((float) value); return true; }
    if (id == kAttackMs)    { core_.setAttackMs    (value);         return true; }
    if (id == kReleaseMs)   { core_.setReleaseMs   (value);         return true; }
    if (id == kKneeDb)      { core_.setKneeDb      ((float) value); return true; }
    if (id == kMakeupDb)    { core_.setMakeupDb    ((float) value); return true; }
    // The schema speaks percent because that is what a user and a model both
    // say; the core speaks 0..1. Converted in exactly one place.
    if (id == kMix)         { core_.setMix ((float) (value * 0.01)); return true; }

    if (id == kMode)
    {
        core_.setCharacter (echojay::characterModeFromIndex ((int) std::lround (value)));
        return true;
    }
    if (id == kDetector)
    {
        core_.setDetectorMode (value >= 0.5 ? echojay::DetectorMode::Rms
                                            : echojay::DetectorMode::Peak);
        return true;
    }
    if (id == kScHpfHz)     { core_.setSidechainHpfHz (value);       return true; }
    if (id == kAutoRelease) { core_.setAutoRelease (value >= 0.5);   return true; }
    if (id == kStereoLink)  { core_.setStereoLink ((float) (value * 0.01)); return true; }
    if (id == kRangeDb)     { core_.setRangeDb ((float) value);      return true; }

    if (id == kLookaheadMs)
    {
        lookaheadMs_ = value;
        core_.setLookaheadMs (value);

        // The number the DAW needs to keep this track in time with every other
        // one. Pushed here as well as from prepareToPlay, because an AI move can
        // change it while the graph is running.
        setLatencySamples (core_.lookaheadSamples());
        return true;
    }
    return false;
}

double EedCompressorProcessor::getParamValue (const juce::String& id) const
{
    if (id == kThresholdDb) return (double) core_.getThresholdDb();
    if (id == kRatio)       return (double) core_.getRatio();
    if (id == kAttackMs)    return core_.getAttackMs();
    if (id == kReleaseMs)   return core_.getReleaseMs();
    if (id == kKneeDb)      return (double) core_.getKneeDb();
    if (id == kMakeupDb)    return (double) core_.getMakeupDb();
    if (id == kMix)         return (double) core_.getMix() * 100.0;

    if (id == kMode)        return (double) (int) core_.getCharacter();
    if (id == kDetector)    return core_.getDetectorMode() == echojay::DetectorMode::Rms
                                   ? 1.0 : 0.0;
    if (id == kScHpfHz)     return core_.getSidechainHpfHz();
    if (id == kLookaheadMs) return lookaheadMs_;
    if (id == kAutoRelease) return core_.isAutoRelease() ? 1.0 : 0.0;
    if (id == kStereoLink)  return (double) core_.getStereoLink() * 100.0;
    if (id == kRangeDb)     return (double) core_.getRangeDb();
    return 0.0;
}

// ---------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------
void EedCompressorProcessor::prepareToPlay (double sampleRate, int)
{
    core_.prepare (sampleRate);
    core_.reset();

    // The ring was just resized for this sample rate, so the sample count the
    // host needs has changed even though the millisecond value has not.
    setLatencySamples (core_.lookaheadSamples());
}

void EedCompressorProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    const int numCh = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels());
    if (numCh <= 0) return;

    float* l = buffer.getWritePointer (0);
    float* r = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

    // BYPASS STILL DELAYS. Once a lookahead is dialled the device is reporting
    // latency and the host is compensating for it whether or not the device is
    // bypassed; returning the signal early would shift this track in time
    // against the rest of the session every time bypass was toggled. So bypass
    // skips the compression and keeps the delay.
    if (isBypassed())
    {
        core_.processDelayOnly (l, r, buffer.getNumSamples());
        return;
    }

    core_.process (l, r, buffer.getNumSamples());
}

juce::AudioProcessorEditor* EedCompressorProcessor::createEditor()
{
    return new EedCompressorEditor (*this);
}

// ---------------------------------------------------------------------------
// registration — the ENTIRE integration of this device
// ---------------------------------------------------------------------------
namespace
{
    BuiltinDevice makeCompressorDevice()
    {
        BuiltinDevice d;
        d.name            = "EchoJay Compressor";
        d.category        = "Dynamics";
        d.descriptiveName = "EchoJay compressor (built in)";
        d.summary         = "Compressor with four character modes (clean VCA, glue bus, "
                            "punch FET, smooth opto), a sidechain high-pass, lookahead, "
                            "auto release, stereo link and a reduction range. Reach for "
                            "it to control level, glue a bus or add density; set `mode` "
                            "first, since it decides how the device feels, then the "
                            "threshold and ratio. Every setting is dialled in real units "
                            "rather than approximated.";
        d.identifier      = "echojay:builtin:compressor";
        d.uid             = 0x456A4350;   // 'EjCP' - frozen once shipped
        d.aliases         = { "EchoJayCompressor", "EchoJay Comp", "EchoJay Compression" };
        d.schema          = EedCompressorProcessor::schema();
        d.create          = [] { return std::make_unique<EedCompressorProcessor>(); };
        return d;
    }

    const BuiltinDeviceRegistrar compressorRegistrar { makeCompressorDevice() };
}
