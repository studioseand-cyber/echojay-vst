/*
  EjmapPhaseB.h - THE PHASE B BATCH, pure parts (5 Oct 2026 evening, Kathy's goal: Sean's run gathers Phase B data).

  --phaseb-all runs AFTER the compressor follow-up: it discovers the installed products per category from the ledger's
  categories.json (eq, limiter, de-esser, saturation + amp_sim, reverb, delay, transient_shaper, gate), the certified
  compressors for gain-cal and timing, and the multibands from the cert folder's outcomes, and runs that category's
  prototype mode on each as a CHILD ejmap process, in priority order. It runs until every product is done; the per-product
  limit is a HANG GUARD only (generous, per category); a product that hits it is recorded "timed out" with its partial data
  and the batch moves on. It is resumable (a finished product is never re-run), Ctrl-C safe (a product's result is written
  into a temp folder and renamed into place only when the product is complete: a product interrupted mid-measurement leaves
  no record and is re-run from its start), and prints a progress line per product (done/total per category, elapsed, an ETA
  from the measured times so far), the same written to cert/phaseb/progress.txt for --phaseb-status.

  This header holds what can be pinned without a plugin: the category table, the progress / ETA arithmetic, the done test,
  the file naming, the progress text. The driver (runPhaseBAll in EjmapCertDriver.h) does the processes and the moves.
*/

#pragma once

#include <juce_core/juce_core.h>
#include <vector>
#include <map>

