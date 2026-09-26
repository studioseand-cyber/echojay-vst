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

    std::printf ("\n==== shm_layout_guard: %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
