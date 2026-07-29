/*
    EedCompressorEditor.cpp  —  see EedCompressorEditor.h.
*/

#include "EedCompressorEditor.h"

using namespace echojay::device;

namespace
{
    // Seven dials on one line plus a meter under them. The rack sizes down from
    // here, and the face editor wraps the row rather than overlapping it.
    constexpr int kDefaultW = 540;
    constexpr int kDefaultH = 190;

    // The device's whole front panel, in one table. Ranges and defaults are NOT
    // here — they come from the schema, so a knob cannot travel somewhere the AI
    // is not allowed to dial.
    const KnobSpec kKnobs[] = {
        { EedCompressorProcessor::kThresholdDb, "THRESH",  " dB", 1, 0.0 },
        { EedCompressorProcessor::kRatio,       "RATIO",   ":1",  2, 4.0 },

        // Attack and release are skewed so their mid-travel sits on a usable
        // value: the interesting part of a 0.1-200 ms range lives in its first
        // tenth, and a linear dial spends most of its throw where nobody sets it.
        { EedCompressorProcessor::kAttackMs,    "ATTACK",  " ms", 1, 10.0 },
        { EedCompressorProcessor::kReleaseMs,   "RELEASE", " ms", 0, 150.0 },

        { EedCompressorProcessor::kKneeDb,      "KNEE",    " dB", 1, 0.0 },
        { EedCompressorProcessor::kMakeupDb,    "MAKEUP",  " dB", 1, 0.0 },
        { EedCompressorProcessor::kMix,         "MIX",     " %",  0, 0.0 },
    };
}

EedCompressorEditor::EedCompressorEditor (EedCompressorProcessor& p)
    : EedDynamicsFaceEditor (p, "COMPRESSOR", "stereo-linked RMS compression",
                             kKnobs, (int) std::size (kKnobs),
                             24.0f,
                             [&p] { return p.gainReductionDb(); },
                             kDefaultW, kDefaultH)
{
}
