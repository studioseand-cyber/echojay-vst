#pragma once
#include <JuceHeader.h>
#include "MeterEngine.h"
#include "EJSpectralEvidence.h"
#include <vector>
#include <mutex>
#include <memory>
#include <atomic>

struct ReferenceResult {
    juce::String name;          // Filename
    juce::String path;          // Full path
    MeterData data;             // Analysed meter data
    float durationSeconds = 0;
    bool isReference = true;
    juce::int64 timestamp = 0;
    std::vector<float> waveformThumbnail; // peak values for waveform display (one per ~1024 samples)
    std::array<float, 64> eqCurve = {};   // averaged spectrum for EQ curve display

    // ===== THE ACCUMULATED MACRO BANDS (Phase 1b commit 3) =====
    // The whole-file mean of the six pink-referenced bands, accumulated in
    // POWER across every analysis block, from MeterEngine's own accumulator so
    // there is one definition of the quantity and not one per caller.
    //
    // DISTINCT FROM data.macroBandDb, WHICH IS UNCHANGED. That field is the
    // meter's ballistic reading after the final block: a ~450 ms tail, and on
    // 14 of Kathy's 45 references it is entirely on the floor. Nothing reads
    // this new field yet; repointing the consumers is a later commit.
    //
    // IT CARRIES ITS OWN WINDOW, because a figure that travels without one
    // cannot be checked (decision 4 of COMPARE_REFERENCE_PLAN). The reduction
    // is named from the vocabulary EJSpectralEvidence already established
    // rather than a second one: this is a WholeFileAverage, the same statistic
    // eqCurve is, over the same blocks.
    std::array<float, 6>  macroBandAccum { -120, -120, -120, -120, -120, -120 };
    bool                  hasMacroBandAccum = false;   // false = not measured, NOT a floor
    float                 macroAccumSeconds = 0.0f;    // the window it covers
    int                   macroAccumBlocks  = 0;       // and how many blocks made it
    echojay::SpectralReduction macroAccumReduction = echojay::SpectralReduction::Unknown;
};

class ReferenceAnalyser
{
public:
    ReferenceAnalyser();
    ~ReferenceAnalyser();
    
    // Analyse an audio file on a background thread
    // Calls onComplete on the message thread when done
    void analyseFile(const juce::File& file,
                     std::function<void(bool success, const juce::String& error)> onComplete);
    
    // Check if currently analysing
    bool isAnalysing() const { return analysing.load(); }
    float getProgress() const { return progress.load(); }
    
    // Force-reset if stuck (called from UI after timeout)
    void forceResetIfStuck()
    {
        if (analysing.load())
        {
            auto now = juce::Time::currentTimeMillis();
            if (now - analysisStartTime > 30000) // 30 seconds timeout
            {
                analysing.store(false);
                progress.store(0.0f);
            }
        }
    }
    
    // Get all reference results
    std::vector<ReferenceResult> getReferences() const;
    int getReferenceCount() const;
    
    // Remove a reference by index
    void removeReference(int index);
    
    // Clear all references
    void clearAll();

    /** SEED THE LIBRARY FROM STORED MEASUREMENTS, WITHOUT DECODING ANYTHING.

        The entries arrive already built, so this class stays free of the index
        types and the conversion can live where both are visible. All this does
        is put them in, under the SAME refMutex getReferences, getReference,
        getReferenceCount, removeReference and clearAll take.

        CALL IT BEFORE ANYTHING IS QUEUED. The worker takes refMutex for its one
        push_back, so seeding after queueing would race the worker's appends and
        decide positions by arrival order. EJReferenceRows.h:148 already records
        that getReferenceCount() - 1 is not a safe way to name an entry.

        IT APPENDS RATHER THAN REPLACING, and asserts nothing about what is
        already there: the caller has reconciled, and reconciliation is the one
        place that decides what the library is. */
    void seedFromStored (const std::vector<ReferenceResult>& seeds);

    /** THE LIBRARY CHANGED. Set by the processor; called on the MESSAGE THREAD
        with NO LOCK HELD, so the handler is free to take refMutex itself
        (getReferences does) without deadlocking.

        `removed` distinguishes the two cases, because they are not symmetrical
        in the index: an add is a union and a removal needs a tombstone, since
        mergeReferenceIndex cannot express a deletion (its own comment says so).

        IT IS NOT FIRED BY seedFromStored, deliberately. Seeding is the index
        being read INTO the analyser; firing there would write back what was
        just read, on every project open, for no change.

        IT IS NOT FIRED BY clearAll EITHER. clearAll empties the vector at
        teardown and reset, and a commit built from an empty library would be a
        union with nothing: harmless to the file, but a removal storm if it ever
        grew tombstones. The one writer of a real removal is removeReference. */
    std::function<void (const juce::String& path, bool removed)> onLibraryChanged;
    
    // Get a specific reference
    ReferenceResult getReference(int index) const;

    // Queue multiple files — analyses run sequentially
    void analyseFiles(const std::vector<juce::File>& files,
                      std::function<void(bool success, const juce::String& error)> onEachComplete);

private:
    void processQueue();

    mutable std::mutex refMutex;
    std::vector<ReferenceResult> references;
    std::atomic<bool> analysing { false };
    std::atomic<float> progress { 0.0f };
    juce::int64 analysisStartTime = 0;
    
    // Pending queue for sequential analysis
    struct QueuedFile {
        juce::File file;
        std::function<void(bool, const juce::String&)> callback;
    };
    std::mutex queueMutex;
    std::vector<QueuedFile> pendingQueue;
    
    // Shared flag: set to false in destructor so in-flight callbacks bail out
    std::shared_ptr<std::atomic<bool>> alive { std::make_shared<std::atomic<bool>>(true) };
    // Tracked thread so destructor can wait for it
    std::unique_ptr<juce::Thread> analyseThread;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ReferenceAnalyser)
};
