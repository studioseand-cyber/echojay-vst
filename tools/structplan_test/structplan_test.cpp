/*  structplan_test — RACK_STRUCTURE_EDIT_SPEC phase 1's gate: the plan and
    journal machinery, pure and file-level, no processor in the room.

    Proves, by content:
      1. A plan round-trips through the journal (write -> read -> equal).
      2. Pre-images are ABSOLUTE and a replayed restore is idempotent.
      3. The identity guard catches the §3e same-name swap — the test that
         has been waiting four days to pass.
      4. A capability-incapable Link refuses the whole plan.
      5. The plan computation, against a synthetic rack with REAL-shaped
         identity — hex uids through the normalization seam, because that
         encoding shipped a feature which couldn't write.

    File IO goes to a scratch dir passed as argv[1].
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
using namespace LinkShm;
using SE = StructureEdit::OpType;

juce::String opsBrief (const StructureEdit::Plan& p)
{
    juce::String s;
    for (const auto& op : p.ops)
        s << (op.type == SE::Remove ? "R" : op.type == SE::Move ? "M"
              : op.type == SE::Create ? "C" : "W")
          << "(" << op.from << ">" << op.to << ")";
    return s;
}
} // namespace

int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    if (argc < 2) { std::printf ("structplan_test: need a scratch dir argv[1]\n"); return 2; }
    const juce::String dir = juce::String::fromUTF8 (argv[1]) + "/";
    juce::ScopedJuceInitialiser_GUI juceInit;
    using namespace StructureEdit;

    // REAL-shaped base identity: sidecar slots with HEX uids, normalized
    // through the one seam. Two same-named slots (two "EchoJay EQ") so the
    // §3e swap is constructible.
    RackSidecarSlot s0; s0.name = "EchoJay EQ";   s0.uid = juce::String::toHexString (0x454A4251); s0.fp = "fp-A";
    RackSidecarSlot s1; s1.name = "AMEK EQ 250";  s1.uid = juce::String::toHexString (0x11223344);
    RackSidecarSlot s2; s2.name = "EchoJay EQ";   s2.uid = juce::String::toHexString (0x454A4252); s2.fp = "fp-B";
    RackSidecarSlot s3; s3.name = "API-560 (s)";  // no identity published at all
    const std::vector<SlotIdentity> base = {
        SlotIdentity::fromSidecar (s0), SlotIdentity::fromSidecar (s1),
        SlotIdentity::fromSidecar (s2), SlotIdentity::fromSidecar (s3) };

    std::printf ("== identity normalization (the hex seam) ==\n");
    check (base[0].uid == juce::String (0x454A4251),
           "sidecar hex uid arrives DECIMAL in the guard's identity", base[0].uid);
    check (base[3].uid.isEmpty(), "absent identity stays absent (no opinion)");

    std::printf ("== the identity guard, including the four-day test ==\n");
    {
        check (verifyBaseIdentity (base, base) == BaseCheck::Match,
               "identical rack: Match");
        auto shorter = base; shorter.pop_back();
        check (verifyBaseIdentity (base, shorter) == BaseCheck::CountMismatch,
               "count change: CountMismatch");
        // THE §3e SAME-NAME SWAP: swap the two "EchoJay EQ" slots. Names
        // per index still match; identity does not. The old names-in-order
        // guard passed this; the identity guard must not.
        auto swapped = base;
        std::swap (swapped[0], swapped[2]);
        check (verifyBaseIdentity (base, swapped) == BaseCheck::IdentityMismatch,
               "the §3e same-name swap is CAUGHT (four days waiting)");
        // Absent-is-no-opinion: a live rack that lost its uid field (older
        // publisher) still matches on names — degradation, not refusal.
        auto anon = base;
        for (auto& s : anon) { s.uid.clear(); s.fp.clear(); }
        check (verifyBaseIdentity (base, anon) == BaseCheck::Match,
               "absent fields on one side: no opinion, still Match");
        // But a swap of the anonymous rack is INVISIBLE to the guard —
        // stated honestly: identity only catches what identity carries.
        auto anonSwapped = anon;
        std::swap (anonSwapped[0], anonSwapped[2]);
        check (verifyBaseIdentity (anon, anonSwapped) == BaseCheck::Match,
               "an identity-less same-name swap still passes (recorded limit, "
               "not a claim)");
    }

    std::printf ("== capability refusal ==\n");
    check (accept (false) == Accept::RefuseIncapable,
           "incapable Link: the WHOLE plan refuses");
    check (accept (true) == Accept::Proceed, "capable Link proceeds");

    std::printf ("== the plan computation, synthetic rack, real identity ==\n");
    {
        // Base [EQ-A, AMEK, EQ-B, API]. The user: removed AMEK, moved API
        // before EQ-B, edited EQ-A (clean) and EQ-B (WITHHELD), created a
        // Comp at the end with a seed state.
        std::vector<CurrentSlot> current = {
            { base[0], 0, /*edited*/ true,  /*withheld*/ false, "state-eqa" },
            { base[3], 3, false, false, {} },
            { base[2], 2, true,  true,  "state-eqb" },
            { { "EchoJay Compressor", juce::String (0x454A4243), "fp-C" },
              -1, false, false, "seed-comp" },
        };
        const auto plan = computePlan ("uid-test", base, current);
        check (plan.removing == 1 && plan.moving >= 1 && plan.creating == 1
                 && plan.committing == 1,
               "counts: remove 1, move >=1, create 1, commit 1", opsBrief (plan));
        check (plan.changed == 2 && plan.withheldEdited == 1 && plan.untouched == 1,
               "asymmetry: changed 2, withheld-edited 1, untouched 1");
        // Order: Removes first, Moves, Creates, Commits — and the ONLY
        // commit is EQ-A; the withheld-edited EQ-B is never in the ops.
        check (! plan.ops.empty() && plan.ops.front().type == SE::Remove
                 && plan.ops.front().from == 1,
               "AMEK's remove leads, by base index");
        int commits = 0; bool eqbCommitted = false, compCreated = false;
        for (const auto& op : plan.ops)
        {
            if (op.type == SE::Commit)
            { ++commits; if (op.stateB64 == "state-eqb") eqbCommitted = true; }
            if (op.type == SE::Create && op.stateB64 == "seed-comp"
                && op.to == 3) compCreated = true;
        }
        check (commits == 1 && ! eqbCommitted,
               "the withheld-edited slot yields NO commit op — moved or not");
        check (compCreated, "the Create carries the main's seed at its position");
        // Determinism: same inputs, same ops.
        const auto plan2 = computePlan ("uid-test", base, current);
        check (opsBrief (plan) == opsBrief (plan2), "the computation is deterministic");

        std::printf ("== journal round-trip + idempotent restore ==\n");
        PreImages pre;
        pre.shape = base;
        pre.states = { "pre-0", "", "pre-2", "pre-3" };
        writeJournal (dir, plan, pre);
        Plan rp; PreImages rpre;
        check (readJournal (dir, "uid-test", rp, rpre), "journal reads back");
        check (opsBrief (rp) == opsBrief (plan) && rp.uid == plan.uid
                 && rp.baseIdentity.size() == plan.baseIdentity.size()
                 && rp.baseIdentity[0].uid == plan.baseIdentity[0].uid
                 && rp.baseIdentity[0].fp  == plan.baseIdentity[0].fp,
               "plan round-trips: ops, uid, identity fields");
        check (rpre.shape.size() == 4 && rpre.states == pre.states,
               "pre-images round-trip whole");
        // Idempotence: restore a half-mutated model once, then AGAIN — the
        // second pass changes nothing, because pre-images are absolute.
        RackModel mangled { { base[2], base[0] }, { "x", "y" } };
        const auto once  = restoreFromJournal (rpre, mangled);
        const auto twice = restoreFromJournal (rpre, once);
        check (once.shape.size() == 4 && once.states == pre.states,
               "restore replaces WHOLESALE from absolute pre-images");
        check (twice.states == once.states
                 && twice.shape.size() == once.shape.size()
                 && twice.shape[0].uid == once.shape[0].uid,
               "replaying the restore is IDEMPOTENT (crash-retry safe)");
        check (verifyBaseIdentity (once.shape, rpre.shape) == BaseCheck::Match,
               "the restored shape IS the pre-image shape, by the guard itself");
        juce::File (journalPath (dir, "uid-test")).deleteFile();
        Plan gone; PreImages gonePre;
        check (! readJournal (dir, "uid-test", gone, gonePre),
               "a completed (deleted) journal reads absent — restore runs at most once");
    }

    std::printf ("== negative control ==\n");
    {
        const int before = failures;
        check (false, "NEGATIVE CONTROL - this line is SUPPOSED to fail");
        check (failures == before + 1, "the harness caught the planted failure");
        --failures;
    }

    std::printf ("\nstructplan_test: %s\n", failures ? "FAILED" : "PASS");
    return failures ? 1 : 0;
}
