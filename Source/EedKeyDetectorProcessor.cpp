/*
    EedKeyDetectorProcessor.cpp  —  see EedKeyDetectorProcessor.h.
*/

#include "EedKeyDetectorProcessor.h"
#include "EedKeyDetectorEditor.h"
#include "EedDeviceRegistry.h"

using echojay::KeyEngine;

// ---------------------------------------------------------------------------
// the dialable contract
// ---------------------------------------------------------------------------
// Every param a human can reach in the editor is here, which is what lets the
// model reach it too — including `analyse` itself: an AI-triggered "listen
// again and tell me the key" is the same shape as the EQ's tame_resonances.
const echojay::ParamSchema& EedKeyDetectorProcessor::schema()
{
    static const echojay::ParamSchema s ({
        { EedKeyDetectorProcessor::kAnalyse, "", 0.0, 1.0, 0.0,
          "set 1 to START a committed analysis pass: the device listens to the "
          "next window_s of playback, commits the key and holds it - the "
          "primary control; reads 1 while listening", true },

        { EedKeyDetectorProcessor::kWindowS, "s",
          (double) KeyEngine::kMinWindowS, (double) KeyEngine::kMaxWindowS,
          (double) KeyEngine::kDefWindowS,
          "how much audio a committed pass listens to before it commits", false },

        { EedKeyDetectorProcessor::kContinuous, "", 0.0, 1.0, 0.0,
          "keep updating the reading instead of committing once", true },

        { EedKeyDetectorProcessor::kSensitivity, "%", 0.0, 100.0, 50.0,
          "how readily the reading switches key: low = sticky, high = follows "
          "modulations quickly", false },

        { EedKeyDetectorProcessor::kTuningHz, "Hz",
          (double) KeyEngine::kMinTuningHz, (double) KeyEngine::kMaxTuningHz, 440.0,
          "reference pitch HINT for the analysis; auto_tuning overrides it "
          "with the detected value", false },

        { EedKeyDetectorProcessor::kAutoTuning, "", 0.0, 1.0, 1.0,
          "detect the track's actual reference pitch instead of assuming the "
          "hint; the detected value is reported as detected_tuning", true },

        { EedKeyDetectorProcessor::kModeLock, "", 0.0, 2.0, 0.0,
          "constrain the search when the user already knows the mode",
          false, { "auto", "major", "minor" } },

        { EedKeyDetectorProcessor::kHold, "", 0.0, 1.0, 0.0,
          "freeze the current reading", true },

        { EedKeyDetectorProcessor::kHpss, "", 0.0, 1.0, 1.0,
          "harmonic/percussive separation - leave on for mixes; off saves CPU "
          "on solo tonal sources", true },

        { EedKeyDetectorProcessor::kLowHz, "Hz",
          (double) KeyEngine::kMinLowHz, (double) KeyEngine::kMaxLowHz,
          (double) KeyEngine::kDefLowHz,
          "bottom of the analysis band", false },

        { EedKeyDetectorProcessor::kHighHz, "Hz",
          (double) KeyEngine::kMinHighHz, (double) KeyEngine::kMaxHighHz,
          (double) KeyEngine::kDefHighHz,
          "top of the analysis band", false },

        { EedKeyDetectorProcessor::kReset, "", 0.0, 1.0, 0.0,
          "set 1 to clear the accumulated audio and the held reading", true },
    });
    return s;
}

bool EedKeyDetectorProcessor::setParamValue (const juce::String& id, double value)
{
    // `analyse` and `reset` are MOMENTARY actions dressed as switches — the
    // shape the schema can carry. Setting 1 fires them; they read back as
    // state (collecting / always 0), so a state restore of "0" is a no-op
    // rather than a phantom trigger.
    if (id == kAnalyse)
    {
        if (value >= 0.5) engine_.startAnalysis();
        else              engine_.cancelAnalysis();
        worker_.notify();   // preempt the 250 ms tick: a manual trigger that
                            // waits for a timer reads as a broken device
        return true;
    }
    if (id == kReset)
    {
        if (value >= 0.5) engine_.clearAccumulation();
        worker_.notify();
        return true;
    }
    if (id == kWindowS)     { engine_.setWindowSeconds ((float) value); return true; }
    if (id == kContinuous)  { engine_.setContinuous (value >= 0.5);     return true; }
    if (id == kSensitivity) { engine_.setSensitivity ((float) value);   return true; }
    if (id == kTuningHz)    { engine_.setTuningHint ((float) value);    return true; }
    if (id == kAutoTuning)  { engine_.setAutoTuning (value >= 0.5);     return true; }
    if (id == kModeLock)    { engine_.setModeLock ((int) std::lround (value)); return true; }
    if (id == kHold)        { engine_.setHold (value >= 0.5);           return true; }
    if (id == kHpss)        { engine_.setHpss (value >= 0.5);           return true; }
    if (id == kLowHz)       { engine_.setLowHz ((float) value);         return true; }
    if (id == kHighHz)      { engine_.setHighHz ((float) value);        return true; }
    return false;
}

