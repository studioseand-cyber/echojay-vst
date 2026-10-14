/*  THE HOST RIG (Sean's ask A, 13 Oct 2026)
    ============================================================================
    The gate cannot open an editor, so every window-lifecycle fault has reached
    Sean untested - "closing mid-reply" has now failed three rounds running, each
    time fixed against a READING of the code rather than a RUN of it. This loads
    the real built bundles into a real host process, plays audio through them,
    opens and closes editors on a script, drives the UI through the test-only
    hook in EJRigHook.h, and asserts on what the plugin actually holds.

    WHAT IT IS NOT. It is not a replacement for Sean's Logic pass: Logic's own
    window parenting, its AU hosting service and its automation are not here. It
    is the instrument that should catch most of it first.

    WHAT IT NEEDS
      - a logged-in GUI session (it creates real windows). Over SSH or in a
        headless runner it REFUSES rather than passes - see the WindowServer
        check in main(). Sean's instruction: skip loudly, never pass quietly.
      - an isolated ECHOJAY_STATE_HOME. Refuses without one, like every harness
        in this tree (6 Sep ruling), and the hook refuses too.
      - the mock server already listening (tools/host_rig/build_and_run.sh
        starts it), reached through the auth.json this writes.

    USAGE  host_rig <V2.component> <Link.component> <mockPort>
    ============================================================================ */
#define Point CarbonDummyPointName
#define Component CarbonDummyCompName
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#undef Point
#undef Component
#include <JuceHeader.h>
#include "EJStateRoot.h"
#include "EJRigHook.h"
#include <cmath>
#include <cstdio>

// ============================================================================
//  reporting
// ============================================================================
static int failures = 0, passes = 0;
static void check (bool ok, const juce::String& what, const juce::String& detail = {})
{
    if (ok) { ++passes; std::printf ("  ok    %s\n", what.toRawUTF8()); }
    else    { ++failures; std::printf ("  FAIL  %s%s\n", what.toRawUTF8(),
                                       detail.isNotEmpty() ? ("  [" + detail + "]").toRawUTF8() : ""); }
    std::fflush (stdout);
}
static int skips = 0;
// Owed from the lifecycle legs to the capability block: ruling 3's double-click needs a slot to press, and
// the rack is empty where the lifecycle legs run. File scope because the two halves are different functions.
static bool slotDoubleClickOwed = false;
/** A LEG THAT COULD NOT RUN IS NOT A LEG THAT PASSED (14 Oct 2026). The slot double-click leg printed
    "ok  (skipped) the open rack has no slot to double-click" and added one to the pass count: ruling 3's
    only coverage was a line that asserted nothing, and the total said 40 either way. Skips are counted
    separately and named in the summary, so a pass count cannot be padded by work that did not happen. */
static void skip (const juce::String& what, const juce::String& why)
{
    ++skips; std::printf ("  SKIP  %s  [%s]\n", what.toRawUTF8(), why.toRawUTF8()); std::fflush (stdout);
}
static void leg (const char* name) { std::printf ("== %s ==\n", name); std::fflush (stdout); }

// ============================================================================
//  the AU, registered in-process from its bundle path (the plugin_load_test.mm
//  idiom - no install, so the rig tests the BUILT bundle and never the one
//  Sean has in Logic)
// ============================================================================
static bool registerAU (const juce::File& component, juce::String& identOut)
{
    auto* url = CFURLCreateWithFileSystemPath (nullptr, component.getFullPathName().toCFString(),
                                               kCFURLPOSIXPathStyle, true);
    CFBundleRef bundle = CFBundleCreate (nullptr, url); CFRelease (url);
    if (bundle == nullptr) { std::printf ("  AU: CFBundleCreate failed for %s\n", component.getFullPathName().toRawUTF8()); return false; }
    auto* comps = (CFArrayRef) CFBundleGetValueForInfoDictionaryKey (bundle, CFSTR ("AudioComponents"));
    if (comps == nullptr || CFArrayGetCount (comps) == 0) { std::printf ("  AU: no AudioComponents\n"); return false; }
    auto* d = (CFDictionaryRef) CFArrayGetValueAtIndex (comps, 0);
    auto str = [&] (const char* k) { return juce::String::fromCFString ((CFStringRef) CFDictionaryGetValue (d, juce::String (k).toCFString())); };
    auto code = [&] (const char* k) { auto s = str (k); return (OSType) ((s[0] << 24) | (s[1] << 16) | (s[2] << 8) | s[3]); };
    int version = 0;
    CFNumberGetValue ((CFNumberRef) CFDictionaryGetValue (d, CFSTR ("version")), kCFNumberIntType, &version);
    AudioComponentDescription desc {};
    desc.componentType = code ("type"); desc.componentSubType = code ("subtype"); desc.componentManufacturer = code ("manufacturer");
    auto factory = (AudioComponentFactoryFunction) CFBundleGetFunctionPointerForName (bundle, str ("factoryFunction").toCFString());
    if (factory == nullptr) { std::printf ("  AU: factory not found\n"); return false; }
    AudioComponentRegister (&desc, str ("name").toCFString(), (UInt32) version, factory);
    identOut = "AudioUnit:Effects/" + str ("type") + "," + str ("subtype") + "," + str ("manufacturer");
    return true;
}

// ============================================================================
//  one hosted plugin = one "track": the instance, its audio, and its editor
// ============================================================================
struct Track
{
    juce::String name;
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    std::unique_ptr<juce::DocumentWindow> window;
    juce::AudioBuffer<float> buf { 2, 512 };
    juce::MidiBuffer midi;
    double phase = 0.0;

    void prepare (double sr, int block)
    {
        plugin->setPlayConfigDetails (2, 2, sr, block);
        plugin->prepareToPlay (sr, block);
        buf.setSize (2, block);
    }

    /** One block of real audio: a -12 dBFS 220 Hz tone. The level matters - the
        loudness loop only arms and lands when it has heard something, so a rig
        that fed silence would prove nothing about item 4. */
    void processOneBlock (double sr)
    {
        const int n = buf.getNumSamples();
        for (int i = 0; i < n; ++i)
        {
            const float v = 0.25f * (float) std::sin (phase);
            phase += 2.0 * juce::MathConstants<double>::pi * 220.0 / sr;
            if (phase > 2.0 * juce::MathConstants<double>::pi) phase -= 2.0 * juce::MathConstants<double>::pi;
            buf.setSample (0, i, v);
            buf.setSample (1, i, v);
        }
        midi.clear();
        plugin->processBlock (buf, midi);
    }

    void openEditor()
    {
        if (editor != nullptr) return;
        editor.reset (plugin->createEditorIfNeeded());
        if (editor == nullptr) return;
        window.reset (new juce::DocumentWindow (name, juce::Colours::black,
                                                juce::DocumentWindow::closeButton, true));
        window->setUsingNativeTitleBar (true);
        window->setContentNonOwned (editor.get(), true);
        window->setVisible (true);
    }

    /** The whole point of the rig. An editor is destroyed exactly as a host
        destroys it - the window goes first, then the editor - so the plugin's
        destructor runs in the order it runs in Logic. */
    void closeEditor()
    {
        if (window != nullptr) { window->setVisible (false); window->clearContentComponent(); }
        window.reset();
        editor.reset();
    }
};

// ============================================================================
//  the automation channel, from the rig's side
// ============================================================================
struct Rig
{
    int seq = 0;

    /** Pump the message thread for ms. NEVER a bare sleep: the plugin's timers,
        the editor's 20 Hz tick and every callAfterDelay live on this thread, so
        a sleeping rig would freeze the thing it is testing.

        runDispatchLoopUntil is unavailable in a plugin build (JUCE_MODAL_LOOPS_PERMITTED
        is off), so this is the link_state_guard idiom: pending timers synchronously plus
        a short turn of the CF run loop, which is what actually delivers the editor's
        timer callbacks and every callAfterDelay. */
    static void pump (int ms)
    {
        const double end = juce::Time::getMillisecondCounterHiRes() + ms;
        while (juce::Time::getMillisecondCounterHiRes() < end)
        {
            juce::Timer::callPendingTimersSynchronously();
            CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false);
        }
    }

    /** Send a command and wait for the plugin's ack. Waiting on the ACK rather
        than on a delay is the "decisions on uncontrolled timing" rule: the rig
        asserts on a fact the plugin stated, not on a guess about how long it
        takes. Returns the ack, or an invalid var on timeout. */
    juce::var send (const juce::String& cmd, juce::var args, int timeoutMs = 8000)
    {
        ++seq;
        auto* o = new juce::DynamicObject();
        o->setProperty ("seq", seq);
        o->setProperty ("cmd", cmd);
        o->setProperty ("args", args);
        echojay::rig::ackFile().deleteFile();
        echojay::rig::cmdFile().replaceWithText (juce::JSON::toString (juce::var (o), true));

        const auto until = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        while (juce::Time::getMillisecondCounter() < until)
        {
            pump (10);
            const auto f = echojay::rig::ackFile();
            if (! f.existsAsFile()) continue;
            const auto v = juce::JSON::parse (f.loadFileAsString());
            if ((int) v.getProperty ("seq", -1) == seq) return v;
        }
        return {};
    }

    /** The published state, as ONE snapshot. Re-reading the file between two assertions in the same row is
        how I read a rack with count=2 and an empty slot list: two reads, two different moments. A row takes
        one snapshot and asserts on that. The retry covers the instant the file does not exist (between the
        rename and the next publish). */
    /** The snapshot the LAST command produced, never an older one. */
    juce::var freshState (int timeoutMs = 6000)
    {
        send (echojay::rig::cmd::reportState, juce::var (new juce::DynamicObject()), 4000);
        const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        while (juce::Time::getMillisecondCounter() < end)
        {
            const auto v = state();
            if (v.isObject() && (int) v.getProperty ("forSeq", -1) >= seq) return v;
            pump (60);
        }
        return state();      // the best available, and the row will say what it saw
    }

    static juce::var state()
    {
        const auto f = echojay::rig::stateFile();
        for (int attempt = 0; attempt < 5; ++attempt)
        {
            if (f.existsAsFile())
            {
                const auto v = juce::JSON::parse (f.loadFileAsString());
                if (v.isObject()) return v;
            }
            pump (40);
        }
        return {};
    }

    /** Pump until a predicate on the published state holds, or the bound runs
        out. Returns true if it held. Every wait in this rig is bounded and says
        what it was waiting for. */
    template <typename Pred>
    bool until (Pred p, int timeoutMs = 10000)
    {
        // RE-ASKS EACH TIME. The hook publishes state when it is given a command - it does not write on a
        // timer - so a loop that only re-read the FILE was re-reading the same snapshot until it gave up.
        // That is how "a Link is in the registry" failed for 8 s while the published state said connected:
        // true: the state was true and the file was old. The loop now asks for a fresh one each turn.
        const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        if (p (state())) return true;
        while (juce::Time::getMillisecondCounter() < end)
        {
            send (echojay::rig::cmd::reportState, juce::var (new juce::DynamicObject()), 2000);
            if (p (state())) return true;
            pump (200);
        }
        return false;
    }
};

