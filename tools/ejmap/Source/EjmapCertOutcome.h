/*
  EjmapCertOutcome.h

  WHICH FAILURES ARE LICENSING FACTS. Spec section 6: a plugin present but unlicensed on
  this machine is recorded `unlicensed_on_host`, never scored as a defect.

  Licence-bound is decided two ways, and the second outranks the first:
    - BY BUNDLE: the PACE rule (EjmapCertDriver.h isPaceWrapped, the probe line's own
      rule), or a bundle the driver cannot find, so it cannot be cleared.
    - BY BEHAVIOUR: the product brought up PACE's UI in the probe's process tree. Found
      29 Sep on kHs Compressor: PACEEdenExperience appeared 3.4 s into --list-params,
      yet its bundle carries no Eden bundle and no PACE bytes, so the bundle scan missed
      it. Behavioural evidence beats the bundle scan. A product that shows PACE's
      activation window IS licence-bound, whatever its bundle says.

  A window from something OTHER than PACE is still recorded as a window, but is not
  called a licensing fact: nothing has shown what it is. A timeout or a crash with no
  licence evidence stays "not reproduced", because a hang is not proof of a licence.
*/

#pragma once

#include <juce_core/juce_core.h>

namespace ejmap::certoutcome
{

// The owner names the driver records for a window, e.g. "PACEEdenExperience [pid 49265]".
inline bool isPaceUi (const juce::StringArray& windowOwners)
{
    for (const auto& o : windowOwners)
        if (o.containsIgnoreCase ("PACE")) return true;
    return false;
}

enum class Failure { unlicensedOnHost, notReproduced };

inline Failure classifyFailure (bool licenceBoundByBundle, const juce::StringArray& windowOwners)
{
    return (licenceBoundByBundle || isPaceUi (windowOwners)) ? Failure::unlicensedOnHost
                                                             : Failure::notReproduced;
}

} // namespace ejmap::certoutcome