namespace ejmap::phaseb
{

// THE CATEGORIES in priority order. mode = the ejmap command; ledger = the categories.json names that feed it (empty = from
// the cert folder: certified compressors for gaincal / timing, the multiband rows for multiband); guard = the hang guard.
struct Category { juce::String name, mode, kindArg; juce::StringArray ledgerCategories; double guardS; juce::String guardWhy; bool optIn = false; };
inline const std::vector<Category>& categories()
{
    static const std::vector<Category> k {
        { "gaincal",     "--cert-gain-cal",      "",          {},                              600.0,  "21 norms x 2-3 levels per gain control + the unnamed pool: ~2-4 min measured" },
        { "timing",      "--cert-timing",        "",          {},                              1200.0, "a burst pair per position, 8-16 positions, segments scaled to slow labels: ~3-8 min measured" },
        { "limiter",     "--cert-limiter",       "",          { "limiter" },                   600.0,  "five ceilings x two oversampling states + the pool's drive test: ~2-3 min" },
        { "eq",          "--cert-eq",            "",          { "eq" },                        1800.0, "three sweeps per band, up to ten bands, the engage search and the pool: ~6-12 min measured" },
        { "deesser",     "--cert-deesser",       "",          { "de-esser" },                  600.0,  "two ladders, the noise ladder, up to six responses, the pool: ~2-4 min" },
        { "saturation",  "--cert-saturation",    "",          { "saturation", "amp_sim" },     900.0,  "three levels per drive control + the pool: ~2-6 min measured (Saphira's twelve: 6)" },
        { "reverb",      "--cert-reverb-delay",  "reverb",    { "reverb" },                    1200.0, "11 mix + 5 decay + 5 time tails + the pool's tail pairs: ~5-8 min measured" },
        { "delay",       "--cert-reverb-delay",  "delay",     { "delay" },                     1200.0, "as reverb plus 5 feedback + 3 tempos: ~5-8 min measured" },
        { "transient",   "--cert-dynamics",      "transient", { "transient_shaper" },          600.0,  "two neutral runs + 5-10 hit runs + the pool's hit pairs: ~2-4 min" },
        { "gate",        "--cert-dynamics",      "gate",      { "gate" },                      900.0,  "5 ramps + 3 range ramps + 9 bursts + the pool's ramp pairs: ~4-6 min" },
        { "multiband",   "--cert-multiband",     "",          {},                              1500.0, "a pairing response per threshold, a ladder per band, 25-30 vocal responses: ~4-8 min measured" },
        // tuners (Kathy, 6 Oct item 5): the tuner certification run again for its data (Humanize with the vibrato held note); the record
        // lands beside the row (phaseb/tuners/tuner/), NEVER in the one store (cert/fixtures/), which --redo tuners leaves untouched
        { "tuners",      "--cert-tuner",         "",          { "pitch" },                     1800.0, "strength + speed sweeps per strength control, flex ladder, humanize (three runs), key/scale grids: ~5-15 min" },
        // gain-all (Kathy, 6 Oct item 6, lowest priority): the gain spec's measurement on every other Phase B product's output / trim /
        // input controls (mix never targeted: the mode's roles are output, makeup, input, trim/gain/level), data only, OPT-IN: runs only
        // when named (--category gainall or --redo gain-all), never in a bare --phaseb-all; records under phaseb/gainall/
        // combined settings (Kathy's NEXT BUILD A1, 6 Oct; accuracy pass, opt-in: --redo combined after gain-cal and timing have run)
        { "combined",    "--cert-combined",      "",          {},                              600.0,  "one process at the composed setting: ~10-20 s", true },
        // real material (A2, 6 Oct; opt-in: --redo material): three generated signals through the tone check's pick
        { "material",    "--cert-material",      "",          {},                              600.0,  "three materials x two passes of 6.5 s: ~1 min", true },
        { "gainall",     "--cert-gain-cal",      "all",       { "eq", "limiter", "de-esser", "saturation", "amp_sim", "reverb", "delay", "transient_shaper", "gate" }, 600.0, "21 norms x 2-3 levels per gain control + the unnamed pool: ~1-4 min", true } };
    return k;
}
inline const Category* categoryNamed (const juce::String& name) { for (const auto& c : categories()) if (c.name == name) return &c; return nullptr; }

// file naming: a product's DONE row and its mode's record
inline juce::String modeWord (const juce::String& mode) { return mode.fromFirstOccurrenceOf ("--cert-", false, false).replace ("-", ""); }   // gaincal, timing, limiter, eq, deesser, saturation, reverbdelay, dynamics, multiband
inline juce::File rowFile (const juce::File& phaseb, const juce::String& category, const juce::String& stem) { return phaseb.getChildFile (category).getChildFile (stem + ".phaseb.json"); }
inline bool isDone (const juce::File& phaseb, const juce::String& category, const juce::String& stem) { return rowFile (phaseb, category, stem).existsAsFile(); }

// THE ATOMIC WRITE: a file is either absent or whole (a temp sibling, then a rename); the DONE row, progress and summary use it
inline void writeAtomic (const juce::File& f, const juce::String& text)
{
    const auto tmp = f.getSiblingFile (f.getFileName() + ".tmp");
    tmp.replaceWithText (text, false, false, "\n");
    f.deleteFile(); tmp.moveFileTo (f);   // rename: the file is either absent or whole
}
// THE RAW TRACE, gzipped beside the records; the target is cleared first because a FileOutputStream APPENDS
constexpr int kGzipWindowBits = 15 + 16;   // a real gzip file (gunzip opens it); 0 would be a bare zlib stream, which gunzip refuses
inline void gzipInto (const juce::File& src, const juce::File& dstDir)
{
    const auto dst = dstDir.getChildFile (src.getFileName() + ".gz"); dst.deleteFile();   // a FileOutputStream APPENDS: a re-run's trace never lands behind an interrupted run's half
    juce::FileOutputStream fo (dst);
    if (! fo.openedOk()) return;
    { juce::GZIPCompressorOutputStream gz (fo, 6, kGzipWindowBits); juce::FileInputStream fi (src); if (fi.openedOk()) gz.writeFromInputStream (fi, -1); gz.flush(); }
}

// THE REDO (Kathy, 6 Oct): --phaseb-all --redo a,b,... names categories whose rows are ALL run again, and/or "nothing_nominated",
// which re-runs exactly the rows that finished ok with no record (the lexicon nominated nothing). Everything else is untouched.
inline bool rowIsNothingNominated (const juce::var& row) { return row.getProperty ("outcome", "").toString() == "ok" && (row.getProperty ("records", {}).size() == 0 || (bool) row.getProperty ("nothing_measured", false) || (int) row.getProperty ("exit_code", 0) == 4); }   // exit 4 = the mode measured nothing: Sean's b0258a7b rows carry no flag, only the code
// an opt-in category runs only when named: by --category, or by --redo (its rows are then made and run)
inline bool categoryRuns (const Category& c, const juce::StringArray& onlyCategories, const juce::StringArray& redo)
{
    if (! onlyCategories.isEmpty()) return onlyCategories.contains (c.name);
    return ! c.optIn || redo.contains (c.name);
}
inline bool rowToRedo (const juce::var& row, const juce::String& category, const juce::StringArray& redo)
{
    if (redo.contains (category)) return true;
    return redo.contains ("nothing_nominated") && rowIsNothingNominated (row);
}

// PROGRESS: counts per category, measured seconds per product, the ETA from the medians so far
struct CategoryProgress { int total = 0, done = 0, ok = 0, timedOut = 0, failed = 0, skipped = 0; std::vector<double> seconds; };
struct Progress { std::map<juce::String, CategoryProgress> cats; double elapsedS = 0.0; juce::String current, startedAt, updatedAt, redo; };
inline double medianOf (std::vector<double> v) { if (v.empty()) return 0.0; std::sort (v.begin(), v.end()); return v.size() % 2 ? v[v.size() / 2] : 0.5 * (v[v.size() / 2 - 1] + v[v.size() / 2]); }
// the ETA: for each category, the products left x that category's median so far - or, with nothing measured there yet, its
// guard's stated estimate (the midpoint of the minutes in guardWhy is not parsed: the overall median so far stands in)
inline double etaSeconds (const Progress& p)
{
    std::vector<double> all; for (const auto& [n, c] : p.cats) all.insert (all.end(), c.seconds.begin(), c.seconds.end());
    const double overall = medianOf (all);
    double eta = 0.0;
    for (const auto& [n, c] : p.cats) { const int left = juce::jmax (0, c.total - c.done); eta += left * (c.seconds.empty() ? overall : medianOf (c.seconds)); }
    return eta;
}
inline juce::String hms (double s) { const int t = (int) std::lround (juce::jmax (0.0, s)); return juce::String (t / 3600) + ":" + juce::String ((t / 60) % 60).paddedLeft ('0', 2) + ":" + juce::String (t % 60).paddedLeft ('0', 2); }
inline juce::String progressLine (const Progress& p, const juce::String& category, const juce::String& product, const juce::String& outcome, double seconds)
{
    int doneAll = 0, totalAll = 0; for (const auto& [n, c] : p.cats) { doneAll += c.done; totalAll += c.total; }
    const auto& c = p.cats.count (category) ? p.cats.at (category) : CategoryProgress();
    return "[" + category + " " + juce::String (c.done) + "/" + juce::String (c.total) + " | all " + juce::String (doneAll) + "/" + juce::String (totalAll) + "] " + product + ": " + outcome + " " + juce::String (seconds, 0) + " s | elapsed " + hms (p.elapsedS) + " | ETA " + hms (etaSeconds (p));
}
inline juce::String progressText (const Progress& p)
{
    juce::String s;
    int doneAll = 0, totalAll = 0; for (const auto& [n, c] : p.cats) { doneAll += c.done; totalAll += c.total; }
    s << "PHASE B PROGRESS (started " << p.startedAt << ", updated " << p.updatedAt << (p.redo.isNotEmpty() ? ", redo " + p.redo : juce::String()) << ")\n";
    s << "all: " << doneAll << "/" << totalAll << " done; elapsed " << hms (p.elapsedS) << "; ETA " << hms (etaSeconds (p)) << (p.current.isNotEmpty() ? "; now: " + p.current : juce::String ("; idle")) << "\n";
    for (const auto& cat : categories())
    {
        if (! p.cats.count (cat.name)) continue;
        const auto& c = p.cats.at (cat.name);
        s << "  " << cat.name.paddedRight (' ', 11) << juce::String (c.done).paddedLeft (' ', 3) << "/" << juce::String (c.total).paddedRight (' ', 3) << " ok " << c.ok << " timed_out " << c.timedOut << " failed " << c.failed << " skipped " << c.skipped
          << (c.seconds.empty() ? juce::String ("  (nothing timed yet)") : "  median " + juce::String (medianOf (c.seconds), 0) + " s over " + juce::String ((int) c.seconds.size()))
          << (c.done < c.total ? "  left " + juce::String (c.total - c.done) : juce::String ("  complete")) << "\n";
    }
    // what is next: the first category with products left
    for (const auto& cat : categories()) if (p.cats.count (cat.name) && p.cats.at (cat.name).done < p.cats.at (cat.name).total) { s << "next: " << cat.name << "\n"; break; }
    return s;
}
inline juce::var progressVar (const Progress& p)
{
    auto* o = new juce::DynamicObject(); o->setProperty ("startedAt", p.startedAt); o->setProperty ("updatedAt", p.updatedAt); o->setProperty ("elapsed_s", std::round (p.elapsedS)); o->setProperty ("eta_s", std::round (etaSeconds (p))); o->setProperty ("current", p.current); if (p.redo.isNotEmpty()) o->setProperty ("redo", p.redo);
    auto* cats = new juce::DynamicObject();
    for (const auto& [n, c] : p.cats) { auto* co = new juce::DynamicObject(); co->setProperty ("total", c.total); co->setProperty ("done", c.done); co->setProperty ("ok", c.ok); co->setProperty ("timed_out", c.timedOut); co->setProperty ("failed", c.failed); co->setProperty ("skipped", c.skipped); juce::Array<juce::var> sec; for (double x : c.seconds) sec.add (std::round (x)); co->setProperty ("seconds", sec); cats->setProperty (n, juce::var (co)); }
    o->setProperty ("categories", juce::var (cats));
    return juce::var (o);
}
inline Progress progressFromVar (const juce::var& v)
{
    Progress p; p.startedAt = v.getProperty ("startedAt", "").toString(); p.updatedAt = v.getProperty ("updatedAt", "").toString(); p.elapsedS = (double) v.getProperty ("elapsed_s", 0.0); p.current = v.getProperty ("current", "").toString(); p.redo = v.getProperty ("redo", "").toString();
    if (auto* cats = v.getProperty ("categories", {}).getDynamicObject())
        for (const auto& kv : cats->getProperties())
        { CategoryProgress c; const auto& x = kv.value; c.total = (int) x.getProperty ("total", 0); c.done = (int) x.getProperty ("done", 0); c.ok = (int) x.getProperty ("ok", 0); c.timedOut = (int) x.getProperty ("timed_out", 0); c.failed = (int) x.getProperty ("failed", 0); c.skipped = (int) x.getProperty ("skipped", 0);
          if (const auto* sec = x.getProperty ("seconds", {}).getArray()) for (const auto& s : *sec) c.seconds.push_back ((double) s); p.cats[kv.name.toString()] = c; }
    return p;
}

} // namespace ejmap::phaseb
