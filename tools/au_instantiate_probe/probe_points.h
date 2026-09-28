#pragma once
// THE POINTS --text-at READS (21t-j, 28 Sep 2026 ruling).
//
// It lives in its own header for one reason: the probe is a console app that can only run with a real AU in
// front of it, so the POINT SET - which is the ruled part - would otherwise be unassertable. A guard includes
// this header and asserts the same function the probe calls.
//
// The ruling: twenty-one points at 0.05 on a continuous control, every detent on a discrete one. Three points
// could not answer what this mode was built for - the MC 77's Input range was taken as -24..0 from a midpoint
// that happened to print -24, and a blank display at 0.5 had no neighbour to bound it.
#include <vector>
#include <algorithm>

namespace echojay {

/** @param numSteps  the control's own getNumSteps(); @param discrete its isDiscrete().
    Returns the normalised positions to read, ascending, endpoints included. */
inline std::vector<float> probeTextAtPoints (int numSteps, bool discrete)
{
    std::vector<float> pts;
    // A DISCRETE control sampled on a 0.05 grid reads the same text several times and misses detents between
    // the grid lines, so its own step count decides where to read. The 128 ceiling is the same one --list-steps
    // uses: past that it is not a set of detents anyone reads, it is a curve.
    if (discrete && numSteps >= 2 && numSteps <= 128)
    {
        for (int i = 0; i < numSteps; ++i) pts.push_back ((float) i / (float) (numSteps - 1));
        return pts;
    }
    for (int i = 0; i <= 20; ++i) pts.push_back (std::min (1.0f, std::max (0.0f, (float) i * 0.05f)));
    return pts;
}

} // namespace echojay
