// remote_ops_guard (10 Oct 2026) - THE V2'S OWN CONTROLS WORK ON A RACK IT DOES NOT OWN.
//
// WHAT WAS MISSING AND WHAT IT COST. The command transport has been rich for months - add, remove, replace,
// move, bypass, set, set_wet, reset_levels, level_match, headroom, undo - but FOUR figures were written
// STRAIGHT ONTO THE HOST (setSlotPreTrimDb / setSlotOutGainDb / setMasterWet / setPreGainDb). That is correct
// for a rack the V2 owns and impossible for one it does not, so on a REMOTE rack the V2's own IN/OUT readouts
// were READ-ONLY: onSlotGainSet is suppressed when `remote`, and the user could see a figure they could not
// move. docs/LINK_REMOTE_CONTROL_PLAN.md section 2 names all four "MISSING as an op".
//
// Stage 2 adds them to the chain channel with acks, under the names
// docs/CONTRACT_LINK_COMMANDS.md section 2.1 already gave them: slot_in, slot_out, master_wet, pre_gain, with
// the contract's own field names (`db` for a gain, `pct` for a wet).
//
// WHAT THIS GUARD ASSERTS, AND WHY EACH PART IS HERE:
//   (1) THE WIRE. parseChainEditOps reads each op and its value off real JSON - the shape a Link actually
//       receives - not a struct filled in by hand. A struct-only leg proves the apply path and nothing about
//       whether the op can ARRIVE.
//   (2) ABSENT IS REFUSED, NOT DEFAULTED. A gain op with no `db` must be refused. If it applied a default it
//       would set unity, a wet op would set fully dry, and once landed neither is distinguishable from the
//       user's own move. This is the half that cannot be seen by looking at a successful case.
//   (3) THE FIGURE ACTUALLY MOVES, through the real applyChainEdits, read back from the host - and the ack
//       reports the LANDED value, not the asked one, so a clamp is never reported as the user's figure.
//   (4) THE CARD CAN DESCRIBE THEM. An op the card cannot describe gets no row; a card with no rows has no
//       height; a card with no height gets no Apply button. That is exactly how the chat-route level-match
//       card came to be readable and not applicable (21t-i), so it is asserted rather than assumed.
//   (5) THEY ARE VALUE OPS FOR R3. A revision that moved for an unrelated reason must not refuse them - these
//       are the ops a user reaches for WHILE adjusting something else.
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "ChainHost.h"
#include "EedGainProcessor.h"
#include "EedDeviceRegistry.h"
#include "EJCmdRing.h"   // stage 3: the value ring
#include "EJRemoteControl.h"   // stage 4: embed is proven, not inferred
#include <cstdio>

namespace {
int failures = 0;
void check (bool ok, const juce::String& w, const juce::String& d = {})
{
    std::printf ("  %s  %s%s\n", ok ? "ok  " : "FAIL", w.toRawUTF8(),
                 d.isNotEmpty() ? ("  [" + d + "]").toRawUTF8() : "");
    if (! ok) ++failures;
}
// The same pump the other guards use: timers fired synchronously plus a short run-loop turn, because
// applyChainEdits hands control back to the message loop between ops (Timer::callAfterDelay(30)).
void pump (double ms)
{
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    while (juce::Time::getMillisecondCounterHiRes() - t0 < ms)
    {
        juce::Timer::callPendingTimersSynchronously();
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.005, false);
    }
}

// Run a batch through the REAL apply path and hand back the result lines.
bool applyOps (ChainHost& h, std::vector<ChainHost::ChainEditOp> ops,
               const juce::StringArray& names, juce::StringArray& out, bool& aborted)
{
    bool done = false; aborted = true; out.clear();
    h.applyChainEdits (ops, -1, names,
                       [&] (const juce::StringArray& r, int, bool ab) { out = r; aborted = ab; done = true; });
    for (int k = 0; k < 80 && ! done; ++k) pump (25);
    return done;
}
} // namespace

