/*
    EedChorusProcessor.cpp  —  see EedChorusProcessor.h.
*/

#include "EedChorusProcessor.h"
#include "EedChorusEditor.h"
#include "EedDeviceRegistry.h"
#include "EedModTempo.h"

#include <cmath>

// ---------------------------------------------------------------------------
// the dialable contract
// ---------------------------------------------------------------------------
const echojay::ParamSchema& EedChorusProcessor::schema()
{
    using L = echojay::LfoCore;
    using C = echojay::ChorusEngine;

    static const echojay::ParamSchema s ({
        { kRateHz, "Hz",
          (double) L::kMinRateHz, (double) L::kMaxRateHz, 0.6,
          "how fast the voices drift; ignored while sync is on", false },

        { kSync, "",
          0.0, 1.0, 0.0,
          "lock the rate to the host tempo and use sync_division instead", true },

        { kDivision, "",
          0.0, (double) (L::kNumDivisions - 1), (double) L::kDefaultDivision,
          "tempo division while sync is on: 0 = 4/1 slowest, 6 = 1/4, "
          "12 = 1/32 fastest", false },

        { kDepth, "%",
          (double) L::kMinDepth, (double) L::kMaxDepth, 40.0,
          "how far the delay time is swept, which is how much pitch wobble "
          "the voices get; the sweep scales with delay_ms", false },

        { kDelayMs, "ms",
          (double) C::kMinDelayMs, (double) C::kMaxDelayMs, 12.0,
          "the delay the voices sit at; under ~8 is flanger-like and metallic, "
          "10-25 is classic chorus, above that reads as a doubling", false },

        { kVoices, "",
          (double) C::kMinVoices, (double) C::kMaxVoices, 2.0,
          "how many delayed copies, spread evenly round the LFO cycle; "
          "more is thicker, at the same level", false },

        { kFeedback, "%",
          (double) C::kMinFeedback, (double) C::kMaxFeedback, 0.0,
          "how much of the wet signal is fed back in; positive is resonant, "
          "negative inverts and hollows it out, 0 is a plain chorus", false },

        { kMix, "%",
          (double) C::kMinMix, (double) C::kMaxMix, 50.0,
          "how much of the chorused signal is heard; 0 is bypass, "
          "50 is the classic half-and-half", false },

        // ---- the depth pass (DEVICE_DEPTH_PLAN.md, Modulation) -------------
        // THE marquee control: it decides what KIND of chorus this is, and the
        // other knobs then shape that rather than defining it.
        { kMode, "",
          0.0, (double) (echojay::kNumChorusModes - 1),
          (double) (int) echojay::ChorusMode::Classic,
          "the chorus character: classic is the standard drifting chorus the "
          "device shipped with, ensemble stacks two extra staggered voices "
          "with per-voice detune for a lush string-machine thickness, "
          "dimension freezes the sweep entirely for a very wide stereo "
          "double with NO pitch wobble (rate, depth and sync do nothing "
          "there) - the classic subtle widener for sources that must stay "
          "in tune",
          false, { "classic", "ensemble", "dimension" } },

        { kSpread, "%",
          (double) C::kMinSpread, (double) C::kMaxSpread,
          (double) C::kDefaultSpreadPct,
          "stereo width of the voices: 0 keeps both channels in step "
          "(mono-safe and narrow), 50 is the classic quarter-cycle offset, "
          "100 sweeps the channels fully counter-phase (widest, folds "
          "worst to mono)", false },

        { kTone, "dB",
          (double) C::kMinToneDb, (double) C::kMaxToneDb, 0.0,
          "tilt on the wet voices only: negative darkens them so they sit "
          "behind the dry signal, positive brightens them in front of it; "
          "0 leaves them flat", false },
    });
    return s;
}

