/*
  EjmapRoleSemantics.h

  ROLES AGAINST EJ MAP'S SEMANTICS, both directions, one file (ruled 28 Sep, shape agreed
  29 Sep). The table is DELIBERATELY ASYMMETRIC:

    role        semantic       role -> semantic                  semantic -> role
    threshold   threshold_db   only when the control's unit is dB   always
    ratio       ratio          always                               always
    attack      attack_ms      only when the unit is time           always
    release     release_ms     only when the unit is time           always
    input       input_db       only when the unit is dB             always
    output      output_db      only when the unit is dB             always
    makeup      makeup_db      only when the unit is dB             always
    mix         mix_pct        only when the unit is %              always
    key / reference / strength   none: EJ Map has no pitch dial set
    (none)      knee_db, range_db, hold_ms, tone, slope_db_oct, anything else: no role

  A threshold displaying 0-10 HAS a role and has NO semantic, which is the reason the
  section 4 sweep exists: the role says which control to turn, the sweep says what it does.

  THE ROLE IS STORED PER CONTROL (EjmapRoles.h ControlRole), and this table only supplies
  defaults, because input-as-threshold proves the role cannot be derived from the
  semantic: a 1176's Input is semantically input_db and carries role threshold. A control
  flagged input_as_threshold therefore maps to its NAME's semantic (input_db), not to
  threshold_db.

  "The unit" is a fixture's recorded unit (EjmapFixtureUnit.h), read into a family by the
  shipped displayUnitFamily and compared by the shipped unitFamiliesAgree
  (Source/EchoJayParamApply.h), so this is not a second unit rule. That makes dBu NOT dB
  (displayUnitFamily reads "dbu" as no family): a dBu threshold keeps its role and gets no
  semantic, the conservative side.
*/

#pragma once

#include "EjmapRoles.h"
#include "EchoJayParamApply.h"

namespace ejmap::rolesemantics
{

struct Row { const char* role; const char* semantic; bool unitGated; };

inline const std::vector<Row>& table()
{
    static const std::vector<Row> t {
        { "threshold", "threshold_db", true  },
        { "ratio",     "ratio",        false },
        { "attack",    "attack_ms",    true  },
        { "release",   "release_ms",   true  },
        { "input",     "input_db",     true  },
        { "output",    "output_db",    true  },
        { "makeup",    "makeup_db",    true  },
        { "mix",       "mix_pct",      true  },
    };
    return t;
}

// A fixture's unit ("dB", "mS", "%", ":1", "kHz") as a family ("db", "ms", "pct", "ratio",
// "hz"), by the shipped display reader. An empty unit is no family.
inline juce::String familyOfFixtureUnit (const juce::String& unit)
{
    if (unit.trim().isEmpty()) return {};
    return echojay::displayUnitFamily ("0 " + unit.trim());
}

// role -> semantic. Empty when the role has no semantic here, or its unit gate fails.
inline juce::String semanticForRole (const juce::String& role, const juce::String& fixtureUnit)
{
    for (const auto& r : table())
    {
        if (role != r.role) continue;
        if (! r.unitGated) return r.semantic;
        const auto family = familyOfFixtureUnit (fixtureUnit);
        if (family.isEmpty()) return {};    // gated rows need a unit that SPEAKS; absence claims nothing
        return echojay::unitFamiliesAgree (echojay::semanticUnit (r.semantic), family)
                   ? juce::String (r.semantic) : juce::String();
    }
    return {};
}

// The per-control form: a control that took the threshold role by the input-as-threshold
// fallback keeps its name's semantic.
inline juce::String semanticFor (const roles::ControlRole& c, const juce::String& fixtureUnit)
{
    if (c.flags.contains ("input_as_threshold")) return semanticForRole ("input", fixtureUnit);
    return semanticForRole (c.role, fixtureUnit);
}

// semantic -> role. Always, for the eight; nothing for every other semantic.
inline juce::String roleForSemantic (const juce::String& semantic)
{
    for (const auto& r : table())
        if (semantic == r.semantic) return r.role;
    return {};
}

} // namespace ejmap::rolesemantics