int main()
{
    std::setvbuf (stdout, nullptr, _IOLBF, 0);
    juce::ScopedJuceInitialiser_GUI gui;
    std::printf ("remote_ops_guard: slot_in, slot_out, master_wet, pre_gain as acked ops\n");
    // The static archive links a built-in's registrar object only when something REFERENCES it, so a guard
    // that merely looks the device up by name finds nothing. Same line level_slot_guard carries, same reason.
    (void) EedGainProcessor::schema();

    std::printf ("\n== (1) THE WIRE: each op and its value arrive off real JSON ==\n");
    {
        const auto ops = ChainHost::parseChainEditOps (
            "{\"edit\":["
            "{\"op\":\"slot_in\",\"slot\":1,\"db\":-3.5},"
            "{\"op\":\"slot_out\",\"slot\":2,\"db\":2.25},"
            "{\"op\":\"master_wet\",\"pct\":40},"
            "{\"op\":\"pre_gain\",\"db\":-6}"
            "]}");
        check (ops.size() == 4, "four value ops parsed off the wire", juce::String ((int) ops.size()));
        if (ops.size() == 4)
        {
            // THE WIRE IS 1-BASED and the struct is 0-based: the contract says "slot is always 1-based in
            // anything a user or a model sees and 0-based nowhere in this contract's JSON", and the parser
            // subtracts one. Asserted here so the conversion is part of the wire's test rather than a
            // convention someone has to remember - my own first draft sent slot 0 and got -1.
            check (ops[0].op == "slot_in"  && ops[0].slot == 0 && std::abs (ops[0].dbValue + 3.5f) < 0.001f,
                   "slot_in carries its slot (wire 1 -> index 0) and its db",
                   "slot " + juce::String (ops[0].slot) + " db " + juce::String (ops[0].dbValue, 2));
            check (ops[1].op == "slot_out" && ops[1].slot == 1 && std::abs (ops[1].dbValue - 2.25f) < 0.001f,
                   "slot_out carries its slot (wire 2 -> index 1) and its db",
                   "slot " + juce::String (ops[1].slot) + " db " + juce::String (ops[1].dbValue, 2));
            check (ops[2].op == "master_wet" && std::abs (ops[2].pctValue - 40.0f) < 0.001f,
                   "master_wet carries its pct and names NO slot", juce::String (ops[2].pctValue, 1));
            check (ops[3].op == "pre_gain" && std::abs (ops[3].dbValue + 6.0f) < 0.001f,
                   "pre_gain carries its db", juce::String (ops[3].dbValue, 2));
        }
    }

    std::printf ("\n== (2) ABSENT IS ABSENT: a value op with no value is REFUSED, never defaulted ==\n");
    {
        // A number that is not a number, and a field that is not there at all - both must land as ABSENT.
        const auto ops = ChainHost::parseChainEditOps (
            "{\"edit\":["
            "{\"op\":\"slot_in\",\"slot\":1},"
            "{\"op\":\"slot_out\",\"slot\":1,\"db\":\"loud\"},"
            "{\"op\":\"master_wet\"},"
            "{\"op\":\"pre_gain\",\"db\":null}"
            "]}");
        check (ops.size() == 4, "the four malformed ops still parse as ops (they are refused at apply, where "
                                "the refusal can be SAID)", juce::String ((int) ops.size()));
        bool allAbsent = ops.size() == 4;
        for (const auto& o : ops)
            if (std::isfinite (o.dbValue) || std::isfinite (o.pctValue)) allAbsent = false;
        check (allAbsent, "every missing or non-numeric value reads as ABSENT (NaN), never as 0 - a gain op "
                          "defaulting to 0 dB would silently set unity and look like the user's own move");
    }

    std::printf ("\n== (3) THE FIGURE MOVES, through the real apply path, and the ack reports what LANDED ==\n");
    {
        auto procHeap = std::make_unique<EchoJayProcessor>();
        auto& proc = *procHeap;
        proc.prepareToPlay (48000.0, 512);
        auto& h = proc.getChainHost();

        const auto* gain = BuiltinDeviceRegistry::instance().findByName ("EchoJay Gain");
        check (gain != nullptr, "precondition: EchoJay Gain is registered");
        if (gain != nullptr)
        {
            h.insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*gain), 0);
            h.insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*gain), 1);
            pump (120);
            check (h.getNumSlots() == 2, "precondition: two slots", juce::String (h.getNumSlots()));

            const juce::StringArray names { "EchoJay Gain", "EchoJay Gain" };

            // slot_in
            {
                std::vector<ChainHost::ChainEditOp> ops;
                ChainHost::ChainEditOp o; o.op = "slot_in"; o.slot = 0; o.dbValue = -3.5f; ops.push_back (o);
                juce::StringArray res; bool ab = true;
                check (applyOps (h, ops, names, res, ab) && ! ab, "slot_in applied", res.joinIntoString (" | "));
                check (std::abs (h.getSlotPreTrimDb (0) + 3.5f) < 0.05f,
                       "slot_in MOVED the slot's pre-trim on the host", juce::String (h.getSlotPreTrimDb (0), 2));
                check (res.joinIntoString (" ").contains ("IN") && res.joinIntoString (" ").contains ("-3.5"),
                       "...and the ack line names IN and the landed figure", res.joinIntoString (" | "));
            }
            // slot_out
            {
                std::vector<ChainHost::ChainEditOp> ops;
                ChainHost::ChainEditOp o; o.op = "slot_out"; o.slot = 1; o.dbValue = 2.25f; ops.push_back (o);
                juce::StringArray res; bool ab = true;
                check (applyOps (h, ops, names, res, ab) && ! ab, "slot_out applied", res.joinIntoString (" | "));
                check (std::abs (h.getSlotOutGainDb (1) - 2.25f) < 0.05f,
                       "slot_out MOVED the slot's output gain on the host",
                       juce::String (h.getSlotOutGainDb (1), 2));
                check (res.joinIntoString (" ").contains ("OUT"),
                       "...and the ack line names OUT", res.joinIntoString (" | "));
            }
            // master_wet
            {
                std::vector<ChainHost::ChainEditOp> ops;
                ChainHost::ChainEditOp o; o.op = "master_wet"; o.pctValue = 40.0f; ops.push_back (o);
                juce::StringArray res; bool ab = true;
                check (applyOps (h, ops, names, res, ab) && ! ab, "master_wet applied", res.joinIntoString (" | "));
                check (std::abs (h.getMasterWet() - 0.40f) < 0.01f,
                       "master_wet MOVED the rack's master wet", juce::String (h.getMasterWet(), 3));
            }
            // pre_gain
            {
                std::vector<ChainHost::ChainEditOp> ops;
                ChainHost::ChainEditOp o; o.op = "pre_gain"; o.dbValue = -6.0f; ops.push_back (o);
                juce::StringArray res; bool ab = true;
                check (applyOps (h, ops, names, res, ab) && ! ab, "pre_gain applied", res.joinIntoString (" | "));
                check (std::abs (h.getPreGainDb() + 6.0f) < 0.05f,
                       "pre_gain MOVED the pre-chain gain", juce::String (h.getPreGainDb(), 2));
            }
            // AND THE REFUSAL, on the same rack, so the pair is one experiment: an op with no value is
            // refused and SAYS WHY, and the figure it names does not move.
            {
                const float before = h.getSlotPreTrimDb (0);
                std::vector<ChainHost::ChainEditOp> ops;
                ChainHost::ChainEditOp o; o.op = "slot_in"; o.slot = 0; ops.push_back (o);   // no db
                juce::StringArray res; bool ab = false;
                applyOps (h, ops, names, res, ab);
                const auto why = res.joinIntoString (" | ");
                check (ab || why.containsIgnoreCase ("without a db"),
                       "a slot_in with NO db is refused, and the refusal says what was missing", why);
                check (std::abs (h.getSlotPreTrimDb (0) - before) < 0.001f,
                       "...and the figure did not move", juce::String (h.getSlotPreTrimDb (0), 2));
            }
        }
    }

    std::printf ("\n== (3b) THE VOCABULARY IS STILL CLOSED ==\n");
    {
        // The other direction, and it is the one that says the validation is doing work rather than waving
        // things through: a plausible-looking op that does NOT exist must still be refused by name. Without
        // this, "slot_in is accepted" would be equally true of a build that accepted anything.
        auto procHeap = std::make_unique<EchoJayProcessor>();
        auto& h = procHeap->getChainHost();
        procHeap->prepareToPlay (48000.0, 512);
        const auto* gain = BuiltinDeviceRegistry::instance().findByName ("EchoJay Gain");
        if (gain != nullptr) { h.insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*gain), 0); pump (120); }
        std::vector<ChainHost::ChainEditOp> ops;
        ChainHost::ChainEditOp o; o.op = "slot_sideways"; o.slot = 0; o.dbValue = 1.0f; ops.push_back (o);
        juce::StringArray res; bool ab = false;
        applyOps (h, ops, juce::StringArray { "EchoJay Gain" }, res, ab);
        const auto why = res.joinIntoString (" | ");
        check (ab || why.containsIgnoreCase ("unknown operation"),
               "an op that does not exist is refused BY NAME - so accepting slot_in is a decision, not a "
               "build that accepts anything", why);
    }

    std::printf ("\n== (4) THE CARD CAN DESCRIBE THEM (no row, no height, no Apply button) ==\n");
    {
        const juce::StringArray names { "EchoJay Gain", "EchoJay EQ" };
        auto line = [&] (const char* opName, int slot, float db, float pct)
        {
            ChainHost::ChainEditOp o; o.op = opName; o.slot = slot; o.dbValue = db; o.pctValue = pct;
            return ChainHost::describeEditOp (o, names);
        };
        const auto inL  = line ("slot_in", 0, -3.5f, std::numeric_limits<float>::quiet_NaN());
        const auto outL = line ("slot_out", 1, 2.0f, std::numeric_limits<float>::quiet_NaN());
        const auto mwL  = line ("master_wet", -1, std::numeric_limits<float>::quiet_NaN(), 40.0f);
        const auto pgL  = line ("pre_gain", -1, -6.0f, std::numeric_limits<float>::quiet_NaN());
        check (inL.isNotEmpty() && inL.contains ("IN") && inL.contains ("-3.5") && inL.contains ("EchoJay Gain"),
               "slot_in's card line names the direction, the figure and the plugin", inL);
        check (outL.isNotEmpty() && outL.contains ("OUT") && outL.contains ("EchoJay EQ"),
               "slot_out's card line names the direction and the plugin", outL);
        check (mwL.isNotEmpty() && mwL.containsIgnoreCase ("master wet") && mwL.contains ("40"),
               "master_wet's card line names the rack's own blend", mwL);
        check (pgL.isNotEmpty() && pgL.containsIgnoreCase ("pre-chain") && pgL.contains ("-6"),
               "pre_gain's card line names the pre-chain gain", pgL);
        // ...and an op whose value never arrived still produces a LINE, so the card has a row to show rather
        // than silently dropping an op the user asked for.
        ChainHost::ChainEditOp bare; bare.op = "slot_in"; bare.slot = 0;
        const auto bareL = ChainHost::describeEditOp (bare, names);
        check (bareL.isNotEmpty() && bareL.containsIgnoreCase ("no db"),
               "an op with no value still gets a row, and the row says the value is missing", bareL);
    }

    std::printf ("\n== (5) STAGE 3: THE VALUE RING ==\n");
    {
        using namespace echojay::cmdring;

        // THE LAYOUT IS THE WIRE. Two processes map this block and are built
        // separately, so a field added in the middle is a silent mis-application
        // between a V2 and a Link of different vintages. The static_asserts in the
        // header make that a compile error; these check the numbers a reader of the
        // contract would check, so the contract and the code can be compared
        // without reading C++.
        check (sizeof (CmdFrame) == 64, "ring: a frame is 64 bytes",
               juce::String ((int) sizeof (CmdFrame)));
        check (sizeof (CmdRing) == 64 + 64 * 256, "ring: the block is a 64-byte header plus 256 frames",
               juce::String ((int) sizeof (CmdRing)));
        check (kMagic == 0xEC4A3003u && kLayoutVersion == 1u && kCapacity == 256u,
               "ring: magic, layout version and capacity are the contract's",
               juce::String::toHexString ((int) kMagic));
        // The enum numbers ARE the wire and may never be reordered. Pinned by value.
        check (kSetParam == 1 && kSlotIn == 2 && kSlotOut == 3 && kSlotWet == 4
               && kMasterWet == 5 && kPreGain == 6 && kLinkGain == 7,
               "ring: the command numbers are pinned - a renumbering is a silent "
               "mis-application between two builds, not a compile error");

        auto ring = std::make_unique<CmdRing>();
        check (! ringUsable (ring.get()),
               "ring: an UNINITIALISED block is refused - a mapped block is written by another process and a "
               "wrong layout read as frames is a segfault");
        initRing (ring.get());
        check (ringUsable (ring.get()), "ring: an initialised block is usable");

        auto frame = [] (uint32_t kind, int slot, uint32_t paramId, double v,
                         uint32_t phase, uint32_t gesture, uint32_t rev)
        {
            CmdFrame f; f.kind = kind; f.slot = slot; f.paramId = paramId; f.value = v;
            f.phase = phase; f.gesture = gesture; f.flags = rev & 0xFFFFu; return f;
        };

        // COALESCING: 400 stream frames for ONE control collapse to the NEWEST.
        for (int i = 0; i < 400; ++i)
            push (ring.get(), frame (kSetParam, 0, 3, (double) i, kStream, 7, 0));
        {
            const auto d = drain (ring.get(), 0);
            check (d.count == 1 && std::abs (d.kept[0].value - 399.0) < 0.001,
                   "ring: 400 stream frames for one control drain to ONE frame, the NEWEST - which is what "
                   "makes a drag cost one write per control instead of one per pixel",
                   juce::String (d.count) + " kept, value " + juce::String (d.kept[0].value, 1)
                       + ", " + juce::String (d.discarded) + " superseded");
            check (d.overflow > 0,
                   "ring: ...and lapping the reader was COUNTED, not hidden (400 frames into a 256 ring)",
                   juce::String ((int) d.overflow));
        }

        // DISTINCT CONTROLS DO NOT COALESCE INTO EACH OTHER.
        push (ring.get(), frame (kSetParam, 0, 3, 1.0, kStream, 8, 0));
        push (ring.get(), frame (kSetParam, 0, 4, 2.0, kStream, 8, 0));   // same slot, other param
        push (ring.get(), frame (kSetParam, 1, 3, 3.0, kStream, 8, 0));   // other slot, same param
        push (ring.get(), frame (kSlotIn,   0, 0, 4.0, kStream, 8, 0));   // other kind
        {
            const auto d = drain (ring.get(), 0);
            check (d.count == 4, "ring: four DISTINCT controls survive as four frames - coalescing is per "
                                 "(kind, slot, paramId), not per gesture", juce::String (d.count));
        }

        // THE BOUNDARIES ARE NEVER DISCARDED, because one undo step per gesture
        // depends on them. This is the half a newest-wins coalescer gets wrong.
        push (ring.get(), frame (kSetParam, 0, 3, 0.0, kBegin,  9, 0));
        for (int i = 1; i <= 50; ++i)
            push (ring.get(), frame (kSetParam, 0, 3, (double) i, kStream, 9, 0));
        push (ring.get(), frame (kSetParam, 0, 3, 99.0, kEnd,   9, 0));
        {
            const auto d = drain (ring.get(), 0);
            int begins = 0, ends = 0, streams = 0;
            for (int i = 0; i < d.count; ++i)
            {
                if (d.kept[i].phase == kBegin)  ++begins;
                if (d.kept[i].phase == kEnd)    ++ends;
                if (d.kept[i].phase == kStream) ++streams;
            }
            check (begins == 1 && ends == 1 && streams == 1 && d.count == 3,
                   "ring: a whole gesture drains to begin + ONE coalesced stream + end - the boundaries are "
                   "never discarded, because they are what make one undo step per gesture possible",
                   juce::String (begins) + " begin, " + juce::String (streams) + " stream, "
                       + juce::String (ends) + " end");
            // ...and the end frame is the SETTLED value, which is the authority.
            bool endIsSettled = false;
            for (int i = 0; i < d.count; ++i)
                if (d.kept[i].phase == kEnd && std::abs (d.kept[i].value - 99.0) < 0.001) endIsSettled = true;
            check (endIsSettled, "ring: ...and the end frame carries the settled value");
        }

        // A STALE STRUCTURE REVISION IS DISCARDED WITH A COUNT, never applied. A V2
        // with an old sidecar cannot name a parameter the Link lacks, but it CAN
        // name the wrong one - the same class of mistake baseSlots guards on the
        // chain channel, so it gets the same treatment.
        push (ring.get(), frame (kSetParam, 0, 3, 5.0, kStream, 10, 4242));   // stale
        push (ring.get(), frame (kSetParam, 0, 3, 6.0, kStream, 10, 777));    // live
        {
            const auto d = drain (ring.get(), 777);
            check (d.dropped == 1 && d.count == 1 && std::abs (d.kept[0].value - 6.0) < 0.001,
                   "ring: a frame stamped with a STALE structure revision is dropped WITH A COUNT and the "
                   "live one applies", juce::String (d.dropped) + " dropped, " + juce::String (d.count) + " kept");
        }
        // ...and BOTH DIRECTIONS: with the matching revision it applies, so the
        // guard is not simply dropping everything.
        push (ring.get(), frame (kSetParam, 0, 3, 8.0, kStream, 11, 777));
        {
            const auto d = drain (ring.get(), 777);
            check (d.dropped == 0 && d.count == 1 && std::abs (d.kept[0].value - 8.0) < 0.001,
                   "ring: ...and a MATCHING revision applies - the check is a decision, not a blanket refusal",
                   juce::String (d.dropped) + " dropped, " + juce::String (d.count) + " kept");
        }
        // An empty ring drains to nothing and says so, rather than handing the audio
        // thread a stale frame it already applied.
        {
            const auto d = drain (ring.get(), 777);
            check (d.count == 0 && d.discarded == 0 && d.dropped == 0,
                   "ring: a drained ring drains to NOTHING - no frame is applied twice",
                   juce::String (d.count));
        }
    }

    std::printf ("\n== (6) STAGE 3: A RING MOVE AND A CARD MOVE END AT THE SAME VALUE, ONE UNDO STEP ==\n");
    {
        // SEAN'S LEG, and it is the one that makes ruling (a) safe to live with. The RT applier moves the
        // audio immediately and the bookkeeping follows on the message thread, so for a window the figure the
        // audio uses and the figure the readback reports are written by two different things. If those two ever
        // disagreed, "one authority per figure" would be a claim and not a fact - so it is measured, on the
        // same figure, by both routes, against each other.
        using namespace echojay::cmdring;
        using RF = ChainHost::RingFigure;

        auto procHeap = std::make_unique<EchoJayProcessor>();
        auto& proc = *procHeap;
        proc.prepareToPlay (48000.0, 512);
        auto& h = proc.getChainHost();
        const auto* gain = BuiltinDeviceRegistry::instance().findByName ("EchoJay Gain");
        check (gain != nullptr, "precondition: EchoJay Gain is registered");
        if (gain != nullptr)
        {
            h.insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*gain), 0);
            pump (150);

            // ---- ROUTE 1: THE CARD. The ordinary setter, as the Link's own UI calls it.
            h.setSlotPreTrimDb (0, -4.0f);
            const float byCard = h.getSlotPreTrimDb (0);
            check (std::abs (byCard + 4.0f) < 0.01f, "the CARD route lands -4.0 dB on the slot's IN",
                   juce::String (byCard, 2));

            // ---- ROUTE 2: THE RING. The audio-thread applier, then the message thread's catch-up.
            h.setSlotPreTrimDb (0, 0.0f); pump (20);          // back to a known place
            const bool rtOk = h.applyRingValueRT (RF::SlotIn, 0, -4.0f);
            check (rtOk, "the RING route's audio-thread applier accepted the move");
            // BEFORE the bookkeeping: the audio has moved and the RECORD has not. Asserted, because this
            // window is the price of the one-buffer bound and a leg that skipped it would be hiding it.
            check (h.pendingRingBookkeeping() == 1,
                   "...and the record is OWED - one move waiting, which is the window ruling (a) accepts",
                   juce::String (h.pendingRingBookkeeping()));
            const int reconciled = h.drainRingBookkeeping();
            const float byRing = h.getSlotPreTrimDb (0);
            check (reconciled == 1 && h.pendingRingBookkeeping() == 0,
                   "...and the message thread reconciles it exactly once",
                   juce::String (reconciled) + " reconciled");

            // THE ASSERTION SEAN ASKED FOR.
            check (std::abs (byRing - byCard) < 0.01f,
                   "STAGE 3: a RING move and a CARD move on the same figure END AT THE SAME VALUE - so the "
                   "audio-thread applier is the authority and the bookkeeping agrees with it, rather than the "
                   "two being separate writers that can drift",
                   "card " + juce::String (byCard, 3) + " vs ring " + juce::String (byRing, 3));

            // ...AND ONE UNDO STEP. A gesture is one step whichever route it came by: if the ring's catch-up
            // pushed a step per frame, a 400-frame drag would need 400 undos to get back.
            auto& H = proc.undoHistory();
            const int depth0 = H.undoDepth();
            h.setSlotPreTrimDb (0, 0.0f); pump (20);
            const int afterCard = H.undoDepth();
            const int cardSteps = afterCard - depth0;
            for (int i = 1; i <= 40; ++i) h.applyRingValueRT (RF::SlotIn, 0, (float) -i * 0.1f);
            check (h.pendingRingBookkeeping() == 40,
                   "forty ring frames applied to the audio, forty records owed",
                   juce::String (h.pendingRingBookkeeping()));
            h.drainRingBookkeeping(); pump (20);
            const int ringSteps = H.undoDepth() - afterCard;
            check (ringSteps <= cardSteps * 1,
                   "STAGE 3: ...and forty ring frames cost NO MORE undo steps than one card move - a drag must "
                   "not need forty undos to get back",
                   "card " + juce::String (cardSteps) + " step(s), 40 ring frames " + juce::String (ringSteps)
                       + " step(s)");
            check (std::abs (h.getSlotPreTrimDb (0) + 4.0f) < 0.05f,
                   "STAGE 3: ...and the figure ends on the LAST frame's value, not the first",
                   juce::String (h.getSlotPreTrimDb (0), 2));
        }
    }

    std::printf ("\n== (7) STAGE 4: open_editor, and embed is PROVEN rather than inferred ==\n");
    {
        // WHAT THIS GUARD CAN AND CANNOT SEE, said plainly. It can prove the op arrives, names the right slot,
        // reaches the owner as a request, and that `embed` is refused unless the same-process proof holds with
        // the refusal SAID. It cannot prove a window appeared or that a knob turns instantly - that needs a
        // real third-party plugin and a screen, and it is Sean's in-host test, not this file's.
        using namespace echojay;

        // THE PROOF ITSELF, both directions and all three cases. One integer, and it cannot be fooled by a
        // host that renames itself or by a version of Logic that changes its mind about view hosting.
        check (embedAllowed (4321, 4321), "embed is allowed when the pids MATCH");
        check (! embedAllowed (4321, 9999), "embed is refused across a process boundary");
        check (! embedAllowed (0, 4321),
               "embed is refused when the Link publishes NO pid - absence is not permission, and treating it "
               "as permission would embed across a boundary in exactly the hosts that sandbox");
        check (! embedAllowed (4321, 0), "embed is refused when our own pid is unknown");
        // ...and the refusal carries a REASON, which is what stops "embed quietly became float".
        check (embedRefusedReason (0, 4321).containsIgnoreCase ("does not publish a process id"),
               "a Link with no pid refuses with that reason", embedRefusedReason (0, 4321));
        check (embedRefusedReason (4321, 9999).contains ("4321") && embedRefusedReason (4321, 9999).contains ("9999"),
               "a cross-process refusal names BOTH pids, so it can be checked rather than believed",
               embedRefusedReason (4321, 9999));
        check (embedRefusedReason (4321, 4321).isEmpty(),
               "...and a MATCH has no refusal reason at all");

        // THE WIRE, and the default. "float" is the only placement promised, so anything that is not "embed"
        // must read as float rather than refusing the op - the user asked for an editor, not a placement.
        const auto ops = ChainHost::parseChainEditOps (
            "{\"edit\":["
            "{\"op\":\"open_editor\",\"slot\":2,\"where\":\"float\"},"
            "{\"op\":\"open_editor\",\"slot\":1,\"where\":\"embed\"},"
            "{\"op\":\"open_editor\",\"slot\":1,\"where\":\"sideways\"},"
            "{\"op\":\"open_editor\",\"slot\":1},"
            "{\"op\":\"close_editor\",\"slot\":3}"
            "]}");
        check (ops.size() == 5, "five editor ops parsed", juce::String ((int) ops.size()));
        if (ops.size() == 5)
        {
            check (ops[0].op == "open_editor" && ops[0].slot == 1 && ops[0].editorWhere == "float",
                   "open_editor carries its slot (wire 2 -> index 1) and where=float",
                   ops[0].editorWhere + " slot " + juce::String (ops[0].slot));
            check (ops[1].editorWhere == "embed", "where=embed is carried as asked", ops[1].editorWhere);
            check (ops[2].editorWhere == "float",
                   "an UNKNOWN placement reads as float - the default, not a refusal of the op",
                   ops[2].editorWhere);
            check (ops[3].editorWhere == "float", "an ABSENT placement reads as float", ops[3].editorWhere);
            check (ops[4].op == "close_editor" && ops[4].slot == 2, "close_editor carries its slot",
                   juce::String (ops[4].slot));
        }

        // AND IT REACHES THE OWNER, with the slot resolved through the map and the embed decision made.
        auto procHeap = std::make_unique<EchoJayProcessor>();
        auto& h = procHeap->getChainHost();
        procHeap->prepareToPlay (48000.0, 512);
        const auto* gain = BuiltinDeviceRegistry::instance().findByName ("EchoJay Gain");
        if (gain != nullptr)
        {
            h.insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*gain), 0);
            h.insertBuiltinAt (BuiltinDeviceRegistry::descriptionFor (*gain), 1);
            pump (150);
            int gotSlot = -1; bool gotOpen = false, gotEmbed = false, called = false;
            h.onEditorRequest = [&] (int sl, bool open, bool emb)
            { gotSlot = sl; gotOpen = open; gotEmbed = emb; called = true; };

            // A FLOAT request on a rack whose peer pid is another process.
            h.editorPeerPid = 999999;
            {
                std::vector<ChainHost::ChainEditOp> o1;
                ChainHost::ChainEditOp e; e.op = "open_editor"; e.slot = 1; e.editorWhere = "float";
                o1.push_back (e);
                juce::StringArray res; bool ab = true;
                applyOps (h, o1, juce::StringArray { "EchoJay Gain", "EchoJay Gain" }, res, ab);
                check (called && gotSlot == 1 && gotOpen && ! gotEmbed,
                       "open_editor reaches the OWNER with the slot resolved and float decided",
                       "slot " + juce::String (gotSlot) + " open " + juce::String ((int) gotOpen));
                check (res.joinIntoString (" ").containsIgnoreCase ("own window"),
                       "...and the ack says the Link has been ASKED, in its own window",
                       res.joinIntoString (" | "));
            }
            // AN EMBED request across a process boundary: honoured as float, and the ack SAYS SO.
            {
                called = false; gotEmbed = true;
                std::vector<ChainHost::ChainEditOp> o2;
                ChainHost::ChainEditOp e; e.op = "open_editor"; e.slot = 0; e.editorWhere = "embed";
                o2.push_back (e);
                juce::StringArray res; bool ab = true;
                applyOps (h, o2, juce::StringArray { "EchoJay Gain", "EchoJay Gain" }, res, ab);
                check (called && ! gotEmbed,
                       "an embed request across a process boundary is NOT embedded");
                check (res.joinIntoString (" ").containsIgnoreCase ("another process"),
                       "...and the ack carries the REASON, so the downgrade is visible rather than silent",
                       res.joinIntoString (" | "));
            }
            // ...and with the pids MATCHING it IS embedded, so the proof is a decision and not a refusal
            // dressed up as one.
            {
                called = false; gotEmbed = false;
                h.editorPeerPid = (int) ::getpid();
                std::vector<ChainHost::ChainEditOp> o3;
                ChainHost::ChainEditOp e; e.op = "open_editor"; e.slot = 0; e.editorWhere = "embed";
                o3.push_back (e);
                juce::StringArray res; bool ab = true;
                applyOps (h, o3, juce::StringArray { "EchoJay Gain", "EchoJay Gain" }, res, ab);
                check (called && gotEmbed,
                       "in ONE process an embed request IS embedded - the proof decides both ways",
                       res.joinIntoString (" | "));
            }
        }
    }

    std::printf ("\n== (8) STAGE 5: UNDO UNDER REMOTE CONTROL - one step per gesture, and it SENDS the value back ==\n");
    {
        // WHAT WAS WRONG, and it was not a bug: EchoJay's undo recorded NOTHING that happened on a Link. The
        // stack is wired to ChainHost mutations IN THIS PROCESS, and a Link Mixer move is a transport command -
        // no ChainHost here mutates, so no hook fires. Undo was never told. Under remote control that is most
        // of the product, because then every rack edit is a command to a Link.
        using namespace echojay::cmdring;
        auto procHeap = std::make_unique<EchoJayProcessor>();
        auto& proc = *procHeap;
        proc.prepareToPlay (48000.0, 512);
        auto& H = proc.undoHistory();

        const juce::String uid = "remoteopsguard01";
        const int depth0 = H.undoDepth();

        // A WHOLE DRAG: one begin, forty streams, one release. The release is the only thing that records.
        const uint32_t g = proc.ringBeginGesture (uid, (int) kSlotIn, 0, 0, 0.0, 0);
        for (int i = 1; i <= 40; ++i)
            proc.ringStreamValue (uid, g, (int) kSlotIn, 0, 0, -0.1 * i, 0);
        proc.ringEndGestureWithUndo (uid, g, (int) kSlotIn, 0, 0, /*before*/ 0.0, /*after*/ -4.0, 0,
                                     "IN on slot 1");
        const int afterDrag = H.undoDepth() - depth0;
        check (afterDrag == 1,
               "STAGE 5: a 42-frame drag is ONE undo step, pushed on RELEASE - without begin/end the V2 cannot "
               "tell a drag from forty writes, and a step per write would need forty undos to get back",
               juce::String (afterDrag) + " step(s) for 42 frames");
        check (H.undoLabel().contains ("IN on slot 1"),
               "...and the step is LABELLED with what it was, so the Undo tooltip can say it", H.undoLabel());

        // A SECOND GESTURE IS A SECOND STEP. Coalescing within a gesture is right; coalescing two gestures
        // would make the second drag unundoable on its own.
        const uint32_t g2 = proc.ringBeginGesture (uid, (int) kSlotIn, 0, 0, -4.0, 0);
        proc.ringEndGestureWithUndo (uid, g2, (int) kSlotIn, 0, 0, -4.0, -6.0, 0, "IN on slot 1");
        check (H.undoDepth() - depth0 == 2,
               "STAGE 5: a SECOND gesture is a SECOND step", juce::String (H.undoDepth() - depth0));

        // A PLAIN ringEndGesture RECORDS NOTHING. That is what a RESTORE uses, and an undo that pushed an
        // undo would make the stack grow every time it was walked.
        const int beforePlain = H.undoDepth();
        const uint32_t g3 = proc.ringBeginGesture (uid, (int) kSlotIn, 0, 0, -6.0, 0);
        proc.ringEndGesture (uid, g3, (int) kSlotIn, 0, 0, -4.0, 0);
        check (H.undoDepth() == beforePlain,
               "STAGE 5: a plain end (what a RESTORE uses) records NOTHING - an undo must not push an undo",
               juce::String (H.undoDepth() - beforePlain));

        // AND UNDO REPORTS RATHER THAN PRETENDING. This uid is not a present Link, so the restore must be a
        // SKIP WITH A LINE, never a silent success that leaves the stack and the rack disagreeing - the next
        // undo would then compound the disagreement.
        // UndoHistory::undo() returns true whenever it POPPED a step - the skip is reported through the
        // status line, which is this codebase's existing convention (the linkActive arm does the same). My
        // first draft expected false and was simply wrong about the contract, not about the behaviour.
        const int redoBefore = H.redoDepth();
        H.undo();
        check (proc.lastUndoStatus().containsIgnoreCase ("no longer present"),
               "STAGE 5: an undo for a Link that is NOT PRESENT is a SKIP THAT SAYS SO - never a silent "
               "success that leaves the stack and the rack disagreeing, because the next undo would then "
               "compound the disagreement", proc.lastUndoStatus());
        check (H.redoDepth() == redoBefore + 1,
               "...and the step still moves to REDO, so a skip loses nothing and the user can try again once "
               "the Link is back", juce::String (H.redoDepth() - redoBefore));
    }

    std::printf ("\n==== remote_ops_guard: %s (%d assertion(s) failed) ====\n",
                 failures == 0 ? "GREEN" : "RED", failures);
    return failures == 0 ? 0 : 1;
}
