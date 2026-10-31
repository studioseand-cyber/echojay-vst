/*
  EjmapWizardCollect.h - THE THREE THINGS ONLY A RUN CAN COLLECT (Kathy, 29 Oct, the coverage audit's item 1-3). Each costs no measuring
  time: it is read from what the measuring already wrote, when the engine packs.

    display_check (ej_display_check/1)  per control, did the plugin's DISPLAY tell the truth about its SOUND? From the acceptance checks
                                        the drafts already hold (a setting written by its display value after the profile was built, then
                                        measured): how many checks, how many within the bar, the worst miss, and a verdict - agrees,
                                        disagrees, inverted (an EQ gain whose sound goes the other way), not_checked. It also travels on a
                                        "measured, but ..." outcome, whose drafts are never sent: those are the plugins it matters most for
    binary                              the plugin's program file: SHA-256 and size of the bundle's executable. No path, no date (an install
                                        date would say something about the person; the hash and size say only which build it is)
    readings (ej_readings/1)            a compact copy of the raw probe lines: every reading per position, the 1 ms time windows kept where
                                        the level moves near the peak and thinned elsewhere, the per-hold window lists and the identity line
                                        dropped. Capped (kReadingsMaxGz / kReadingsMaxText): coarser windows, then none, then withheld

  They travel in the result's payload, never as files: a file of a schema B's server does not know makes the whole result "held".
*/
#pragma once

#include <juce_core/juce_core.h>
#include <CommonCrypto/CommonDigest.h>
#include <cctype>
#include <cmath>
#include <map>
#include <optional>
#include <regex>
#include <vector>

