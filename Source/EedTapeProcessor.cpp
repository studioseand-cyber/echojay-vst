/*
    EedTapeProcessor.cpp  —  see EedTapeProcessor.h.
*/

#include "EedTapeProcessor.h"
#include "EedTapeEditor.h"
#include "EedDeviceRegistry.h"

using echojay::TapeEngine;

EedTapeProcessor::EedTapeProcessor()
{
    resetParamsToDefaults();
}

// ---------------------------------------------------------------------------
// the dialable contract
// ---------------------------------------------------------------------------
const echojay::ParamSchema& EedTapeProcessor::schema()
{
    // SPEED is a continuous number rather than a three-way switch on purpose.
    // A machine's speed is a real quantity that scales the head bump, the HF
    // loss and the wobble depth together; advertising it as a number lets the
    // model ask for 22 ips and get exactly the halfway point, instead of being
    // forced onto whichever of three presets the UI happened to pick.
    static const echojay::ParamSchema s ({
        { kSpeedIps, "ips",
          (double) TapeEngine::kMinSpeedIps, (double) TapeEngine::kMaxSpeedIps, 15.0,
          "tape speed; faster is cleaner and brighter with a higher head bump, "
          "slower is darker with deeper wow and flutter", false, {} },

        { kDriveDb, "dB",
          (double) TapeEngine::kMinDriveDb, (double) TapeEngine::kMaxDriveDb, 6.0,
          "how hard the tape is hit; output is level compensated, so this adds "
          "compression and harmonics rather than volume", false, {} },

        { kBias, "%",
          (double) TapeEngine::kMinBias, (double) TapeEngine::kMaxBias, 0.0,
          "bias offset; 0 is optimal, either direction pushes the signal onto a "
          "more asymmetric part of the curve and brings in even harmonics",
          false, {} },

        { kWow, "%",
          (double) TapeEngine::kMinWow, (double) TapeEngine::kMaxWow, 20.0,
          "slow pitch drift, under 1 Hz - the sound of a tape that will not sit "
          "still", false, {} },

        { kFlutter, "%",
          (double) TapeEngine::kMinFlutter, (double) TapeEngine::kMaxFlutter, 20.0,
          "fast pitch jitter, around 7-12 Hz - the shimmer on a sustained note",
          false, {} },

        { kHeadBump, "dB",
          (double) TapeEngine::kMinBumpDb, (double) TapeEngine::kMaxBumpDb, 2.0,
          "low-frequency lift at the head bump, whose frequency follows tape "
          "speed (about 50 Hz at 15 ips)", false, {} },

        // ---- the depth pass ------------------------------------------------
        // The machine SCALES what the other knobs already do rather than
        // replacing them, so its description leads with what each one sounds
        // like — that text is the whole interface the model dials it through.
        { kMode, "", 0.0, (double) (echojay::kNumTapeMachines - 1),
          (double) (int) echojay::TapeMachine::Studio,
          "the machine: studio is the clean bright 15-30 ips reference (the "
          "default - exactly the device as shipped), vintage is softer with "
          "more compression, deeper wow and a darker top, cassette is narrow "
          "at both ends with the most flutter and the most noise. Each scales "
          "what speed, drive, wow, flutter and head bump already do",
          false, { "studio", "vintage", "cassette" } },

        { kHiss, "%",
          (double) TapeEngine::kMinHiss, (double) TapeEngine::kMaxHiss, 0.0,
          "tape noise, gated to signal presence so it breathes with the "
          "material instead of humming through the silences; cassette mode is "
          "the noisiest. 0 is off", false, {} },

        { kCrosstalk, "%",
          (double) TapeEngine::kMinCrosstalk, (double) TapeEngine::kMaxCrosstalk, 0.0,
          "bleed between the channels at the heads - subtle stereo glue that "
          "narrows the image slightly; the mono sum is untouched. 0 is off",
          false, {} },

        { kMix, "%", 0.0, 100.0, 100.0,
          "dry/wet blend; the dry path is delay matched, so a partial mix does "
          "not comb filter", false, {} },

        { kOutputDb, "dB",
          (double) TapeEngine::kMinOutDb, (double) TapeEngine::kMaxOutDb, 0.0,
          "final output trim", false, {} },
    });
    return s;
}

