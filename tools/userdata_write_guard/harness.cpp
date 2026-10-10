// userdata_write_guard (10 Oct 2026) - A SETTINGS WRITE MUST NOT DESTROY WHAT IT DID NOT AUTHOR.
//
// WHY THIS GUARD EXISTS AT ALL, which is a finding in itself. The rule it tests was already written as a pure
// predicate (Source/EJUserDataWrite.h) for exactly the right reason - open list 217: the gate links harnesses and
// never EchoJayAPI.cpp, so anything asserted about that file is presence-only, a string a pin can grep for and
// not behaviour a suite can run. But the only thing executing those predicates was tools/mapfps_test, and
// mapfps_test IS NOT REGISTERED IN ctest. The rule was moved into a predicate so a test could run it, and then
// the test that ran it was outside the gate. A predicate nobody executes in the gate is a text pin with extra
// steps.
//
// WHAT IT GUARDS. EchoJayAPI::saveUserSettings is a read-modify-write: GET /api/data, copy the user's
// collections forward, add the profile, POST the result. Two defects have lived in that shape:
//
//   (1) A FAILED READ BECAME A WRITE (open list 225, closed 23 Sep). It fell through to writing chats, albums,
//       reviews and refTracks as EMPTY ARRAYS, so a failed GET submitted a record asserting the user has none
//       of any of them.
//   (2) AN ABSENT KEY BECAME AN EMPTY ARRAY (closed 10 Oct, from B). The 23 Sep fix deliberately left this,
//       recording that omitting a key would be safe ONLY under MERGE semantics and that merge-or-replace was
//       UNANSWERED. B has now read the server: A MISSING KEY KEEPS THE STORED VALUE, AN EXPLICIT EMPTY ARRAY
//       CLEARS IT. So `[]` was never a harmless echo - it is the one shape that deletes.
//
// BOTH DIRECTIONS, on every rule here: a guard that only ever passes is as useless as one that only ever fails.
// The RED for (2) was measured before the fix by reverting the predicate to the old four keys and the old
// "write it anyway" answer: three assertions went red - the OMITTED rule, the key count, and pinnedProjects.
#include <JuceHeader.h>
#include "EJUserDataWrite.h"
#include <cstdio>

namespace {
int failures = 0;
void check (bool ok, const juce::String& w, const juce::String& d = {})
{
    std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(),
                 d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : "");
    if (! ok) ++failures;
}
} // namespace

int main()
{
    std::setvbuf (stdout, nullptr, _IOLBF, 0);
    using namespace echojay;
    std::printf ("userdata_write_guard: a failed read must not write, and an absent key must not be emptied\n");

    std::printf ("\n== PIN1: A FAILED READ MUST NOT BECOME A WRITE (open list 225) ==\n");
    {
        // The one that matters, on its own rather than only as a row of a table: a good read proceeds.
        check (userDataWriteMayProceed (200, true, true),
               "PIN1: a 200 with an object body and a live root proceeds");

        // The three ways the old code shipped an empty record, each named, because a table alone would not say
        // WHICH input was load-bearing.
        check (! userDataWriteMayProceed (500, true, true),
               "PIN1: a non-200 is a FAILED READ, not an empty account");
        check (! userDataWriteMayProceed (200, false, false),
               "PIN1: a 200 carrying a non-object body says nothing about the account");
        check (! userDataWriteMayProceed (200, true, false),
               "PIN1: a 200 whose root would not resolve says nothing either");

        // 0 IS A STATUS TOO. The transport leaves statusCode at 0 when the connection never happened, which is
        // the commonest real failure and the one a `!= 200` written as `>= 400` would miss.
        check (! userDataWriteMayProceed (0, true, true),
               "PIN1: statusCode 0, an unreachable server, refuses");

        // ...and exactly ONE of the eight combinations proceeds, so a predicate returning a constant fails this.
        int yes = 0;
        for (int sc = 0; sc < 2; ++sc)
            for (int ob = 0; ob < 2; ++ob)
                for (int rt = 0; rt < 2; ++rt)
                    if (userDataWriteMayProceed (sc ? 200 : 500, ob != 0, rt != 0)) ++yes;
        check (yes == 1, "PIN1: exactly ONE of the eight input combinations proceeds",
               juce::String (yes));
    }

    std::printf ("\n== PIN2: AN OMITTED KEY, NEVER AN EMPTY ARRAY (10 Oct 2026, from B) ==\n");
    {
        check (userDataMayForwardKey (true),
               "PIN2: a key the read CARRIED is forwarded");
        check (! userDataMayForwardKey (false),
               "PIN2: a key the read did NOT carry is OMITTED - under the merge semantics B confirmed, "
               "writing [] for it is a DELETE");

        // THE FIVE KEYS, by name and by count. The count is load-bearing: pinnedProjects was the one missing, so
        // a settings save unpinned every song under replace semantics and survived under merge only because the
        // key was absent. Luck is not a mechanism.
        const auto keys = userDataForwardedKeys();
        check (keys.size() == 5, "PIN2: five collection keys are forwarded",
               juce::String (keys.size()) + ": " + keys.joinIntoString (","));
        for (const char* k : { "chats", "albums", "reviews", "refTracks", "pinnedProjects" })
            check (keys.contains (k),
                   juce::String ("PIN2: \"") + k + "\" is forwarded from the read");

        // ...and NOT the profile, which this client DOES author. A forwarded key is one the plugin must hand
        // back untouched; the profile is the only thing this write is actually for.
        check (! keys.contains ("profile"),
               "PIN2: \"profile\" is NOT on the forwarded list - it is the one thing this write authors");

        // No duplicates: a key forwarded twice would be a second writer for one field, which is the fault shape
        // this codebase keeps meeting (two authors for one string).
        juce::StringArray uniq (keys); uniq.removeDuplicates (false);
        check (uniq.size() == keys.size(), "PIN2: no key is forwarded twice",
               juce::String (keys.size()) + " -> " + juce::String (uniq.size()));
    }

    std::printf ("\n== PIN3: baseUpdatedAt IS A VERSION THE READ SUPPLIED, OR IT IS NOT SENT ==\n");
    {
        check (userDataMaySendBaseUpdatedAt ("2026-10-10T12:00:00.000Z"),
               "PIN3: a stamp the read supplied is sent as baseUpdatedAt");
        check (! userDataMaySendBaseUpdatedAt (""),
               "PIN3: an EMPTY stamp is not sent - a write must not claim to be based on a version it "
               "never read, because the server would take it as a conflict check that PASSED");
    }

    std::printf ("\n==== userdata_write_guard: %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
