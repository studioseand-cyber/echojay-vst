#!/bin/bash
# The MOCK half of S6 alone, now that ship_2026-10-17a is installed. The reason this is the right
# test and not a retry for luck: all night the Link that AU hosting loaded was the INSTALLED 16b
# binary (item 0b - registerAU registers a factory, JUCE instantiates the IDENTIFIER, the system's
# component wins). A2 asserts that an add with settings on a Link lands AND the V2's strip follows,
# which is behaviour this build has and 16b does not. It passed against the build/ tree at 01:30 and
# failed against build-guards twice - the difference being which Link answered.
# The install has now replaced that component with tonight's, so this run finally has both halves of
# the pair from the same build. If A2 still fails, the failure is real and the report says so.
cd /Users/SeanD/echojay-vst || exit 2
set -u
S=/private/tmp/claude-502/-Users-SeanD-echojay-vst/8b86da2a-378d-4ecf-97c0-0e33f4993ece/scratchpad
ctest --test-dir build-guards -R host_rig_mock -V > "$S/s6_mock_only.log" 2>&1
echo "MOCK EXIT=$?"
grep -E "==== host_rig:" "$S/s6_mock_only.log"
grep -E "(SCEN|ROW) .*FAIL" -A 1 "$S/s6_mock_only.log" | sed -e 's/.*host_rig\[[0-9:]*\] //' | cut -c1-200