bool EedTapeProcessor::setParamValue (const juce::String& id, double value)
{
    if (id == kSpeedIps) { engine_.setSpeedIps    ((float) value); return true; }
    if (id == kDriveDb)  { engine_.setDriveDb     ((float) value); return true; }
    if (id == kBias)     { engine_.setBias        ((float) value); return true; }
    if (id == kWow)      { engine_.setWow         ((float) value); return true; }
    if (id == kFlutter)  { engine_.setFlutter     ((float) value); return true; }
    if (id == kHeadBump) { engine_.setHeadBumpDb  ((float) value); return true; }
    if (id == kMix)      { engine_.setMixPercent  ((float) value); return true; }
    if (id == kOutputDb) { engine_.setOutputDb    ((float) value); return true; }
    if (id == kHiss)     { engine_.setHiss        ((float) value); return true; }
    if (id == kCrosstalk){ engine_.setCrosstalk   ((float) value); return true; }
    if (id == kMode)
    {
        engine_.setMachine (echojay::tapeMachineFromIndex ((int) std::lround (value)));
        return true;
    }
    return false;
}

double EedTapeProcessor::getParamValue (const juce::String& id) const
{
    if (id == kSpeedIps) return (double) engine_.getSpeedIps();
    if (id == kDriveDb)  return (double) engine_.getDriveDb();
    if (id == kBias)     return (double) engine_.getBias();
    if (id == kWow)      return (double) engine_.getWow();
    if (id == kFlutter)  return (double) engine_.getFlutter();
    if (id == kHeadBump) return (double) engine_.getHeadBumpDb();
    if (id == kMix)      return (double) engine_.getMixPercent();
    if (id == kOutputDb) return (double) engine_.getOutputDb();
    if (id == kMode)     return (double) (int) engine_.getMachine();
    if (id == kHiss)     return (double) engine_.getHiss();
    if (id == kCrosstalk)return (double) engine_.getCrosstalk();
    return 0.0;
}

// ---------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------
void EedTapeProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine_.prepare (sampleRate, samplesPerBlock);
    engine_.reset();
    spectrumTap_.clear();

    // The transport delay plus the oversampler. Reporting only the oversampler's
    // share would leave the track 2.5 ms early against everything in parallel
    // with it, which reads as a phase problem rather than as a latency one.
    setLatencySamples (engine_.latencySamples());
}

double EedTapeProcessor::getTailLengthSeconds() const
{
    return (double) engine_.latencySamples() / (getSampleRate() > 0.0 ? getSampleRate() : 44100.0);
}

void EedTapeProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    if (isBypassed()) return;

    const int numCh = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels());
    if (numCh <= 0) return;

    float* l = buffer.getWritePointer (0);
    float* r = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

    engine_.process (l, r, buffer.getNumSamples());

    spectrumTap_.write (l, buffer.getNumSamples());
}

juce::AudioProcessorEditor* EedTapeProcessor::createEditor()
{
    return new EedTapeEditor (*this);
}

// ---------------------------------------------------------------------------
// registration — the ENTIRE integration of this device
// ---------------------------------------------------------------------------
namespace
{
    BuiltinDevice makeTapeDevice()
    {
        BuiltinDevice d;
        d.name            = "EchoJay Tape";
        d.category        = "Harmonic";
        d.descriptiveName = "EchoJay tape machine (built in)";
        // ASCII ONLY in registry text - see the note in EedPhaseInvertProcessor.
        d.summary         = "Tape saturation with a real transport: wow, flutter, head bump, "
                            "speed-dependent HF loss, three machines (studio, vintage, "
                            "cassette), gated hiss and channel crosstalk. Reach for it to "
                            "glue a mix, take the edge off something digital, or add "
                            "movement to a static part; set `mode` first - it decides how "
                            "the machine feels - then speed and drive. Reports about 3.4 ms "
                            "of latency (the transport delay plus oversampling), which the "
                            "host compensates.";
        d.identifier      = "echojay:builtin:tape";
        d.uid             = 0x456A5450;   // 'EjTP' - frozen once shipped
        d.aliases         = { "EchoJayTape", "EchoJay Tape Machine", "EchoJay Reel" };
        d.schema          = EedTapeProcessor::schema();
        d.create          = [] { return std::make_unique<EedTapeProcessor>(); };
        return d;
    }

    const BuiltinDeviceRegistrar tapeRegistrar { makeTapeDevice() };
}