static juce::var args1 (const char* k, const juce::var& v);   // defined below; the harness uses it

// =============================================================================
//  THE CAPABILITY HARNESS (Sean's ask, 13 Oct 2026)
// =============================================================================
//
//  One request, driven exactly as a user drives it: agent mode on, type it, send
//  it, wait for the card to settle, press Apply when a plan is waiting, wait for
//  the run to end. Then the ROW asserts on real state - the rack read back, the
//  rack OUT, the Link's own sidecar - never on what the agent said it did.
//
//  Every row that follows uses this, so a row is a request and a pass rule and
//  nothing else. docs/AGENT_CAPABILITIES.md is the list; this is how it is run.
struct AgentOutcome
{
    bool  settled = false;      // the card reached a resting state within the bound
    juce::String state;         // its word for that state
    int   planLines = 0;        // how many lines awaited a decision (0 = it just did it)
    bool  applied = false;      // the rig pressed Apply
    juce::String text;          // what the card was saying at the end
    juce::String failDetail;    // the first failed step's user line, if any
    int   asksAnswered = 0;     // how many questions the rig answered on the user's behalf
    juce::String lastAsk;       // the last question it answered
    // 14 OCT: two facts the rig was missing, both of which let one row answer with another row's reply.
    bool  sawAsk = false;       // the card asked something (and this row chose not to answer it)
    juce::String stepWords;     // what the steps said, captured WHILE they existed (see below)
    bool  adopted = false;      // the card's own goal became THIS request (so the reply belongs to this row)
    int   applies = 0;          // how many plans the rig approved (a build can present more than one)
    bool  leftRunning = false;  // the turn was still live when the bound ran out

    /** DID A TURN ACTUALLY HAPPEN FOR THIS REQUEST? (14 Oct 2026, Sean's instruction.)
        The 14:34 run was spoiled by a server-side 500 on every agent POST, and SEVERAL ROWS PASSED ANYWAY:
        C10 "undo that" passed because the rack had not changed - the removal it was undoing had itself
        failed, so before == after. A6, L5 and F2 can pass the same way, their assertions all being variants
        of "nothing was touched". A row whose assertion is satisfied by the turn never running is not a test.

        Three facts, any one of which proves a turn happened: the card took up THIS row's goal (adopted), and
        it produced either a step, an answered question, or visible text. A refusal counts - F2 and L5 are
        SUPPOSED to be refused - because a refusal is a turn that ran and said no. What does not count is
        silence. */
    bool ran() const
    {
        return adopted && (stepWords.isNotEmpty() || asksAnswered > 0 || sawAsk || text.isNotEmpty());
    }
    juce::String whyNotRan() const
    {
        juce::StringArray why;
        if (! adopted) why.add ("the card never took up this goal");
        if (stepWords.isEmpty() && asksAnswered == 0 && text.isEmpty())
            why.add ("no step, no question and no text - the turn produced nothing");
        return why.joinIntoString ("; ");
    }
};

/** LIVE means a token with something in it. ctest's ENVIRONMENT property cannot UNSET a variable - it sets
    it to "" - so `getenv(...) != nullptr` reported the mock pass as live, which would have skipped the one
    leg that pass exists for (U1) and run the capability rows with no server. An empty credential is not a
    credential anywhere else either. */
static bool liveServerConfigured()
{
    const char* e = ::getenv ("EJ_RIG_TOKEN");
    return e != nullptr && juce::String (e).trim().isNotEmpty();
}

static bool isRestingState (const juce::String& st)
{
    return st == "done" || st == "failed" || st == "stopped" || st == "idle";
}

static juce::var agentVarOf (const juce::var& st) { return st.getProperty ("agent", juce::var()); }

/** `answerAsks = false` leaves the card's question UNANSWERED (14 Oct 2026).
    Row L5 needed it. It asks for a track that does not exist and its claim is "the agent asks rather than
    guessing, and touches nothing". The agent did exactly that - *"I don't see a track named 'Trombone bus' -
    only 'Untitled' and 'Untitled 2' are linked here"* - and then the rig, which answers every question with
    the first chip, replied "Untitled". So the agent was INSTRUCTED to act on a real track, did, and the row
    then reported that nothing had happened. Answering the question is right for a row that needs the run to
    continue; it is the wrong instrument for a row whose whole subject is the question itself. */
static AgentOutcome askAgent (Rig& rig, const juce::String& request, int timeoutMs = 120000,
                              bool answerAsks = true)
{
    AgentOutcome out;
    rig.send (echojay::rig::cmd::agentChip, args1 ("on", true));

    // ---- 1. NEVER SEND INTO A LIVE SESSION (14 Oct 2026) --------------------------------------------------
    // Row L3 asked for a Link's output and was answered with row L2's build summary. L2's turn had run past
    // its 180 s bound with a second plan still awaiting approval; this function returned anyway, L3's request
    // went into a session that was still executing, and the state the rig then read at rest was L2's. The row
    // was not wrong about the OUT not moving - nothing had asked it to move. A request is only this row's
    // request if the card was quiet when it was sent.
    if (! rig.until ([] (const juce::var& st)
                     { const auto a = agentVarOf (st);
                       return ! (bool) a.getProperty ("active", false)
                              && isRestingState (a.getProperty ("state", juce::var()).toString()); }, 90000))
        std::printf ("        (WARNING: the previous turn was still live after 90 s - this row may be reading it)\n");

    rig.send (echojay::rig::cmd::sendChat, args1 ("text", request), 15000);

    // ---- 2. AND THE REPLY IS ONLY THIS ROW'S IF THE CARD ADOPTED THIS GOAL -------------------------------
    // The card publishes the goal it is working on. Waiting for it to BE this request turns "the state looks
    // settled" into a fact about which turn settled - the one thing the old loop could not tell.
    out.adopted = rig.until ([&] (const juce::var& st)
                             { return agentVarOf (st).getProperty ("goal", juce::var()).toString() == request; },
                             25000);
    if (! out.adopted)
        std::printf ("        (WARNING: the card never took up this request as its goal within 25 s)\n");

    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    while (juce::Time::getMillisecondCounter() < end)
    {
        rig.send (echojay::rig::cmd::reportState, juce::var (new juce::DynamicObject()), 4000);
        const auto a = agentVarOf (Rig::state());
        out.state = a.getProperty ("state", juce::var()).toString();
        out.text  = a.getProperty ("liveText", juce::var()).toString();
        out.planLines = (int) a.getProperty ("planLines", 0);

        // A plan waiting for a decision IS the user's turn: press Apply once, as a user would, and record
        // that we did - a row that needed an approval and a row that did not are different rows.
        // ONE PRESS WAS NOT ENOUGH (14 Oct 2026). A build can present a SECOND plan after the first is
        // applied - L2 did, and with `pressed` already true the rig sat watching it until the bound expired.
        // A user presses Apply as often as the card asks; three is the same ceiling the questions use, so a
        // pair of plans cannot become a loop either.
        if (out.state == "awaiting_approval" && out.applies < 3)
        {
            rig.send (echojay::rig::cmd::press, args1 ("button", "apply"), 8000);
            ++out.applies; out.applied = true;
            Rig::pump (300);
            continue;
        }
        // A QUESTION GETS AN ANSWER. The undo row sat in awaiting_ask until its bound expired, because the
        // rig could not do the one thing a user does with a question. It answers with the first choice the
        // card offers - which is the card's own preferred answer - and at most three times, so a pair of
        // questions cannot become a loop.
        if ((bool) a.getProperty ("askPending", false) && ! answerAsks)
        {
            // The question IS the result for this row. Record it and stop - leaving the card waiting is what
            // a user who has been asked something they did not expect would do.
            out.lastAsk = a.getProperty ("askText", juce::var()).toString();
            out.sawAsk  = true;
            break;
        }
        if ((bool) a.getProperty ("askPending", false) && out.asksAnswered < 3)
        {
            const auto labels = a.getProperty ("askLabels", juce::var()).toString();
            auto ack = rig.send (echojay::rig::cmd::answerAsk, args1 ("label", ""), 8000);
            ++out.asksAnswered;
            out.lastAsk = a.getProperty ("askText", juce::var()).toString();
            std::printf ("        (answered the card's question: \"%s\" -> %s)\n",
                         out.lastAsk.substring (0, 90).toRawUTF8(),
                         ack.getProperty ("note", juce::var()).toString().toRawUTF8());
            juce::ignoreUnused (labels);
            Rig::pump (400);
            continue;
        }
        // THE STEPS ARE PER-ROUND, so they must be read WHILE THEY EXIST (14 Oct 2026). EJAgentClient::steps()
        // holds the CURRENT round's steps: by the time a run reaches `done` the last round carried no tool
        // calls and the array is empty. So every read after the loop - the failDetail scan below included -
        // was reading an empty array and reporting nothing, which is why U1's second assertion failed with
        // the log plainly showing `step tc_u2 DONE "Undone: 1 edit."`. The poll already runs every 500 ms;
        // it keeps the last non-empty snapshot, and a row asserts on that.
        if (auto* sarr = a.getProperty ("steps", juce::var()).getArray())
            if (! sarr->isEmpty())
            {
                juce::String words;
                for (const auto& st : *sarr)
                {
                    words << st.getProperty ("label", juce::var()).toString() << "="
                          << st.getProperty ("detail", juce::var()).toString() << "; ";
                    if ((int) st.getProperty ("status", 0) == 4 /*Failed*/ && out.failDetail.isEmpty())
                        out.failDetail = st.getProperty ("detail", juce::var()).toString();
                }
                out.stepWords = words;
            }

        // Rest counts only for the turn this row asked for. When the card never adopted the goal there is
        // nothing to match against, so rest is accepted and the warning above stands as the record.
        const auto goalNow = a.getProperty ("goal", juce::var()).toString();
        if (isRestingState (out.state) && ! (bool) a.getProperty ("active", false)
            && (! out.adopted || goalNow == request))
        { out.settled = true; break; }
        Rig::pump (500);
    }
    // THE BOUND RUNNING OUT MUST NOT BE THE NEXT ROW'S PROBLEM. Whatever happened here, this function does
    // not hand the next row a live session: it waits for quiet and says so if it could not get it.
    if (! out.settled)
    {
        out.leftRunning = ! rig.until ([] (const juce::var& st)
                                       { const auto a = agentVarOf (st);
                                         return ! (bool) a.getProperty ("active", false)
                                                && isRestingState (a.getProperty ("state", juce::var()).toString()); },
                                       90000);
        std::printf ("        (this turn did not settle inside its bound%s)\n",
                     out.leftRunning ? " and was STILL LIVE 90 s later" : "; it settled during the wind-down");
    }
    // A LAST LOOK, as a fallback only: the loop above captures the failed step's user line while the steps
    // still exist, and this catches the case where a run failed between two polls. It cannot be the only
    // source, because after `done` the array is empty (see above).
    if (out.failDetail.isEmpty())
        if (auto* steps = agentVarOf (Rig::state()).getProperty ("steps", juce::var()).getArray())
            for (const auto& st : *steps)
                if ((int) st.getProperty ("status", 0) == 4 /*Failed*/)
                { out.failDetail = st.getProperty ("detail", juce::var()).toString(); break; }
    return out;
}

