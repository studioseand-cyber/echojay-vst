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
// ROUND 5's PINS, the same way: one header per part.
#if __has_include("k5a_pins.h")
 #include "k5a_pins.h"
 #define EJ_K5A 1
#endif
#if __has_include("k5b_pins.h")
 #include "k5b_pins.h"
 #define EJ_K5B 1
#endif
#if __has_include("k5c_pins.h")
 #include "k5c_pins.h"
 #define EJ_K5C 1
#endif
#if __has_include("k5d_pins.h")
 #include "k5d_pins.h"
 #define EJ_K5D 1
#endif
#if __has_include("k6a_pins.h")
 #include "k6a_pins.h"
 #define EJ_K6A 1
#endif
#if __has_include("k6b_pins.h")
 #include "k6b_pins.h"
 #define EJ_K6B 1
#endif
#if __has_include("k7a_pins.h")
 #include "k7a_pins.h"
 #define EJ_K7A 1
#endif
#if __has_include("k7b_pins.h")
 #include "k7b_pins.h"
 #define EJ_K7B 1
#endif
// FIX 8a's PINS: the press uses exactly what is on screen.
#if __has_include("k8a_pins.h")
 #include "k8a_pins.h"
 #define EJ_K8A 1
#endif
// ROUND 8b's PINS: the audition switch and the level match.
#if __has_include("k8b_pins.h")
 #include "k8b_pins.h"
 #define EJ_K8B 1
#endif
// ROUND 8c's PINS: the after curve and the closed loop.
#if __has_include("k8c_pins.h")
 #include "k8c_pins.h"
 #define EJ_K8C 1
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
#ifdef EJ_K5A
    k5apins::runK5aPins (check);
#endif
#ifdef EJ_K5B
    k5bpins::runK5bPins (check);
#endif
#ifdef EJ_K5C
    k5cpins::runK5cPins (check);
#endif
#ifdef EJ_K5D
    k5dpins::runK5dPins (check);
#endif
#ifdef EJ_K6A
    k6apins::runK6aPins (check);
#endif
#ifdef EJ_K6B
    k6bpins::runK6bPins (check);
#endif
#ifdef EJ_K7A
    k7apins::runK7aPins (check);
#endif
#ifdef EJ_K7B
    k7bpins::runK7bPins (check);
#endif
#ifdef EJ_K8A
    k8apins::runK8aPins (check);
#endif
#ifdef EJ_K8B
    k8bpins::runK8bPins (check);
#endif
#ifdef EJ_K8C
    k8cpins::runK8cPins (check);
#endif
    std::cout << (failN == 0 ? "PASS" : "FAIL") << "  (" << passN << " ok, " << failN << " failed)\n";
    return failN == 0 ? 0 : 1;
}
