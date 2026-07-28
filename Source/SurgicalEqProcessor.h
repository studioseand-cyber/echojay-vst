/*
    SurgicalEqProcessor.h  —  JUCE AudioProcessor wrapper around EqEngine.

    This is EchoJay's built-in surgical EQ as a juce::AudioProcessor, so it can
    live in the chain like any hosted plugin (Route B: as a built-in graph node;
    it also compiles as its own target if we ever ship it standalone). It owns
    the authoritative band model on the message thread and drives the lock-free
    EqEngine on the audio thread.

    House-style choices, matched to the rest of EchoJay:
      * No AudioProcessorValueTreeState. EchoJay uses no APVTS anywhere; state is
        hand-rolled JSON (see get/setStateInformation). A 24-band EQ would also
        mean ~190 host params, which we deliberately avoid.
      * The AI apply path is a DIRECT typed write (applyEqBands) — it parses the
        eq_bands move into exact BandSpecs and pushes them to the engine. No
        anchor tables, no interpolation, no read-back/revert. This is the whole
        point: an EQ EchoJay owns is dialled exactly.

    NOTE: written against the JUCE that EchoJay already links; it is not compiled
    in the Cowork sandbox (no JUCE/Xcode there). Build it in the normal project.
*/

#pragma once

#include <JuceHeader.h>
#include "EqEngine.h"
#include "EqMove.h"
#include <array>
#include <atomic>
#include <cstdint>

class SurgicalEqProcessor : public juce::AudioProcessor
{
public:
    SurgicalEqProcessor();
    ~SurgicalEqProcessor() override = default;

    // ---- audio ------------------------------------------------------------
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    // ---- editor -----------------------------------------------------------
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    // ---- metadata ---------------------------------------------------------
    const juce::String getName() const override { return "EchoJay EQ"; }
    bool   acceptsMidi()  const override { return false; }
    bool   producesMidi() const override { return false; }
    bool   isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int  getNumPrograms()                            override { return 1; }
    int  getCurrentProgram()                         override { return 0; }
    void setCurrentProgram (int)                     override {}
    const juce::String getProgramName (int)          override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    // ---- state ------------------------------------------------------------
    void getStateInformation (juce::MemoryBlock& dest) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // ---- band model (message thread; used by editor) ----------------------
    static constexpr int kNumBands = echojay::EqEngine::kMaxBands;

    void                 setBand (int index, const echojay::BandSpec& spec); // UI edit
    echojay::BandSpec    getBand (int index) const;
    void                 setBypassed (bool b);
    bool                 isBypassed() const { return bypassed_.load(); }
    void                 setSoloBand (int index);            // -1 == none
    int                  getSoloBand() const;

    // engine handle for the editor's analyzer / dynamic metering (read-only use)
    echojay::EqEngine&   getEngine() noexcept { return engine_; }

    // ---- analysis taps (audio thread writes, message thread reads) --------
    // Two mono rings the editor FFTs for its spectrum overlay: one written
    // BEFORE the engine (the signal arriving) and one AFTER (what the EQ did).
    // The editor picks which to show; both are always captured, so toggling is
    // instant rather than waiting for a ring to refill.
    //
    // Deliberately lock-free and deliberately racy: the writer never blocks,
    // never allocates, and never waits on the reader. A visualiser that tears
    // one frame under contention is invisible; an audio thread that blocks is
    // a dropout. The reader takes the most recent samples and accepts that.
    static constexpr int kAnalysisRingSize = 8192;   // power of two
    static constexpr int kAnalysisRingMask = kAnalysisRingSize - 1;

    // Copies the most recent `maxSamples` samples in chronological order from
    // the post-EQ ring (postEq) or the pre-EQ ring. Returns how many.
    int readAnalysis (float* dest, int maxSamples, bool postEq) const noexcept;

    // ---- AI apply (exact) -------------------------------------------------
    // Parse an eq_bands move (the value of the "eq_bands" key) and apply it as a
    // per-band merge. Returns a short human-readable summary for the chat log.
    // Shape: [{ "type":"bell", "freq_hz":203, "gain_db":-3.0, "q":4.5,
    //           "band":3 (optional 1-based), "enabled":true (optional),
    //           "slope_db_oct":24 (HP/LP), "disable":true (optional),
    //           "dynamic":{ "threshold_db":-20,"range_db":-6,
    //                       "attack_ms":2,"release_ms":60 } }, ...]
    // appliedOut/skippedOut report the real per-band outcome so a caller can
    // say what actually landed instead of assuming the whole request did.
    juce::String applyEqBands (const juce::var& eqBandsArray,
                               int* appliedOut = nullptr,
                               int* skippedOut = nullptr);

    // Serialise the current band model back to an eq_bands var (for the AI to
    // see current state, and for chain-state round-tripping).
    juce::var    currentEqBandsVar() const;

private:
    // One lock-free mono ring. Both taps share this type so the pre and post
    // paths cannot drift apart in their real-time safety.
    struct AnalysisRing
    {
        std::array<float, (size_t) kAnalysisRingSize> data {};
        std::atomic<uint32_t>                        write { 0 };

        // Audio thread. Bounded fixed-array writes + one release store: no
        // locks, no allocation, nothing that can block.
        void push (const juce::AudioBuffer<float>& buffer) noexcept;
        int  read (float* dest, int maxSamples) const noexcept;   // message thread
        void clear() noexcept;                                    // not RT
    };

    void pushToEngine();                    // publish whole model -> engine (msg thread)
    static echojay::BandSpec specFromVar (const juce::var& entry, bool& disableOut,
                                          int& bandOut, bool& okOut);

    echojay::EqEngine     engine_;
    echojay::BandSpec     bands_[kNumBands];        // authoritative model
    juce::CriticalSection modelLock_;               // guards bands_ (message-thread edits)
    std::atomic<bool>     bypassed_ { false };

    // Analysis taps. The arrays are untouched by any lock; only each write
    // index is atomic, which is all the reader needs to find the newest span.
    AnalysisRing preRing_, postRing_;

    double sampleRate_ = 44100.0;
    int    blockSize_  = 512;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SurgicalEqProcessor)
};
