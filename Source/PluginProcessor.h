#pragma once
#include <JuceHeader.h>
#include "MeterEngine.h"
#include "PluginScanner.h"
#include "ReferenceAnalyser.h"
#include "WaveformRecorder.h"

enum class ChannelType {
    FullMix = 0,
    // Vocals
    LeadVocal, BackingVocal, Adlibs, VocalBus,
    // Drums
    Kick, Snare, HiHat, Overheads, DrumBus, Percussion,
    // Bass
    Bass808, BassGuitar, SubBass, SynthBass,
    // Keys & Guitar
    Piano, Keys, AcousticGuitar, ElectricGuitar, GuitarBus,
    // Synths
    SynthLead, SynthPad, SynthPluck, SynthBus,
    // Strings & Brass
    Strings, Brass, Woodwind, Orchestral,
    // FX & Other
    FX, Reverb, Delay, Foley, Ambient,
    // Buses
    MasterBus, InstrumentBus, MusicBus,
    // Custom
    Other
};

static const juce::StringArray channelTypeNames = {
    "Mix Bus",
    "Lead Vocal", "Backing Vocal", "Adlibs", "Vocal Bus",
    "Kick", "Snare", "Hi-Hat", "Overheads", "Drum Bus", "Percussion",
    "Bass / 808", "Bass Guitar", "Sub Bass", "Synth Bass",
    "Piano", "Keys", "Acoustic Guitar", "Electric Guitar", "Guitar Bus",
    "Synth Lead", "Synth Pad", "Synth Pluck", "Synth Bus",
    "Strings", "Brass", "Woodwind", "Orchestral",
    "FX", "Reverb", "Delay", "Foley", "Ambient",
    "Master Bus", "Instrument Bus", "Music Bus",
    "Other"
};

enum class CaptureState { Idle, Capturing, Complete };

struct CaptureSnapshot {
    juce::String id;
    juce::String name;
    ChannelType channelType;
    juce::String customChannelName;
    MeterData averagedData;
    juce::int64 timestamp;
    float durationSeconds;
    std::vector<float> waveformThumbnail;
    std::array<float, 64> eqCurve = {};
    juce::String wavFilePath;
    
    juce::String getChannelDisplayName() const {
        if (channelType == ChannelType::Other && customChannelName.isNotEmpty())
            return customChannelName;
        return channelTypeNames[(int)channelType];
    }
};

class EchoJayProcessor : public juce::AudioProcessor
{
public:
    EchoJayProcessor();
    ~EchoJayProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    MeterEngine& getMeterEngine() { return meterEngine; }
    PluginScanner& getPluginScanner() { return pluginScanner; }
    ReferenceAnalyser& getReferenceAnalyser() { return refAnalyser; }
    WaveformRecorder& getWaveformRecorder() { return waveformRecorder; }

    // Save captured audio to WAV in the project/capture folder
    juce::String saveCaptureWAV();
    juce::File getCaptureFolder() const;
    
    // Build AI compare context between a capture and a reference
    juce::String buildCompareContext(const CaptureSnapshot& capture, const ReferenceResult& reference) const;
    // Build AI compare context between two captures
    juce::String buildCompareContext(const CaptureSnapshot& a, const CaptureSnapshot& b) const;

    ChannelType getChannelType() const { return channelType; }
    void setChannelType(ChannelType t);
    juce::String getCustomChannelName() const { return customChannelName; }
    void setCustomChannelName(const juce::String& name) { customChannelName = name; }
    juce::String getEffectiveChannelName() const;
    
    bool isChannelTypePromptDismissed() const { return channelTypePromptDismissed; }
    void setChannelTypePromptDismissed(bool dismissed);

    juce::String getGenre() const { return genre; }
    void setGenre(const juce::String& g) { genre = g; }

    CaptureState getCaptureState() const { return captureState.load(); }
    void startCapture();
    void stopCapture();
    void resetCapture();
    float getCaptureDuration() const;

    std::vector<CaptureSnapshot> getSnapshots() const;
    void renameSnapshot(int index, const juce::String& newName);
    void deleteSnapshot(int index);
    CaptureSnapshot getLatestSnapshot() const;
    int getSnapshotCount() const;

    // Returns true once when auto-feedback is ready (consumed on read)
    bool shouldAutoFeedback() const { return autoFeedbackReady.exchange(false); }
    bool isAudioSilent() const { return audioSilent.load(); }
    bool isTransportPlaying() const { return transportPlaying.load(); }

    // Chat history — stored here so it persists when the editor is destroyed/recreated
    struct ChatEntry { 
        juce::String role, content; 
        bool hasWaveform = false;
        std::vector<float> waveform;  // simplified thumbnail (peak values)
        float durationSeconds = 0.0f;
        float lufs = -100.0f;
        juce::String wavFilename;
        juce::String wavFilePath;
    };
    std::vector<ChatEntry> chatHistory;
    juce::StringArray chatRoles, chatContents; // for API context window

private:
    MeterEngine meterEngine;       // Live meters (always running)
    MeterEngine captureEngine;     // Capture pass meters (reset each capture)
    PluginScanner pluginScanner;
    ReferenceAnalyser refAnalyser;
    WaveformRecorder waveformRecorder; // Audio recording + waveform thumbnail

    ChannelType channelType { ChannelType::FullMix };
    juce::String customChannelName;
    bool channelTypePromptDismissed = false;
    juce::String genre { "hip-hop" };

    // Auto-detection
    
    
    // Spectrum accumulators during capture — both maintained simultaneously
    std::array<float, 64> spectrumPeak = {};   // peak-hold (for individual channels)
    std::array<float, 64> spectrumSum = {};     // running sum (for average on buses/mixes)
    int spectrumFrames = 0;

    // Capture
    std::atomic<CaptureState> captureState { CaptureState::Idle };
    juce::int64 captureStartTime = 0;
    int captureSampleCount = 0;
    mutable std::mutex snapshotMutex;
    std::vector<CaptureSnapshot> snapshots;
    int passCounter = 0;

    // Auto-feedback
    mutable std::atomic<bool> autoFeedbackReady { false };

    // Silence detection (triggers auto-stop when DAW stops)
    std::atomic<bool> audioSilent { true };
    std::atomic<bool> transportPlaying { false };
    bool wasTransportPlaying = false;
    int silenceCounter = 0;
    bool wasReceivingAudio = false;

    // Background WAV save thread — destructor waits for it to finish
    std::unique_ptr<juce::Thread> saveThread;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EchoJayProcessor)
};
