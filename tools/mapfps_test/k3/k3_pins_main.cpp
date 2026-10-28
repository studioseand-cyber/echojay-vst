// K3's pins on their own: the mr and mb rows of k3_pins.h, printed the way
// mapfps_test prints rows, so a gate or a mutant runner reads them the same.
//
// usage: tools/mapfps_test/k3/build_and_run.sh   (BUILD_DIR as for ma10)

#include <JuceHeader.h>
#include <iostream>
#include "k3_pins.h"

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
    std::cout << (failN == 0 ? "PASS" : "FAIL") << "  (" << passN << " ok, " << failN << " failed)\n";
    return failN == 0 ? 0 : 1;
}