namespace ejmap::wizcollect
{
inline constexpr const char* kDisplayCheckSchema = "ej_display_check/1";
inline constexpr const char* kReadingsSchema = "ej_readings/1";
inline constexpr juce::int64 kReadingsMaxGz = 1500000;      // a result may be 4,000,000 bytes compressed (contract §4): the readings take at most this
inline constexpr juce::int64 kReadingsMaxText = 12000000;   // ... and 32 MB uncompressed
inline constexpr double kEqGainBarDb = 0.5, kEqFreqBarPct = 5.0, kToneBarDb = 0.5, kLevelBarDb = 0.5;   // EJ Map's own acceptance bars

//==============================================================================
// 1. THE DISPLAY CHECK
struct Check { juce::String control, quantity, unit, at, why; bool ran = false, within = false; double miss = NAN, target = NAN, measured = NAN, tolerance = NAN; };

inline double num (const juce::var& v) { return v.isDouble() || v.isInt() || v.isInt64() ? (double) v : NAN; }
inline juce::String controlOf (const juce::var& o)
{
    const auto c = o.getProperty ("control", {});
    if (c.isObject()) { const auto n = c.getProperty ("name", "").toString(); return n.isNotEmpty() ? n : c.getProperty ("control", "").toString(); }
    return c.isString() ? c.toString() : juce::String();
}
// "0.05 % off", "-0.33 ms off", "188.2 % off": every check that states its miss only in words states it this way
inline std::optional<std::pair<double, juce::String>> missFromWhy (const juce::String& why)
{
    static const std::regex re (R"((-?\d+(?:\.\d+)?)\s*(%|dB|ms|cents|s)\s+off)");
    std::smatch m; const auto s = why.toStdString();
    if (! std::regex_search (s, m, re)) return std::nullopt;
    return std::make_pair (std::stod (m[1].str()), juce::String (m[2].str()));
}
inline juce::String fmt (double v) { return juce::String (v, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd ("."); }

// one draft, profile or tone check -> its checks (by shape: the schemas name the same things differently)
inline void checksFrom (const juce::var& doc, std::vector<Check>& out, const juce::String& amount)
{
    if (! doc.isObject()) return;
    auto notRun = [&] (const juce::String& c, const juce::String& q, const juce::var& a) { Check k; k.control = c; k.quantity = q; k.why = a.getProperty ("why", "").toString(); out.push_back (k); };
    if (const auto* ss = doc.getProperty ("sections", {}).getArray())   // a channel strip: each section's own profile
        for (const auto& s : *ss) if (s.getProperty ("profile", {}).isObject()) checksFrom (s.getProperty ("profile", {}), out, {});
    if (const auto* bs = doc.getProperty ("bands", {}).getArray())   // EQ: gain and frequency, read apart
        for (const auto& b : *bs)
        {
            const auto gc = b.getProperty ("gain_control", "").toString(), fc = b.getProperty ("freq_control", "").toString();
            if (const auto* acc = b.getProperty ("acceptance", {}).getArray())
                for (const auto& a : *acc)
                {
                    if (! (bool) a.getProperty ("ran", false)) { notRun (gc, "gain", a); continue; }
                    Check g; g.control = gc; g.quantity = "gain"; g.unit = "dB"; g.tolerance = kEqGainBarDb; g.ran = true;
                    g.target = num (a.getProperty ("target_db", {})); g.measured = num (a.getProperty ("measured_gain_db", {})); g.miss = num (a.getProperty ("gain_miss_db", {}));
                    g.at = std::isnan (g.target) ? juce::String() : fmt (g.target) + " dB"; g.within = ! std::isnan (g.miss) && std::abs (g.miss) <= kEqGainBarDb; out.push_back (g);
                    const auto off = num (a.getProperty ("figure_off_pct", {}));
                    if (fc.isNotEmpty() && ! std::isnan (off))
                    {   Check f; f.control = fc; f.quantity = "frequency"; f.unit = "%"; f.tolerance = kEqFreqBarPct; f.ran = true; f.miss = off; f.within = std::abs (off) <= kEqFreqBarPct;
                        const auto hz = num (a.getProperty ("target_hz", {})); f.at = std::isnan (hz) ? juce::String() : fmt (hz) + " Hz"; out.push_back (f); }
                }
        }
    for (const char* key : { "filters", "filter_bands" })   // filters: the labelled cutoff against the measured one
        if (const auto* fs = doc.getProperty (key, {}).getArray())
            for (const auto& f : *fs)
                if (const auto* ps = f.getProperty ("positions", {}).getArray())
                    for (const auto& p : *ps)
                        if (p.hasProperty ("within_tolerance"))
                        {   Check k; k.control = f.getProperty ("control", "").toString(); k.quantity = "cutoff"; k.unit = "%"; k.tolerance = num (f.getProperty ("tolerance_pct", kEqFreqBarPct)); k.ran = true;
                            k.at = p.getProperty ("display", "").toString(); k.miss = num (p.getProperty ("off_pct", {})); k.within = (bool) p.getProperty ("within_tolerance", false); out.push_back (k); }
    if (const auto* fs = doc.getProperty ("shifts", {}).getArray())   // pitch shift: the label against the measured shift
        for (const auto& f : *fs)
            if (const auto* ps = f.getProperty ("positions", {}).getArray())
                for (const auto& p : *ps)
                    if (p.hasProperty ("agrees") && (bool) p.getProperty ("read", false))
                    {   Check k; k.control = f.getProperty ("control", "").toString(); k.quantity = "pitch"; k.unit = "cents"; k.tolerance = num (f.getProperty ("tolerance_cents", {})); k.ran = true;
                        k.at = p.getProperty ("display", "").toString(); k.miss = num (p.getProperty ("error_cents", {})); k.within = (bool) p.getProperty ("agrees", false); out.push_back (k); }
    if (const auto c = doc.getProperty ("ceiling", {}); c.isObject())   // a limiter: the true peak against the ceiling's label
        if (const auto* acc = c.getProperty ("acceptance", {}).getArray())
            for (const auto& a : *acc)
            {
                const auto ctl = controlOf (c);
                if (! (bool) a.getProperty ("ran", false)) { notRun (ctl, "ceiling", a); continue; }
                const auto st = a.getProperty ("setting", {}); const auto lab = num (st.getProperty ("label_db", {}));
                const auto tp = num (a.getProperty (a.getProperty ("judged_by", "").toString() == "sample_peak" ? "out_sample_peak_db" : "out_true_peak_db", {}));   // 30 Oct: as the row was judged
                Check k; k.control = ctl; k.quantity = "ceiling"; k.unit = "dB"; k.tolerance = num (a.getProperty ("bar_db", {})); k.ran = true;
                k.at = st.getProperty ("ceiling_display", "").toString(); k.miss = tp - lab; k.within = (bool) a.getProperty ("pass", false); out.push_back (k);
            }
    if (const auto d = doc.getProperty ("deesser", {}); d.isObject() && d.getProperty ("acceptance", {}).isObject())   // a de-esser: GR at the pick
    {
        const auto a = d.getProperty ("acceptance", {}); const auto ctl = d.getProperty ("amount_control", "").toString();
        if (! (bool) a.getProperty ("ran", false)) notRun (ctl, "gain_reduction", a);
        else { Check k; k.control = ctl; k.quantity = "gain_reduction"; k.unit = "dB"; k.tolerance = num (a.getProperty ("bar_db", {})); k.ran = true;
               k.at = a.getProperty ("threshold_display", "").toString(); k.miss = num (a.getProperty ("miss_db", {})); k.within = (bool) a.getProperty ("pass", false); out.push_back (k); }
    }
    if (const auto* acc = doc.getProperty ("acceptance", {}).getArray())   // saturation (by word), space (by map), gate / transient (by step)
        for (const auto& a : *acc)
        {
            if (! a.isObject()) continue;
            juce::String ctl, q;
            if (a.hasProperty ("map")) { q = a.getProperty ("map", "").toString(); ctl = controlOf (doc.getProperty (q, {})); }
            else if (a.hasProperty ("word")) { q = "level"; ctl = controlOf (doc.getProperty ("drive", {})); }
            else if (a.getProperty ("step", {}).isString()) { q = a.getProperty ("step", "").toString().upToFirstOccurrenceOf (" ", false, false); ctl = controlOf (doc.getProperty (q, {})); }
            else continue;
            if (! (bool) a.getProperty ("ran", false)) { notRun (ctl, q, a); continue; }
            if (a.hasProperty ("word"))
            {
                const auto at = a.getProperty ("drive_display", "").toString() + " (" + a.getProperty ("word", "").toString() + ")";
                Check l; l.control = ctl; l.quantity = "level"; l.unit = "dB"; l.tolerance = kLevelBarDb; l.ran = true; l.at = at; l.miss = num (a.getProperty ("measured_level_db", {}));
                l.within = (bool) a.getProperty ("level_within_0_5", false); out.push_back (l);
                Check t; t.control = ctl; t.quantity = "thd_band"; t.ran = true; t.at = at; t.within = (bool) a.getProperty ("thd_in_band", false); out.push_back (t);
                continue;
            }
            Check k; k.control = ctl; k.quantity = q; k.ran = true; k.at = a.getProperty ("step", "").toString(); k.within = (bool) a.getProperty ("pass", false);
            if (const auto m = missFromWhy (a.getProperty ("why", "").toString())) { k.miss = m->first; k.unit = m->second; }
            out.push_back (k);
        }
    // a compressor's tone check (its own file, or a profile's block): the GR asked of the amount control against the GR measured
    const auto tc = doc.getProperty ("tone_check", {}).isObject() ? doc.getProperty ("tone_check", {})
                  : (doc.hasProperty ("pass_within_0_5_db") && doc.hasProperty ("g_db") ? doc : juce::var());
    if (tc.isObject())
    {
        auto one = [&] (const juce::var& t)
        {   const auto g = num (t.getProperty ("g_db", {})), gr = num (t.getProperty ("gr_measured_db", {}));
            Check k; k.control = amount.isNotEmpty() ? amount : juce::String ("amount"); k.quantity = "gain_reduction"; k.unit = "dB"; k.tolerance = kToneBarDb; k.ran = true;
            k.at = std::isnan (g) ? juce::String() : "GR " + fmt (g) + " dB"; k.miss = gr - g; k.within = (bool) t.getProperty ("pass_within_0_5_db", false); out.push_back (k); };
        one (tc);
        if (const auto* dl = tc.getProperty ("deep_levels", {}).getArray()) for (const auto& t : *dl) if ((bool) t.getProperty ("ran", false)) one (t);
    }
}

// an EQ gain whose sound goes the other way: at least two checks, and at least half of those run, of a 1 dB+ request that measured 0.5 dB+ the other way
inline bool inverted (const std::vector<const Check*>& ran)
{
    int inv = 0;
    for (const auto* c : ran) if (! std::isnan (c->target) && ! std::isnan (c->measured) && std::abs (c->target) >= 1.0 && std::abs (c->measured) >= 0.5 && c->target * c->measured < 0) ++inv;
    return inv >= 2 && 2 * inv >= (int) ran.size();
}

inline juce::var round3 (double v) { return std::isnan (v) ? juce::var() : juce::var (std::round (v * 1000.0) / 1000.0); }

// the plugin's drafts / profiles / tone checks -> one ej_display_check/1 block (void when nothing was checked or listed)
inline juce::var displayCheck (const juce::Array<juce::var>& docs)
{
    juce::String amount;
    for (const auto& d : docs) if (d.getProperty ("schema", "").toString() == "ej_comp_profile/1") amount = controlOf (d.getProperty ("amount", {}));
    std::vector<Check> cs; for (const auto& d : docs) checksFrom (d, cs, amount);
    if (cs.empty()) return {};
    std::vector<std::pair<juce::String, juce::String>> order; std::map<std::pair<juce::String, juce::String>, std::vector<const Check*>> by;
    for (const auto& c : cs) { const auto k = std::make_pair (c.control, c.quantity); if (! by.count (k)) order.push_back (k); by[k].push_back (&c); }
    juce::Array<juce::var> controls; std::map<juce::String, int> tally;
    for (const auto& k : order)
    {
        const auto& l = by[k]; std::vector<const Check*> ran; int within = 0; double worst = NAN; juce::String unit, whyNot; double tol = NAN;
        for (const auto* c : l)
        {   if (! c->ran) { if (whyNot.isEmpty()) whyNot = c->why; continue; }
            ran.push_back (c); if (c->within) ++within;
            if (! std::isnan (c->miss) && (std::isnan (worst) || std::abs (c->miss) > std::abs (worst))) worst = c->miss;
            if (unit.isEmpty()) unit = c->unit; if (std::isnan (tol)) tol = c->tolerance; }
        const juce::String verdict = ran.empty() ? "not_checked" : k.second == "gain" && inverted (ran) ? "inverted" : within == (int) ran.size() ? "agrees" : "disagrees";
        ++tally[verdict];
        auto* o = new juce::DynamicObject(); o->setProperty ("control", k.first.isNotEmpty() ? juce::var (k.first) : juce::var()); o->setProperty ("quantity", k.second);
        o->setProperty ("verdict", verdict); o->setProperty ("checks", (int) ran.size()); o->setProperty ("within", within);
        if (unit.isNotEmpty()) o->setProperty ("unit", unit);
        if (! std::isnan (tol)) o->setProperty ("tolerance", round3 (tol));
        if (! std::isnan (worst)) o->setProperty ("worst_miss", round3 (worst));
        if (ran.empty() && whyNot.isNotEmpty()) o->setProperty ("why", whyNot);
        juce::Array<juce::var> at; for (const auto* c : ran) { if (at.size() >= 12) break; auto* s = new juce::DynamicObject(); s->setProperty ("at", c->at);
                                                                if (! std::isnan (c->miss)) s->setProperty ("miss", round3 (c->miss)); s->setProperty ("within", c->within); at.add (juce::var (s)); }
        if (! at.isEmpty()) o->setProperty ("readings", at);
        controls.add (juce::var (o));
    }
    auto* d = new juce::DynamicObject(); d->setProperty ("schema", kDisplayCheckSchema);
    d->setProperty ("source", "the acceptance checks of the measuring pass: settings written by their display value after the profile was built, then measured");
    auto* t = new juce::DynamicObject(); for (const char* v : { "agrees", "disagrees", "inverted", "not_checked" }) t->setProperty (v, tally[v]);
    d->setProperty ("summary", juce::var (t)); d->setProperty ("controls", controls);
    return juce::var (d);
}

//==============================================================================
// 2. THE PROGRAM FILE
// the bundle's executable: the one file in Contents/MacOS, else the one named as the bundle, else the first by name (EjmapDiskStamp's pick)
inline juce::File executableOf (const juce::File& bundle)
{
    if (bundle.existsAsFile()) return bundle;
    auto kids = bundle.getChildFile ("Contents/MacOS").findChildFiles (juce::File::findFiles, false);
    if (kids.isEmpty()) return {};
    if (kids.size() == 1) return kids[0];
    for (const auto& k : kids) if (k.getFileName() == bundle.getFileNameWithoutExtension()) return k;
    kids.sort(); return kids[0];
}
inline juce::String sha256Of (const juce::File& f)
{
    juce::FileInputStream in (f); if (! in.openedOk()) return {};
    CC_SHA256_CTX ctx; CC_SHA256_Init (&ctx); juce::HeapBlock<char> buf (1 << 20);
    for (;;) { const auto n = in.read (buf.getData(), 1 << 20); if (n <= 0) break; CC_SHA256_Update (&ctx, buf.getData(), (CC_LONG) n); }
    unsigned char md[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_Final (md, &ctx);
    return juce::String::toHexString (md, CC_SHA256_DIGEST_LENGTH, 0);
}
// cached by path + size + modification time (the cache stays on this Mac: it is the only place a path is written)
struct BinaryStamps
{
    explicit BinaryStamps (juce::File cacheFile) : file (std::move (cacheFile)) { cache = juce::JSON::parse (file.loadFileAsString()); if (! cache.isObject()) cache = juce::var (new juce::DynamicObject()); }
    juce::var stampOf (const juce::File& bundle)
    {
        const auto exe = executableOf (bundle); if (! exe.existsAsFile()) return {};
        const auto key = exe.getFullPathName(); const auto bytes = exe.getSize(); const auto mt = exe.getLastModificationTime().toMilliseconds();
        auto e = cache.getProperty (key, {});
        if (! (e.isObject() && (juce::int64) e.getProperty ("bytes", 0) == bytes && (juce::int64) e.getProperty ("mtime_ms", 0) == mt && e.getProperty ("sha256", "").toString().length() == 64))
        {   const auto h = sha256Of (exe); if (h.length() != 64) return {};
            auto* o = new juce::DynamicObject(); o->setProperty ("sha256", h); o->setProperty ("bytes", bytes); o->setProperty ("mtime_ms", mt); e = juce::var (o);
            cache.getDynamicObject()->setProperty (key, e); dirty = true; }
        auto* s = new juce::DynamicObject(); s->setProperty ("sha256", e.getProperty ("sha256", {})); s->setProperty ("bytes", e.getProperty ("bytes", {}));   // what is SENT: no path, no date
        return juce::var (s);
    }
    void save() { if (dirty) { const auto t = file.getSiblingFile (file.getFileName() + ".tmp"); t.replaceWithText (juce::JSON::toString (cache)); t.moveFileTo (file); dirty = false; } }
    juce::File file; juce::var cache; bool dirty = false;
};

//==============================================================================
// 3. THE READINGS
inline bool isWindow (const juce::String& t) { return t == "twin" || t == "hwin" || t == "rwin" || t == "bwin"; }
inline double outDbOf (const juce::String& line)
{
    const auto p = juce::StringArray::fromTokens (line, "\t", ""); const int i = p.indexOf ("out_db");
    return i >= 0 && i + 1 < p.size() ? p[i + 1].getDoubleValue() : -600.0;
}
inline juce::String twoDecimals (const juce::String& line)   // -42.5803 -> -42.58 (cut, not rounded): the probe prints more digits than any reading holds
{
    const auto in = line.toStdString(); std::string o; o.reserve (in.size());
    for (size_t i = 0; i < in.size();)
    {
        const bool numStart = std::isdigit ((unsigned char) in[i]) && (i == 0 || ! std::isalnum ((unsigned char) in[i - 1]));
        if (! numStart) { o += in[i++]; continue; }
        size_t j = i; while (j < in.size() && std::isdigit ((unsigned char) in[j])) ++j;
        if (j + 1 < in.size() && in[j] == '.' && std::isdigit ((unsigned char) in[j + 1]))
        {   size_t k = j + 1; while (k < in.size() && std::isdigit ((unsigned char) in[k])) ++k;
            if (k < in.size() && (in[k] == '.' || std::isalpha ((unsigned char) in[k]))) { o.append (in, i, k - i); i = k; continue; }   // "1.10.1", "2.5x": not a reading
            o.append (in, i, juce::jmin (k, j + 3) - i); i = k; continue; }
        o.append (in, i, j - i); i = j;
    }
    return juce::String (o);
}
// one process's lines -> the kept ones. coarse 1: 1 ms windows where the level moves (0.5 dB) within 20 dB of the file's peak, every 10th
// down to 40 dB, every 20th below; coarse 4: the same, gaps x4 and moves of 2 dB; coarse 0: no windows at all. param lines: the first
// process's whole snapshot, then only what changed. Never: the identity line ("probe:"), a hold's window list, a line naming a home folder
inline void compactInto (const juce::StringArray& lines, std::map<juce::String, juce::String>& base, int coarse, juce::StringArray& out)
{
    juce::StringArray wins; int pw = 0;
    auto flush = [&]
    {
        if (wins.isEmpty() || coarse == 0) { wins.clear(); return; }
        double mx = -600.0; for (const auto& w : wins) mx = juce::jmax (mx, outDbOf (w));
        double last = NAN; int li = -1000000; const double move = coarse > 1 ? 2.0 : 0.5;
        for (int i = 0; i < wins.size(); ++i)
        {
            const auto v = outDbOf (wins[i]); const int gap = i - li;
            const bool keep = v < mx - 40 ? gap >= 20 * coarse : v < mx - 20 ? gap >= 10 * coarse : (std::isnan (last) || std::abs (v - last) >= move || gap >= 5 * coarse);
            if (keep) { out.add (twoDecimals (wins[i])); last = v; li = i; }
        }
        wins.clear();
    };
    for (const auto& l0 : lines)
    {
        const auto l = l0.trimEnd(); if (l.isEmpty()) continue;
        const auto t = l.upToFirstOccurrenceOf ("\t", false, false).upToFirstOccurrenceOf (" ", false, false);
        if (isWindow (t)) { wins.add (l); continue; }
        flush();
        if (t == "probe:" || l.contains ("/Users/")) continue;
        if (t == "param")
        {   const auto p = juce::StringArray::fromTokens (l, "\t", "");
            if (p.size() >= 3) { const auto it = base.find (p[1]); if (it != base.end() && it->second == p[2]) continue; if (it == base.end()) base[p[1]] = p[2]; }
            out.add (l); continue; }
        if (t == "pwin" && (++pw % 4) != 0) continue;
        out.add (twoDecimals (t == "hold" ? juce::String (std::regex_replace (l.toStdString(), std::regex (R"(\twin\t[^\t]*)"), "")) : l));
    }
    flush();
}
inline juce::StringArray linesOf (const juce::File& f)
{
    if (f.getFileExtension() != ".gz") return juce::StringArray::fromLines (f.loadFileAsString());
    juce::FileInputStream fin (f); if (! fin.openedOk()) return {};
    juce::GZIPDecompressorInputStream gin (&fin, false, juce::GZIPDecompressorInputStream::gzipFormat);
    return juce::StringArray::fromLines (gin.readEntireStreamAsString());
}
inline juce::int64 gzSize (const juce::String& text)
{
    juce::MemoryOutputStream mo; { juce::GZIPCompressorOutputStream gz (mo, 6); gz.writeText (text, false, false, nullptr); gz.flush(); } return (juce::int64) mo.getDataSize();
}
// a plugin's raw files (sorted, "# <tag>" before each) -> ej_readings/1: the finest level that fits the caps, else withheld
inline juce::var readingsFrom (const juce::Array<juce::File>& raw, const juce::String& stem, juce::int64 maxGz = kReadingsMaxGz, juce::int64 maxText = kReadingsMaxText)
{
    if (raw.isEmpty()) return {};
    std::vector<std::pair<juce::String, juce::StringArray>> files;
    for (const auto& f : raw)
    {   auto tag = f.getFileName(); if (tag.startsWith (stem + ".")) tag = tag.substring (stem.length() + 1); else tag = tag.fromFirstOccurrenceOf (".", false, false);
        files.push_back ({ tag.upToLastOccurrenceOf (".txt", false, false), linesOf (f) }); }
    juce::int64 lastGz = 0;
    for (const int coarse : { 1, 4, 0 })
    {
        juce::StringArray out; std::map<juce::String, juce::String> base;
        for (const auto& [tag, lines] : files) { out.add ("# " + tag); compactInto (lines, base, coarse, out); }
        const auto text = out.joinIntoString ("\n");
        lastGz = gzSize (text);
        if (lastGz <= maxGz && text.getNumBytesAsUTF8() <= (size_t) maxText)
        {
            auto* r = new juce::DynamicObject(); r->setProperty ("schema", kReadingsSchema);
            r->setProperty ("format", "the probe's own tab-separated lines; \"# <tag>\" starts each measuring process; readings cut to 2 decimals (control display texts as the plugin wrote them)");
            r->setProperty ("windows", coarse == 1 ? "1 ms windows kept where the level moves 0.5 dB within 20 dB of the process's peak (else every 5th), every 10th to 40 dB below, every 20th further down"
                                     : coarse == 4 ? "coarser to fit: moves of 2 dB, gaps x4" : "none: too large with them");
            r->setProperty ("processes", (int) files.size()); r->setProperty ("lines", out);
            return juce::var (r);
        }
    }
    auto* r = new juce::DynamicObject(); r->setProperty ("schema", kReadingsSchema);
    r->setProperty ("withheld", "too large: " + juce::String (lastGz) + " bytes compressed without windows (the cap is " + juce::String (maxGz) + ")");
    return juce::var (r);
}
// which raw files are this plugin's: under its id stem (<format>_<uid>_<version>) or its product stem (the compressor's sweeps and tone
// checks), followed by a tag - "X_1.2." must not take "X_1.2.3.<tag>"
inline bool rawFileOf (const juce::String& name, const juce::String& stem)
{
    if (stem.isEmpty() || ! name.startsWith (stem + ".")) return false;
    const auto next = name[stem.length() + 1];
    return ! juce::CharacterFunctions::isDigit (next);
}
} // namespace ejmap::wizcollect
