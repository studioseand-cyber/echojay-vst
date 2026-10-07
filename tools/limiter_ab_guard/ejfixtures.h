#pragma once
// ejfixtures.h (limiter_ab_guard, 7 Oct 2026): the synthetic cases, generated here so Sean only has to RENDER them.
// The generator and the analyser share one layout, so the analyser knows where every tone segment and every
// transient is without detecting it - and the onset detector is cross-checked against the layout on these cases.
//
// Every level below is the level IN THE FILE; the limiter's gain (+8.2 dB in Sean's A/B) is applied by the
// limiter under test, so a -2.2 dBFS tone arrives at the ceiling +6 dB over, 0 dBFS +8.2 dB over.
#include "ejwav.h"
#include "ejdsp.h"
#include <string>
#include <vector>

namespace ejfix {

struct Segment { double t0, t1, levelDb; };                                   // a steady tone segment
struct Event   { double t; double lengthSec; double levelDb; int chanMask; }; // a transient: 1 = L, 2 = R, 3 = both
struct Layout
{
    std::string name, description;
    double seconds = 0;
    double toneHz = 0, toneHz2 = 0;      // tone cases: the fundamental(s)
    double backgroundHz = 0, backgroundDb = -600;   // probe cases: the steady tone the GR envelope is read from
    std::vector<Segment> segments;
    std::vector<Event> events;
    std::vector<std::pair<double, double>> markers;   // broadband alignment markers (t0, t1): Gaussian white noise, -28 dBFS RMS
    bool isTone()  const { return toneHz > 0; }
    bool isProbe() const { return ! events.empty(); }
};

inline const std::vector<std::string>& syntheticNames()
{
    static const std::vector<std::string> n { "tone_997", "tone_50", "tone_imd", "probe_transients", "panned_transient" };
    return n;
}

// Every synthetic case opens and closes with 0.4 s of Gaussian white noise at -28 dBFS RMS (sample peaks near
// -16 dBFS, true peaks a little higher; -8 dBFS or so after the +8.2 dB gain, so no limiter touches it. Uniform
// noise was tried first: its inter-sample peaks run 6 dB above its sample peaks and a true-peak limiter DID touch it). A tone is periodic, so correlating a tone render against its source is ambiguous to within
// a period; the alignment is taken on these broadband markers instead, where there is exactly one answer.
constexpr double kLead = 0.6;

inline Layout layout (const std::string& name)
{
    Layout L; L.name = name;
    if (name == "tone_997" || name == "tone_50")
    {
        L.toneHz = name == "tone_997" ? 997.0 : 50.0;
        L.description = name == "tone_997" ? "997 Hz sine stepping -8.2/-5.2/-2.2/0.0/-2.2/-8.2 dBFS, 4 s each: THD per drive, and the attack/release step response at each boundary"
                                           : "50 Hz sine, same steps: does the limiter hold a gain or ride the waveform (LF distortion), and the LF release";
        double t = kLead;
        for (double lv : { -8.2, -5.2, -2.2, 0.0, -2.2, -8.2 }) { L.segments.push_back ({ t, t + 4.0, lv }); t += 4.0; }
        L.seconds = t + kLead;
    }
    else if (name == "tone_imd")
    {
        L.toneHz = 19000.0; L.toneHz2 = 20000.0;
        L.description = "19 kHz + 20 kHz pair, sum peak -2.2 dBFS then 0.0 dBFS, 6 s each: intermodulation (1 kHz, 18 kHz, 21 kHz products)";
        L.segments = { { kLead, kLead + 6.0, -2.2 }, { kLead + 6.0, kLead + 12.0, 0.0 } };
        L.seconds = kLead + 12.0 + kLead;
    }
    else if (name == "probe_transients")
    {
        L.backgroundHz = 1000.0; L.backgroundDb = -14.0;
        L.description = "1 kHz at -14 dBFS (never limited) carrying 3 kHz bursts of 1 sample, 0.5, 2, 10, 50, 200 and 1000 ms at 0 dBFS then at -4.2 dBFS, every 2.5 s: the GR envelope around each burst gives the lookahead, the attack window shape and the release against hold time";
        double t = kLead + 0.5;
        for (double lv : { 0.0, -4.2 })
            for (double len : { 0.0, 0.0005, 0.002, 0.010, 0.050, 0.200, 1.000 }) { L.events.push_back ({ t, len, lv, 3 }); t += 2.5; }
        L.seconds = t + kLead;
    }
    else if (name == "panned_transient")
    {
        L.backgroundHz = 1000.0; L.backgroundDb = -14.0;
        L.description = "1 kHz at -14 dBFS on both channels; 10 ms bursts at 0 dBFS on LEFT only x8, then on both x4, every 1.5 s: the right channel's dip measures the channel linking";
        double t = kLead + 0.5;
        for (int i = 0; i < 8; ++i) { L.events.push_back ({ t, 0.010, 0.0, 1 }); t += 1.5; }
        for (int i = 0; i < 4; ++i) { L.events.push_back ({ t, 0.010, 0.0, 3 }); t += 1.5; }
        L.seconds = t + kLead;
    }
    else { L.seconds = 0; }   // unknown: real material, no layout
    if (L.seconds > 0) L.markers = { { 0.1, 0.5 }, { L.seconds - 0.5, L.seconds - 0.1 } };
    return L;
}

// Render a layout to audio. Deterministic: the same name and rate give the same samples.
inline ejwav::Audio generate (const Layout& L, double sr)
{
    ejwav::Audio a; a.sampleRate = sr;
    const size_t N = (size_t) std::llround (L.seconds * sr);
    a.ch.assign (2, std::vector<double> (N, 0.0));
    const double twoPi = 2.0 * ejdsp::kPi;
    if (L.isTone())
    {
        for (const auto& s : L.segments)
        {
            const size_t n0 = (size_t) std::llround (s.t0 * sr), n1 = std::min (N, (size_t) std::llround (s.t1 * sr));
            const double A = ejdsp::lin (s.levelDb);
            for (size_t n = n0; n < n1; ++n)
            {
                double v = L.toneHz2 > 0 ? 0.5 * A * (std::sin (twoPi * L.toneHz * (double) n / sr) + std::sin (twoPi * L.toneHz2 * (double) n / sr))   // sum peak = A
                                         : A * std::sin (twoPi * L.toneHz * (double) n / sr);
                a.ch[0][n] = v; a.ch[1][n] = v;
            }
        }
    }
    else if (L.isProbe())
    {
        const double bg = ejdsp::lin (L.backgroundDb);
        for (size_t n = 0; n < N; ++n) { const double v = bg * std::cos (twoPi * L.backgroundHz * (double) n / sr); a.ch[0][n] = v; a.ch[1][n] = v; }
        for (const auto& e : L.events)
        {
            // Bursts start on a multiple of 48 samples at 48 k, where the 1 kHz background and the 3 kHz burst are both
            // at a positive cosine peak, so the event's SAMPLE peak is exactly background + burst: no inter-sample excess
            // to argue about in the retention numbers.
            const size_t n0 = (size_t) (std::llround (e.t * sr) / 48) * 48;
            const double burstAmp = ejdsp::lin (e.levelDb) - bg;
            if (e.lengthSec <= 0.0)
            {   // a single-sample impulse
                for (int c = 0; c < 2; ++c) if (e.chanMask & (1 << c)) a.ch[(size_t) c][n0] += burstAmp;
                continue;
            }
            const size_t len = (size_t) std::llround (e.lengthSec * sr);
            const size_t edge = std::max<size_t> (1, std::min<size_t> ((size_t) std::llround (0.0002 * sr), len / 4));   // 0.2 ms raised-cosine edges, at most a quarter of the burst
            std::vector<double> shape (len), bgv (len);
            for (size_t k = 0; k < len; ++k)
            {
                double w = 1.0;
                if (k < edge) w = 0.5 - 0.5 * std::cos (ejdsp::kPi * (double) k / (double) edge);
                else if (k + edge > len) w = 0.5 - 0.5 * std::cos (ejdsp::kPi * (double) (len - k) / (double) edge);
                shape[k] = w * std::cos (twoPi * 3000.0 * (double) (n0 + k) / sr);
                bgv[k]   = bg * std::cos (twoPi * L.backgroundHz * (double) (n0 + k) / sr);
            }
            // Scale the burst so the event's SAMPLE peak (burst + background) is exactly the nominal level, whatever
            // the burst length: a short burst's 3 kHz peaks need not coincide with the background's.
            double amp = ejdsp::lin (e.levelDb) - bg;
            for (int it = 0; it < 4; ++it) { size_t kStar = 0; double best = -1; for (size_t k = 0; k < len; ++k) { const double v = std::abs (amp * shape[k] + bgv[k]); if (v > best) { best = v; kStar = k; } } if (shape[kStar] > 1e-6) amp = (ejdsp::lin (e.levelDb) - bgv[kStar]) / shape[kStar]; }
            for (size_t k = 0; k < len && n0 + k < N; ++k)
                for (int c = 0; c < 2; ++c) if (e.chanMask & (1 << c)) a.ch[(size_t) c][n0 + k] += amp * shape[k];
        }
    }
    for (const auto& m : L.markers)   // LAST, so nothing above overwrites them
    {
        const size_t n0 = (size_t) std::llround (m.first * sr), n1 = std::min (N, (size_t) std::llround (m.second * sr)), edge = (size_t) std::llround (0.005 * sr);
        uint32_t seed = 0x5EED1234u + (uint32_t) n0; const double A = ejdsp::lin (-28.0) * std::sqrt (3.0);   // sum of 4 uniforms (sigma = A/sqrt(3)) -> Gaussian, RMS -28 dBFS
        for (size_t n = n0; n < n1; ++n)
        {
            double w = 1.0; if (n - n0 < edge) w = 0.5 - 0.5 * std::cos (ejdsp::kPi * (double) (n - n0) / (double) edge); else if (n1 - n <= edge) w = 0.5 - 0.5 * std::cos (ejdsp::kPi * (double) (n1 - n) / (double) edge);
            for (int c = 0; c < 2; ++c) { double g = 0; for (int j = 0; j < 4; ++j) { seed = seed * 1664525u + 1013904223u; g += ((double) (seed >> 8) / 16777216.0) * 2.0 - 1.0; } a.ch[(size_t) c][n] = A * w * g * 0.5; }
        }
    }
    return a;
}

} // namespace ejfix
