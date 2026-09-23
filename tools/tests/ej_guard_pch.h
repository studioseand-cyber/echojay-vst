// The one precompiled header for the whole guard suite.
//
// ORDER MATTERS AND IS NOT ARBITRARY. Every guard in tools/ opens with <CoreFoundation/CoreFoundation.h> and only
// then <JuceHeader.h>, because JuceHeader.h ends with "using namespace juce;" and MacTypes.h declares a struct Point:
// pull JUCE in first and every later CoreFoundation include fails with "reference to 'Point' is ambiguous".
// A precompiled header is injected at the TOP of the translation unit, so it has to preserve that order itself.
// With this file as the PCH, each guard's own two includes become no-ops and no guard source changes.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
