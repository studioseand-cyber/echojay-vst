// Standalone test for the universal dialable param contract (no JUCE). Build:
//   g++ -std=c++17 -O2 -I../Source param_schema_test.cpp -o schematest && ./schematest

#include "EedParamSchema.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

using namespace echojay;
static int g_fail = 0;
static void check (bool c, const std::string& w)
{ std::printf ("  [%s] %s\n", c ? "PASS" : "FAIL", w.c_str()); if (! c) ++g_fail; }

static ParamSchema makeSchema()
{
    return ParamSchema ({
        { "level_db", "dB", -60.0, 24.0, 0.0, "output level", false },
        { "pan",      "",    -1.0,  1.0, 0.0, "stereo position", false },
        { "auto_gain","",     0.0,  1.0, 0.0, "cancel loudness change", true },
        { "type",     "",     0.0,  3.0, 0.0, "the saturation curve", false,
          { "tube", "tape", "diode", "soft" } },
    });
}

int main()
{
    const auto schema = makeSchema();

    std::printf ("== id normalisation (a miss is a knob that never turns) ==\n");
    check (ParamSchema::normalizeId ("attack_ms") == "attackms", "underscores dropped");
    check (ParamSchema::normalizeId ("Attack-MS") == "attackms", "case + dash folded");
    check (ParamSchema::normalizeId ("attackMs")  == "attackms", "camelCase folded");
    check (ParamSchema::normalizeId (" attack ms ") == "attackms", "spaces dropped");

    std::printf ("== find is tolerant of separator/case, strict about identity ==\n");
    check (schema.find ("level_db") != nullptr, "exact id found");
    check (schema.find ("levelDb")  != nullptr, "camelCase id found");
    check (schema.find ("LEVEL-DB") != nullptr, "shouty kebab id found");
    check (schema.find ("level")    == nullptr, "a DIFFERENT id is not matched");
    check (schema.find ("wobble")   == nullptr, "unknown id rejected");
    check (schema.size() == 4, "schema reports its size");

    std::printf ("== clamping to the advertised range ==\n");
    const auto* lvl = schema.find ("level_db");
    check (lvl != nullptr && lvl->clamp (0.0)    == 0.0,   "in-range value untouched");
    check (lvl != nullptr && lvl->clamp (999.0)  == 24.0,  "above max clamps to max");
    check (lvl != nullptr && lvl->clamp (-999.0) == -60.0, "below min clamps to min");
    check (lvl != nullptr && lvl->clamp (-60.0)  == -60.0, "min itself is legal");

    std::printf ("== NaN maps to the default, never into a gain coefficient ==\n");
    const double nan = std::numeric_limits<double>::quiet_NaN();
    check (lvl != nullptr && lvl->clamp (nan) == lvl->def, "NaN -> default (0)");
    {
        // A NaN default would defeat the point, so prove the guard uses the real
        // default of a param whose default is not zero.
        ParamSpec p { "ratio", "", 1.0, 20.0, 4.0, "", false };
        check (p.clamp (nan) == 4.0, "NaN -> that param's own default (4)");
    }

    std::printf ("== advertisement text (what the model reads AND the server validates) ==\n");
    {
        const auto line = ParamSchema::describeLine (*schema.find ("level_db"));
        check (line == "level_db (dB, -60..24, default 0) - output level",
               "continuous param line: " + line);
    }
    {
        const auto line = ParamSchema::describeLine (*schema.find ("pan"));
        check (line == "pan (-1..1, default 0) - stereo position",
               "unitless param omits the unit: " + line);
    }
    {
        const auto line = ParamSchema::describeLine (*schema.find ("auto_gain"));
        check (line == "auto_gain (on/off, default off) - cancel loudness change",
               "boolean param reads as on/off, not 0..1: " + line);
    }
    {
        const auto line = ParamSchema::describeLine (*schema.find ("type"));
        check (line == "type (tube|tape|diode|soft, default tube) - the saturation curve",
               "choice param advertises NAMES, not an index: " + line);
    }
    check (ParamSchema::number (3.0)   == "3",    "3.0 advertises as \"3\"");
    check (ParamSchema::number (-7.5)  == "-7.5", "-7.5 keeps its decimal");
    check (ParamSchema::number (20000) == "20000","20000 is not 2e+04");

    std::printf ("== choices: a selector is as settable as a dial ==\n");
    {
        const auto* type = schema.find ("type");
        check (type != nullptr && type->indexOfChoice ("tube")  == 0, "\"tube\" -> 0");
        check (type != nullptr && type->indexOfChoice ("DIODE") == 2, "case folded");
        check (type != nullptr && type->indexOfChoice ("Low-Shelf") == -1,
               "a label that is not ours -> -1 (skipped, never guessed at)");
        check (type != nullptr && type->indexOfChoice ("") == -1, "empty -> -1");
        check (type != nullptr && type->choiceLabel (2.0) == "diode", "2 -> \"diode\"");
        check (type != nullptr && type->choiceLabel (1.4) == "tape", "1.4 rounds to \"tape\"");
        check (type != nullptr && type->choiceLabel (99.0) == "soft",
               "out of range clamps to the last choice, never reads off the end");
        check (schema.find ("level_db")->choiceLabel (0.0).empty(),
               "a continuous param has no choice label");
    }

    std::printf ("== describe() is one line per param ==\n");
    {
        const auto all = schema.describe();
        int lines = 1;
        for (char c : all) if (c == '\n') ++lines;
        check (lines == 4, "4 params -> 4 lines");
        check (ParamSchema().describe().empty(), "empty schema describes as empty");
    }

    std::printf ("\n%s (%d failures)\n", g_fail ? "FAILURES" : "ALL PASS", g_fail);
    return g_fail ? 1 : 0;
}
