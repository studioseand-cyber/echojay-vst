// Console harness for EchoJayWorkspace::runRoundTripSelfTest() — proves the
// REAL chatToVar/parseChat objects round-trip (byte-stable, no linkUid keys
// on pre-C1 chats, channel fields intact). Build + run: ./build_and_run.sh
// (needs a completed cmake build for compile flags + the static lib).
#include "EchoJayWorkspace.h"
int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    return EchoJayWorkspace::runRoundTripSelfTest() ? 0 : 1;
}
