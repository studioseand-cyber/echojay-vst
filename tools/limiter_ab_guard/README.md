# limiter_ab_guard — offline limiter A/B instrument (session L, 7 Oct 2026)

Standalone: no JUCE, no plugin library. Builds in seconds at -j 2.

    ./tools/limiter_ab_guard/build_and_run.sh             # configure + build + selftest
    ./build-limiter-ab/limiter_ab_guard selftest           # every metric proven on synthetic signals, both directions
    ./build-limiter-ab/limiter_ab_guard gen <folder>       # write source_<case>.wav for the synthetic cases
    ./build-limiter-ab/limiter_ab_guard analyse <folder> [--gain 8.2] [--ceiling 0.0] [--trace] [--strict] [--case x]

Files: `ejwav.h` (WAV in/out), `ejdsp.h` (FFT, K-weighting from the product, an 8x/96-tap true-peak meter of its own),
`ejfixtures.h` (the synthetic layouts, shared by the generator and the analyser), `ejmetrics.h` (alignment first and
asserted; loudness; peaks and overs; GR envelope; hits with retention, pre-dip, hold and release limbs; tone THD/IMD
and step response; pumping and crest; channel linking), `limiter_ab_guard.cpp` (CLI, report, the §4 rule, selftest).

What the report means, in one line each:
- `offset`: the render's constant latency against the source, in samples. Not constant = REFUSED.
- `TRUE PEAK` / `overs`: the harness's own 8x meter; overs are inter-sample peaks above ceiling + 0.05 dB.
- `GR`: 10log10(out energy / (G^2 in energy)) per block, G = the stated gain. Zero when the limiter does nothing.
- `pre(ms)`: how long before an onset the gain starts to fall = the lookahead actually used.
- `hold(ms)`: how long GR stays at its minimum; `t10..t90`: ms from the end of the hold to that fraction recovered.
  A single exponential has t90/t63 = 2.3; two limbs show as a ratio well off that.
- `retained`: output attack peak minus input attack peak (after the gain), per hit — what "lifeless" measures as.
- tone `GR steady` vs `THD`: a limiter that holds a gain reads THD -100; one that rides the waveform reads -20.
