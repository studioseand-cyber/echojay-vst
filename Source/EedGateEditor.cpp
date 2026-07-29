/*
    EedGateEditor.cpp  —  see EedGateEditor.h.
*/

#include "EedGateEditor.h"

using namespace echojay::device;

namespace
{
    constexpr int kDefaultW = 480;
    constexpr int kDefaultH = 190;

    const KnobSpec kKnobs[] = {
        { EedGateProcessor::kThresholdDb,  "THRESH", " dB", 1, 0.0 },
        { EedGateProcessor::kRangeDb,      "RANGE",  " dB", 1, 0.0 },
        { EedGateProcessor::kAttackMs,     "ATTACK", " ms", 2, 1.0 },
        { EedGateProcessor::kHoldMs,       "HOLD",   " ms", 0, 50.0 },
        { EedGateProcessor::kReleaseMs,    "RELEASE"," ms", 0, 200.0 },
        { EedGateProcessor::kHysteresisDb, "HYST",   " dB", 1, 0.0 },
    };
}

EedGateEditor::EedGateEditor (EedGateProcessor& p)
    // The meter scale follows the gate's own reach: a gate routinely pulls 40 dB
    // down, and a 24 dB scale would sit pinned at the bottom saying nothing.
    : EedDynamicsFaceEditor (p, "GATE", "hold + hysteresis, attenuates by range",
                             kKnobs, (int) std::size (kKnobs),
                             60.0f,
                             [&p] { return p.gainReductionDb(); },
                             kDefaultW, kDefaultH)
{
}
