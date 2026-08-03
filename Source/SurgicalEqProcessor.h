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
#include "EedDeviceProcessor.h"
#include "EqEngine.h"
#include "EqMove.h"
#include <array>
#include <atomic>
#include <cstdint>

// A STRUCTURED device (BUILTIN_SUITE_PLAN.md §3): its primary move is the
// eq_bands array, not the flat params map. It inherits the flat path from
// EedDeviceProcessor for its device-global knobs and overrides applyStructured to
// resolve both shapes, so ChainHost dispatches to it exactly like any other
// built-in and needs no EQ-specific knowledge.
class SurgicalEqProcessor : public EedDeviceProcessor
{
public:
    SurgicalEqProcessor();
    ~SurgicalEqProcessor() override = default;

    // ---- audio ------------------------------------------------------------
    // Buses, MIDI, programs and the editor flag all come from EedDeviceProcessor.
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    // ---- editor -----------------------------------------------------------
    juce::AudioProcessorEditor* createEditor() override;

    // ---- metadata ---------------------------------------------------------
    const juce::String getName() const override { return "EchoJay EQ"; }

    // ---- the dialable contract (device-global knobs) ----------------------
    // The EQ's bands are dialled through eq_bands, which is an array and cannot
    // be expressed as flat params. What IS here is the device-global stage.
    //
    // phase_mode and ms_mode joined the schema the moment their DSP landed
    // (P4 / P2) — a schema is a promise that dialling a param does something,
    // and P1 deliberately kept them out until it did.
    static const echojay::ParamSchema& schema();

    const echojay::ParamSchema& paramSchema() const override { return schema(); }
    bool   setParamValue (const juce::String& id, double value) override;
    double getParamValue (const juce::String& id) const override;

    static constexpr const char* kOutputDb  = "output_db";
    static constexpr const char* kAutoGain  = "auto_gain";
    static constexpr const char* kPhaseMode = "phase_mode";
    static constexpr const char* kMsMode    = "ms_mode";

    // ---- state ------------------------------------------------------------
    void getStateInformation (juce::MemoryBlock& dest) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // ---- band model (message thread; used by editor) ----------------------
    static constexpr int kNumBands = echojay::EqEngine::kMaxBands;

    void                 setBand (int index, const echojay::BandSpec& spec); // UI edit
    echojay::BandSpec    getBand (int index) const;
    // Overridden so the engine's own bypass stays in step with the base's flag.
    void                 setBypassed (bool b) override;
    void                 setSoloBand (int index);            // -1 == none
    int                  getSoloBand() const;

    // ---- device-global settings (message thread; used by editor + AI) ------
    void  setOutputDb (float db);                 // clamped to ±24
    float getOutputDb() const { return engine_.getOutputDb(); }
    void  setAutoGain (bool on);
    bool  getAutoGain() const { return engine_.getAutoGain(); }
    // Makeup currently applied (dB, 0 when auto-gain is off), and what it WOULD
    // apply for the current bands — the UI shows the first when lit and can
    // preview the second.
    float autoGainDbApplied() const { return engine_.autoGainDbApplied(); }
    float autoGainDbTarget()  const { return engine_.autoGainDbTarget(); }

    // Phase mode is the ENGINE's now (P4): setting it swaps the DSP path and
    // re-reports latency to the host. ms_mode stays here — it is a display
    // preference (the analyzer's M/S view), not routing; routing is per band.
    using PhaseMode = echojay::EqEngine::PhaseMode;
    void      setPhaseMode (PhaseMode m);
    PhaseMode getPhaseMode() const       { return engine_.getPhaseMode(); }
    int       phaseLatencySamples() const { return engine_.latencySamples(); }
    void      setMsMode (bool on)        { msMode_ = on; }
    bool      getMsMode() const          { return msMode_; }

    // The host hook EedDeviceProcessor stubs to 0.0 — wired here so a host
    // that flushes tails knows the linear-phase FIR is still draining.
    double getTailLengthSeconds() const override;

    // engine handle for the editor's analyzer / dynamic metering (read-only use)
    echojay::EqEngine&   getEngine() noexcept { return engine_; }

    // ---- analysis taps (audio thread writes, message thread reads) --------
    // Mono (mid) rings the editor FFTs for its spectrum overlay: one written
    // BEFORE the engine (the signal arriving) and one AFTER (what the EQ did).
    // The editor picks which to show; both are always captured, so toggling is
    // instant rather than waiting for a ring to refill. P2 adds SIDE
    // companions for the analyzer's M/S view; P3 grew the PRE ring to hold the
    // ~1.4 s capture the resonance hunt analyses.
    //
    // Deliberately lock-free and deliberately racy: the writer never blocks,
    // never allocates, and never waits on the reader. A visualiser that tears
    // one frame under contention is invisible; an audio thread that blocks is
    // a dropout. The reader takes the most recent samples and accepts that.
    static constexpr int kAnalysisRingSize = 8192;   // power of two
    static constexpr int kHuntRingSize     = 65536;  // ~1.4 s at 48 k

    // Copies the most recent `maxSamples` samples in chronological order from
    // the chosen ring: post/pre EQ, mid (default) or side lane. Returns how
    // many.
    int readAnalysis (float* dest, int maxSamples, bool postEq,
                      bool side = false) const noexcept;

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