double EedKeyDetectorProcessor::getParamValue (const juce::String& id) const
{
    if (id == kAnalyse)     return engine_.isCollecting() ? 1.0 : 0.0;
    if (id == kReset)       return 0.0;
    if (id == kWindowS)     return (double) engine_.getWindowSeconds();
    if (id == kContinuous)  return engine_.getContinuous() ? 1.0 : 0.0;
    if (id == kSensitivity) return (double) engine_.getSensitivity();
    if (id == kTuningHz)    return (double) engine_.getTuningHint();
    if (id == kAutoTuning)  return engine_.getAutoTuning() ? 1.0 : 0.0;
    if (id == kModeLock)    return (double) engine_.getModeLock();
    if (id == kHold)        return engine_.getHold() ? 1.0 : 0.0;
    if (id == kHpss)        return engine_.getHpss() ? 1.0 : 0.0;
    if (id == kLowHz)       return (double) engine_.getLowHz();
    if (id == kHighHz)      return (double) engine_.getHighHz();
    return 0.0;
}

// ---------------------------------------------------------------------------
// audio — a READER: the buffer is never written
// ---------------------------------------------------------------------------
void EedKeyDetectorProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine_.prepare (sampleRate, samplesPerBlock);
    if (! workerStarted_)
    {
        workerStarted_ = true;
        worker_.startThread();
    }
}

void EedKeyDetectorProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // Clear any output channel with no input behind it — the one write this
    // device is allowed, and it never touches a channel that carries signal.
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    if (isBypassed()) return;

    const int numCh = juce::jmin (buffer.getNumChannels(), getTotalNumInputChannels());
    if (numCh <= 0) return;

    // Read-only tap. The audio path is bit-identical BY CONSTRUCTION: there
    // is no code below that writes the buffer, at any setting.
    engine_.pushBlock (buffer.getReadPointer (0),
                       numCh > 1 ? buffer.getReadPointer (1) : nullptr,
                       buffer.getNumSamples());
}

juce::AudioProcessorEditor* EedKeyDetectorProcessor::createEditor()
{
    return new EedKeyDetectorEditor (*this);
}

// ---------------------------------------------------------------------------
// registration — the ENTIRE integration of this device
// ---------------------------------------------------------------------------
namespace
{
    BuiltinDevice makeKeyDetectorDevice()
    {
        BuiltinDevice d;
        d.name            = "EchoJay Key Detector";
        d.category        = "Analysis";      // new category; ranked in
                                             // EedDeviceRegistry.cpp's order
        d.descriptiveName = "EchoJay musical key detector (built in)";

        // ASCII ONLY (see the template's warning about mojibake in the feed).
        d.summary         = "READER, not an effect: audio passes through "
                            "bit-identically while it detects the musical key, "
                            "confidence and reference tuning of whatever flows "
                            "through the chain. Add it when a move depends on "
                            "the key; set analyse:1 to run a listening pass, "
                            "then read [DETECTED KEY].";

        // Frozen once shipped (saved chain XML carries both).
        d.identifier      = "echojay:builtin:keydetector";
        d.uid             = 0x456A4B44;      // 'EjKD'

        d.aliases         = { "EchoJayKeyDetector", "Key Detector", "KeyDetector" };
        d.schema          = EedKeyDetectorProcessor::schema();
        d.create          = [] { return std::make_unique<EedKeyDetectorProcessor>(); };
        return d;
    }

    const BuiltinDeviceRegistrar keyDetectorRegistrar { makeKeyDetectorDevice() };
}
