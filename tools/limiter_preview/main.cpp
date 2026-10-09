// limiter_preview main (session L, 8 Oct 2026). Usage:
//   limiter_preview <renders folder> <out folder> <case>=<gain> [<case>=<gain> ...] [--times 8,16,30] [--filmstrip Assets/wet_knob_filmstrip.png]
// For each case: reads source_<case>.wav, runs the v2 engine (Transparent, TP on) at that gain with the meter tap
// attached, lets the panel consume the tap, and at each time writes <case>_<size>_<time>s.png at 480x338 and 340x260.
#include <juce_gui_basics/juce_gui_basics.h>
#include "ejwav.h"
#include "EJLimiterV2Core.h"
#include "EedLimiterPanelV2.h"
#include <cstdio>
#include <string>
#include <vector>

int main (int argc, char** argv)
{
    if (argc < 4) { std::fprintf (stderr, "usage: limiter_preview <renders> <out> case=gain ... [--times 8,16,30] [--filmstrip path]\n"); return 2; }
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::File renders (argv[1]), out (argv[2]); out.createDirectory();
    std::vector<std::pair<std::string, double>> cases; std::vector<double> times { 8.0, 16.0, 30.0 }; juce::File strip;
    for (int i = 3; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--times" && i + 1 < argc) { times.clear(); for (const auto& t : juce::StringArray::fromTokens (argv[++i], ",", "")) times.push_back (t.getDoubleValue()); }
        else if (a == "--filmstrip" && i + 1 < argc) strip = juce::File::getCurrentWorkingDirectory().getChildFile (argv[++i]);
        else { const auto eq = a.find ('='); if (eq == std::string::npos) { std::fprintf (stderr, "bad case %s\n", a.c_str()); return 2; } cases.push_back ({ a.substr (0, eq), std::atof (a.c_str() + eq + 1) }); }
    }
    juce::Image stripImg; if (strip.existsAsFile()) stripImg = juce::ImageFileFormat::loadFrom (strip);
    const int sizes[2][2] = { { EedLimiterPanelV2::kDefaultW, EedLimiterPanelV2::kDefaultH }, { EedLimiterPanelV2::kMinW, EedLimiterPanelV2::kMinH } };
    for (const auto& [cs, gain] : cases)
    {
        ejwav::Audio src; try { src = ejwav::read (renders.getChildFile ("source_" + cs + ".wav").getFullPathName().toStdString()); } catch (const std::exception& ex) { std::fprintf (stderr, "%s\n", ex.what()); return 2; }
        for (int sz = 0; sz < 2; ++sz)
        {
            echojay::limv2::MeterTap tap; tap.prepare (src.sampleRate);
            auto tuning = echojay::limv2::transparent(); tuning.maxLookaheadMs = tuning.lookaheadMs * 5.0 / 0.18;   // the plugin's: the knob's 5 ms label = 1.67 ms of window
            echojay::limv2::Core core; core.prepare (src.sampleRate, tuning); core.setFixedLatency (true); core.setInputGainDb (gain); core.setCeilingDb (0.0); core.setTruePeak (true); core.setMeterTap (&tap); core.reset();
            EedLimiterPanelV2 panel (tap); panel.setKnobFilmstrip (stripImg); panel.setSize (sizes[sz][0], sizes[sz][1]);
            EedLimiterPanelV2::Model m; m.gainDb = gain; m.ceilingDb = 0.0; m.truePeak = true; m.latencySamples = core.latencySamples(); m.sampleRate = src.sampleRate; m.lookaheadMs = 0.18; m.attackMs = 275.0; m.releaseMs = 400.0; m.linkPct = 75.0; m.releaseLinkPct = 100.0; panel.setModel (m); panel.setSpeedIndex (1); panel.setAdvancedOpen (true);
            const size_t N = src.frames(); std::vector<float> L (N), R (N); for (size_t n = 0; n < N; ++n) { L[n] = (float) src.ch[0][n]; R[n] = (float) src.ch[src.channels() > 1 ? 1 : 0][n]; }
            size_t pos = 0; size_t ti = 0;
            while (pos < N && ti < times.size())
            {
                const size_t until = std::min (N, (size_t) (times[ti] * src.sampleRate));
                while (pos < until) { const int n = (int) std::min<size_t> (512, until - pos); float* p[2] = { L.data() + pos, R.data() + pos }; core.process (p, 2, n); pos += (size_t) n; }
                panel.refreshNow();
                juce::Image img = panel.createComponentSnapshot (panel.getLocalBounds(), true, 2.0f);
                const juce::File png = out.getChildFile (juce::String (cs) + "_" + juce::String (sizes[sz][0]) + "x" + juce::String (sizes[sz][1]) + "_" + juce::String ((int) times[ti]) + "s.png");
                juce::FileOutputStream os (png); if (os.openedOk()) { os.setPosition (0); os.truncate(); juce::PNGImageFormat().writeImageToStream (img, os); }
                std::printf ("wrote %s (%d x %d at 2x)\n", png.getFullPathName().toRawUTF8(), img.getWidth(), img.getHeight());
                ++ti;
            }
        }
    }
    return 0;
}