    // THE entry point for an AI move — the one funnel every caller uses (chain
    // build path, chain edit path, /eqtest, state restore).
    //
    // Accepts both shapes of settings_structured, which is the whole point:
    //   * an ARRAY  → legacy, treated as eq_bands verbatim. No existing move
    //                 breaks, ever.
    //   * an OBJECT → resolved in a deliberate order (SURGICAL_EQ_ENHANCEMENTS
    //                 §0): eq_preset lays a base, eq_settings merges over it,
    //                 eq_bands overrides per band, eq_action runs last so it
    //                 sees the finished state. Every key is optional.
    //
    // Returns a human-readable summary, or an EMPTY string when the var carried
    // nothing this device understands — that is how a caller tells "applied
    // zero bands on purpose" (an eq_settings-only move) from "this wasn't for
    // me". appliedOut/skippedOut count bands only.
    //   * an OBJECT carrying "params" → the universal flat path, handled by
    //                 EedDeviceProcessor for the device-global knobs.
    juce::String applyStructured (const juce::var& structured,
                                  int* appliedOut = nullptr,
                                  int* skippedOut = nullptr) override;

    // Device-global settings merge: absent key = leave as-is. Returns a summary
    // of what actually changed, empty if nothing did.
    juce::String applyEqSettings (const juce::var& settings);

    // eq_action (P3). tame_resonances: analyse the PRE capture, place dynamic
    // bells (or static notches) on the peaks through the ordinary
    // auto-allocating merge — hand-dialled bands are never clobbered. Returns
    // what it found, or an honest "found nothing".
    juce::String applyEqAction (const juce::var& action,
                                int* appliedOut = nullptr, int* skippedOut = nullptr);

    // eq_preset (P5). Resolves a built-in preset name to bands + settings and
    // applies it as the BASE (full replace of the band model), per the funnel
    // order — explicit eq_bands in the same move then refine it.
    juce::String applyEqPreset (const juce::String& name);

    // Serialise the current band model back to an eq_bands var (for the AI to
    // see current state, and for chain-state round-tripping).
    juce::var    currentEqBandsVar() const;

    // The eq_settings counterpart, same purpose.
    juce::var    currentEqSettingsVar() const;

private:
    // One lock-free mono ring. Every tap shares this type so the lanes cannot
    // drift apart in their real-time safety; the size is a parameter only
    // because the hunt needs a seconds-long PRE capture and the display taps
    // do not.
    template <int SizeV>
    struct AnalysisRing
    {
        static_assert ((SizeV & (SizeV - 1)) == 0, "ring size must be a power of two");
        static constexpr int kSize = SizeV;
        static constexpr int kMask = SizeV - 1;

        std::array<float, (size_t) SizeV> data {};
        std::atomic<uint32_t>             write { 0 };

        // Audio thread. Bounded fixed-array writes + one release store: no
        // locks, no allocation, nothing that can block. `side` pushes the
        // 0.5*(L-R) lane (silence for mono), otherwise the 0.5*(L+R) mid.
        void push (const juce::AudioBuffer<float>& buffer, bool side) noexcept
        {
            const int numCh = juce::jmin (buffer.getNumChannels(), 2);
            const int n     = buffer.getNumSamples();
            if (numCh <= 0 || n <= 0) return;

            const uint32_t w = write.load (std::memory_order_relaxed);

            if (numCh == 1)
            {
                const float* src = buffer.getReadPointer (0);
                for (int i = 0; i < n; ++i)
                    data[(size_t) ((w + (uint32_t) i) & kMask)] = side ? 0.0f : src[i];
            }
            else
            {
                const float* l = buffer.getReadPointer (0);
                const float* r = buffer.getReadPointer (1);
                if (side)
                    for (int i = 0; i < n; ++i)
                        data[(size_t) ((w + (uint32_t) i) & kMask)] = 0.5f * (l[i] - r[i]);
                else
                    for (int i = 0; i < n; ++i)
                        data[(size_t) ((w + (uint32_t) i) & kMask)] = 0.5f * (l[i] + r[i]);
            }

            // Release: the samples above must be visible before the index that
            // claims they exist. Unsigned wraparound of the index is
            // well-defined and the mask makes it the correct ring position.
            write.store (w + (uint32_t) n, std::memory_order_release);
        }

        int read (float* dest, int maxSamples) const noexcept     // message thread
        {
            if (dest == nullptr || maxSamples <= 0) return 0;

            const int      n     = juce::jmin (maxSamples, kSize);
            const uint32_t w     = write.load (std::memory_order_acquire);
            const uint32_t start = w - (uint32_t) n;              // newest n samples

            for (int i = 0; i < n; ++i)
                dest[i] = data[(size_t) ((start + (uint32_t) i) & kMask)];

            return n;
        }

        void clear() noexcept                                     // not RT
        {
            data.fill (0.0f);
            write.store (0, std::memory_order_release);
        }
    };

    void pushToEngine();                    // publish whole model -> engine (msg thread)
    static echojay::BandSpec specFromVar (const juce::var& entry, bool& disableOut,
                                          int& bandOut, bool& okOut);

    echojay::EqEngine     engine_;
    echojay::BandSpec     bands_[kNumBands];        // authoritative model
    juce::CriticalSection modelLock_;               // guards bands_ (message-thread edits)
    // bypassed_ lives in EedDeviceProcessor — one flag, so the base's state
    // round-trip and the engine cannot disagree about it.

    // Device-global settings. output_db / auto_gain / phase_mode live in the
    // engine (it is the thing that applies them); ms_mode is a pure display
    // preference so it stays message-thread-side.
    bool      msMode_    = false;

    // Analysis taps. The arrays are untouched by any lock; only each write
    // index is atomic, which is all the reader needs to find the newest span.
    // The PRE mid ring is hunt-sized (P3); the display-only lanes stay small.
    AnalysisRing<kHuntRingSize>     preRing_;
    AnalysisRing<kAnalysisRingSize> postRing_, preSideRing_, postSideRing_;

    double sampleRate_ = 44100.0;
    int    blockSize_  = 512;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SurgicalEqProcessor)
};
