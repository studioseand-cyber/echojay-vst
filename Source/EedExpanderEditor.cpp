/*
    EedExpanderEditor.cpp  —  see EedExpanderEditor.h.
*/

#include "EedExpanderEditor.h"

using namespace echojay::device;

namespace
{
    constexpr int kDefaultW = 420;
    constexpr int kDefaultH = 190;

    const KnobSpec kKnobs[] = {
        { EedExpanderProcessor::kThresholdDb, "THRESH",  " dB", 1, 0.0 },
        { EedExpanderProcessor::kRatio,       "RATIO",   ":1",  2, 3.0 },
        { EedExpanderProcessor::kAttackMs,    "ATTACK",  " ms", 2, 2.0 },
        { EedExpanderProcessor::kReleaseMs,   "RELEASE", " ms", 0, 250.0 },
        { EedExpanderProcessor::kRangeDb,     "RANGE",   " dB", 1, 0.0 },
    };
}

EedExpanderEditor::EedExpanderEditor (EedExpanderProcessor& p)
    // Scale to the range knob's own ceiling, so the meter never runs out of
    // travel before the device does.
    : EedDynamicsFaceEditor (p, "EXPANDER", "downward expansion, range-limited",
                             kKnobs, (int) std::size (kKnobs),
                             60.0f,
                             [&p] { return p.gainReductionDb(); },
                             kDefaultW, kDefaultH)
{
}
