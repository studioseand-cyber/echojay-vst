// K3's pins on their own: the kr and mb rows of k3_pins.h, printed the way
// mapfps_test prints rows, so a gate or a mutant runner reads them the same.
//
// usage: tools/mapfps_test/k3/build_and_run.sh   (BUILD_DIR as for ma10)

#include <JuceHeader.h>
#include <iostream>
#include "k3_pins.h"
// ROUND 4's PINS, ONE HEADER PER PART, so each part commits its own file. A
// part not yet in the tree is simply absent from the run.
#if __has_include("k4a_pins.h")
 #include "k4a_pins.h"
 #define EJ_K4A 1
#endif
#if __has_include("k4b_pins.h")
 #include "k4b_pins.h"
 #define EJ_K4B 1
#endif
#if __has_include("k4c_pins.h")
 #include "k4c_pins.h"
 #define EJ_K4C 1
#endif

int main()
{
    int passN = 0, failN = 0;
    auto check = [&] (bool ok, const char* name, const juce::String& detail = {})
    {
        if (ok) { ++passN; std::cout << "  ok    " << name << "\n"; }
        else
        {
            ++failN;
            std::cout << "  FAIL  " << name << "\n";
            if (detail.isNotEmpty()) std::cout << "        " << detail << "\n";
        }
    };
    k3pins::runK3Pins (check);
#ifdef EJ_K4A
    k4apins::runK4aPins (check);
#endif
#ifdef EJ_K4B
    k4bpins::runK4bPins (check);
#endif
#ifdef EJ_K4C
    k4cpins::runK4cPins (check);
#endif
    std::cout << (failN == 0 ? "PASS" : "FAIL") << "  (" << passN << " ok, " << failN << " failed)\n";
    return failN == 0 ? 0 : 1;
}
