/*
  EjmapTailRoles.h - A TAIL WITH BOTH ROLES, SECTION BY SECTION (Kathy, 20 Oct). A plugin whose signature is a tail (reverb or a feedback
  delay: the reads cannot tell them apart) is no longer sent to review when its CONTROLS say what it holds. The controls are grouped into
  sections by their names ("SlapMute", "SlapTime" -> section "slap"; "Short Delay Color" -> "short delay"; "Verb Mix" -> "verb"), each
  section takes a role from its words (delay: delay / dly / echo / slap / tape / throw / tap / feedback; reverb: reverb / verb / plate /
  room / hall / chamber / space / spring / pre-delay / decay / size / damping), and:
    both roles   -> measured as BOTH, each pass with the other role's sections silenced (their mute on, else their on/off off, else their
                    level / mix / send at its minimum), so a reverb's tail is never read as a delay's and the other way round;
    one role     -> that role;
    neither      -> review, as before.
  Pure: names (and their texts) in, roles and writes out. Pinned in RoundTripTest.cpp (WZ-TAIL-1).
*/
#pragma once

#include <juce_core/juce_core.h>
#include <map>
#include <optional>
#include <tuple>
#include <vector>

namespace ejmap::tailroles
{
enum class Role { none, delay, reverb };
inline const char* roleName (Role r) { return r == Role::delay ? "delay" : r == Role::reverb ? "reverb" : "none"; }

// the words of a control name: split on anything not a letter or digit, and on lower -> Upper ("SlapMute" -> slap, mute), lower-cased
inline juce::StringArray wordsOf (const juce::String& name)
{
    juce::StringArray out; juce::String cur;
    auto flush = [&] { if (cur.isNotEmpty()) { out.add (cur.toLowerCase()); cur.clear(); } };
    for (int i = 0; i < name.length(); ++i)
    {
        const auto c = name[i];
        if (! juce::CharacterFunctions::isLetterOrDigit (c)) { flush(); continue; }
        if (i > 0 && juce::CharacterFunctions::isUpperCase (c) && juce::CharacterFunctions::isLowerCase (name[i - 1])) flush();
        if (i > 0 && juce::CharacterFunctions::isDigit (c) != juce::CharacterFunctions::isDigit (name[i - 1])) flush();
        cur << juce::String::charToString (c);
    }
    flush(); return out;
}
inline bool isDelayWord (const juce::String& w)
{ for (const char* t : { "delay", "dly", "echo", "slap", "tape", "throw", "tap", "feedback", "fbk", "fb", "repeat", "repeats", "pingpong", "ping" }) if (w == t) return true; return false; }
inline bool isReverbWord (const juce::String& w)
{ for (const char* t : { "reverb", "verb", "plate", "room", "hall", "chamber", "space", "spring", "predelay", "decay", "size", "damp", "damping", "diffusion" }) if (w == t) return true; return false; }

// a role word that names a PARAMETER, not a section: "Size" at 0 is a small reverb, not a silent one - never written as a section's send
inline bool isParameterWord (const juce::String& w)
{ for (const char* t : { "size", "decay", "damp", "damping", "diffusion", "predelay", "feedback", "fbk", "fb", "repeat", "repeats", "tap" }) if (w == t) return true; return false; }
// a control's SECTION and ROLE: the words up to and including the first role word name the section; "pre" + "delay" is a reverb's
// pre-delay; a feedback word ("FB Decay Time") makes a decay a delay's
struct Placed { juce::String section; Role role = Role::none; };
inline Placed place (const juce::String& name)
{
    const auto w = wordsOf (name); Placed p; juce::StringArray key;
    for (int i = 0; i < w.size(); ++i)
    {
        key.add (w[i]);
        const bool pre = w[i] == "pre" && i + 1 < w.size() && w[i + 1] == "delay";
        if (pre) { key.add ("delay"); p.role = Role::reverb; break; }
        if (isDelayWord (w[i])) { p.role = Role::delay; break; }
        if (isReverbWord (w[i])) { p.role = Role::reverb; break; }
    }
    if (p.role == Role::reverb && w.contains ("decay") && (w.contains ("fb") || w.contains ("feedback") || w.contains ("fbk"))) p.role = Role::delay;
    if (p.role != Role::none) p.section = key.joinIntoString (" ");
    return p;
}

// the controls as the engage search takes them: (index, name, is a switch, text -> norm)
using Controls = std::vector<std::tuple<int, juce::String, bool, std::map<juce::String, float>>>;
struct Write { int index = -1; double norm = 0.0; juce::String control, set, why; };
// A ROLE NEEDS A REAL ROLE WORD (20 Oct, the 20 Oct run's names): "Pre Delay" alone (Enigma, a phaser) is not a reverb; "Feedback" alone
// (MCombMB, a comb filter) is not a delay; a detector's delay or a look-ahead (MDrumEnhancer "Detector delay") is not a delay at all
inline bool strongWord (const juce::String& w, Role role)
{
    if (role == Role::reverb) { for (const char* t : { "reverb", "verb", "plate", "room", "hall", "chamber", "space", "spring", "decay", "size" }) if (w == t) return true; return false; }
    for (const char* t : { "delay", "dly", "echo", "slap", "tape", "throw", "tap" }) if (w == t) return true; return false;
}
inline bool notATailControl (const juce::StringArray& w)
{ for (const char* t : { "detector", "lookahead", "look", "sidechain", "sc", "dynamics", "attack", "release" }) if (w.contains (t)) return true; return false; }
struct Roles { std::map<juce::String, Role> sections; bool delay = false, reverb = false;
               juce::StringArray delaySections, reverbSections; std::vector<int> delayControls, reverbControls; };
inline Roles rolesOf (const Controls& controls)
{
    Roles r;
    for (const auto& [idx, name, sw, texts] : controls)
    {
        const auto p = place (name); if (p.role == Role::none) continue;
        const auto w = wordsOf (name); if (notATailControl (w)) continue;
        r.sections[p.section] = p.role;
        bool strong = false; for (const auto& x : w) strong = strong || strongWord (x, p.role);
        if (p.role == Role::delay) { r.delay = r.delay || strong; r.delaySections.addIfNotAlreadyThere (p.section); r.delayControls.push_back (idx); }
        else { r.reverb = r.reverb || strong; r.reverbSections.addIfNotAlreadyThere (p.section); r.reverbControls.push_back (idx); }
    }
    return r;
}
// A TEXT SHOWN AT SEVERAL POSITIONS keeps the one nearest an END of the travel (20 Oct, GW VoiceCentric: "Off" at 0.0 and 0.5 - the last one
// won, 0.5 never landed on the two-step switch and the reverb stayed On through the "silenced" pass)
inline void keepEndmost (std::map<juce::String, float>& texts, const juce::String& text, float norm)
{
    auto toEnd = [] (float n) { return juce::jmin (n, 1.0f - n); };
    const auto it = texts.find (text);
    if (it == texts.end() || toEnd (norm) < toEnd (it->second)) texts[text] = norm;
}
// the norm of the first text that matches one of the words (else the fallback)
inline double normOfText (const std::map<juce::String, float>& texts, std::initializer_list<const char*> words, double fallback, juce::String& textOut)
{
    for (const char* w : words) for (const auto& [t, n] : texts) if (t.trim().equalsIgnoreCase (w)) { textOut = t; return n; }
    return fallback;
}
// SILENCE A ROLE BY EVERY MEANS A SECTION OFFERS (20 Oct, CLA Epic: its PlateMute opens On, yet PlateLevel still moved the tail - a
// plugin's "mute" is not always silence): its mute ON, its on/off OFF (a bypass ON), its level / mix / send / amount / volume at the
// minimum, and a control named only by the section ("Reverb", "Short Delay": a send level) at the minimum - every one of them, one write
// per control, the switches first
inline std::vector<Write> silence (const Controls& controls, Role role)
{
    std::vector<Write> out; std::map<juce::String, std::pair<int, Write>> best; std::vector<std::pair<int, Write>> all;   // every silencing write, ranked
    for (const auto& [idx, name, sw, texts] : controls)
    {
        const auto p = place (name); if (p.role != role) continue;
        const auto w = wordsOf (name); Write x; x.index = idx; x.control = name; int rank = 99;
        if (w.contains ("mute")) { rank = 0; x.norm = normOfText (texts, { "on", "mute", "muted" }, 1.0, x.set); x.why = "its mute on"; }
        else if (w.contains ("bypass")) { rank = 1; x.norm = normOfText (texts, { "on", "bypass", "bypassed" }, 1.0, x.set); x.why = "its bypass on"; }
        else if ((w.contains ("on") && w.contains ("off")) || w.contains ("onoff") || w.contains ("enable") || w.contains ("active"))
        { rank = 1; x.norm = normOfText (texts, { "off", "disabled" }, 0.0, x.set); x.why = "its switch off"; }
        else if (w.contains ("level") || w.contains ("mix") || w.contains ("send") || w.contains ("amount") || w.contains ("volume") || w.contains ("wet"))
        { rank = 2; x.norm = 0.0; x.why = "its level at the minimum"; }
        else if (w.joinIntoString (" ") == p.section && ! isParameterWord (w[w.size() - 1])) { rank = 3; x.norm = 0.0; x.why = "its send (named only by the section) at the minimum"; }
        if (rank == 99) continue;
        if (x.set.isEmpty()) x.set = juce::String (x.norm, 3);
        x.why = "section '" + p.section + "': " + x.why; all.push_back ({ rank, x });
    }
    std::stable_sort (all.begin(), all.end(), [] (const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [rank, x] : all) out.push_back (x);
    (void) best; return out;
}
// DID THE SILENCING LAND (20 Oct)? The probe prints each write it made - "preset <idx> ..." (EJ_PROBE_PRESET) or "set <idx> ..." (a mode's
// set= list) - with "landed_by <how>". The writes whose line says "unlanded" (or that have no line at all) did not silence anything; the
// FIRST line for an index is the silencing write (it leads the set= list)
inline juce::StringArray unlanded (const std::vector<Write>& ws, const juce::String& probeOut)
{
    std::map<int, juce::String> how;
    for (const auto& line : juce::StringArray::fromLines (probeOut))
    {   const auto f = juce::StringArray::fromTokens (line, "\t", ""); if (f.size() < 2 || (f[0] != "preset" && f[0] != "set")) continue;
        const int lb = f.indexOf ("landed_by"); if (how.count (f[1].getIntValue()) == 0) how[f[1].getIntValue()] = lb >= 0 && lb + 1 < f.size() ? f[lb + 1] : juce::String ("?"); }
    juce::StringArray out;
    for (const auto& w : ws) { const auto it = how.find (w.index); if (it == how.end() || it->second == "unlanded" || it->second == "?") out.add (w.control + (it == how.end() ? " (no preset line)" : " (unlanded)")); }
    return out;
}
inline juce::String presetOf (const std::vector<Write>& ws)   // the probe's EJ_PROBE_PRESET form: "index:norm,..."
{ juce::StringArray s; for (const auto& w : ws) s.add (juce::String (w.index) + ":" + juce::String (w.norm, 6)); return s.joinIntoString (","); }
// the parameter list as the probe prints it (--list-params: "index<TAB>name<TAB>..."): (index, name), no texts
inline Controls controlsFromListParams (const juce::String& out)
{
    Controls c;
    for (const auto& line : juce::StringArray::fromLines (out))
    {   const auto f = juce::StringArray::fromTokens (line, "\t", ""); if (f.size() < 2 || ! f[0].containsOnly ("0123456789") || f[0].isEmpty()) continue;
        c.push_back ({ f[0].getIntValue(), f[1], false, {} }); }
    return c;
}
} // namespace ejmap::tailroles