bool EedChorusProcessor::setParamValue (const juce::String& id, double value)
{
    auto& lfo = engine_.lfo();

    if (id == kRateHz)   { lfo.setRateHz            ((float) value); return true; }
    if (id == kSync)     { lfo.setTempoSync         (value >= 0.5);  return true; }
    if (id == kDivision) { lfo.setDivisionIndex     ((int) std::lround (value)); return true; }
    if (id == kDepth)    { lfo.setDepthPercent      ((float) value); return true; }
    if (id == kDelayMs)  { engine_.setDelayMs       ((float) value); return true; }
    if (id == kVoices)   { engine_.setVoices        ((int) std::lround (value)); return true; }
    if (id == kFeedback) { engine_.setFeedbackPercent ((float) value); return true; }
    if (id == kMix)      { engine_.setMixPercent    ((float) value); return true; }
    if (id == kSpread)   { engine_.setSpreadPercent ((float) value); return true; }
    if (id == kTone)     { engine_.setToneDb        ((float) value); return true; }
    if (id == kMode)
    {
        engine_.setMode (echojay::chorusModeFromIndex ((int) std::lround (value)));
        return true;
    }
    return false;
}

double EedChorusProcessor::getParamValue (const juce::String& id) const
{
    const auto& lfo = engine_.lfo();

    if (id == kRateHz)   return (double) lfo.getRateHz();
    if (id == kSync)     return lfo.getTempoSync() ? 1.0 : 0.0;
    if (id == kDivision) return (double) lfo.getDivisionIndex();
    if (id == kDepth)    return (double) lfo.getDepthPercent();
    if (id == kDelayMs)  return (double) engine_.getDelayMs();
    if (id == kVoices)   return (double) engine_.getVoices();
    if (id == kFeedback) return (double) engine_.getFeedbackPercent();
    if (id == kMix)      return (double) engine_.getMixPercent();
    if (id == kMode)     return (double) (int) engine_.getMode();
    if (id == kSpread)   return (double) engine_.getSpreadPercent();
    if (id == kTone)     return (double) engine_.getToneDb();
    return 0.0;
}

// ---------------------------------------------------------------------------
// audio
// ---------------------------------------------------------------------------
void EedChorusProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine_.prepare (sampleRate, samplesPerBlock);
    engine_.reset();
}

void EedChorusProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    if (isBypassed()) return;

    echojay::pushHostTempo (*this, engine_.lfo());

    const int numCh = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels());
    if (numCh <= 0) return;

    float* l = buffer.getWritePointer (0);
    float* r = numCh > 1 ? buffer.getWritePointer (1) : nullptr;

    engine_.process (l, r, buffer.getNumSamples());
}

juce::AudioProcessorEditor* EedChorusProcessor::createEditor()
{
    return new EedChorusEditor (*this);
}

// ---------------------------------------------------------------------------
// registration — the ENTIRE integration of this device
// ---------------------------------------------------------------------------
namespace
{
    BuiltinDevice makeChorusDevice()
    {
        BuiltinDevice d;
        d.name            = "EchoJay Chorus";
        d.category        = "Modulation";
        d.descriptiveName = "EchoJay chorus (built in)";
        // ASCII ONLY - see the note in DeviceTemplate/README.md.
        d.summary         = "Several detuned, drifting copies of the signal. Reach for "
                            "it to thicken a thin part and widen it - guitars, synths, "
                            "backing vocals - or, at short delay times with feedback, "
                            "for a flanger-like sweep. Three characters via mode: "
                            "classic, ensemble (lush stacked voices), dimension (very "
                            "wide with zero pitch wobble - the safe widener).";
        d.identifier      = "echojay:builtin:chorus";
        d.uid             = 0x456A4348;   // 'EjCH' - frozen once shipped
        d.aliases         = { "EchoJayChorus", "Chorus" };
        d.schema          = EedChorusProcessor::schema();
        d.create          = [] { return std::make_unique<EedChorusProcessor>(); };
        return d;
    }

    const BuiltinDeviceRegistrar chorusRegistrar { makeChorusDevice() };
}
