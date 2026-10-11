/*
  EjmapSleepGuard.h - the preflight refuses to start a run the Mac may sleep through (Kathy, 11 Oct: Sean's Mac slept ~4.5 h under
  `caffeinate -i`; 22 saturation rows filed "slept"). PURE: the three pmset readings in, the verdict and the exact fix out.

  OK only on AC power AND (a PreventSystemSleep assertion is held - `caffeinate -dimsu` / `-s` - OR the system sleep timer is 0, the
  Energy setting "Prevent automatic sleeping when the display is off"). `caffeinate -i` alone (PreventUserIdleSystemSleep) is not
  enough: it is what Sean ran. On battery: refused (caffeinate -s holds on AC only; a lid-close on battery sleeps regardless).
*/
#pragma once

#include <juce_core/juce_core.h>
#include <optional>

namespace ejmap::sleepguard
{

struct Verdict { bool ok = false; juce::String line, fix; };
inline int assertionCount (const juce::String& assertions, const char* name)
{
    for (const auto& l : juce::StringArray::fromLines (assertions))
    { auto t = juce::StringArray::fromTokens (l.trim(), " \t", ""); t.removeEmptyStrings();   // runs of spaces give empty tokens
      if (t.size() >= 2 && t[0] == name) return t[t.size() - 1].getIntValue(); }
    return 0;
}
inline std::optional<int> sleepTimer (const juce::String& settings)
{
    for (const auto& l : juce::StringArray::fromLines (settings))
    { auto t = juce::StringArray::fromTokens (l.trim(), " \t", ""); t.removeEmptyStrings();   // runs of spaces give empty tokens
      if (t.size() >= 2 && t[0] == "sleep" && t[1].containsOnly ("0123456789")) return t[1].getIntValue(); }
    return std::nullopt;
}
inline const char* kFix = "start the run as   caffeinate -dimsu \"$BIN\" --run-all ...   (not caffeinate -i), keep the Mac on its charger, and set System "
                          "Settings > Energy > \"Prevent automatic sleeping when the display is off\" ON";
inline Verdict judge (const juce::String& powerSource, const juce::String& assertions, const juce::String& settings)
{
    Verdict v;
    const bool ac = powerSource.contains ("'AC Power'");
    const int pss = assertionCount (assertions, "PreventSystemSleep"), puiss = assertionCount (assertions, "PreventUserIdleSystemSleep");
    const auto timer = sleepTimer (settings);
    if (! ac) { v.line = "REFUSED - on battery: the Mac can sleep (caffeinate -s holds on AC power only; a lid-close on battery sleeps regardless)"; v.fix = kFix; return v; }
    if (pss > 0) { v.ok = true; v.line = "ok - on AC power, system sleep held off (PreventSystemSleep " + juce::String (pss) + ")"; return v; }
    if (timer && *timer == 0) { v.ok = true; v.line = "ok - on AC power, the system sleep timer is off (sleep 0)"; return v; }
    v.line = "REFUSED - the Mac can sleep: no PreventSystemSleep assertion" + juce::String (puiss > 0 ? " (only PreventUserIdleSystemSleep - caffeinate -i - which is not enough)" : "")
           + " and the sleep timer is " + (timer ? juce::String (*timer) + " min" : juce::String ("unknown"));
    v.fix = kFix; return v;
}

} // namespace ejmap::sleepguard
