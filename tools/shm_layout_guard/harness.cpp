// shm_layout_guard — 21t-g item 1d (26 Sep 2026): the registry slot's layout did NOT move when its alignment
// claim was lowered from 128 to 64.
//
// WHY A GOLDEN IMAGE AND NOT A ROUND TRIP THROUGH THE SHIPPED WRITER. A round trip through this binary's own
// writer and reader agrees with itself whatever the layout is - it would pass just as happily on a struct where
// every field had moved. What has to be proved is agreement with the BYTES IN THE FIELD: every installed EchoJay
// writes a RegistrySlot at these offsets, and an older Link is writing them into the same file right now. So the
// guard builds a 128-byte image by hand, at the literal offsets, with values chosen to be unmistakable, and reads
// it through the CURRENT struct. If a field moved, the value it reads back is wrong.
//
// It also proves the two facts the mapping depends on: the stride (slot N at 64 + N*128) and that a slot can be
// read at the 64-byte-aligned address the mapping actually produces.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "LinkShm.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <cstring>

int main()
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    int failures = 0;
    auto check = [&] (bool ok, const juce::String& what, const juce::String& detail = {})
    {
        std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", what.toRawUTF8(),
                     detail.isNotEmpty() ? ("  [" + detail + "]").toRawUTF8() : "");
        if (! ok) ++failures;
    };

    // ---- the sizes and the claim -------------------------------------------------------------------------
    check (sizeof (RegistrySlot) == 128, "a slot is 128 bytes",
           juce::String ((int) sizeof (RegistrySlot)));
    check (sizeof (RegistryHeader) == 64, "the header is 64 bytes",
           juce::String ((int) sizeof (RegistryHeader)));
    check (alignof (RegistrySlot) <= sizeof (RegistryHeader),
           "the slot claims no more alignment than the mapping provides (slot 0 lands at the header's size)",
           "alignof " + juce::String ((int) alignof (RegistrySlot)) + " vs header "
           + juce::String ((int) sizeof (RegistryHeader)));

    // ---- the golden image: a slot as every shipped binary writes one ------------------------------------
    // Values picked so a wrong offset cannot coincidentally read right: distinct magic numbers, and strings
    // whose first bytes differ.
    uint8_t golden[128];
    std::memset (golden, 0, sizeof (golden));
    const uint32_t inUse = 1u, numCh = 2u, beat = 0xABCDEF01u, active = 1u;
    const float sr = 48000.0f, gain = -6.5f;
    const char* name = "Nafe Lead Vocal";
    const char* file = "audio_nafe.bin";
    const char* uid  = "lnk_0a1b2c";        // 11 chars + NUL = the field's full width
    std::memcpy (golden +   0, &inUse,  4);
    std::memcpy (golden +   4, name,    std::strlen (name) + 1);
    std::memcpy (golden +  44, file,    std::strlen (file) + 1);
    std::memcpy (golden +  92, &sr,     4);
    std::memcpy (golden +  96, &numCh,  4);
    std::memcpy (golden + 100, &beat,   4);
    std::memcpy (golden + 104, &active, 4);
    std::memcpy (golden + 108, uid,     std::strlen (uid) + 1);
    std::memcpy (golden + 120, &gain,   4);
    golden[124] = 2;    // placement: insert
    golden[125] = 1;    // dialCapable

    // Read it through the CURRENT struct, at the 64-byte-aligned offset the mapping produces.
    std::vector<uint8_t> map ((size_t) sizeof (RegistryHeader) + 4u * 128u, 0);
    std::memcpy (map.data() + sizeof (RegistryHeader), golden, sizeof (golden));
    const auto* slot0 = reinterpret_cast<const RegistrySlot*> (map.data() + sizeof (RegistryHeader));

    check (slot0->inUse == 1u, "inUse reads back", juce::String ((int) slot0->inUse));
    check (juce::String::fromUTF8 (slot0->displayName) == "Nafe Lead Vocal", "displayName reads back",
           juce::String::fromUTF8 (slot0->displayName));
    check (juce::String::fromUTF8 (slot0->audioFile) == "audio_nafe.bin", "audioFile reads back",
           juce::String::fromUTF8 (slot0->audioFile));
    check (std::abs (slot0->sampleRate - 48000.0f) < 0.01f, "sampleRate reads back",
           juce::String (slot0->sampleRate, 1));
    check (slot0->numChannels == 2u, "numChannels reads back", juce::String ((int) slot0->numChannels));
    check (slot0->heartbeat == 0xABCDEF01u, "heartbeat reads back",
           juce::String::toHexString ((int) slot0->heartbeat));
    check (slot0->activeFlag == 1u, "activeFlag reads back", juce::String ((int) slot0->activeFlag));
    check (juce::String::fromUTF8 (slot0->instanceUid) == "lnk_0a1b2c", "instanceUid reads back",
           juce::String::fromUTF8 (slot0->instanceUid));
    check (std::abs (slot0->gainDb - (-6.5f)) < 0.01f, "gainDb reads back", juce::String (slot0->gainDb, 2));
    check (slot0->placement == 2, "placement reads back", juce::String ((int) slot0->placement));
    check (slot0->dialCapable == 1, "dialCapable reads back", juce::String ((int) slot0->dialCapable));

    // ---- the stride: slot N is at 64 + N*128, which is what regSlots() + N must produce ------------------
    {
        const auto* base = reinterpret_cast<const RegistrySlot*> (map.data() + sizeof (RegistryHeader));
        const auto* third = base + 2;
        const size_t byteOffset = (size_t) (reinterpret_cast<const uint8_t*> (third) - map.data());
        check (byteOffset == sizeof (RegistryHeader) + 2u * 128u,
               "slot 2 sits at the header's size plus two strides - the mapping arithmetic is unchanged",
               juce::String ((int) byteOffset));
    }

    // ---- 21t-g item 6d: a uid that would be truncated is REFUSED, not cut -------------------------------
    // The field is char[12] - 11 usable - and every uid the product makes is 10 hex characters. The old strncpy
    // cut anything longer in silence, and a cut uid is a Link that exists and cannot be found by name.
    {
        std::vector<uint8_t> reg ((size_t) sizeof (RegistryHeader) + (size_t) kRegMaxSlots * 128u, 0);
        void* map = reg.data();
        check (sizeof (RegistrySlot::instanceUid) == 12,
               "6d. the uid field is 12 bytes - 11 characters and a terminator",
               juce::String ((int) sizeof (RegistrySlot::instanceUid)));
        const int okSlot = LinkShm::claimSlot (map, "Nafe Lead Vocal", "nafe.bin", "0a1b2c3d4e", 48000.0f, 2);
        check (okSlot >= 0, "6d. a real product uid (10 hex characters) is accepted", juce::String (okSlot));
        const auto readBack = juce::String::fromUTF8 (reinterpret_cast<const RegistrySlot*> (
                                  reg.data() + sizeof (RegistryHeader))[okSlot >= 0 ? okSlot : 0].instanceUid);
        check (readBack == "0a1b2c3d4e", "6d. ...and reads back whole", readBack);
        const int tooLong = LinkShm::claimSlot (map, "Too Long", "x.bin", "trk_levels_1", 48000.0f, 2);
        check (tooLong < 0,
               "6d. a uid too long for the field is REFUSED (RED as it stood: strncpy cut it and said nothing)",
               juce::String (tooLong));
    }

    // ---- 06d item 4 (8 Oct 2026): THE STRIP SHOWS WHAT THE DAW HEARS, BARS INCLUDED --------------------
    // Sean 11:22: moving a Link channel's fader did not move its meter. The Link taps its meters POST-chain and
    // PRE-fader on purpose (the 21t-d ruling: levelling a group needs what each channel delivers INTO its trim),
    // flags the frame kFrameHasPreTrim, and the V2 adds the trim back AT INGEST. That conversion covered the
    // loudness fields only, and the bar is drawn from the per-channel ones.
    std::printf ("== 06d item 4: as-heard conversion, and the ingest site converts EVERY displayed field ==\n");
    {
        LinkMeterFrame f {};
        f.fieldsMask = kFrameHasPreTrim;
        check (framePreTrim (f), "the frame says it published pre-trim");
        // The arithmetic, both directions, on the figure a -6 dB fader produces.
        check (std::abs (frameLoudnessAsHeard (-14.0f, -6.0f, f) + 20.0f) < 0.001f,
               "a -6 dB trim reads 6 dB lower as-heard (-14 -> -20)",
               juce::String (frameLoudnessAsHeard (-14.0f, -6.0f, f), 2));
        check (std::abs (frameLoudnessAsHeard (-14.0f, 0.0f, f) + 14.0f) < 0.001f,
               "and at trim 0 it is the published figure unchanged - a fader at unity cannot move a meter");
        // A SILENT field stays silent: -100 is the frame's "no reading", and adding a trim to it would invent one.
        check (frameLoudnessAsHeard (-100.0f, -6.0f, f) <= -99.0f,
               "a field with no reading is not shifted into a fake one",
               juce::String (frameLoudnessAsHeard (-100.0f, -6.0f, f), 1));
        // A frame WITHOUT the bit is post-trim already and must not be touched twice.
        LinkMeterFrame oldFrame {};
        check (std::abs (frameLoudnessAsHeard (-14.0f, -6.0f, oldFrame) + 14.0f) < 0.001f,
               "an older Link's frame carries no pre-trim bit and is left exactly as it always was");

        // STRUCTURAL, and this is the half that catches the next one: every field the strip DISPLAYS must be in
        // the ingest conversion. The bug was not the arithmetic - it was a list that had six fields missing, in a
        // block whose own comment promises "ONE conversion, here at ingest, so every reader sees one consistent
        // figure". A new frame field added and forgotten is the same bug again.
        std::ifstream fed ("Source/PluginEditor.cpp");
        std::stringstream sed_;
        sed_ << fed.rdbuf();
        const juce::String src (sed_.str());
        const int at = src.indexOf ("if (framePreTrim (f))");
        check (at > 0, "found the ingest conversion block");
        // The window has to span the whole conversion block. 1800 characters stopped four lines short of the end
        // and the leg reported the product as broken - a boundary the leg chose, not a fault it found. Bounded at
        // the block's own closing brace instead, with a generous cap as a backstop.
        const int closeAt = src.indexOf (at, "\n            }");
        const juce::String block = src.substring (at, closeAt > at ? closeAt : at + 4000);
        for (const char* fld : { "momentary", "shortTerm", "integrated", "truePeakMax", "truePeakCur",
                                 "shortTermTP", "shortTermMax",
                                 "peakL", "peakR", "peakFastL", "peakFastR", "rmsL", "rmsR" })
            check (block.contains (juce::String ("st.frame.") + fld + " ")
                       || block.contains (juce::String ("st.frame.") + fld + "="),
                   juce::String ("ingest converts st.frame.") + fld, fld);
    }

    std::printf ("\n==== shm_layout_guard: %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