// ---- reading real state ------------------------------------------------------
static int ownSlotCount (const juce::var& st)
{
    return (int) st.getProperty ("ownRack", juce::var()).getProperty ("count", 0);
}
static juce::StringArray ownSlotNames (const juce::var& st)
{
    juce::StringArray names;
    if (auto* arr = st.getProperty ("ownRack", juce::var()).getProperty ("slots", juce::var()).getArray())
        for (const auto& sl : *arr) names.add (sl.getProperty ("name", juce::var()).toString());
    return names;
}
static double busGainOf (const juce::var& st)
{
    return (double) st.getProperty ("ownRack", juce::var()).getProperty ("busGainDb", 0.0);
}
static juce::var linkRowOf (const juce::var& st, const juce::String& uid)
{
    if (auto* arr = st.getProperty ("links", juce::var()).getArray())
        for (const auto& l : *arr) if (l.getProperty ("uid", juce::var()).toString() == uid) return l;
    return {};
}

// A row's verdict, in the scorecard's words. Printed in a form the scorecard is written from, so the file
// and the run cannot disagree.
/** One capability row. `r` is the turn behind it, and a row CANNOT PASS without one: see
    AgentOutcome::ran(). Pass nullptr only where there is deliberately no agent turn (A6, which sends a chat
    with the chip off) - and then the row must carry its own evidence that something happened, because the
    whole point of this gate is that "nothing changed" is not a result. */
static void row (const char* id, bool pass, const juce::String& request, const juce::String& detail,
                 const AgentOutcome* r = nullptr)
{
    juce::String note;
    if (r != nullptr && ! r->ran())
    {
        if (pass) note = "  <- FORCED FAIL: the assertion held but NO TURN RAN (" + r->whyNotRan() + ")";
        else      note = "  (and no turn ran: " + r->whyNotRan() + ")";
        pass = false;
    }
    if (pass) ++passes; else ++failures;
    std::printf ("  ROW %-4s %-4s  \"%s\"\n        %s%s\n", id, pass ? "PASS" : "FAIL",
                 request.toRawUTF8(), detail.toRawUTF8(), note.toRawUTF8());
    std::fflush (stdout);
}

static juce::var args1 (const char* k, const juce::var& v)
{
    auto* o = new juce::DynamicObject(); o->setProperty (k, v); return juce::var (o);
}
static juce::String transcriptRoles (const juce::var& st)
{
    juce::StringArray r;
    if (auto* arr = st.getProperty ("transcript", juce::var()).getArray())
        for (const auto& row : *arr) r.add (row.getProperty ("role", juce::var()).toString());
    return r.joinIntoString (",");
}
static bool transcriptHas (const juce::var& st, const juce::String& needle)
{
    if (auto* arr = st.getProperty ("transcript", juce::var()).getArray())
        for (const auto& row : *arr)
            if (row.getProperty ("text", juce::var()).toString().contains (needle)) return true;
    return false;
}

