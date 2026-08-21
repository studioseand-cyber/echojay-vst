/*  racklock_test — the rack lock's pure decisions and its two transports,
    pinned with no processor in the room (RACK_LOCK_BUILD_BRIEF §2a/§3).

    What it proves BY CONTENT:
      - the sidecar's lastEditMs field genuinely round-trips (2a: a field
        that is always zero reads as "edited at the epoch" and the main
        would acquire instantly, forever, looking exactly like a pass);
      - a sidecar written WITHOUT the field reads 0 (old-writer tolerance);
      - RackLock::read — absent, stale, fresh-foreign, fresh-mine;
      - RackLock::decide — FCFS beats a holder, holding renews, a recent
        local edit waits, quiet (or no-data) acquires;
      - the racklock-<uid>.json writer/reader pair (one author, both sides
        and this test) round-trips id and owner, and freshness expires.

    Usage: run via build_and_run.sh, which passes a scratch dir as argv[1].
*/

#include <JuceHeader.h>
#include "LinkShm.h"

#include <cstdio>

namespace
{
int failures = 0;
void check (bool ok, const juce::String& what, const juce::String& detail = {})
{
    std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", what.toRawUTF8(),
                 detail.isNotEmpty() ? ("  [" + detail + "]").toRawUTF8() : "");
    if (! ok) ++failures;
}
} // namespace

int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    if (argc < 2) { std::printf ("racklock_test: need a scratch dir argv[1]\n"); return 2; }
    const juce::String dir = juce::String::fromUTF8 (argv[1]) + "/";
    juce::ScopedJuceInitialiser_GUI juceInit;

    using RL = LinkShm::RackLock;

    std::printf ("== sidecar lastEditMs transport (2a) ==\n");
    {
        const double stamp = (double) juce::Time::currentTimeMillis() - 1234.0;
        LinkShm::RackSidecar rc;
        rc.valid = true; rc.uid = "testuid00"; rc.name = "Vocal"; rc.revision = 3;
        LinkShm::RackSidecarSlot s;
        s.name = "EchoJay EQ"; s.format = "EchoJayBuiltin";
        s.lastEditMs = stamp;
        rc.slots.push_back (s);
        LinkShm::RackSidecarSlot s2;
        s2.name = "Other"; s2.format = "VST3";
        s2.lastEditMs = stamp - 5000.0;
        rc.slots.push_back (s2);
        LinkShm::writeRackSidecar (dir, rc);

        const auto back = LinkShm::readRackSidecar (dir, "testuid00");
        check (back.valid, "sidecar round-trips valid");
        check (back.slots.size() == 2, "both slots survive",
               juce::String ((int) back.slots.size()));
        check (back.slots.size() == 2 && back.slots[0].lastEditMs == stamp,
               "lastEditMs is genuinely WRITTEN and READ (not zero)",
               juce::String (back.slots.empty() ? 0.0 : back.slots[0].lastEditMs));
        check (back.slots.size() == 2 && back.slots[1].lastEditMs == stamp - 5000.0,
               "per-slot stamps stay distinct");
    }
    {
        // An old writer (no field): reads 0, which decide() treats as quiet.
        LinkShm::RackSidecar rc;
        rc.valid = true; rc.uid = "testuid01"; rc.name = "Old"; rc.revision = 1;
        LinkShm::RackSidecarSlot s; s.name = "X"; s.format = "AU";
        rc.slots.push_back (s);   // lastEditMs defaulted 0 -> key not written
        LinkShm::writeRackSidecar (dir, rc);
        const auto back = LinkShm::readRackSidecar (dir, "testuid01");
        check (back.slots.size() == 1 && back.slots[0].lastEditMs == 0.0,
               "absent field reads 0 (old-writer sidecar)");
    }

    std::printf ("== RackLock::read ==\n");
    {
        check (RL::read ({},        0.0,    "me") == RL::Claim::Free,  "absent file: Free");
        check (RL::read ("other",   9999.0, "me") == RL::Claim::Free,  "stale foreign: Free");
        check (RL::read ("other",   100.0,  "me") == RL::Claim::Other, "fresh foreign: Other");
        check (RL::read ("me",      100.0,  "me") == RL::Claim::Mine,  "fresh mine: Mine");
        check (RL::read ("me",      9999.0, "me") == RL::Claim::Free,  "stale mine: Free (renewals lapsed)");
        check (RL::read ("anyone",  100.0,  {})   == RL::Claim::Other, "Link side (no id of its own): any fresh claim is Other");
    }

    std::printf ("== RackLock::decide ==\n");
    {
        const double now = 1000000.0;
        check (RL::decide (RL::Claim::Other, false, now, 0.0) == RL::Acquire::HeldByOther,
               "FCFS: fresh foreign holds, we wait");
        check (RL::decide (RL::Claim::Other, true, now, 0.0) == RL::Acquire::HeldByOther,
               "a fresh foreign id beats even a holder (two writers is the one forbidden state)");
        check (RL::decide (RL::Claim::Mine, true, now, now - 100.0) == RL::Acquire::Take,
               "holding renews, recency does not re-gate the holder");
        check (RL::decide (RL::Claim::Free, false, now, now - 100.0) == RL::Acquire::WaitRecency,
               "a local edit 100ms ago: wait");
        check (RL::decide (RL::Claim::Free, false, now, now - (LinkShm::kRackLockRecencyMs + 1.0))
                   == RL::Acquire::Take,
               "a local edit older than the window: acquire");
        check (RL::decide (RL::Claim::Free, false, now, 0.0) == RL::Acquire::Take,
               "no stamp at all (0): acquire — 2a's transport check above is "
               "what keeps this arm honest");
    }

    std::printf ("== racklock file: one author, both directions ==\n");
    {
        LinkShm::writeRackLockFile (dir, "testuid02", "lock-abc", "EchoJay on \"Stereo Out\"");
        juce::String id, owner; double age = -1.0;
        const bool present = LinkShm::readRackLockFile (dir, "testuid02", id, owner, age);
        check (present,                       "file written and found");
        check (id == "lock-abc",              "lockId round-trips", id);
        check (owner == "EchoJay on \"Stereo Out\"", "owner name round-trips", owner);
        check (age >= 0.0 && age < LinkShm::kRackLockExpireMs, "freshly written reads fresh",
               juce::String (age));
        check (RL::read (id, age, {}) == RL::Claim::Other, "the Link would show locked");
        check (RL::read (id, age + LinkShm::kRackLockExpireMs, {}) == RL::Claim::Free,
               "the same file past expiry reads Free (owner-gone recovery)");
        juce::File (LinkShm::racklockPath (dir, "testuid02")).deleteFile();
        juce::String id2, owner2; double age2 = -1.0;
        check (! LinkShm::readRackLockFile (dir, "testuid02", id2, owner2, age2),
               "clean release: deleted file reads absent");
    }

    std::printf ("== negative control ==\n");
    {
        const int before = failures;
        check (false, "NEGATIVE CONTROL - this line is SUPPOSED to fail");
        check (failures == before + 1, "the harness caught the planted failure");
        --failures;   // the control is not a real failure
    }

    std::printf ("\nracklock_test: %s\n", failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
