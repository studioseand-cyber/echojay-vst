// blind_pack (session L, 8 Oct 2026): level-matched blind listening files. Takes up to four renders of one case, measures
// each one's integrated LUFS (the harness's BS.1770 loudness), scales every file DOWN to the quietest (never up: a
// limited file scaled up would exceed its ceiling), shuffles them into A/B/C/D with a stated seed, writes float32 WAVs
// and appends the key to KEY.txt in the output folder.
//   blind_pack <outdir> <case> <seed> <label1>=<file1> [<label2>=<file2> ...]
#include "ejmetrics.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main (int argc, char** argv)
{
    if (argc < 5) { std::fprintf (stderr, "usage: blind_pack <outdir> <case> <seed> label=file ...\n"); return 2; }
    const std::string outdir = argv[1], cs = argv[2]; const unsigned seed = (unsigned) std::atoi (argv[3]);
    // 10 Oct 2026: "--unmatched" as the last argument = no loudness scaling (files as rendered, at the same input gain)
    struct Item { std::string label, path; ejwav::Audio a; double lufs = 0; };
    std::vector<Item> items;
    for (int i = 4; i < argc; ++i) { const std::string s = argv[i]; if (s == "--unmatched") { Item u; u.label = "--unmatched"; items.push_back (std::move (u)); continue; } const auto eq = s.find ('='); if (eq == std::string::npos) { std::fprintf (stderr, "bad item %s\n", s.c_str()); return 2; } Item it; it.label = s.substr (0, eq); it.path = s.substr (eq + 1); try { it.a = ejwav::read (it.path); } catch (const std::exception& ex) { std::fprintf (stderr, "%s\n", ex.what()); return 2; } it.lufs = ejm::loudness (it.a.ch, it.a.sampleRate).integrated; items.push_back (std::move (it)); }
    bool unmatched = false; if (! items.empty() && items.back().label == "--unmatched") { unmatched = true; items.pop_back(); }
    double target = 0; for (const auto& it : items) target = std::min (target == 0 ? it.lufs : target, it.lufs);
    // 10 Oct 2026: every file padded with silence to the LONGEST (a print's silent tail made the file sizes name the key)
    size_t longest = 0; for (const auto& it : items) longest = std::max (longest, it.a.frames());
    for (auto& it : items) for (auto& c : it.a.ch) c.resize (longest, 0.0);
    // deterministic shuffle (LCG) so the key is reproducible from the seed
    std::vector<size_t> order (items.size()); for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    unsigned st = seed * 2654435761u + 12345u; for (size_t i = order.size(); i > 1; --i) { st = st * 1664525u + 1013904223u; const size_t j = (st >> 8) % i; std::swap (order[i - 1], order[j]); }
    std::FILE* key = std::fopen ((outdir + "/KEY.txt").c_str(), "a"); if (! key) { std::fprintf (stderr, "cannot write key in %s\n", outdir.c_str()); return 2; }
    if (unmatched) std::fprintf (key, "case %s  (seed %u; UNMATCHED: no scaling, files as rendered at the same input gain; padded to one length)\n", cs.c_str(), seed);
    else std::fprintf (key, "case %s  (seed %u; every file scaled to the quietest one's integrated loudness, %.2f LUFS; padded to one length)\n", cs.c_str(), seed, target);
    for (size_t k = 0; k < order.size(); ++k)
    {
        auto& it = items[order[k]]; const double scaleDb = unmatched ? 0.0 : target - it.lufs; const double sc = ejdsp::lin (scaleDb);
        for (auto& c : it.a.ch) for (double& v : c) v *= sc;
        const std::string name = cs + "_" + std::string (1, (char) ('A' + k)) + ".wav";
        ejwav::writeFloat32 (outdir + "/" + name, it.a);
        std::fprintf (key, "  %s = %-12s %s  (was %.2f LUFS, scaled %+.2f dB)\n", name.c_str(), it.label.c_str(), it.path.c_str(), it.lufs, scaleDb);
        std::printf ("wrote %s/%s  <- %s (%.2f LUFS, scaled %+.2f dB)\n", outdir.c_str(), name.c_str(), it.label.c_str(), it.lufs, scaleDb);
    }
    std::fprintf (key, "\n"); std::fclose (key);
    return 0;
}