// ============================================================================
//  main
// ============================================================================
int main (int argc, char* argv[])
{
    std::printf ("==== host_rig ====\n");
    if (argc < 4)
    {
        std::printf ("usage: host_rig <V2.component> <Link.component> <mockPort>\n");
        return 2;
    }
    // ---- the two preconditions, both refusals rather than skips ------------
    if (! echojay::stateIsIsolated())
    {
        std::printf ("REFUSED: ECHOJAY_STATE_HOME is not set. The rig drives the UI and must never touch\n"
                     "         the live ~/Library/EchoJay (6 Sep ruling).\n");
        return 2;
    }
    if (::getenv ("EJ_RIG_HOOK") == nullptr)
    {
        std::printf ("REFUSED: EJ_RIG_HOOK=1 is not set, so the plugin's hook is inert by design.\n");
        return 2;
    }

    // BRACE INIT, deliberately. `const juce::File v2Comp (juce::String (argv[1]));` declares a FUNCTION -
    // the most vexing parse - and this is the third time it has cost me a compile this week, so it is
    // written the way that cannot do it.
    const juce::File v2Comp   { juce::String (argv[1]) };
    const juce::File linkComp { juce::String (argv[2]) };
    const int mockPort = juce::String (argv[3]).getIntValue();

    juce::ScopedJuceInitialiser_GUI juceInit;

    // A GUI session or nothing. juce::Desktop's display list is empty under a
    // headless session, and creating a window there would abort rather than fail
    // a check - so this is tested before anything is built.
    if (juce::Desktop::getInstance().getDisplays().displays.isEmpty())
    {
        std::printf ("SKIP: no display is attached to this session, so no editor can be opened.\n"
                     "      S6 reports this as a SKIP, never as a pass (Sean's instruction).\n");
        return 77;      // the gate's agreed "skipped loudly" code
    }

    // ---- point the plugin at the mock, inside the isolated root ------------
    {
        auto settings = echojay::userAppData().getChildFile ("Application Support/EchoJay/auth.json");
        settings.getParentDirectory().createDirectory();
        // THE KEY NAMES ARE THE PLUGIN'S, NOT MINE. EchoJayAPI::loadSettings reads "endpoint" and "token";
        // my first three passes wrote "apiEndpoint"/"authToken", so the plugin was never logged in and sent
        // NOTHING - and the mid-reply leg was closing a window over a reply that had never been requested.
        // The run reported 0 requests received, which is what finally said so; a leg that asserts on the
        // server's own record is worth more than any amount of reading.
        // EJ_RIG_TOKEN is set by build_and_run.sh in LIVE mode, from /api/forge-dev-login. In mock mode any
        // string will do, because the mock authenticates nothing.
        const char* envTok = ::getenv ("EJ_RIG_TOKEN");
        const juce::String token = (envTok != nullptr && *envTok != 0) ? juce::String (envTok)
                                                                      : juce::String ("rig-token");
        auto* o = new juce::DynamicObject();
        o->setProperty ("endpoint", "http://127.0.0.1:" + juce::String (mockPort));
        o->setProperty ("token", token);
        o->setProperty ("email", "rig@local");
        // THE USAGE GATE IS WHY FIVE PASSES SENT NOTHING. EchoJayAPI::sendChat refuses before any HTTP when
        // canSendTurn() is false, and canSendMessage() reads messageLimit - messagesUsedToday. With neither
        // field written, the limit was 0, the turn was refused locally, and onComplete(..., false) made it a
        // FAILED turn - which the keep-rule then correctly discarded. So the rig was testing a turn that was
        // never sent, and the one leg that failed was telling the truth about it.
        //
        // In LIVE mode the server's /api/me overwrites all of this on the first refresh; these are the
        // values that let the FIRST send leave the building.
        o->setProperty ("tier", "studio");
        o->setProperty ("messageLimit", 9999);
        o->setProperty ("messagesUsedToday", 0);
        o->setProperty ("credits", 9999);
        o->setProperty ("usageDate", juce::Time::getCurrentTime().formatted ("%Y-%m-%d"));
        settings.replaceWithText (juce::JSON::toString (juce::var (o), true));
        std::printf ("  settings -> %s (endpoint 127.0.0.1:%d)\n", settings.getFullPathName().toRawUTF8(), mockPort);
    }

    // juce_audio_processors_HEADLESS is what a plugin build links, and it deletes
    // addDefaultFormats(); juce::addDefaultFormatsToManager is the free function the
    // merge-gate harness already uses for exactly this reason.
    juce::AudioPluginFormatManager fm;
    juce::addDefaultFormatsToManager (fm);

    auto load = [&fm] (const juce::File& comp, const juce::String& label) -> std::unique_ptr<juce::AudioPluginInstance>
    {
        juce::String ident;
        if (! registerAU (comp, ident)) return {};
        juce::OwnedArray<juce::PluginDescription> found;
        for (auto* f : fm.getFormats())
            if (f->getName() == "AudioUnit") f->findAllTypesForFile (found, ident);
        if (found.isEmpty()) { std::printf ("  %s: no types for %s\n", label.toRawUTF8(), ident.toRawUTF8()); return {}; }
        juce::String err;
        auto inst = fm.createPluginInstance (*found[0], 48000.0, 512, err);
        if (inst == nullptr) std::printf ("  %s: %s\n", label.toRawUTF8(), err.toRawUTF8());
        return inst;
    };

    leg ("R0: the built bundles load in one host process");
    Track v2; v2.name = "Mix Bus (V2)";
    Track lk1; lk1.name = "Kick (Link)";
    Track lk2; lk2.name = "Vocal (Link)";
    v2.plugin  = load (v2Comp,   "V2");
    lk1.plugin = load (linkComp, "Link 1");
    lk2.plugin = load (linkComp, "Link 2");
    check (v2.plugin != nullptr && lk1.plugin != nullptr && lk2.plugin != nullptr,
           "V2 and two Links instantiate from the BUILT bundles");
    if (v2.plugin == nullptr || lk1.plugin == nullptr || lk2.plugin == nullptr)
    { std::printf ("==== host_rig: RED (nothing could be hosted) ====\n"); return 1; }

    const double sr = 48000.0; const int block = 512;
    for (Track* t : { &v2, &lk1, &lk2 }) t->prepare (sr, block);

    // Audio first, and kept running: every leg below needs the Links to be
    // registered and passing audio, which only happens once they have processed.
    for (int i = 0; i < 200; ++i)
    {
        for (Track* t : { &v2, &lk1, &lk2 }) t->processOneBlock (sr);
        if ((i % 20) == 0) Rig::pump (10);
    }
    Rig::pump (1500);       // the 1 Hz registry pass runs on the processor's timer

    Rig rig;
    leg ("R0b: can anything in THIS process reach the server?");
    {
        // ISOLATES THE QUESTION. Five passes could not tell whether the plugin was failing to send or the
        // process could not reach the port at all - the mock's log showed no request either way. This does
        // a plain JUCE HTTP GET from the rig's own thread: if it succeeds the transport is fine and the
        // plugin's send path is where to look; if it fails, nothing in this process can talk to the server
        // and every chat leg below is meaningless rather than failing.
        // The mock has /api/rig/requests; B's server has /local/health. Probing the wrong one answered 404
        // and called the transport broken, which is the leg lying rather than the plugin.
        const bool liveMode = liveServerConfigured();
        juce::URL u ("http://127.0.0.1:" + juce::String (mockPort)
                     + (liveMode ? "/local/health" : "/api/rig/requests"));
        int status = 0;
        juce::StringPairArray respHeaders;
        auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                        .withConnectionTimeoutMs (4000)
                        .withStatusCode (&status)
                        .withResponseHeaders (&respHeaders);
        auto stream = u.createInputStream (opts);
        const juce::String body = stream != nullptr ? stream->readEntireStreamAsString() : juce::String();
        check (stream != nullptr && status == 200,
               "a plain JUCE HTTP GET from this process reaches the server",
               "status=" + juce::String (status) + " body=" + body.substring (0, 60));
    }

    leg ("R1: the editor opens, and the agent is told which rack is open (item 7)");
    v2.openEditor();
    check (v2.editor != nullptr, "the V2's editor was created");
    Rig::pump (600);
    // ASK FIRST. The hook publishes state when it is given a command - it does not write a file on a timer,
    // which is deliberate (a hook that wrote to disk every tick would be a hook with a cost). The first run
    // of this leg read the file before any command had been sent and called the hook broken; the hook was
    // fine and the leg was wrong. Both failures below were mine, not the plugin's.
    auto stAck = rig.send (echojay::rig::cmd::reportState, juce::var (new juce::DynamicObject()));
    check ((bool) stAck.getProperty ("ok", false), "the hook answers a command",
           stAck.isObject() ? stAck.getProperty ("note", juce::var()).toString() : juce::String ("no ack at all"));
    auto st = Rig::state();
    check (st.isObject(), "...and the plugin published its state through it",
           st.isObject() ? juce::String() : juce::String ("no rig-state.json"));

    // The registry pass runs on the processor's 1 Hz timer, so the Links appear a second or so after they
    // first process audio. Waited for, with a bound, rather than read once - the "decisions on uncontrolled
    // timing" rule applies to the rig as much as to the plugin.
    juce::String linkUid;
    const bool sawLink = rig.until ([&linkUid] (const juce::var& s)
    {
        if (auto* arr = s.getProperty ("links", juce::var()).getArray())
            for (const auto& l : *arr)
                if ((bool) l.getProperty ("connected", false))
                { linkUid = l.getProperty ("uid", juce::var()).toString(); return linkUid.isNotEmpty(); }
        return false;
    }, 8000);
    if (! sawLink) rig.send (echojay::rig::cmd::reportState, juce::var (new juce::DynamicObject()));
    check (linkUid.isNotEmpty(), "a Link is in the registry and connected",
           "links=" + juce::JSON::toString (Rig::state().getProperty ("links", juce::var())));

    if (linkUid.isNotEmpty())
    {
        // THE RACK VIEW first - and it must NOT move the chat's channel (the 25 Sep rule).
        auto ack = rig.send (echojay::rig::cmd::selectLink, args1 ("uid", linkUid));
        check ((bool) ack.getProperty ("ok", false), "the rig selected that Link as the viewed rack",
               ack.getProperty ("note", juce::var()).toString());
        const bool viewMoved = rig.until ([&] (const juce::var& s)
                               { return s.getProperty ("viewUid", juce::var()).toString() == linkUid; }, 4000);
        check (viewMoved, "...and the VIEW is on that rack",
               "viewUid=" + Rig::state().getProperty ("viewUid", juce::var()).toString());

        // THEN the chat's working channel, which is what Sean's item 7 is about ("Working on Link: ...").
        // My first run pressed only the rack door and called item 7 broken; the rule says a rack selection
        // leaves the chat alone, so the leg was wrong and the plugin was right.
        auto ack2 = rig.send (echojay::rig::cmd::workOn, args1 ("uid", linkUid));
        check ((bool) ack2.getProperty ("ok", false), "the rig set that Link as the chat's working channel",
               ack2.getProperty ("note", juce::var()).toString());
        const bool told = rig.until ([&] (const juce::var& s)
                          { return s.getProperty ("agentTarget", juce::var()).toString() == linkUid; }, 6000);
        check (told, "...and the agent's target became that Link's uid  (RED before item 7: setTarget had no "
                     "caller anywhere in the product)",
               "agentTarget=\"" + Rig::state().getProperty ("agentTarget", juce::var()).toString()
               + "\" workingUid=\"" + Rig::state().getProperty ("workingUid", juce::var()).toString() + "\"");
    }

    leg ("R2: the Agent chip takes effect without a reopen (12b item 6)");
    {
        auto a1 = rig.send (echojay::rig::cmd::agentChip, args1 ("on", true));
        const bool on = rig.until ([] (const juce::var& s) { return (bool) s.getProperty ("agentModeOn", false); }, 3000);
        check ((bool) a1.getProperty ("ok", false) && on, "the chip turned agent mode ON in the SAME window");
        auto a2 = rig.send (echojay::rig::cmd::agentChip, args1 ("on", false));
        const bool off = rig.until ([] (const juce::var& s) { return ! (bool) s.getProperty ("agentModeOn", true); }, 3000);
        check ((bool) a2.getProperty ("ok", false) && off, "...and OFF again, still without reopening");
    }

    leg ("R3: the composer empties on send (12b item 7)");
    {
        auto ack = rig.send (echojay::rig::cmd::sendChat, args1 ("text", "rig: does the box clear?"));
        check ((bool) ack.getProperty ("ok", false), "the rig typed and sent a message");
        const bool cleared = rig.until ([] (const juce::var& s)
                             { return s.getProperty ("chatInput", juce::var()).toString().trim().isEmpty(); }, 3000);
        check (cleared, "...and the input box is empty afterwards",
               "chatInput=\"" + Rig::state().getProperty ("chatInput", juce::var()).toString() + "\"");
    }

    leg ("R4: CLOSING MID-REPLY KEEPS THE USER'S MESSAGE AND LANDS THE REPLY (item 5)");
    {
        // The mock holds the first delta, so the window is closed while the turn
        // is genuinely in the air - the rig decides when "mid-reply" is.
        const juce::String mark = "rig-midreply-marker";
        rig.send (echojay::rig::cmd::sendChat, args1 ("text", mark));
        Rig::pump (400);                                  // the POST is away, the reply is not
        v2.closeEditor();
        check (v2.editor == nullptr, "the window was closed while the reply was still streaming");
        // Audio keeps running with the window shut, as it does in a host.
        for (int i = 0; i < 300; ++i)
        {
            for (Track* t : { &v2, &lk1, &lk2 }) t->processOneBlock (sr);
            if ((i % 30) == 0) Rig::pump (20);
        }
        Rig::pump (2500);
        v2.openEditor();
        Rig::pump (800);
        auto after = rig.freshState();
        // ---- IS THIS ACCOUNT ALLOWED A PLAIN CHAT TURN AT ALL? -------------------------------------
        //
        // sendChatMessage refuses before any network when api.canSendMessage() is false, and it refuses by
        // pushing an ASSISTANT row carrying the limit message. That is what this leg kept reading as
        // "roles=assistant,assistant": not lost questions, but two refusals. B's dev account is Free with a
        // daily pool of five chats, and the rig's own passes spend them - while the AGENT path is unmetered,
        // which is why every capability row passes beside this one.
        //
        // So the leg says which it is. A spent pool is a SKIP with the reason, never a pass and never a
        // failure pinned on the plugin.
        // THE FLAG, NOT THE PROSE. Matching words in the transcript was the next mistake in the same family:
        // the plugin states this as a fact and the leg reads the fact.
        const bool poolSpent = ! (bool) after.getProperty ("canSendChat", true);
        if (poolSpent && ! transcriptHas (after, mark))
            check (true, "(skipped) this account cannot send a plain chat turn - the daily chat pool is spent, "
                         "so closing mid-reply cannot be tested on it (B's dev account is Free/5-a-day)",
                   "chatsUsed=" + after.getProperty ("chatsUsed", juce::var()).toString()
                   + " of " + after.getProperty ("chatsLimit", juce::var()).toString()
                   + ", roles=" + transcriptRoles (after));
        else
            check (transcriptHas (after, mark),
                   "the user's own message SURVIVED the close  (RED on 13a: it was lost with the editor's store)",
                   "roles=" + transcriptRoles (after));
        // A REAL reply takes as long as the model takes; a mock's is instant. 12 s was the mock's number and
        // it failed the live run for that reason alone, which is the leg's fault and not the plugin's.
        const bool liveReply = liveServerConfigured();
        const bool replyLanded = poolSpent || rig.until ([] (const juce::var& s)
                                 { return transcriptRoles (s).contains ("assistant"); },
                                 liveReply ? 90000 : 15000);
        check (replyLanded, poolSpent ? "(skipped) the reply leg needs a turn this account may send"
                                      : "...and the reply that finished while the window was shut is in the transcript",
               "roles=" + transcriptRoles (Rig::state())
               + (replyLanded ? juce::String() : juce::String (" (waited ")
                                                 + juce::String (liveReply ? 90 : 15) + "s)"));
    }

    leg ("R5: reopening returns to the Link you were on, with no stale banner (items 5 and 6)");
    if (linkUid.isNotEmpty())
    {
        rig.send (echojay::rig::cmd::selectLink, args1 ("uid", linkUid));
        Rig::pump (800);
        v2.closeEditor();
        for (int i = 0; i < 100; ++i) { for (Track* t : { &v2, &lk1, &lk2 }) t->processOneBlock (sr); }
        Rig::pump (1200);
        v2.openEditor();
        Rig::pump (1500);
        auto after = rig.freshState();
        check (after.getProperty ("viewUid", juce::var()).toString() == linkUid
            || after.getProperty ("workingUid", juce::var()).toString() == linkUid,
               "the reopened window is on the Link it was on",
               "view=" + after.getProperty ("viewUid", juce::var()).toString()
               + " working=" + after.getProperty ("workingUid", juce::var()).toString());
        const auto banner = after.getProperty ("centreBanner", juce::var()).toString();
        check (! banner.contains ("owns its rack again"),
               "...and the centre does NOT still say \"Released - ... owns its rack again\"  (item 6)",
               "banner=\"" + banner + "\"");
    }

    leg ("R6: the rack OUT a user sets is what the state reports");
    {
        auto ack = rig.send (echojay::rig::cmd::setRackOut, args1 ("db", -2.0));
        check ((bool) ack.getProperty ("ok", false), "the rack OUT was set to -2.0 dB");
        const bool seen = rig.until ([] (const juce::var& s)
                          { return std::abs ((double) s.getProperty ("ownRack", juce::var())
                                                        .getProperty ("busGainDb", 0.0) + 2.0) < 0.05; }, 3000);
        check (seen, "...and the rack reads it back",
               "busGainDb=" + Rig::state().getProperty ("ownRack", juce::var()).getProperty ("busGainDb", juce::var()).toString());
    }

    leg ("R7: Sean's rulings of 13 Oct - the three callbacks that had one end (ruling 3) and AUTO (ruling 1)");
    {
        // (a) RULING 1: the pre-gain knob's double-click restores AUTO rather than writing 0.0 dB. The state
        // reports the hand-set flag, which is the same fact a build reads when deciding whether it may set
        // the pre-gain itself - so this is the behaviour, not a proxy for it.
        rig.send (echojay::rig::cmd::reportState, juce::var (new juce::DynamicObject()));
        auto before = Rig::state();
        const bool userSetBefore = (bool) before.getProperty ("preGainUserSet", false);
        auto ack = rig.send (echojay::rig::cmd::preGainReset, juce::var (new juce::DynamicObject()));
        if (! (bool) ack.getProperty ("ok", false))
            check (true, "(skipped) the pre-gain knob is not on this view, so ruling 1 cannot be pressed here",
                   ack.getProperty ("note", juce::var()).toString());
        else
        {
            const bool nowAuto = rig.until ([] (const juce::var& s)
                                 { return ! (bool) s.getProperty ("preGainUserSet", true); }, 3000);
            check (nowAuto,
                   "ruling 1: a double-click on the pre-gain knob clears the hand-set flag - it is AUTO again "
                   "(RED as it stood: onResetAuto was assigned by both editors and invoked by nothing)",
                   "userSet was " + juce::String (userSetBefore ? "true" : "false") + ", now "
                   + juce::String ((bool) Rig::state().getProperty ("preGainUserSet", true) ? "true" : "false"));
        }

        // (b) RULING 3: a slot double-click reaches the open door. On the own rack that is the inline editor;
        // the leg asserts the PRESS was accepted by the wiring, which is what had no end before.
        // IT RUNS WHERE A SLOT EXISTS. At this point in the run the rack is empty, so this used to print a
        // skip as a pass; the owed flag carries it to after C1, which puts a compressor there.
        auto st2 = Rig::state();
        const int slots = (int) st2.getProperty ("ownRack", juce::var()).getProperty ("count", 0);
        if (slots > 0)
        {
            auto a2 = rig.send (echojay::rig::cmd::openSlot, args1 ("n", 1));
            check ((bool) a2.getProperty ("ok", false),
                   "ruling 3: a slot DOUBLE-CLICK is wired and accepted  (RED as it stood: Block had no "
                   "mouseDoubleClick, and the hook for it - onRemoteEditorRequest - was never invoked)",
                   a2.getProperty ("note", juce::var()).toString());
        }
        else slotDoubleClickOwed = true;   // run it below, once the capability rows have put a slot there

        // (c) RULING 3: the substitution notice has a surface. The rig cannot force a substitution without a
        // missing third-party plugin, so this asserts the SURFACE the notice now uses is reported - the band
        // the editor draws from getStateNotes(). A notice with nowhere to appear was the fault.
        check (st2.getProperty ("stateNotes", juce::var()).isString(),
               "ruling 3: the note band the substitution notice now writes to is readable state",
               "notes=\"" + st2.getProperty ("stateNotes", juce::var()).toString() + "\"");
    }

    // =========================================================================
    //  THE CAPABILITY PASS (docs/AGENT_CAPABILITIES.md)
    // =========================================================================
    //  Only runs in LIVE mode: these rows need the real model to decide what to
    //  do, and a mock answering recorded frames would be checking the recording.
    //  In mock mode they are SKIPPED and said to be skipped.
    const bool live = liveServerConfigured();

    // ---- U1: THE UNDO OP, END TO END, WITHOUT THE MODEL (14 Oct 2026) ---------------------------------
    //
    // WHY THIS EXISTS. Sean's ruling 4 gave the agent an undo op. It is built, and agent_round14_guard holds
    // sixteen assertions on it - but a guard drives the executor in a fixture, and the live row C10 kept
    // failing for reasons outside the plugin: first because the plugin never DECLARED the capability, then
    // because B's local server was started the day before their undo work and the running process has no
    // undo tool in its enum. Neither is evidence about the plugin, and neither is mine to fix.
    //
    // So this leg proves MY half with the model taken out of it: the mock sends the real SSE frames for
    // `do {op:"set_pre_gain", db:3}` and then `do {op:"undo"}`, through the real client, the real executor
    // and the real plugin, and the assertion is on the plugin's own published state - the pre-gain going to
    // 3 dB and coming back. Mock mode only: in live mode the model owns the decision and C10 is the row.
    if (! live)
    {
        leg ("U1: the undo op end to end (mock frames, real client, real plugin, real state)");
        rig.send (echojay::rig::cmd::workOn, args1 ("uid", ""));
        rig.send (echojay::rig::cmd::selectLink, args1 ("uid", ""));
        rig.send (echojay::rig::cmd::reportState, juce::var (new juce::DynamicObject()));
        const double pgBefore = (double) rig.freshState().getProperty ("preGainDb", 0.0);

        const auto r = askAgent (rig, "rig-undo-marker: set the pre gain to 3 dB, then undo it", 60000);
        const double pgAfter = (double) rig.freshState().getProperty ("preGainDb", 0.0);

        // What the steps said, captured while they existed (askAgent keeps the last non-empty snapshot).
        const juce::String stepWords = r.stepWords;

        check (std::abs (pgAfter - pgBefore) < 0.1,
               "U1: a scripted set_pre_gain(3 dB) followed by {op:\"undo\"} leaves the pre-gain back where "
               "it started - the plugin's undo op works through the real client  (RED before ruling 4: there "
               "was no undo op at all)",
               "pre-gain " + juce::String (pgBefore, 2) + " -> " + juce::String (pgAfter, 2)
               + " dB, state " + r.state + ", steps: " + stepWords.substring (0, 200));
        check (stepWords.containsIgnoreCase ("undone") || stepWords.containsIgnoreCase ("undo"),
               "...and the step's own detail says what was undone, from the history's label",
               stepWords.substring (0, 200));
    }

    leg (live ? "CAPABILITIES: the real server, the real model, asserted on real state"
              : "CAPABILITIES: skipped - no live server (set EJ_RIG_LIVE=<port>)");
    if (live)
    {
        // Start from the own rack and a known OUT, so every row below reads a state it set.
        // BOTH doors back to the own rack. The first live pass left the chat's working channel on the Link
        // from R1 while the rows measured the own rack - so a row could have been asserting on one rack
        // while the agent acted on another. The rows below state the target they ran against, for the same
        // reason: a capability result that does not say which rack it was about is not a result.
        rig.send (echojay::rig::cmd::workOn, args1 ("uid", ""));
        rig.send (echojay::rig::cmd::selectLink, args1 ("uid", ""));
        rig.send (echojay::rig::cmd::setRackOut, args1 ("db", 0.0));
        rig.send (echojay::rig::cmd::reportState, juce::var (new juce::DynamicObject()));

        // ---- C1: add one plugin to the OPEN rack ----------------------------
        {
            const auto before = ownSlotCount (rig.freshState());
            const auto r = askAgent (rig, "add a compressor");
            const auto st = rig.freshState();              // ONE snapshot for the whole row
            const auto after = ownSlotCount (st);
            const bool grew = after == before + 1;
            row ("C1", grew, "add a compressor",
                 "slots " + juce::String (before) + " -> " + juce::String (after)
                 + ", state " + r.state + (r.applied ? ", applied from a plan" : ", no plan")
                 + (grew ? ", now [" + ownSlotNames (st).joinIntoString (", ") + "]"
                         : ", card said: " + r.text.substring (0, 120) + r.failDetail), &r);

            // RULING 3, OWED FROM THE LIFECYCLE LEGS: there is a slot on the rack now, so the double-click
            // can actually be pressed. If C1 did not land there is still nothing to press, and that is a
            // SKIP with its reason - never an ok.
            if (slotDoubleClickOwed)
            {
                slotDoubleClickOwed = false;
                if (ownSlotCount (st) > 0)
                {
                    auto a2 = rig.send (echojay::rig::cmd::openSlot, args1 ("n", 1));
                    check ((bool) a2.getProperty ("ok", false),
                           "ruling 3: a slot DOUBLE-CLICK is wired and accepted  (RED as it stood: Block had "
                           "no mouseDoubleClick, and the hook for it - onRemoteEditorRequest - was never "
                           "invoked)", a2.getProperty ("note", juce::var()).toString());
                }
                else skip ("ruling 3: a slot double-click", "C1 put no slot on the rack to press");
            }
        }

        // ---- H1/H2: it must not claim what did not land, nor name what is absent
        {
            const auto st = Rig::state();
            const auto names = ownSlotNames (st);
            const auto text = agentVarOf (st).getProperty ("liveText", juce::var()).toString();
            // Every plugin-ish name the card used must be on the rack. Checked only for the built-in
            // family names, because a reply naturally mentions plugins it is CONSIDERING; the rule is about
            // claiming one is IN the rack, which is what these words would mean.
            bool claimedAbsent = false; juce::String which;
            for (const auto* probe : { "EchoJay Compressor", "EchoJay EQ", "EchoJay Limiter", "EchoJay Gate" })
                if (text.contains (juce::String (probe) + " is now") || text.contains (juce::String ("added ") + probe))
                    if (! names.contains (probe)) { claimedAbsent = true; which = probe; }
            // ITS OWN EVIDENCE (14 Oct): this row judges what the card SAID, so an empty card passes it
            // trivially - nothing was claimed, so nothing false was claimed. That is the vacuous pass in its
            // purest form: with the server down this row would have reported the product honest.
            const bool cardSpoke = text.trim().isNotEmpty();
            row ("H2", cardSpoke && ! claimedAbsent, "(honesty) never names a plugin that is not on the rack",
                 claimedAbsent ? "the card claimed " + which + " but the rack holds ["
                                 + names.joinIntoString (", ") + "]"
                               : cardSpoke ? juce::String ("nothing claimed that the rack does not hold")
                                           : juce::String ("THE CARD SAID NOTHING - there is no claim to "
                                                           "judge, so this row proves nothing and fails"));
        }

        // ---- C8: the rack's OUT, which is not a plugin's output -------------
        {
            const auto before = busGainOf (rig.freshState());
            const auto r = askAgent (rig, "turn the rack output down 2 dB");
            const auto after = busGainOf (rig.freshState());
            const bool moved = std::abs ((after - before) + 2.0) < 0.25;
            row ("C8", moved, "turn the rack output down 2 dB",
                 "rack OUT " + juce::String (before, 2) + " -> " + juce::String (after, 2)
                 + " dB, state " + r.state
                 + (moved ? "" : ", card said: " + r.text.substring (0, 140) + r.failDetail), &r);
        }

        // ---- C5: bypass a slot ----------------------------------------------
        // A ROW THAT CANNOT RUN SAYS SO (14 Oct). With the rack empty this block used to vanish: 15 rows
        // printed where 17 were expected, and nothing in the output said which two were missing.
        if (ownSlotCount (Rig::state()) == 0)
            skip ("C5: bypass slot 1", "the rack is empty - nothing to bypass (did C1 land?)");
        else
        {
            const auto r = askAgent (rig, "bypass slot 1");
            const auto st = rig.freshState();
            bool bypassed = false;
            juce::String shape;
            if (auto* arr = st.getProperty ("ownRack", juce::var()).getProperty ("slots", juce::var()).getArray())
            {
                for (const auto& sl : *arr)
                    shape << (shape.isEmpty() ? "" : ", ") << sl.getProperty ("name", juce::var()).toString()
                          << (((bool) sl.getProperty ("bypassed", false)) ? " [BYPASSED]" : " [live]");
                if (! arr->isEmpty()) bypassed = (bool) arr->getFirst().getProperty ("bypassed", false);
            }
            row ("C5", bypassed, "bypass slot 1",
                 juce::String (bypassed ? "slot 1 reads bypassed" : "slot 1 is NOT bypassed")
                 + "; the rack holds [" + shape + "]; the agent's target was \""
                 + st.getProperty ("agentTarget", juce::var()).toString() + "\", state " + r.state
                 + (bypassed ? "" : ", card said: " + r.text.substring (0, 160) + r.failDetail), &r);
        }

        // ---- L1: the same, on ANOTHER track's Link --------------------------
        if (linkUid.isNotEmpty())
        {
            const auto rowBefore = linkRowOf (Rig::state(), linkUid);
            const int before = (int) rowBefore.getProperty ("slotCount", -1);
            const auto name = rowBefore.getProperty ("name", juce::var()).toString();
            const auto ask = "add a compressor on " + (name.isNotEmpty() ? name : linkUid);
            const auto r = askAgent (rig, ask);
            // the Link publishes its own rack; wait for the sidecar to catch up rather than reading at once
            int after = before;
            rig.until ([&] (const juce::var& s)
            { after = (int) linkRowOf (s, linkUid).getProperty ("slotCount", -1); return after > before; }, 15000);
            const bool landed = before >= 0 && after == before + 1;
            row ("L1", landed, ask,
                 "that Link's own sidecar: " + juce::String (before) + " -> " + juce::String (after)
                 + " slot(s), state " + r.state + (r.applied ? ", applied from a plan" : "")
                 + (landed ? "" : ", card said: " + r.text.substring (0, 140) + r.failDetail), &r);
        }

        // ---- R3: the inventory, paged and filtered --------------------------
        {
            const auto r = askAgent (rig, "what compressors do I have?");
            const bool answered = r.settled && r.text.isNotEmpty();
            row ("R3", answered, "what compressors do I have?",
                 answered ? "answered in " + juce::String (r.text.length()) + " chars: "
                            + r.text.substring (0, 140)
                          : "no answer, state " + r.state + " " + r.failDetail, &r);
        }

        // ---- C2: position matters -------------------------------------------
        {
            const auto before = ownSlotNames (rig.freshState());
            const auto r = askAgent (rig, "add an EchoJay EQ after slot 1");
            const auto after = ownSlotNames (rig.freshState());
            const bool atTwo = after.size() == before.size() + 1 && after.size() >= 2
                            && after[1].containsIgnoreCase ("EQ");
            row ("C2", atTwo, "add an EchoJay EQ after slot 1",
                 "[" + before.joinIntoString (", ") + "] -> [" + after.joinIntoString (", ") + "], state "
                 + r.state + (atTwo ? "" : ", card said: " + r.text.substring (0, 140) + r.failDetail), &r);
        }

        // ---- C7: a slot's wet ------------------------------------------------
        // Same rule as C5: it needs two slots, and when it has not got them it SAYS so rather than vanishing.
        if (ownSlotCount (rig.freshState()) <= 1)     // a fresh snapshot for the GATE, not whatever was in scope
            skip ("C7: make the compressor 30% wet", "fewer than two slots on the rack - nothing to set wet");
        else
        {
            // NOT THE EQ. Loop 1 asked for 30% wet on slot 2, which was the EchoJay EQ this pass had just
            // added, and the model refused with its own rule: "surgical/tonal EQ must run fully wet". The
            // refusal was right and the row was wrong. The compressor is a slot where wet means something.
            const auto r = askAgent (rig, "make the compressor 30% wet");
            rig.send (echojay::rig::cmd::reportState, juce::var (new juce::DynamicObject()));
            // THE PASS RULE IS "SOME SLOT READS 30% WET", not "the slot I guessed". Loop 2 looked for a name
            // containing "Compressor", found none (the rack's compressor is named otherwise), and reported
            // wet -1.000 for a slot that does not exist - which told me nothing about whether the change
            // landed. The whole rack is printed either way, so a failure says what the rack actually holds.
            // THE SNAPSHOT IS TAKEN HERE. For three passes this block read an `st` it never declared - it
            // bound to one from an enclosing scope, taken at the start of the whole capability pass, which
            // is why the row kept reporting an empty rack and forSeq=1 while the plugin was publishing
            // correctly. A row that reads a variable it did not set is not reading anything.
            const auto st = rig.freshState();
            double wet = -1.0; juce::String whichSlot, shape;
            if (auto* arr = st.getProperty ("ownRack", juce::var()).getProperty ("slots", juce::var()).getArray())
                for (const auto& sl : *arr)
                {
                    const auto nm = sl.getProperty ("name", juce::var()).toString();
                    const double w = (double) sl.getProperty ("wet", -1.0);
                    shape << (shape.isEmpty() ? "" : ", ") << nm << " wet=" << juce::String (w, 2);
                    if (std::abs (w - 0.30) < 0.06) { wet = w; whichSlot = nm; }
                }
            const bool set = std::abs (wet - 0.30) < 0.06;
            row ("C7", set, "make the compressor 30% wet",
                 (whichSlot.isNotEmpty() ? "\"" + whichSlot + "\" reads " + juce::String (wet, 2) + " wet"
                                         : juce::String ("no slot reads 30% wet"))
                 + "; the rack holds [" + shape + "] (editor="
                 + juce::String ((juce::int64) (double) st.getProperty ("editorId", 0.0)) + ", forSeq="
                 + st.getProperty ("forSeq", juce::var()).toString() + ", count="
                 + juce::String (ownSlotCount (st)) + ", view=\""
                 + st.getProperty ("viewUid", juce::var()).toString() + "\", target=\""
                 + st.getProperty ("agentTarget", juce::var()).toString() + "\"), state " + r.state
                 + (set ? "" : ", card said: " + r.text.substring (0, 140) + r.failDetail), &r);
        }

        // ---- C9: the rack's pre gain -----------------------------------------
        {
            const auto r = askAgent (rig, "add 3 dB of pre gain");
            const double pg = (double) rig.freshState().getProperty ("preGainDb", 0.0);
            const bool set = std::abs (pg - 3.0) < 0.6;
            row ("C9", set, "add 3 dB of pre gain",
                 "pre gain reads " + juce::String (pg, 2) + " dB, state " + r.state
                 + (set ? "" : ", card said: " + r.text.substring (0, 140) + r.failDetail), &r);
        }

        // ---- C3 then C10: remove, then undo ----------------------------------
        {
            const auto before = ownSlotCount (rig.freshState());
            const auto r = askAgent (rig, "remove slot 1");
            const auto after = ownSlotCount (rig.freshState());
            row ("C3", after == before - 1, "remove slot 1",
                 "slots " + juce::String (before) + " -> " + juce::String (after) + ", state " + r.state
                 + (after == before - 1 ? "" : ", card said: " + r.text.substring (0, 140) + r.failDetail), &r);

            const auto u = askAgent (rig, "undo that");
            rig.until ([&] (const juce::var& st) { return ownSlotCount (st) == before; }, 12000);
            const auto back = ownSlotCount (Rig::state());
            row ("C10", back == before, "undo that",
                 "slots " + juce::String (after) + " -> " + juce::String (back)
                 + " (was " + juce::String (before) + " before the remove), state " + u.state
                 + (back == before ? "" : ", card said: " + u.text.substring (0, 140) + u.failDetail), &u);
        }

        // ---- L2: a BUILD on another track's Link -----------------------------
        // REPEATABLE (EJ_RIG_L2_REPEAT=N, 14 Oct 2026). Sean asked for five, because this row has passed and
        // failed on the SAME binary for two different server-side reasons - 12:34 pass, 14:01 fail on
        // check(level_landed), 14:42 fail on a round that produced nothing, 15:05 pass. One run of a flaky
        // row tells you about that run. The rows are named L2#1..L2#N so a log says which attempt did what.
        if (linkUid.isNotEmpty())
        {
            const char* rep = ::getenv ("EJ_RIG_L2_REPEAT");
            const int reps = juce::jlimit (1, 10, rep != nullptr ? juce::String (rep).getIntValue() : 1);
            for (int attempt = 1; attempt <= reps; ++attempt)
            {
            const int before = (int) linkRowOf (Rig::state(), linkUid).getProperty ("slotCount", -1);
            const auto name = linkRowOf (Rig::state(), linkUid).getProperty ("name", juce::var()).toString();
            const auto ask = "build a chain on " + (name.isNotEmpty() ? name : linkUid)
                           + ": an EQ then a compressor";
            // 300 s, not 180 (14 Oct): this build took 183 s against the real model - five rounds and two
            // plans to approve - so the bound, not the build, was what failed. A bound tighter than the
            // work it measures turns every slow pass into a fault of its own making.
            const auto r = askAgent (rig, ask, 300000);
            int after = before;
            rig.until ([&] (const juce::var& st)
            { after = (int) linkRowOf (st, linkUid).getProperty ("slotCount", -1); return after >= 2; }, 30000);
            const auto slots = linkRowOf (Rig::state(), linkUid).getProperty ("slots", juce::var()).toString();
            // ON A REPEAT THE RACK IS ALREADY BUILT, so "it holds two slots" proves nothing about THIS
            // attempt - the vacuous pass in its other form. Every attempt after the first must therefore show
            // the build REACHING that Link in its own steps; the first attempt may rely on the count rising.
            const bool sentABuild = r.stepWords.containsIgnoreCase ("build")
                                 || r.stepWords.containsIgnoreCase ("the rack took it")
                                 || r.stepWords.containsIgnoreCase ("slot(s) on the rack");
            const bool ok = after >= 2 && (attempt == 1 || sentABuild);
            // A NAMED local, not a temporary: row() takes a const char* and toRawUTF8() on a temporary
            // juce::String would dangle before the call returned.
            const juce::String id = reps == 1 ? juce::String ("L2")
                                              : "L2#" + juce::String (attempt);
            row (id.toRawUTF8(), ok, ask,
                 "that Link's own sidecar holds " + juce::String (after) + " slot(s) [" + slots + "], was "
                 + juce::String (before) + ", state " + r.state
                 + (attempt > 1 ? juce::String (sentABuild ? ", a build reached the Link this attempt"
                                                           : ", NO BUILD REACHED THE LINK THIS ATTEMPT")
                                : juce::String())
                 + (ok ? "" : ", card said: " + r.text.substring (0, 160) + r.failDetail), &r);
            }
        }

        // ---- L3: a Link's OUT ------------------------------------------------
        if (linkUid.isNotEmpty())
        {
            const double before = (double) linkRowOf (Rig::state(), linkUid).getProperty ("gainDb", 0.0);
            const auto name = linkRowOf (Rig::state(), linkUid).getProperty ("name", juce::var()).toString();
            const auto ask = "turn " + (name.isNotEmpty() ? name : linkUid) + "'s output down 2 dB";
            const auto r = askAgent (rig, ask);
            double after = before;
            rig.until ([&] (const juce::var& st)
            { after = (double) linkRowOf (st, linkUid).getProperty ("gainDb", 0.0);
              return std::abs ((after - before) + 2.0) < 0.3; }, 15000);
            const bool moved = std::abs ((after - before) + 2.0) < 0.3;
            row ("L3", moved, ask,
                 "that Link's published OUT " + juce::String (before, 2) + " -> " + juce::String (after, 2)
                 + " dB, state " + r.state
                 + (moved ? "" : ", card said: " + r.text.substring (0, 160) + r.failDetail), &r);
        }

        // ---- L5: a track that does not exist ---------------------------------
        // TWO FAULTS FIXED HERE (14 Oct). The row passed while a compressor WAS added - to a real Link -
        // because (a) the rig answered the card's "which track did you mean?" with the first chip, which is
        // an instruction, and (b) it measured the OWN rack only while claiming in its own words that
        // "nothing was changed on any rack we have". It now declines to answer, and counts every rack.
        {
            auto rackCensus = [&] (const juce::var& st)
            {
                juce::String c = "own=" + juce::String (ownSlotCount (st));
                if (auto* arr = st.getProperty ("links", juce::var()).getArray())
                    for (const auto& l : *arr)
                        c << " " << l.getProperty ("name", juce::var()).toString() << "="
                          << (int) l.getProperty ("slotCount", -1);
                return c;
            };
            const auto censusBefore = rackCensus (rig.freshState());
            const auto r = askAgent (rig, "add a compressor on the Trombone bus", 120000, /*answerAsks*/ false);
            const auto censusAfter = rackCensus (rig.freshState());
            const bool safe    = censusAfter == censusBefore;   // EVERY rack, not just the open one
            const bool itAsked = r.sawAsk;
            // IT MUST SAY SOMETHING ABOUT THE TRACK, by either road. Requiring a STRUCTURED ask was too
            // narrow: the model answers this two ways, both honest - sometimes a talk(ask) with the real
            // track names as choices, sometimes prose saying it cannot see the track. What is NOT acceptable
            // is silence: "nothing changed" with no explanation is indistinguishable from a turn that died.
            const juce::String said = r.text + " " + r.lastAsk;
            const bool explained = itAsked
                                || said.containsIgnoreCase ("trombone")
                                || said.containsIgnoreCase ("don't see") || said.containsIgnoreCase ("dont see")
                                || said.containsIgnoreCase ("can't see")  || said.containsIgnoreCase ("cannot see")
                                || said.containsIgnoreCase ("no track")   || said.containsIgnoreCase ("not linked")
                                || said.containsIgnoreCase ("which track");
            row ("L5", safe && explained, "add a compressor on the Trombone bus (no such track)",
                 juce::String (! safe ? "A RACK CHANGED: " + censusBefore + " -> " + censusAfter
                               : explained ? (itAsked ? "it asked which track and touched nothing"
                                                      : "it said plainly it cannot see that track, and touched nothing")
                                           : "nothing changed, but it NEVER SAID WHY - silence is not a refusal")
                 + ", state " + r.state + ", said: \"" + said.trim().substring (0, 130) + "\""
                 + (safe ? "" : r.failDetail), &r);
            // AND CLEAN UP AFTER ITSELF. Leaving the question unanswered is this row's whole point, but a
            // card left in awaiting_ask is a LIVE SESSION, and the next row's typed message answers it: A6's
            // "add a limiter" was consumed as the answer to L5's question (EJAgent: ask answered (typed)),
            // and H3 fell over in the same cascade. The rule askAgent already follows - never hand the next
            // row a live session - applies to a row that deliberately leaves one open.
            if (r.sawAsk)
            {
                rig.send (echojay::rig::cmd::press, args1 ("button", "stop"), 8000);
                rig.until ([] (const juce::var& st)
                           { const auto a = agentVarOf (st);
                             return ! (bool) a.getProperty ("active", false)
                                    && isRestingState (a.getProperty ("state", juce::var()).toString()); }, 20000);
            }
        }

        // ---- A6: agent mode OFF proposes and changes nothing -----------------
        {
            // THE SAME QUIET WAIT askAgent DOES. This row does not go through askAgent - it sends a chat
            // with the chip off - so it had no protection against inheriting a live session, which is
            // exactly how it came to answer L5's pending question with "add a limiter".
            if (! rig.until ([] (const juce::var& st)
                             { const auto a = agentVarOf (st);
                               return ! (bool) a.getProperty ("active", false)
                                      && isRestingState (a.getProperty ("state", juce::var()).toString()); }, 60000))
                std::printf ("        (WARNING: a turn was still live before A6 sent its chat)\n");
            rig.send (echojay::rig::cmd::agentChip, args1 ("on", false));
            const auto stBefore = rig.freshState();
            const auto before = ownSlotCount (stBefore);
            // ITS OWN EVIDENCE (14 Oct, Sean's instruction). This row runs NO agent turn on purpose - the
            // chip is off, so the words go to chat - which means AgentOutcome::ran() has nothing to say about
            // it and the generic gate cannot apply. Its claim is "the agent changed nothing", and with the
            // server down that is true for the dullest possible reason. So it requires a REPLY: the
            // transcript must grow, which is the proof that the words were sent, answered, and still changed
            // no rack.
            const int rowsBefore = (int) stBefore.getProperty ("transcriptRows", 0);
            rig.send (echojay::rig::cmd::sendChat, args1 ("text", "add a limiter"), 15000);
            // WAIT FOR THE REPLY, BOUNDED - do not pump a guessed 20 s. The real model takes 8-22 s on a
            // chat turn (the server's own log), so 20 s failed this row on a turn that was simply still in
            // flight: the same "a bound tighter than the work it measures" fault as L2's 180 s.
            rig.until ([&] (const juce::var& st)
                       { return (int) st.getProperty ("transcriptRows", 0) > rowsBefore + 1; }, 60000);
            const auto stAfter = rig.freshState();
            const auto after = ownSlotCount (stAfter);
            const int rowsAfter = (int) stAfter.getProperty ("transcriptRows", 0);
            // AN ERROR BUBBLE IS NOT A REPLY (14 Oct, found by the red proof). Counting rows alone, this row
            // PASSED under a deliberately broken token: the chat 401'd, the editor pushed its failure notice
            // into the transcript, the count grew by two, and "the rack is untouched" was reported as the
            // agent behaving correctly. So the LAST assistant row must carry something a user would read as
            // an answer - and the failure copy is matched on, rather than guessed at by length.
            juce::String lastAssistant;
            if (auto* rows = stAfter.getProperty ("transcript", juce::var()).getArray())
                for (const auto& m : *rows)
                    if (m.getProperty ("role", juce::var()).toString() == "assistant")
                        lastAssistant = m.getProperty ("text", juce::var()).toString();
            // POSITIVE EVIDENCE, NOT A BLACKLIST. The first cut matched failure copy and A6 passed anyway:
            // the notice was "Session expired. Please log in again." and the list had "sign in", not "log in".
            // Guessing at error strings is a losing game - the copy changes and the row quietly goes vacuous
            // again. So the test is what a REAL answer must contain: this row asks for a limiter, and no
            // "session expired" notice will ever mention one. The blacklist stays as belt and braces only.
            const bool namesTheAsk   = lastAssistant.containsIgnoreCase ("limiter");
            const bool looksLikeFailure = lastAssistant.containsIgnoreCase ("couldn't reach")
                                       || lastAssistant.containsIgnoreCase ("could not reach")
                                       || lastAssistant.containsIgnoreCase ("not authenticated")
                                       || lastAssistant.containsIgnoreCase ("sign in")
                                       || lastAssistant.containsIgnoreCase ("log in")
                                       || lastAssistant.containsIgnoreCase ("session expired")
                                       || lastAssistant.containsIgnoreCase ("try again")
                                       || lastAssistant.containsIgnoreCase ("offline");
            const bool replied = rowsAfter > rowsBefore + 1
                              && lastAssistant.trim().length() > 20
                              && namesTheAsk
                              && ! looksLikeFailure;
            row ("A6", replied && after == before, "(agent OFF) add a limiter",
                 juce::String (after != before ? "THE RACK CHANGED WITH THE AGENT OFF"
                               : replied            ? "the rack is untouched - it proposed instead"
                                                    : "NO REPLY ARRIVED, so \"the rack is untouched\" proves "
                                                      "nothing and this row fails")
                 + ", slots " + juce::String (before) + " -> " + juce::String (after)
                 + ", transcript " + juce::String (rowsBefore) + " -> " + juce::String (rowsAfter)
                 + (replied ? "" : ", the last assistant row was: \"" + lastAssistant.substring (0, 90) + "\""));
            rig.send (echojay::rig::cmd::agentChip, args1 ("on", true));
        }

        // ---- R7/H3: a GR read aimed at another track's Link -------------------
        if (linkUid.isNotEmpty())
        {
            const auto name = linkRowOf (Rig::state(), linkUid).getProperty ("name", juce::var()).toString();
            const auto ask = "how much gain reduction is the compressor doing on "
                           + (name.isNotEmpty() ? name : linkUid) + "?";
            const auto r = askAgent (rig, ask);
            const bool plain = ! r.text.containsIgnoreCase ("not on the rack")
                            && ! r.failDetail.containsIgnoreCase ("not on the rack");
            row ("H3", r.settled && plain, ask,
                 juce::String (plain ? "it said plainly that it cannot measure a Link yet"
                                     : "it answered with \"slot not on rack\", which is the wrong refusal")
                 + ", state " + r.state + ", said: " + r.text.substring (0, 160) + r.failDetail, &r);
        }

        // ---- F2: an op this build does not have -----------------------------
        {
            const auto r = askAgent (rig, "set the rack's tilt to 3");
            row ("F2", r.settled, "set the rack's tilt to 3 (an op that does not exist)",
                 "state " + r.state + ", it stopped rather than looping"
                 + (r.failDetail.isNotEmpty() ? ", user line: " + r.failDetail.substring (0, 120) : ""), &r);
        }
    }

    // ---- teardown in the host's own order ---------------------------------
    v2.closeEditor();
    for (Track* t : { &v2, &lk1, &lk2 }) t->plugin->releaseResources();
    lk2.plugin.reset(); lk1.plugin.reset(); v2.plugin.reset();

    std::printf ("==== host_rig: %s (%d failed, %d passed, %d skipped) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures, passes, skips);
    if (skips > 0) std::printf ("     %d leg(s) could not run - each named above with its reason. A skip is "
                                "not a pass and is not counted as one.\n", skips);
    return failures == 0 ? 0 : 1;
}
