#include "LinkProcessor.h"
#include "EJRemoteControl.h"   // stage 1: the Link processes its own rack; the lease is control only
#include "EJCmdRing.h"        // stage 3: the value ring, drained in processBlock
#include "EJRackSidecarFill.h"   // ruling 1 (21s-b): one source for the slot list
#include "EJSourceType.h"      // round (b): what this channel IS, and where that answer came from
#include "EJStateRoot.h"   // 6 Sep 2026: every user-state path resolves through the isolatable root
#include <signal.h>   // kill(pid, 0): publisher liveness (C4b)
#include <unistd.h>
#include "EedLatencyLog.h"
#include "LinkEditor.h"
#include "LinkShm.h"
#include "FaderTaper.h"   // shared mixer-fader mute taper (P17)
#include "NativeClip.h"   // EchoJay_NSLog — chain-build diagnostics
#include "EedKeyDetectorProcessor.h"   // hosted-detector frame preference (Tier 1)
#include "EedKeyFeed.h"   // COMMIT 4: KeyDisplayPrefs (keyShowRelative)

// Item-1 (Active persistence) diagnosis logging — on by default until the
// flip point is confirmed in the field; EJLinkState: lines in the monitor.
#ifndef ECHOJAY_LINK_STATE_DIAG
 #define ECHOJAY_LINK_STATE_DIAG 1
#endif

LinkProcessor::LinkProcessor()
    : AudioProcessor(BusesProperties()
          .withInput ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput("Output", juce::AudioChannelSet::stereo(), true))
{
    // Per-instance identity (commands/acks/registry address; also the ring
    // file part for unnamed Links). Serialised in state; regenerated on
    // registry collision after track duplication.
    instanceUid_ = juce::String::toHexString(juce::Random::getSystemRandom().nextInt64()).removeCharacters("-").substring(0, 10);
    // ---- FINDABLE IN THIS PROCESS, AND ITS EDITORS LEASED (26 Oct 2026, real-editor round 1) ----
    // The directory is how a V2 in the same process reaches THIS instance instead of loading a copy
    // of the plugin; the hook is how any holder of one of these slots' editors is told to let go
    // before the slot moves. Both are inert until something borrows: today the only borrower is this
    // Link's own plugin window.
    syncLocalDirectory ("constructed");
    chainHost.onSlotsAboutToChange = [this] (int slot0, const char* why)
    {
        const int from = slot0 < 0 ? 1 : slot0 + 1;
        const int n = echojay::EditorLease::revokeFrom (instanceUid_, from,
                                                        why != nullptr ? juce::String (why) : juce::String ("the rack changed"));
        if (n > 0)
            EchoJay_NSLog (("EJLease: " + juce::String (n) + " editor(s) of this Link from slot "
                            + juce::String (from) + " up were closed because " + why).toRawUTF8());
    };
   #if ECHOJAY_NO_VISUAL_FFT
    // Build-configuration diagnostic, and this pass's content check: the
    // 4096-point VISUAL FFT is compiled out of the Link (nothing here reads
    // a spectrum). The 2048-point ANALYSIS FFT stays, so macroBandDb and
    // the published bandRel[6] are unaffected. Absent from the main
    // plugin's binary, which consumes the visual spectrum.
    EchoJay_NSLog("EJLinkMeter: visual FFT gated out of this build; "
                  "analysis FFT and bandRel unaffected");
   #endif
    startTimerHz(30); // 30Hz meter-frame publish (fast ballistics step at
                      // 10Hz was ~1.3 dB per sample); polls every 3rd tick
                      // (~100ms, the old cadence), heartbeat every 30th (1s)

    // Key detection (KEY_DETECTOR_SPEC.md §9, KEY_PRECONDITION_SPEC.md §5.1):
    // PASSIVE, duty-cycled — a committed 8 s pass roughly every 30 s, armed
    // by schedulePassiveKeyPass() only while the transport rolls and signal
    // is present. Not continuous mode: the key does not change four times a
    // second, and this Link has no wheel to animate (live chroma off). The
    // worker idles at a 250 ms wakeup between passes.
    keyEngine_.setContinuous(false);
    keyEngine_.setWindowSeconds(kKeyPassWindowS);
    keyEngine_.setLiveChromaEnabled(false);
    keyWorker_.startThread();

    // Mirror hosted chain latency into the host on EVERY chain change —
    // Link sits on parallel and phase-critical tracks, so this must track
    // build/add/remove/bypass exactly.
    // ---- SEAN'S #1, 16 Oct 2026: THE LINK CAN ASK FOR A PARAMETER MAP AT LAST ----------------------
    //
    // A third-party plugin can only be dialled through a parameter map, and ChainHost::onNeedParamMaps is
    // the only route to one. It was installed ONLY by EchoJayEditor (the main and borrowed hosts), so a
    // third-party slot living on a Link could never ASK: its own log said
    // "fetch_requested=n  in_flight=n  fetch_wired=n  cached_maps=181 -> noMap", and the settings simply
    // never applied. Sean lost four plugins to this (MTransient, bx_townhouse, SSL X-Comp, Transient
    // Master) while built-ins dialled perfectly on the same Link, because built-ins need no map.
    //
    // THIS PROCESS HAS NO NETWORK CLIENT and should not grow one - no credentials, no HTTP stack. So the
    // "fetch" is a REQUEST FILE that the V2 serves from its own authenticated API, and everything else in
    // ChainHost is untouched: mapsRequested_, pendingMapFps_ and storeParamMaps's mapArrived re-dial sweep
    // all behave exactly as they do on the main host. fetch_wired reads `y` for the first time.
    chainHost.onNeedParamMaps = [this] (const juce::StringArray& fps)
    {
        if (fps.isEmpty()) return;
        int err = 0;
        const auto dir = LinkShm::resolveDir (err);
        if (dir.isEmpty() || instanceUid_.isEmpty()) return;
        const juce::File f (dir + "maps-wanted-" + instanceUid_ + ".json");
        // MERGED, not overwritten: two slots can want different maps within the same second, and the V2
        // reads this file on a 1 Hz timer. Losing one of them would leave that slot undialled for ever.
        juce::StringArray want;
        if (f.existsAsFile())
        {
            // THE ROOT IS HELD IN A NAMED VAR, and temp_var_grep_guard caught me writing it the other
            // way an hour ago: `juce::JSON::parse(...).getDynamicObject()` takes a raw pointer INTO A
            // TEMPORARY, which dies at the end of the full expression - so the pointer dangles for the
            // whole of the body that uses it. That is the 18 Sep Pro Tools crash exactly, and the guard
            // exists because of it. A reference-counted root that outlives every pointer taken from it.
            const auto rootVar = juce::JSON::parse (f.loadFileAsString());
            if (auto* o = rootVar.getDynamicObject())
                if (auto* arr = o->getProperty ("fps").getArray())
                    for (const auto& v : *arr) want.addIfNotAlreadyThere (v.toString());
        }
        for (const auto& fp : fps) want.addIfNotAlreadyThere (fp);
        auto* o = new juce::DynamicObject();
        o->setProperty ("v", 1);
        o->setProperty ("uid", instanceUid_);
        juce::Array<juce::var> arr;
        for (const auto& fp : want) arr.add (fp);
        o->setProperty ("fps", arr);
        o->setProperty ("atMs", (double) juce::Time::currentTimeMillis());
        f.replaceWithText (juce::JSON::toString (juce::var (o), true));
        EchoJay_NSLog (("EJParamMaps(Link): asked the V2 for " + juce::String (fps.size())
                        + " map(s), " + juce::String (want.size()) + " outstanding - "
                        + f.getFileName()).toRawUTF8());
    };

    // ---- SEAN'S #9 (16 Oct 2026): THE LINK'S SUGGESTED SETTINGS AFTER A DIAL --------------------
    //
    // "the Link's Suggested Settings for it says 'No suggested settings for this plugin'" - after an
    // agent add that dialled Renaissance Axx's threshold correctly. ChainHost DOES write the slot's
    // text when a dial lands ("Applied automatically\nthreshold -20 dB") and raises
    // onSlotSettingsChanged to say so. The main plugin's editor has listened to that since the dial
    // work shipped; THIS PROCESS NEVER SET THE HOOK AT ALL, so the Link's own display model kept
    // whatever it had when the slot was created - which for an agent add is nothing.
    //
    // resyncChainModelFromHost + notifyChainModel, the same pair every other Link edit road ends
    // with, so the card, the sidecar and a reopened window all read the one text the host wrote.
    chainHost.onSlotSettingsChanged = [this]
    {
        resyncChainModelFromHost();
        notifyChainModel();
    };

    chainHost.onChainChanged = [this]
    {
        updateChainLatency();
        // Same stale-hold problem as the main plugin: the Link's own meterEngine_
        // reads POST-rack (its mix-bus strip), so a rack change leaves its held
        // true peak / peak / overs describing audio the rack no longer produces.
        // Drop those holds; integrated LUFS / LRA keep accumulating. Message
        // thread signal only.
        meterEngine_.resetHolds();
        // 19 Oct 2026: and the published loudest-3 s hold goes with them, for the same reason - it
        // describes audio this rack no longer produces. It is NOT cleared by a loop window, which
        // is the whole point of keeping it here rather than reading the tally's max.
        pubShortMaxDb_ = -200.0f;
    };

    // ---- LEVELLING V2 (10 Oct 2026): THIS LINK'S OWN STAGES ------------------------------------------
    // match  -> this Link's own output gain, post its whole rack (reverb included). It is the mixer strip's
    //           GAIN, a control that already exists, per Sean's ruling; 08c's staleness machinery handles the
    //           shared ownership rather than the loop fighting the user for the fader.
    // target -> the final limiter slot's own input_db, exactly as the V2 resolves it for its own rack.
    // The RACK's own gain, independent of whatever stage a match drives (20 Oct 2026) - see the V2
    // twin: a Level slot's gain is rack-level and must not land on a slot's OUT trim.
    loudnessLoop_.resolveRackGainStage = [this] () -> LoudnessLoop::Stage
    {
        LoudnessLoop::Stage st;
        st.name       = "rack_out";
        st.outsideTap = true;
        st.ready      = [] { return true; };
        st.readDb     = [this] { return gainDb_.load (std::memory_order_relaxed); };
        st.writeDb    = [this] (float db) { setGainDb (db); };
        return st;
    };
    loudnessLoop_.resolveStage = [this] (bool match) -> LoudnessLoop::Stage
    {
        LoudnessLoop::Stage st;
        if (match)
        {
            // ---- THE SLOT CARD'S OUT TRIM (19 Oct 2026, his correction at 14:0x) -------------
            //
            // *"Level matching uses the SLOT CARD's OUT trim, the per-slot in/out we built for
            // exactly this, never the plugin's own make-up/output... The 14:00 run set the EchoJay
            // Compressor's MAKEUP to 3.93 dB instead; that's the bug to undo."*
            //
            // It is undone: this wrote the plugin's makeup_db/output_db, which was my reading of his
            // 12:27 instruction and is now explicitly wrong. A plugin's make-up is part of how the
            // user set the plugin up; EchoJay moving it silently rewrites their setting. The slot's
            // OUT trim is EchoJay's own gain, exists for this, and shows on the card as "OUT -3.9".
            //
            // ---- AND IT IS NOT AN OUTSIDE TAP (19 Oct 2026, his 17:00 run) ---------------------
            //
            // *"The agent added Tube-Tech CL 1B, Sean played afterwards, and the slot card's OUT
            // trim never moved."*  It moved twice. The log has the whole arithmetic:
            //
            //   opening: chain in -16.6, out -23.2  ->  OUT trim +0.0 -> +6.64 dB      (right)
            //   verify:  "the output measures -9.8 against an aim of -16.6"  ->  -0.14 dB
            //   verify:  "-23.3 against -16.6"  ->  stopped
            //
            // -23.2 + 6.64 = -16.6, which is the match landing exactly on its aim. The verify read
            // -9.8, which is -16.6 + 6.64: the gain counted a SECOND time. So it "corrected" by
            // -6.8 dB, cancelled the match it had just made, and left the trim at -0.14 - which is
            // what he saw as "never moved".
            //
            // I set outsideTap TRUE here reasoning from the 21t-k ruling: the SLOT's out tally is
            // taken before the slot's out gain, so a sensor reading THAT tally cannot see the trim.
            // True, and not the tally the loop reads. The loop measures getChainOutLevels() - the
            // CHAIN's output tally, at the end of the whole chain, downstream of every slot's out
            // gain - so the trim is already in the reading and effectiveOutDb must not add it again.
            // (`rack_out` below IS applied after that tally, and keeps outsideTap for that reason.)
            //
            // The opening hid the error: it runs with the trim still at 0, where adding it twice
            // adds nothing. Only the verify, running at +6.64, could expose it - which is why this
            // arrived as "the trim never moved" rather than as a wrong number.
            // THE SLOT THE ADD PUT IN, when this build named one (his 17:09 item 2); otherwise the
            // outermost, which is what every match used before and is still right for a dial.
            const int namedTarget = (matchTargetSlot_ >= 0 && matchTargetSlot_ < chainHost.getNumSlots()
                                     && chainHost.getSlotInfo (matchTargetSlot_).name.trim()
                                            .equalsIgnoreCase (matchTargetName_))
                                        ? matchTargetSlot_ : -1;
            if (namedTarget < 0 && matchTargetSlot_ >= 0)
                EchoJay_NSLog ("EJLoudness: the slot this change added is no longer where it was, so the "
                               "match uses the outermost slot instead - an identity that moved is not a target");
            if (const int slot = namedTarget >= 0 ? namedTarget : outermostSlot(); slot >= 0)
            {
                st.name       = "slot_out";
                st.outsideTap = false;   // INSIDE the chain-output tally the loop measures (above)
                // 19 Oct (his 18:20): the plugin's own name, for the one line a dead slot earns.
                st.label      = [this, slot] { return slot < chainHost.getNumSlots()
                                                   ? chainHost.getSlotInfo (slot).name : juce::String(); };
                st.ready      = [this, slot] { return slot < chainHost.getNumSlots(); };
                st.readDb     = [this, slot] { return chainHost.getSlotOutGainDb (slot); };
                st.writeDb    = [this, slot] (float db)
                {
                    const float clamped = juce::jlimit (-24.0f, 24.0f, db);
                    EchoJay_NSLog (("EJLoudness: level match writes the SLOT CARD's OUT trim on slot "
                                    + juce::String (slot + 1) + " (\"" + chainHost.getSlotInfo (slot).name
                                    + "\") to " + juce::String (clamped, 2) + " dB - his 14:0x rule. The "
                                      "plugin's own make-up is left exactly as the user set it.").toRawUTF8());
                    chainHost.setSlotOutGainDb (slot, clamped);
                };
                return st;
            }
            // ---- NO SLOT TO WRITE: THE MATCH DOES NOT RUN (Sean's ruling, 20 Oct 2026) --------
            //
            // *"A level match with no slot to write does not run. No fallback to rack_out. An empty
            // rack changes nothing, so there's nothing to match. A loudness TARGET Sean has chosen
            // on purpose (dynamic, pushed, a level target) is a different feature and may still use
            // rack_out."*
            //
            // What stood here fell back to this Link's own rack gain, and his 18:20 screenshot is
            // what that costs: a figure on the rack's OUT captioned "level match" for a correction
            // no slot had asked for. The reasoning in the ruling is the part worth keeping in sight -
            // a match CORRECTS WHAT THE CHAIN DID TO THE LEVEL, so a chain that did nothing has
            // nothing to correct, and any number written in that case is describing something else.
            //
            // REFUSED BY RETURNING A STAGE THAT IS NOT READY, which is the loop's existing road for
            // "there is nowhere to put this": arm() tests stageReady() and logs its own refusal. The
            // name is kept so that line reads properly, and this one says WHY in the ruling's terms.
            // The TARGET branch below is untouched and still uses rack_out, per the ruling.
            if (! saidNoMakeupControl_)
            {
                saidNoMakeupControl_ = true;
                EchoJay_NSLog ("EJLoudness: this rack has no slots, so there is NO level match - a "
                               "match corrects what the chain did to the level, and an empty chain "
                               "did nothing. EchoJay's own rack gain is NOT used as a fallback (his "
                               "ruling of 20 Oct); a loudness TARGET he chose on purpose still may. "
                               "(said once)");
            }
            st.name  = "slot_out";     // what it WOULD have been, so the refusal line names it
            st.ready = [] { return false; };
            return st;
        }
        st.name  = "limiter_in";
        // ---- THE SLOT IS NAMED WHEN B NAMED IT (his evening contract, 20 Oct 2026) -----------
        // This addressed `n - 1` - the LAST slot - which is an index standing in for an identity and
        // would drive a different plugin's input the moment a later add went after the limiter (the
        // 17:09 lesson). A dial names its slot; everything else still takes the last, which is what
        // every target before this round did.
        const int named = loudnessLoop_.namedLimiterSlot;
        // B's named CONTROL is no longer a road for the push (his 17:22 ruling): the IN trim is the
        // only one. It is still what the ceiling check and the read-back name, so it stays read -
        // here it is deliberately unused.
        juce::ignoreUnused (loudnessLoop_.namedLimiterControl);
        const int useSlot = (named >= 0 && named < chainHost.getNumSlots()) ? named : -1;
        st.ready = [this, useSlot]
        {
            const int n = chainHost.getNumSlots();
            if (useSlot >= 0) return useSlot < n;   // B named it: its presence is the whole test
            return n > 0 && ChainHost::isLimiterLikeName (chainHost.getSlotInfo (n - 1).name);
        };
        // ---- THE TARGET DRIVES THE SLOT CARD'S IN TRIM (his 17:22 ruling, 21 Oct 2026) --------
        //
        // *"The limiter target drives the slot card's IN trim (the pre-trim before the plugin) on
        // EVERY limiter, ours and third-party, never the plugin's own input control. One road, the
        // same as the OUT match, and immune to mis-mapped input controls."*
        //
        // THREE ROADS BECAME ONE. What stood here tried input_db (ours), then B's named control
        // (a third party), then the slot's pre-trim - and the read and the write had already been
        // caught out of step once, which is two of the three roads disagreeing in a single pair.
        // His MLimiterX hour is the case against the named control: a name matched, written, and
        // confirmed from its own echo, on a plugin whose knob never moved. EchoJay's own trim node
        // needs no map, no name and no display; it exists on every slot of every brand, and it is
        // the same road the OUT match already writes, so both land on the card he is looking at.
        //
        // The label is for the one line a dead slot earns, as before.
        st.label  = [this, useSlot] () -> juce::String
        {
            const int n = chainHost.getNumSlots();
            const int i = useSlot >= 0 ? useSlot : n - 1;
            return (i >= 0 && i < n) ? chainHost.getSlotInfo (i).name : juce::String();
        };
        st.readDb = [this, useSlot] () -> float
        {
            const int n = chainHost.getNumSlots(); if (n <= 0) return 0.0f;
            const int i = useSlot >= 0 ? useSlot : n - 1;
            return (i >= 0 && i < n) ? chainHost.getSlotPreTrimDb (i) : 0.0f;
        };
        st.writeDb = [this, useSlot] (float db)
        {
            const int n = chainHost.getNumSlots(); if (n <= 0) return;
            const int i = useSlot >= 0 ? useSlot : n - 1;
            if (i < 0 || i >= n) return;
            // THE RANGE COVERS THE PUSH (his ruling asks it checked): setSlotPreTrimDb clamps to
            // [-24, +12] (21t-d) and +12 is the push's own ceiling, so the trim carries everything
            // the loop may ask for.
            const float v = juce::jlimit (-12.0f, 12.0f, db);
            chainHost.setSlotPreTrimDb (i, v);
            chainHost.captureSlotStateNow (i);   // the landing has to be in what gets saved
            EchoJay_NSLog (("EJLoudness: the level target writes the SLOT CARD's IN trim on slot "
                            + juce::String (i + 1) + " (\"" + chainHost.getSlotInfo (i).name
                            + "\") to " + juce::String (v, 2) + " dB - EchoJay's own trim ahead of the "
                              "plugin, every brand, one road (his 17:22 ruling). The plugin's own input "
                              "control is never written and its ceiling is left where the build set "
                              "it.").toRawUTF8());
        };
        return st;
    };
    // A Link is a CHANNEL or a BUS. Only a mix bus or master hits a target (Sean, 9 Oct), and a Link is
    // neither, so it volume-matches.
    //
    // 13 Oct 2026, CORRECTING THIS COMMENT: it used to end "unless the chain's own `option` says otherwise -
    // which armFromChain honours". armFromChain would honour it, but the option never arrives: the build
    // command carries `calibration` and not the `leveling` block, and nothing on this side reads one. So a
    // Link ALWAYS volume-matches today. That is the right outcome for every Link under the 9 Oct ruling, so
    // it changes no behaviour - but the comment described a path that does not exist, and until item 11 below
    // nothing called armFromChain on a Link at all, so neither half could be caught by using it.
    loudnessLoop_.aimIsTarget = [] { return false; };
    loudnessLoop_.isPlaying   = [this] { return transportPlaying_.load (std::memory_order_relaxed); };
    loudnessLoop_.logLine     = [] (const juce::String& l) { EchoJay_NSLog (l.toRawUTF8()); };
}

LinkProcessor::~LinkProcessor()
{
    // ---- FIRST OF ALL: NOBODY IS LEFT SHOWING AN EDITOR OF THIS INSTANCE ------------------------
    // The design's teardown table, top line: *"the Link processor is destroyed (track deleted,
    // plugin removed): destructor's FIRST act: drop from the directory + revoke every lease"*. The
    // revoke tells each holder to let go, destroys each editor and only then is this object allowed
    // to come apart - so a V2 that is showing one of these slots cannot be left pointing at it.
    // borrowerGone covers the other direction (this Link's own window, as a borrower) and can only
    // find anything if a lease was left behind, which is a bug and says so.
    {
        const int revoked = echojay::EditorLease::revokeAll (instanceUid_, "the Link is going away");
        const int stray   = echojay::EditorLease::borrowerGone (this);
        echojay::LocalInstances::drop (this);
        chainHost.onSlotsAboutToChange = nullptr;
        if (revoked > 0 || stray > 0)
            EchoJay_NSLog (("EJLease: this Link closed " + juce::String (revoked)
                            + " lent editor(s) as it went away"
                            + (stray > 0 ? ", and returned " + juce::String (stray)
                                           + " lease(s) IT still held - a borrower that did not return one"
                                         : juce::String())).toRawUTF8());
    }
    // The window goes before the editors it was showing could be touched again.
    slotEditorWin_.reset();
    lentEditors_.clear();
    // FIRST, before stopTimer and before any member is torn down: expire the async token so a message already
    // queued against this object returns instead of running. See aliveToken_ in the header for the crash this
    // closes (ring_leak_guard, 4 crashes in 20 runs, updateShmState reached from the message queue).
    aliveToken_.reset();
    stopTimer();
    // The hooks this object owns on its own chainHost, dropped before any member goes: chainHost
    // outlives nothing here, but a callback that reaches a half-destroyed LinkProcessor is the same
    // class of fault aliveToken_ above closes, and it costs two lines to make it impossible.
    chainHost.onSlotSettingsChanged = nullptr;
    chainHost.onChainChanged = nullptr;
    // Audio thread guaranteed stopped before destructor
    releaseRegistrySlot();
    // CLEAN-EXIT HYGIENE (25 Aug 2026): the uid is per-launch, so this
    // instance's uid-keyed files are unreachable the moment it dies —
    // delete them here rather than leaving them for the reaper (crashes
    // still litter; the main's sweep is the backstop). structplan is NOT
    // deleted: a lingering journal is a rollback, and completion already
    // deletes it. NOT in releaseRegistrySlot — that also runs on live
    // re-claims mid-session.
    if (instanceUid_.isNotEmpty() && resolvedDir.isNotEmpty())
    {
        for (const auto& p : { "rack-" + instanceUid_ + ".json",
                               "racklock-" + instanceUid_ + ".json",
                               "lease-" + instanceUid_ + ".json",
                               "ctrl-cmd-" + instanceUid_ + ".json",
                               "ctrl-ack-" + instanceUid_ + ".json",
                               "chain-cmd-" + instanceUid_ + ".json",
                               "chain-ack-" + instanceUid_ + ".json" })
            juce::File(resolvedDir + p).deleteFile();
    }
    // THE SLOT-METER FILE, unmapped AND unlinked here: it is uid-keyed and per-launch like every
    // file above, so leaving it would litter the shared directory with one file per Link that has
    // ever run. The ring_leak_guard family is the reason this is not left to a sweep.
    if (slotMeterMap_ != nullptr)
    {
        LinkShm::closeMapped (slotMeterMap_, echojay::kSlotMeterFileSize, slotMeterFd_,
                              slotMeterPath_, /*unlink*/ true);
        slotMeterMap_ = nullptr; slotMeterFd_ = -1; slotMeterPath_.clear();
    }
    const juce::String ringPath = shmOpenedKey;   // full path, set at openRing
    closeRingNow();
    if (ringPath.isNotEmpty())
        juce::File(ringPath).deleteFile();
    LinkShm::closeRegistry(regMap, regFd);
    regMap = nullptr; regFd = -1;
}

// =============================================================================
//  juce::Timer
// =============================================================================
// ---- SEAN'S #1 (16 Oct 2026): THE MAPS THE V2 FETCHED ON THIS RACK'S BEHALF ---------------------
//
// The request half is wired in the constructor (chainHost.onNeedParamMaps). This is the answer: the
// maps go in through storeParamMaps, which is the SAME door the main host uses - so the re-dial sweep
// that already exists (DialTrigger::mapArrived) does the dialling and nothing here re-implements it.
//
// AN EMPTY ANSWER IS STILL AN ANSWER. If the server has no map for that plugin, failMapFetch turns the
// slot's state from "pending" into a terminal noMap with a reason, instead of leaving it waiting for
// ever - which is what it did before this channel existed.
void LinkProcessor::serviceMapsReady()
{
    if (instanceUid_.isEmpty()) return;
    int err = 0;
    const auto dir = LinkShm::resolveDir (err);
    if (dir.isEmpty()) return;
    const juce::File f (dir + "maps-ready-" + instanceUid_ + ".json");
    if (! f.existsAsFile()) return;
    const auto v = juce::JSON::parse (f.loadFileAsString());
    f.deleteFile();                                  // consumed exactly once
    auto* o = v.getDynamicObject();
    if (o == nullptr) return;
    const auto mapsV = o->getProperty ("maps");
    auto* maps = mapsV.getDynamicObject();
    const int served = maps != nullptr ? maps->getProperties().size() : 0;
    EchoJay_NSLog (("EJParamMaps(Link): the V2 answered with " + juce::String (served)
                    + " map(s) (status " + o->getProperty ("status").toString() + ")").toRawUTF8());
    if (served > 0)
    {
        chainHost.storeParamMaps (mapsV);            // caches AND re-dials the waiting slots
    }
    else
    {
        // Nothing to store: tell the slots so, by their fingerprints, so each reaches a terminal
        // state it can explain rather than sitting in "pending" for the rest of the session.
        juce::StringArray asked;
        asked.addTokens (o->getProperty ("asked").toString(), ",", "");
        chainHost.failMapFetch (asked);
    }
}

void LinkProcessor::timerCallback()
{
    // Heartbeat once per second (timer runs at 30Hz since v0.8.5; only the
    // meter publish uses the full rate)
    // Solo fabric scan at ~4Hz: cheap by construction (shared-memory
    // registry walk; sidecar parses only when a file's mtime moved).
    if (++soloScanDivider_ >= 8) { soloScanDivider_ = 0; soloFabricScan(); }
    calibTickOwnRack();   // 21t-d: while THIS process owns the rack, it owns the calibration loop
    // ---- THE RING: map it, then reconcile what the audio thread already applied ----------------
    // Stage 3. The mapping happens HERE and never on the audio thread; processBlock only ever reads an atomic
    // pointer. A rack with no ring is not a failure - the file channel carries everything, which is the
    // correct fallback - so this is attempted quietly and once.
    ringTick();
    // ---- FOLLOW layer 2: the tapped slot's parameter values, on the tick that already exists ---
    // One branch per tick when no live view is on this Link, which is the normal state. The cost of
    // the feature when it is not on screen is zero, not "small" - the same bar stage A set.
    // CONTROL (C1) BEFORE THE PUBLISH, so a value that lands on this tick is published on the same
    // one: the V2's mirror is the acknowledgement, and an acknowledgement a tick late is a knob
    // that springs back for 33 ms.
    // ---- THE RING, WHEN NO AUDIO IS RUNNING (his 16:35 fault, 26 Oct 2026) --------------------
    //
    // *"changing settings no longer carries over to the plugin on the Link"* - and the log's answer
    // was that no Link was processing audio at all: every one of them published the same level
    // figures for minutes. The ring is drained in processBlock, so with the transport stopped a
    // knob moved in the V2's live view sat in the ring and nothing applied it.
    //
    // So this thread drains it too, under the SAME single-consumer claim the audio thread takes
    // (his condition: an atomic try-claim, not a time check). The clock below is policy only - keep
    // out of the audio thread's way while it is servicing the ring, so a move still lands on the
    // same buffer whenever audio runs.
    drainRingWhileAudioIdle();
    applyStagedSetParams();
    publishShadowValuesTick();
    // ---- PER-SLOT METERS, on the same tick (his 25 Oct ask) -----------------------------------
    // Unlike the values file this is not gated on a tap: the figures belong on every slot card of
    // this rack whenever a V2 is showing it, and the V2 cannot ask for them without engaging - which
    // is the thing his 20 Oct ruling forbids. So the Link publishes whenever it has a rack, and a
    // rack with no slots publishes slotCount=0, which is how a V2 tells "no slots" from "no Link".
    publishSlotMetersTick();
    // SEAN'S #1 (16 Oct 2026): has the V2 answered the maps this rack asked for? On the heartbeat
    // divider below rather than every 30 Hz tick: it is a file stat, and the answer is not urgent to
    // the millisecond - a dial that lands a second after the plugin appeared is still a dial, and a
    // dial that never lands is what this fixes.
    if (++heartbeatDivider_ >= 30)
    {
        heartbeatDivider_ = 0;
        serviceMapsReady();
        // Bump heartbeat so the consumer can detect we're alive vs. crashed.
        // Registered-but-inactive Links heartbeat too — they must stay
        // visible (not reaped as stale) so remote re-activation works.
        if (rackLeaseScope_)
            EchoJay_NSLog("EJCtx(link): addressed by a rack-scope lease (the rack keeps processing in place)");
        if (regSlotIdx >= 0)
        {
            LinkShm::bumpHeartbeat(regMap, regSlotIdx);
            // 21t-j: ...and the snapshot rides the heartbeat, so a row that somehow missed a publish is corrected
            // within a second rather than staying wrong until someone clicks something. Once per second, four
            // stores into shared memory.
            publishRegistrySnapshot ("heartbeat");
            // COMP_PROFILE_SPEC_v1 section 5: THIS TRACK'S OWN LOUD LEVEL, PUBLISHED FOR V2 (2 Oct 2026).
            // Sean's 14:20 session: the build targeted a Link rack on Aitch_4_01 and the body carried
            // track_level loud_rms -12.47 / loud_peak +0.76 - measured on V2's OWN input, which on Stereo Out is
            // the finished mix. The Link's own tap read PEAK -5.0. The server then picks a compressor threshold
            // for the wrong signal, which is worse than picking none.
            // Every ChainHost already measures section 5 on its pre-chain tap, including this one, so the reading
            // exists here - it just had no way to reach V2. A small JSON beside the ctrl/ack files carries it:
            // absent means "no reading", which V2 treats as "send no track_level at all".
            publishTrackLevel();
            // Mirror current heartbeat into diag for the editor to read
            if (regMap)
                diag.heartbeat = LinkShm::loadRelaxed(
                    &LinkShm::regSlots(regMap)[regSlotIdx].heartbeat);
        }
        else
        {
            // THE CLAIM RETRY (26 Aug 2026 regression): updateShmState is
            // EVENT-driven — nothing recalls it after the UidClaimGate's
            // Wait arm returns unclaimed, so a Link probing its own ghost
            // waited FOREVER and no Link registered at all. While
            // unregistered, retry the claim here, once per second — the
            // cadence every Wait/adopt threshold was designed against.
            claimRegistrySlot();
        }
    }

#if ECHOJAY_LINK_STATE_DIAG
    // Item-1 diagnosis: log the Active value once after full initialisation
    if (!loggedInitState_)
    {
        loggedInitState_ = true;
        EchoJay_NSLog(("EJLinkState: post-init linkOn="
                       + juce::String((int)linkOn.load())
                       + " name=\"" + linkName + "\" uid=" + instanceUid_).toRawUTF8());
    }
#endif

    // Phase N: adopt a freshly arrived host track name. updateShmState is
    // the established rename path (re-claim slot, republish displayName);
    // repeated host renames coalesce through the dirty flag, and a restore
    // that seeded the same name is a no-op via appliedHostName_.
    if (hostNameDirty_.exchange(false, std::memory_order_acq_rel))
    {
        auto n = getHostTrackName();
        if (n != appliedHostName_)
        {
            appliedHostName_ = n;
            EchoJay_NSLog(("EJLinkState: host track name \"" + n
                           + "\" (user name \"" + linkName + "\")").toRawUTF8());
            updateShmState();
        }
    }

    // Polls keep their pre-30Hz cadence (~100ms): every 3rd tick. Only the
    // meter publish wants the full rate.
    if (heartbeatDivider_ % 3 == 0)
    {
        pollChainCommand();
        pollEditLease();
        pollRackLock();
        pollControlCommand();
        pollKeyCommand();
        pollSessionProjectName();
        publishRackSidecar();   // Phase R: revision-gated, usually a no-op
    }
    // Passive key detection duty cycle — once per second is plenty for a
    // scheduler whose shortest interval is 30 s.
    if (heartbeatDivider_ % 30 == 0)
        schedulePassiveKeyPass();
    // Structure-plan journal check (phase 2): AFTER the DAW's session
    // restore has settled — the restore runs at instantiation and this is
    // the first quiet 1s tick after it — and at most once per process (the
    // journal's delete-on-completion makes a rerun read absent anyway).
    if (!planJournalChecked_ && heartbeatDivider_ % 30 == 0
        && !resolvedDir.isEmpty() && instanceUid_.isNotEmpty())
    {
        planJournalChecked_ = true;
        if (chainHost.planJournalRestoreIfPresent(resolvedDir, instanceUid_))
            // The restore rebuilt the rack in chainHost — same four-step
            // sync, or a crash-recovered Link shows the pre-crash shape
            // until its next local edit (the identical defect, latent).
            syncModelAfterStructuralChange();
    }
    publishMeterFrame();

    // One-shot arming note for the position stamps (stage 0), off the audio
    // thread. Doubles as the behavioural marker: a Link on a host that
    // supplies playhead positions logs this exactly once per instance.
    if (!stampArmedLogged_ && stampsArmed_.load(std::memory_order_relaxed))
    {
        stampArmedLogged_ = true;
        EchoJay_NSLog("EJLinkRing: position stamps armed");
    }
}

void LinkProcessor::publishRackSidecar()
{
    if (instanceUid_.isEmpty()) return;
    if (resolvedDir.isEmpty())
    {
        int err = 0;
        resolvedDir = LinkShm::resolveDir(err);
        if (resolvedDir.isEmpty()) return;
    }
    // THE CURVE IS POLLED, NOT NOTIFIED, and that is the whole fix for the
    // bug where it drew once and never moved again.
    //
    // The obvious design is to publish when something tells you a parameter
    // changed. It does not work HERE, because the built-in devices have no
    // parameters to change: SurgicalEqProcessor deliberately runs without an
    // APVTS and without juce parameters (its editor calls setBand and
    // applyStructured straight into the engine), and nothing on that path
    // calls updateHostDisplay either. So audioProcessorParameterChanged and
    // audioProcessorChanged NEVER fire for the EQ, the hosted-change epoch
    // never moves, and a publish gated on notifications is silent forever.
    // The one publish that did happen came from chainRevision when the EQ was
    // ADDED, which is exactly the reported symptom.
    //
    // So the authoritative trigger is the DATA. getMagnitudeResponse is 64
    // points times kMaxBands complex multiplies with no FFT and no allocation
    // (analysisScratch_ is a member), which at this poll rate is nothing, and
    // unlike a notification it cannot be silently defeated by a device that
    // forgets to announce itself. The epoch stays as an ADDITIONAL trigger:
    // it is correct for real hosted plugins and costs nothing here.
    const int  rev      = chainHost.getChainRevision();
    const int  epoch    = chainHost.getHostedChangeEpoch();
    const bool uidMoved = (instanceUid_ != lastPublishedUid_);   // 6 Sep 2026: a re-minted identity moves its sidecar with it, or the solo fabric cannot find it
    const bool revMoved = (rev   != lastPublishedRackRev_);
    // ---- A DIAL STARTING OR FINISHING IS A PUBLISH TRIGGER (his 21:32, 24 Oct 2026) -----------
    // Readers wait on `dialPending` and on the confirmation, and a dial write only bumps the
    // hosted-parameter epoch - which republishes through the SETTLE path below, i.e. late. Late is
    // the whole fault: the agent's read-back has a budget of one look unless the slot reports
    // pending, so a pending flag that arrives after the read-back has given up is a figure he is
    // told cannot be read. Its own trigger, and it bypasses the settle with the other urgent ones.
    const auto dialFp      = echojay::dialStateFingerprint (chainHost);
    const bool dialMoved   = (dialFp != lastPublishedDialFp_);
    const bool epMoved  = (epoch != lastPublishedEpoch_);
    // §8 closed loop: a mute-state FLIP must republish promptly — the main
    // confirms the commanded mute through this sidecar, and a stale
    // muteEngaged would false-trip the watchdog into solo.
    const bool muteNow = linkMuteWanted();
    const bool muteMoved = (muteNow != lastPublishedMuteEngaged_)
        || (muteUserOn_.load(std::memory_order_relaxed) != lastPublishedMuteUser_)
        || (soloOn_.load(std::memory_order_relaxed) != lastPublishedSoloOn_);
    const double nowMs  = juce::Time::getMillisecondCounterHiRes();

    // The EQ curve, fetched ONCE for the rack rather than per slot: the
    // accessor already resolves "the first built-in EQ" and there is at most
    // one curve to publish. Absent (no EQ, or the slot could not answer) it
    // stays empty and NO eqMagDb key is written, which is what makes an old
    // Link and a new one indistinguishable to a reader: both simply have no
    // curve, and neither claims a flat one.
    std::vector<int16_t> curve;
    const int eqSlot = chainHost.findFirstBuiltinEqSlot();
    if (eqSlot >= 0)
    {
        std::vector<int16_t> tmp((size_t) LinkShm::kEqCurvePoints, (int16_t) 0);
        if (chainHost.getBuiltinEqCurveDeciDb(tmp.data(), LinkShm::kEqCurvePoints))
            curve = std::move(tmp);
    }
    // Track when the curve last MOVED, so a drag can coalesce. Compared
    // against the last COMPUTED curve, not the last published one: otherwise
    // every tick of a slow drag would look like "still changing" relative to
    // a published value that is deliberately behind.
    if (curve != lastComputedCurve_)
    {
        lastComputedCurve_ = curve;
        lastCurveChangeMs_ = nowMs;
    }
    const bool curveMoved = (curve != lastPublishedCurve_);

    // Pre-gain moved (value, hand-set flag, or input-known state), polled the
    // same way as the curve because it fires no notification. Publishing on
    // an input-known transition lets a stale "--" on the main plugin resolve
    // to the real figure once audio is heard.
    const float pgDb    = chainHost.getPreGainDb();
    const bool  pgUser  = chainHost.isPreGainUserSet();
    const bool  pgKnown = chainHost.getChainInLevels().known;
    const bool preGainMoved = (pgDb    != lastPublishedPreGainDb_)
                           || (pgUser  != lastPublishedPreGainUserSet_)
                           || (pgKnown != lastPublishedPreGainInputKnown_);

    // ---- ITEM 13, THE HALF PHASE A MISSED (18 Oct 2026, 18:37) --------------------------------
    //
    // Sean: *"check gr right after 'Got it, you can stop' failed with 'I have no fresh reading from
    // that track yet'."*  The log says why, and it is not the freshness window: the measurements were
    // 15 s old while playback had ended 7 s earlier, because THIS GATE is revision-gated - the
    // sidecar is written when the RACK changes, and playing a song changes no slots at all. So phase
    // A published live measurements into a file nothing republished while the audio ran.
    //
    // Widening the staleness window to cover that would be the exact fault his 15 Oct ruling names:
    // quoting a window from before the section he just played. So the LINK republishes while it is
    // measuring - bounded to kMeterPublishMs, and only while there is something to measure (audio
    // heard and a slot with a reading), so a quiet rack is as silent on disk as it ever was.
    // ---- AND THE TRIGGER IS THE DATA (18 Oct 2026, 19:19 - the second miss) -------------------
    //
    // My first cut asked the transport ("has this rack heard audio") and said nothing in the log, so
    // when it did not fire there was nothing to read: his measurements were still 10 s old at the
    // moment check gr ran. The comment forty lines above this one says exactly why that was the
    // wrong instrument - "the authoritative trigger is the DATA ... unlike a notification it cannot
    // be silently defeated" - and a meter is data.
    //
    // So: a cheap digest of what the slots are MEASURING. When it moves, the sidecar is republished,
    // bounded to kMeterPublishMs. No transport question, no heardSeconds, and a line in the log the
    // first time and then at most once a second, so the next failure is readable instead of inferred.
    bool metersMoved = false;
    {
        double digest = 0.0;
        for (int i = 0; i < chainHost.getNumSlots(); ++i)
        {
            // ---- AND THE TRIM THIS LINK WRITES ITSELF (19 Oct 2026, his 17:04 item 2) ----------
            //
            // *"The OUT value showed on the Link's own slot card but the main V2 rack view of that
            // Link didn't update. Refresh the V2's view of a Link's slot when the Link writes its
            // OUT trim."*
            //
            // The sidecar schema has carried outGainDb all along (LinkShm RackSlot), and the V2
            // re-reads every Link's sidecar at ~1 Hz - so the only thing missing was a REPUBLISH.
            // The trigger is this digest, and the trims were not in it, which is the item-13 lesson
            // written twenty lines above verbatim: "a digest that does not include what it publishes
            // is a digest that never moves, and the republish it guards never happens."
            //
            // So the level match's own write now moves the digest, and the V2 sees the figure on its
            // next read. The IN trim rides along for the same reason - it is published by the same
            // schema and changed by the same kind of command.
            digest += (double) chainHost.getSlotOutGainDb (i)  * 41.0
                    + (double) chainHost.getSlotPreTrimDb (i)  * 43.0;
            const auto pic = chainHost.slotPicture (i);
            if (pic.grKnown)   digest += pic.grHeldDb * 7.0 + pic.grLoudestDb * 13.0 + (double) pic.grWindowFrames;
            if (pic.hasPeaks()) digest += pic.outTpDb * 3.0 + pic.inTpDb;
            // AND WHAT THE SLOT HEARD (18 Oct 2026): this is what moves on a third-party rack, which
            // is every rack his session has had on the Kick - without it the digest was dead weight.
            if (const auto lv = chainHost.getSlotLevels (i); lv.measured && lv.in.known)
            {
                digest += lv.in.p90 * 17.0 + lv.in.p50 * 5.0 + (double) lv.in.windowSeconds;
                // 19 Oct 2026: AND the hop percentiles, which are the figures a reader now quotes,
                // plus the out leg and the crests. The item-13 lesson, verbatim: a digest that does
                // not include what it publishes is a digest that never moves, and the republish it
                // guards never happens. (The braces matter: `lv` is scoped to the if-statement, and
                // without them this block sat outside it and would not compile.)
                float t = 0.0f, l = 0.0f;
                if (lv.in.hopLevelPercentiles (t, l))
                    digest += (double) l * 23.0 + (double) t * 11.0 + (double) lv.in.hopLevelCount;
                if (lv.out.hopLevelPercentiles (t, l))
                    digest += (double) l * 29.0 + (double) t * 19.0;
                float c = 0.0f;
                if (lv.in.hopCrest (c))  digest += (double) c * 31.0;
                if (lv.out.hopCrest (c)) digest += (double) c * 37.0;
            }
        }
        if (std::abs (digest - lastPublishedMeterDigest_) > 1.0e-6
            && (nowMs - lastMeterPublishMs_) >= kMeterPublishMs)
        {
            metersMoved = true;
            lastPublishedMeterDigest_ = digest;
        }
    }
    if (metersMoved)
    {
        lastMeterPublishMs_ = nowMs;
        if (nowMs - lastMeterLogMs_ >= 1000.0)
        {
            lastMeterLogMs_ = nowMs;
            EchoJay_NSLog ("EJRackSidecar: republishing because the meters moved - this Link's "
                           "measurements go out while it is measuring, not only when the rack "
                           "changes (item 13, 18 Oct)");
        }
    }
    else if (chainHost.getNumSlots() > 0 && nowMs - lastMeterLogMs_ >= 60000.0)
    {
        // ---- AND WHEN THERE IS NOTHING TO PUBLISH, SAY SO (18 Oct 2026) -------------------------
        // He asked three times why this Link sent no measurements, and the honest answer took a
        // code read to find: a third-party plugin's gain reduction is not measured anywhere in this
        // build (slotPicture computes GR for an EchoJay device only), so the digest above is
        // identically zero and cannot move. That is a fact about the RACK, and a rack that cannot be
        // measured should say it in the log rather than leave the silence to be interpreted.
        lastMeterLogMs_ = nowMs;
        int measurable = 0;
        for (int i = 0; i < chainHost.getNumSlots(); ++i)
            if (const auto pic = chainHost.slotPicture (i); pic.grKnown || pic.hasPeaks()) ++measurable;
        if (measurable == 0)
            EchoJay_NSLog (("EJRackSidecar: nothing to publish - none of this rack's "
                            + juce::String (chainHost.getNumSlots())
                            + " slot(s) has a reading of its own (an EchoJay device reports its "
                              "gain reduction; a third-party plugin's is not measured by this "
                              "build, so check(gr) on one answers not_certified)").toRawUTF8());
    }
    // ---- AND THE SOURCE TYPE MOVING IS A REASON TO PUBLISH (B's capture, 20 Oct 2026) ---------
    //
    // *"channelRole as {kind, name} only, without the lead-vocal source type Sean set on Nafe
    // Main_01."*  His log has the set, at 21:08:54:
    //     EJSourceType: HE set this Link to lead_vocal - provenance "user" ... Republished at once
    // and the republish did nothing, because this gate did not know about the field. setSourceType-
    // FromUser called publishRackSidecar and publishRackSidecar returned here.
    //
    // THE SAME LESSON, THE THIRD TIME IN THIS FILE: *"a digest that does not include what it
    // publishes is a digest that never moves, and the republish it guards never happens"* - written
    // for the meters (item 13) and again for the slot trims (his 17:04), and I added a new published
    // field without adding its trigger. So the trigger is here, beside the others, rather than in the
    // setter: every road that changes the field gets the publish, not just the one I remembered.
    // ---- AND THE STAMP IS TAKEN AT THE WRITE, NOT AT THE TEST (B's 21 Oct capture) -----------
    //
    // *"Nafe Main_01 still arrives with no sourceType, although Sean set it to lead vocal yesterday.
    // Check whether a user setting restored from the Link's saved state is published on load (your
    // publish trigger compares against lastPublished; a restored value may never count as a
    // change)."*
    //
    // B is right about the symptom and the cause is one layer deeper than the comparison. The
    // comparison DOES see a restored value as a change - both sides start empty. What loses it is
    // that I recorded "published" HERE, at the test, and a pure source-type change moves none of
    // revMoved / preGainMoved / metersMoved, so it falls through to the SETTLE path below and
    // returns without writing anything. By then the stamp says it went out, so srcMoved is false for
    // ever and the field never travels again. The first publish is lost permanently.
    //
    // So the stamp moves to the one place that knows a write happened - after the sidecar is
    // actually written, at the end of this function. A flag that claims an outcome before the
    // outcome is the same fault as a caption that credits an abandoned match, and it is the second
    // one this week.
    const bool srcMoved = (sourceType_     != lastPublishedSourceType_)
                       || (sourceTypeFrom_ != lastPublishedSourceTypeFrom_);
    if (!revMoved && !epMoved && !curveMoved && !preGainMoved && !muteMoved && !uidMoved && !metersMoved
        && !srcMoved && !dialMoved)
        return;
    // A meter-driven publish is not a gesture to coalesce: its whole point is to be RECENT, and the
    // settle below would hold it back by exactly the interval that made the reading stale.
    if (!revMoved && !preGainMoved && !metersMoved && !dialMoved)
    {
        // A settle test ALONE would starve under sustained automation: a host
        // automating an EQ emits changes every block, so the "last change"
        // stamp never falls behind and a pure settle would wait forever,
        // leaving the curve frozen exactly while it is moving most. So the
        // settle coalesces a gesture and the staleness bound guarantees
        // progress. Whichever comes first.
        const double lastMoveMs = juce::jmax(chainHost.lastHostedChangeMs(),
                                             lastCurveChangeMs_);
        const bool settled = (nowMs - lastMoveMs)         >= kRackSettleMs;
        const bool stale   = (nowMs - lastRackPublishMs_) >= kRackMaxStaleMs;
        if (!settled && !stale) return;
    }
    lastPublishedUid_ = instanceUid_; lastPublishedRackRev_ = rev;
    // WITH THE OTHER STAMPS, and not next to the field it describes: these are all taken in the one
    // place that knows this publish is really going out, which is this function's own rule.
    lastPublishedDialFp_ = dialFp;
    lastPublishedMuteEngaged_ = muteNow;
    lastPublishedMuteUser_ = muteUserOn_.load(std::memory_order_relaxed);
    lastPublishedSoloOn_   = soloOn_.load(std::memory_order_relaxed);
    lastPublishedEpoch_   = epoch;
    lastPublishedCurve_   = curve;
    lastPublishedPreGainDb_        = pgDb;
    lastPublishedPreGainUserSet_   = pgUser;
    lastPublishedPreGainInputKnown_ = pgKnown;
    lastRackPublishMs_    = nowMs;

    LinkShm::RackSidecar rc;
    rc.valid     = true;
    rc.uid       = instanceUid_;
    rc.borrowCapable = true;   // this binary honors the rack-scoped lease
    rc.structureEditCapable = true;   // and can journal/apply a structure plan
    rc.inContextCapable     = true;   // §8: mutes on lease muteOut
    rc.ackPerSeq            = true;   // v9: answers also land in ctrl-ack-<uid>-<seq>.json
    {
        // Round 53 (C4): who we are, so a main counts us only from its own host.
        const auto& h = ChainHost::getHostIdentity();
        rc.publisherPid  = (int) ::getpid();
        rc.hostPid       = h.pid;
        rc.hostStartSec  = h.startSec;
        rc.hostStartUsec = h.startUsec;
    }
    rc.muteEngaged = linkMuteWanted();   // ACTUAL silence, any reason
    rc.muteUser = muteUserOn_.load(std::memory_order_relaxed);
    rc.soloOn   = soloOn_.load(std::memory_order_relaxed);
    rc.muteSoloCapable = true;   // this binary composes and scans
    rc.name      = effectiveDisplayName();
    rc.revision  = rev;
    rc.masterWet = chainHost.getMasterWet();
    // ---- WHAT THIS CHANNEL IS (Sean's round (b), 20 Oct 2026) ------------------------------
    // Guessed from this Link's own display name unless he has set it, and published with its
    // PROVENANCE so a guess can never be mistaken for a decision. Refreshed here rather than once
    // at startup because the name is the host's and arrives late - and because a RENAME should
    // re-guess, which is exactly what mayReplace(Name, Name) allows and what it forbids over a
    // value he set himself.
    refreshSourceTypeGuess();
    rc.sourceType     = sourceType_;
    rc.sourceTypeFrom = sourceTypeFrom_;
    rc.sourceTypeName = sourceTypeName_;
    // Pre-chain gain mirror (18 Aug 2026): the main plugin's mixer shows and
    // drives it in Pre mode. inputKnown so a strip with no level heard shows
    // "unset" rather than a confident 0.
    rc.preGainDb         = pgDb;
    rc.preGainUserSet    = pgUser;
    rc.preGainInputKnown = pgKnown;
    // RULING 1 (21s-b): the slot list is filled by the ONE shared function (EJRackSidecarFill.h), so the Link and
    // a main plugin holding this rack's lease cannot describe the same rack differently. Everything only the Link
    // can know - the lease flags and the EQ curve - is passed in.
    echojay::fillRackSidecarSlots (rc, chainHost, lastLocalRackEditMs_, eqSlot, curve,
                                   [this] (int i)
                                   {
                                       // Stage 1: the leased slot says so, so the main plugin can see its lease
                                       // landed (and see it die: controlled vanishing while it still holds the
                                       // lease is the teardown signal).
                                       return leaseActive_.load (std::memory_order_relaxed)
                                              && (rackLeaseScope_ || i == leaseSlot0_);
                                   });
    // 21t-d: THE LOOP STATE SURVIVES THIS PUBLISH. The Link rewrites its whole sidecar every time, so without
    // this the calibration state V2 wrote during a lease would be erased the moment the lease ended - the loop
    // would not "stop at handover", it would be deleted by it. When this process is running the loop it writes
    // its own state; when it is not, it carries forward whatever is already on disk.
    // ---- 11 Oct 2026: ONE PUBLISHER -----------------------------------------------------------------
    // What stood here carried a lease holder's slots forward whenever our own chain read 0 slots, because
    // 21t-e was true then: "while the lease is held our own chain is PARKED (0 slots) and the lease holder has
    // just republished the real picture". It is not true any more. A build on this rack arrives as a command
    // and lands HERE (see sendChainToLink's note), so this chain is the rack, and 0 slots means 0 slots. The
    // carry-forward would now resurrect a description of a rack the user had just emptied - and it is the
    // sixth bug on the plan's list: the two-publisher sidecar, "the class of bug where the Link and the lease
    // holder describe the same rack differently".
    // 4 Oct 2026 (save fix): what our saved chunk said its revision was, for the V2's tie-break.
    rc.restoredChainRev = restoredChainRev_;
    rc.restoredLeaseBaseRev = restoredLeaseBaseRev_;
    if (calibLoop_.active()) rc.calib = calibLoop_.toVar();
    else
    {
        const auto existing = LinkShm::readRackSidecar(resolvedDir, instanceUid_);
        if (existing.valid && ! existing.calib.isVoid()) rc.calib = existing.calib;
    }
    // 3 Oct 2026 (13:52 item 2): the Link mirrors its own loop's target and last reading onto its own slot, the
    // same way the V2 side does at calibStore. A Link rack's [CURRENT CHAIN] is built from this host.
    if (calibLoop_.slot >= 0)
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        chainHost.setSlotGrState(calibLoop_.slot, calibLoop_.active() ? calibLoop_.lo : nan,
                                 calibLoop_.active() ? calibLoop_.hi : nan, calibLoop_.lastGr);
    }
    // 21t-i: THE STORED RECORD RIDES THE SIDECAR, so V2 can compose a block about this channel while this Link is
    // quiet or parked. A publish that happens before anything has been heard must not blank a record already there.
    if (levelRecord_.valid) rc.levels = levelRecord_.toVar();
    else
    {
        const auto existing = LinkShm::readRackSidecar(resolvedDir, instanceUid_);
        if (existing.valid && ! existing.levels.isVoid()) rc.levels = existing.levels;
    }
    LinkShm::writeRackSidecar(resolvedDir, rc);
    // THE STAMP, now that the write has happened (B's 21 Oct capture). Taken from `rc`, which is
    // what actually went to disk, rather than from the members - so a field the composer decided not
    // to write cannot be recorded as published either.
    lastPublishedSourceType_     = rc.sourceType;
    lastPublishedSourceTypeFrom_ = rc.sourceTypeFrom;
}

// 21t-d (25 Sep 2026): THE LINK DRIVES THE LOOP ONCE IT OWNS THE RACK.
// The handover machinery below is kept: a loop can still be read back from the sidecar at the step count and
// drive a previous owner left it at, with the first window on this host not judged (two hosts, two tally
// histories). What is gone is the previous owner.
//
// ---- 11 Oct 2026, THE REMOVALS: THIS PROCESS ALWAYS MEASURES ITS OWN RACK ------------------------------
//
// What stood here returned early whenever a lease was held, because 21t-d was true then: "while the rack is
// leased the per-slot tallies that matter are V2's borrowHost ones, and V2 advances the loop". The V2 hosted a
// copy that was fed this channel's audio and summed back into the mix, so its tallies were of a real signal.
//
// Stage 1 ended that and this gate became a measurement taken in the wrong place. The V2's copy is still fed
// the ring - which taps POST-RACK - and this rack is no longer bypassed while leased, so what that copy
// measured was this channel processed by this rack and then processed AGAIN by its own. A compressor's
// threshold was being calibrated against doubly-processed audio, and nobody could hear it because stage 1 had
// already stopped anyone listening to that copy. No figure Sean saw was flagged, which is exactly the GR unit
// bug's shape: a number with nothing to contradict it.
//
// So the owner follows the audio. This process hosts the plugins, hears the channel in its own place in the
// graph, and runs the loop. The V2 asks for a calibration by sending the block as a command, which is the
// road startCalibFromBlock already arrives by.
void LinkProcessor::calibTickOwnRack()
{
    if (instanceUid_.isEmpty() || resolvedDir.isEmpty()) return;
    if (! calibLoadedFromSidecar_)
    {
        const auto rc = LinkShm::readRackSidecar(resolvedDir, instanceUid_);
        calibLoop_ = rc.valid ? echojay::CalibLoop::fromVar(rc.calib) : echojay::CalibLoop{};
        // ...AND ITS COMPANIONS, so a lease handover continues ALL of them and not just the first
        // (his 17:5x). Absent on an older Link's sidecar, which reads as none.
        calibExtra_.clear();
        if (rc.valid)
            if (auto* arr = rc.calibExtra.getArray())
                for (const auto& v : *arr)
                {
                    auto c = echojay::CalibLoop::fromVar (v);
                    if (c.active()) calibExtra_.push_back (c);
                }
        calibLoadedFromSidecar_ = true;
        const auto restamp = [this] (echojay::CalibLoop& l)
        {
            if (! l.running()) return;
            EchoJay_NSLog (l.onHandover().toRawUTF8());
            calibLastWindowMs_ = juce::Time::getMillisecondCounterHiRes();
            // 21t-m item 6c: THE HOST THAT TAKES OVER STAMPS ITS OWN REVISION. chainRevision is per-ChainHost,
            // so the number V2 stamped means nothing here; comparing against it would cancel every loop the
            // moment the lease came back, which is the opposite of what case (4) requires.
            l.slotIdent = chainHost.slotIdentityKey (l.slot);   // (n): re-stamped on handover
        };
        restamp (calibLoop_);
        for (auto& c : calibExtra_) restamp (c);
    }
    bool wrote = false;
    tickOneCalib (calibLoop_, wrote);
    // ---- AND EVERY COMPANION, EACH ON ITS OWN SLOT (his 17:5x ruling) ----------------------
    // One hold per compressor, which is the V2's rule since 30 Sep. A companion that has ended is
    // dropped here rather than ticked for ever.
    for (auto& extra : calibExtra_)
        tickOneCalib (extra, wrote);
    calibExtra_.erase (std::remove_if (calibExtra_.begin(), calibExtra_.end(),
                                       [] (const echojay::CalibLoop& c) { return ! c.active(); }),
                       calibExtra_.end());
    // ONE SIDECAR WRITE PER TICK, with the primary and the companions in it. Writing per loop would
    // publish three times for one window and leave readers seeing a half-updated rack.
    if (wrote) publishCalibState();
}

// ---- ONE PUBLISHER FOR THE LOOP STATE (his 17:5x ruling, 21 Oct 2026) ------------------------
//
// Called by the tick after a judged window AND by startCalibMany, which is the half leg CO5 caught:
// a batch that had just started was not on the sidecar until its first window closed, so a lease
// handover in that gap would have found the primary and none of the companions. A start is a state
// change like any other, and the state V2 renders from has to carry it.
void LinkProcessor::publishCalibState()
{
    // THE DIRECTORY IS RESOLVED HERE IF IT HAS NOT BEEN YET, the same way setShadowSlot does: a
    // START can be the first thing that ever needs to publish, and leg CO4 caught exactly that -
    // an early return on an unresolved dir looks identical to "nothing to say".
    if (resolvedDir.isEmpty()) { int e = 0; resolvedDir = LinkShm::resolveDir (e); }
    if (resolvedDir.isEmpty() || instanceUid_.isEmpty()) return;
    auto rc = LinkShm::readRackSidecar (resolvedDir, instanceUid_);
    if (! rc.valid) { rc.uid = instanceUid_; rc.valid = true; if (rc.revision < 0) rc.revision = 0; }
    rc.calib = calibLoop_.toVar();
    // THE COMPANIONS RIDE BESIDE IT, absent when there are none - so an older V2 reads `calib`
    // exactly as it does today and a new one can show all three (additive, like every other
    // sidecar field).
    if (! calibExtra_.empty())
    {
        juce::Array<juce::var> arr;
        for (const auto& c : calibExtra_) arr.add (c.toVar());
        rc.calibExtra = juce::var (arr);
    }
    else rc.calibExtra = juce::var();
    LinkShm::writeRackSidecar (resolvedDir, rc);
}

// ---- ONE LOOP'S TICK, SO A LINK CAN RUN SEVERAL (his 17:5x ruling, 21 Oct 2026) --------------
//
// *"Please build the companion loops so several compressors dialled on one Link each get their own
// loop, as the V2's own rack does. Sean's vocal chains on Links have two or three."*
//
// THE BODY IS UNCHANGED, and that is deliberate: this is the Link's measurement path, so the edit
// is a RENAME AND A PARAMETER and nothing else. Every `loop` became `loop`, every sidecar
// write became the caller's job (one write per tick, not one per loop), and the loading half -
// which is about the Link's own state and not about any one loop - stayed with the caller.
//
// calib_link_guard was run on the same fixture before and after this change, as he asked.
void LinkProcessor::tickOneCalib (echojay::CalibLoop& loop, bool& wroteSidecar)
{
    if (instanceUid_.isEmpty() || resolvedDir.isEmpty()) return;
    if (! loop.active())
    {
        // 5 Oct 2026: ...and a LANDED slot is still measured. This is the path a finished loop falls down every
        // tick on this side, and it did nothing at all - the V2 twin has ticked the watch here since 4 Oct.
        chainHost.tickLowGainWatch();
        return;
    }
    if (loop.slot < 0 || loop.slot >= chainHost.getNumSlots()) return;

    // 21t-m item 6c (29 Sep 2026 ruling): A LOOP THE RACK MOVED UNDER IS CANCELLED HERE TOO. Sean's log is the
    // V2's, but a loop handed to the Link runs on the Link's own chain, and a rebuild there leaves exactly the
    // same stale loop re-posting a line for a plugin that has gone. Same test, same counter, same cancellation.
    // (n) 30 Sep 2026: SLOT IDENTITY, not the rack revision - see PluginProcessor::calibTick for why.
    const auto nowIdent = chainHost.slotIdentityKey (loop.slot);
    if (! ChainHost::slotIdentityStillMatches (loop.slotIdent, nowIdent))
    {
        EchoJay_NSLog (("EJThreshold: CANCELLED - slot " + juce::String (loop.slot + 1)
                        + " no longer holds \"" + loop.plugin + "\" (" + loop.slotIdent
                        + " -> " + nowIdent + "); its pending line is dropped with it").toRawUTF8());
        loop = {};
        wroteSidecar = true;   // the caller publishes the cleared state with the rest
        return;
    }

    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    if (calibLastWindowMs_ <= 0.0) calibLastWindowMs_ = nowMs;
    const double sinceMs = nowMs - calibLastWindowMs_;
    if (sinceMs < 3000.0) return;           // one decision per 3 s window, never per tick
    calibLastWindowMs_ = nowMs;

    const auto lv = chainHost.getSlotLevels(loop.slot);
    echojay::CalibLoop::Window w;
    w.measured = lv.measured && lv.in.known && lv.out.known;
    w.silent   = lv.measured && ! lv.in.known;
    // 21t-j (28 Sep 2026, the general compressor rule): THE TWO SENSORS, from this slot's own taps.
    //   GR   = (in SHORTMAX - in SHORT90) - (out SHORTMAX - out SHORT90): the crest the compressor took off the
    //          loud phrases. Makeup gain shifts both output terms together and cancels out of the difference.
    //   LEVEL= out SHORT90 - in SHORT90 (INT when SHORT90 has not closed a window yet): what the slot is adding.
    // A window missing any of the four is NOT a sample: nothing is derived from a figure that is not there.
    {
        const auto& I = lv.in; const auto& O = lv.out;
        const bool haveCrest = I.maxShortTermDb == I.maxShortTermDb && I.shortTermP90Db == I.shortTermP90Db
                            && O.maxShortTermDb == O.maxShortTermDb && O.shortTermP90Db == O.shortTermP90Db;
        const bool have90 = I.shortTermP90Db == I.shortTermP90Db && O.shortTermP90Db == O.shortTermP90Db;
        // GR IS A LEVEL REDUCTION, NOT A CREST CHANGE (2 Oct 2026 ruling, Sean's 19:43 session).
        //
        // This read the CREST difference - "the crest the compressor took off" - chosen because makeup gain
        // shifts both output terms together and cancels out of it. Makeup-immune, and blind to the case that
        // matters: a compressor with a slow attack on sustained material pulls SHORTMAX and SHORT90 down
        // TOGETHER, so the crest barely moves however hard it is working. Sean's CL 1B read -0.4..+0.4 dB
        // throughout while its gain reduction genuinely rose with the drive, and the loop walked the drive to
        // +6 dB believing it had bought nothing.
        //
        // The level figure is the real one, and the correction is exact because the IN tally is taken AFTER the
        // pre-trim (so the drive is not in it) and levelChangeDb is taken BEFORE EchoJay's slot OUT gain (so the
        // mirror is not in it either). What remains between in and out is the plugin: its gain reduction, less
        // its own makeup. The profile knows that makeup as static_gain_db; with no profile it is unknown and 0,
        // which can UNDERSTATE the reduction on a unit with built-in makeup - so the crest figure rides the log
        // beside it, and a disagreement between the two is visible rather than silent.
        w.grCrestDb = haveCrest ? ((I.maxShortTermDb - I.shortTermP90Db) - (O.maxShortTermDb - O.shortTermP90Db))
                                : 0.0f;
        // ...less the plugin's OWN makeup, which the profile knows (spec section 7 subtracts static_gain_db for
        // exactly this reason). Unknown without a profile, and then 0 - stated, not hidden.
        // WHICH SENSOR, AND WHY IT IS A CHOICE (2 Oct 2026, corrected the same evening).
        //
        // LEVEL is the real gain reduction - but it counts EVERY level change between in and out, including a
        // static one the compressor is not responsible for: a slot that simply attenuates, a unit with negative
        // make-up, staging. level_loop_guard case (9) caught exactly that: a fixture slot 6 dB down read as 6 dB
        // of compression, the loop decided it was over the 2-3 band and backed the drive to -6 dB. The spec
        // subtracts static_gain_db for this reason, and that figure only exists when a profile is attached.
        //
        // CREST is blind to static level entirely, which is why it was chosen - and why it read ~0 on Sean's CL 1B,
        // a slow unit on sustained material whose SHORTMAX and SHORT90 came down together.
        //
        // So: LEVEL when a profile tells us the static offset, CREST when nothing does. Neither is right
        // everywhere, the choice is recorded per window in grSensor, and the figure not used still rides the log -
        // a disagreement between them is visible rather than decided silently.
        // BOTH FIGURES, ALWAYS (3 Oct 2026): the agreement rule needs the level figure even when no profile
        // supplies static_gain_db, so it is computed here regardless and the loop decides what to do with it.
        // 3 Oct 2026 (13:52 ruling): THE OFFSET MAY COME FROM THE BLOCK, not only from a map profile. One
        // accessor answers both, so the two twins and the loop's own agreement rule cannot disagree about whether
        // this slot has a static offset. Sean's session: the map had no comp_profile, the block carried
        // from_profile, and the crest fallback reported about 0 dB on a compressor doing 2-3.
        w.inShortTermDb = I.shortTermDb;
    w.inShortTermP90Db = I.shortTermP90Db;   // item 4, log only
        w.grLevelKnown = have90;
        w.grLevelDb = have90 ? (-(O.shortTermP90Db - I.shortTermP90Db) + loop.staticGainDb()) : 0.0f;
        // ---- 3 Oct 2026 (Kathy, refined): THE UNIT'S LOW-LEVEL GAIN, SAMPLED LIVE. LOG ONLY --------------
        // A window where the compressor is genuinely BELOW THRESHOLD measures its low-level gain as out minus in,
        // which is what static_gain_db claims to be. Two gates, in order of how much they prove:
        //   CONFIRMED    6 dB under the block's in_at_gr1_dbfs - the dialled setting's own 1 dB point, so "below
        //                threshold" is the setting's own statement rather than our inference.
        //   FALLBACK     10 dB under the track's remembered loud level, tagged unconfirmed, until B ships the
        //                field. Same reference: a SLOT tally is Weighting::Plain (offsetDb() == 0), so its
        //                shortTermDb is plain RMS dBFS, as are in_at_gr1_dbfs and TrackLevel's loud_rms_dbfs.
        // Windows near the noise floor and silent ones are ignored: out minus in on near-nothing is noise over
        // noise, and it would sit in the median looking like data.
        //
        // WHY 10 AND NOT 15 (4 Oct 2026, measured). At 15 dB this gathered NOTHING all session - "(waiting, n=0)"
        // in every CL 1B window from 15:06 to 15:09 while the loop read 1.3-2.3 dB of gain reduction on a playing
        // vocal. The figure compared is a THREE-SECOND short-term window, which averages phrase and gap together
        // and so sits within roughly 6-10 dB of the track's p95; 15 dB below it is a near-silent bar, which on this
        // material also falls under the -60 dBFS floor and is excluded. The gate was unreachable, not unlucky.
        //
        // Relaxing it is safe BY CONSTRUCTION, not by hope: the two-band agreement test is the guard. If 10 dB
        // admits windows where the unit is still working, the two input bands disagree and the line reports
        // "not confirmed" with both figures rather than a number. A wrong reading cannot pass itself off as right;
        // the worst case is that we still have no figure, which is where we already are.
        if (I.known && O.known && ! w.silent
            && std::isfinite (I.shortTermDb) && std::isfinite (O.shortTermDb)
            && I.shortTermDb > -60.0f)
        {
            const float gr1 = loop.inAtGr1Dbfs;
            const auto  tl  = chainHost.trackLevelReading();
            const bool  haveGr1 = std::isfinite (gr1);
            const float ceilingDb = haveGr1 ? gr1 - 6.0f
                                            : (tl.valid ? tl.loudRmsDbfs - 10.0f
                                                        : std::numeric_limits<float>::quiet_NaN());
            if (std::isfinite (ceilingDb) && I.shortTermDb <= ceilingDb)
                chainHost.noteLowLevelGainSample (loop.slot, I.shortTermDb,
                                            O.shortTermDb - I.shortTermDb, haveGr1);
        }

        // ...and the line itself, composed from the slot's store. Beside the PROFILE's figure, which is what
        // Kathy wants compared: a measured 0.0 against a profile -0.7 is the finding, either way it falls.
        {
            ChainHost::LowGainReading lg;
            const bool have = chainHost.lowLevelGain (loop.slot, lg);
            loop.lowGainText = echojay::CalibLoop::lowGainLine (have, lg.medianDb, lg.loBandDb, lg.hiBandDb,
                                                             lg.rangeLoDbfs, lg.rangeHiDbfs, lg.samples,
                                                             lg.twoBandAgreed, lg.confirmedGate,
                                                             loop.staticGainKnown()
                                                                 ? loop.staticGainDb()
                                                                 : std::numeric_limits<float>::quiet_NaN());
        }

        const bool haveStatic = loop.staticGainKnown();
        // 6 Oct 2026 (Sean's ruling): ON AN INPUT-DRIVE AMOUNT CONTROL, OUT-MINUS-IN IS NOT GAIN REDUCTION.
        // His UAD 1176LN Rev E at Input -21: the level method read -4.5 and called it "4.5 dB louder out than in",
        // while the VU was pinned and he could hear heavy compression. On a unit whose amount control is INPUT
        // DRIVE, out-minus-in is the input gain MINUS the gain reduction, so the two are inseparable and the figure
        // says nothing about GR. The level method is only trustworthy when a profile states static_gain_db AND the
        // control being moved is not itself an input gain.
        // Order, as ruled: the plugin's own GR meter if it exposes one, then crest, otherwise say plainly that GR
        // cannot be measured - never a level figure dressed up as GR.
        const bool inputDrive  = loop.amountIsInputDrive();
        const bool haveMeter   = loop.grReadable && std::isfinite (loop.sensedGrDb);
        if (inputDrive && haveMeter)
        {
            w.grDb = std::abs (loop.sensedGrDb);
            w.grSensor = "meter";
        }
        else if (inputDrive && ! haveStatic)
        {
            // 6 Oct 2026 (Sean's revision): CREST IS NOT TRUSTED ON ITS OWN HERE. His 1176's own meter showed about
            // 20 dB of gain reduction while crest-inputdrive read 0.6 and the level method read -5.0 - crest is
            // blind to static level by design, and on a heavily driven input-drive unit the programme's crest barely
            // moves, so a near-zero crest figure says nothing. When the two sensors disagree by more than a few dB
            // neither is usable and the honest answer is that GR cannot be measured. A WRONG number is worse than no
            // number: 0.6 against a 20 dB reality is what let the hold write OUT -5.0 and call it level matched.
            const bool bothKnown = haveCrest && have90;
            const float disagreeDb = bothKnown ? std::abs (w.grCrestDb - w.grLevelDb) : 0.0f;
            if (bothKnown && disagreeDb > echojay::CalibLoop::kSensorDisagreeDb)
            {
                w.grDb = std::numeric_limits<float>::quiet_NaN();
                w.grSensor = "unmeasurable-inputdrive";
            }
            else
            {
                w.grDb = haveCrest ? w.grCrestDb : std::numeric_limits<float>::quiet_NaN();
                w.grSensor = haveCrest ? "crest-inputdrive" : "unmeasurable-inputdrive";
            }
        }
        else if (haveStatic && have90)
        {
            w.grDb = w.grLevelDb;
            w.grSensor = "level-static";
        }
        else
        {
            w.grDb = w.grCrestDb;
            w.grSensor = haveCrest ? "crest" : "none";
        }
        w.levelChangeDb = have90 ? (O.shortTermP90Db - I.shortTermP90Db)
                                 : ((I.known && O.known) ? (O.levelDb - I.levelDb) : 0.0f);
        if (! haveCrest) w.measured = false;   // no crest pair, no sample
    }
    // 21t-m item 1: EchoJay's own per-slot output gain, read from the CONTROL. levelChangeDb above is taken
    // before it (21t-k item 3), so the hold's residual is the two added - and a hold that has already corrected
    // the slot can see its own correction instead of re-reading the same untouched excess four times.
    w.slotOutGainDb = chainHost.getSlotOutGainDb (loop.slot);
    w.slotPreTrimDb = chainHost.getSlotPreTrimDb (loop.slot);   // 21t-m item 1: the drive in front of the plugin
    // 6 Oct 2026 (Sean's item 1), LOG ONLY: this slot's OUT loud-phrase level and the CHAIN's pair, so a session log
    // shows where level is lost rather than only that it was. O is the slot's own output tally; the chain pair is the
    // host's own in/out, which exist whether or not a hold is running.
    w.outShortTermP90Db = chainHost.getSlotLevels (loop.slot).out.shortTermP90Db;
    { const auto ci = chainHost.getChainInLevels();  w.chainInP90Db  = ci.shortTermP90Db;
      const auto co = chainHost.getChainOutLevels(); w.chainOutP90Db = co.shortTermP90Db; }
    w.inTruePeakDb = w.measured ? lv.in.truePeakDb : -200.0f;
    w.heardSeconds = lv.in.heardSeconds;   // 21t-i: the question quotes what was actually heard
    // 21t-j (B's note 1): THE PLUGIN'S OWN GR METER, when the block named one. Read as the plugin prints it; a
    // control whose text is not a number publishes no figure, and then the reply carries none.
    if (! loop.senseParams.isEmpty() && loop.grReadable)
    {
        float grFromPlugin = 0.0f;
        if (chainHost.readControlDb (loop.slot, loop.senseParams[0], grFromPlugin))
            w.sensedGrDb = std::abs (grFromPlugin);
    }
    // 21t-j (owed): THE CROSS-CHECK, the same five windows and the same line as V2's side - one header, one
    // behaviour, whichever host owns the rack.
    if (! loop.senseParams.isEmpty())
    {
        int windowNo = 0;
        if (loop.takeSenseLog (windowNo))
        {
            float raw = 0.0f, parsed = 0.0f; juce::String text; bool okParse = false;
            if (chainHost.readControlRaw (loop.slot, loop.senseParams[0], raw, text, okParse, parsed))
                EchoJay_NSLog (echojay::CalibLoop::senseCrossCheckLine (loop.plugin, loop.senseParams[0],
                                                                       raw, text, okParse, parsed, windowNo)
                                   .toRawUTF8());
            else
                EchoJay_NSLog (("EJGrMeter: " + loop.plugin + " \"" + loop.senseParams[0]
                                + "\" is NOT a control on this plugin - the block named a meter that is not "
                                  "there").toRawUTF8());
        }
    }

    // 21t-i: the measure-and-ask line is left in the STATE (askOwed) rather than posted here - this process has no
    // chat. V2 hands it to the chat out of the sidecar, from whichever host measured the window.
    const auto step = loop.onWindow(w, sinceMs);
    if (step.logLine.isNotEmpty()) EchoJay_NSLog(step.logLine.toRawUTF8());
    if (step.writeDrive)
    {
                // 21t-m (29 Sep 2026 ruling): THE DRIVE RAISES IN BY A RUNG AND LOWERS THE LIVE OUT BY THE SAME.
        // newPost used to go to setSlotTrimDb - the COMPARE trim, which is only in circuit during an A/B - so
        // every rung of every loop raised the chain by a dB and nothing took it back. Sean's 21:53-21:54
        // compressor ran pre=+1/+2/+3 with post=-1/-2/-3 and came out 3 dB louder.
chainHost.setSlotPreTrimDb(loop.slot, step.newPre);
        chainHost.setSlotOutGainDb(loop.slot, step.newPost);   // 21t-m: the LIVE out, not the compare trim
        chainHost.resetSlotShortTermStats(loop.slot, "the loop moved the drive");   // 21t-j: both legs start again
    }
    // 21t-g item 2: the threshold actuator, the same write on this side - the loop is one header and a handover
    // must not change which knob is being dialled.
    // 21t-j: BRACED. Appending a second statement to an unbraced `if` made the reset UNCONDITIONAL, so a
    // Link-hosted slot reset both legs on every window and could never close one - the sample would never form.
    if (step.writeParams)
    {
        chainHost.setSlotControlsToValue(loop.slot, step.paramNames, step.paramValue);
        chainHost.resetSlotShortTermStats(loop.slot, "the loop moved the actuator");   // both legs start again
    }
    // 5 Oct 2026 (Sean's item 4): THE LANDED SLOT KEEPS BEING MEASURED ON THIS SIDE TOO. The loop is one header
    // and a handover must not change whether the low-level-gain watch runs. "landed" rather than "finished"
    // because a build hold lands and stays OPEN for section 7's check - see the V2 twin for the full note.
    if (loop.dynamicsSlot && (step.finished || loop.landed))
        chainHost.armLowGainWatch (loop.slot, loop.inAtGr1Dbfs);
    // 21t-m (29 Sep 2026 ruling): the hold's write to a PLUGIN'S output control is gone - it writes EchoJay's
    // own per-slot OUT and nothing else, so there is no step.writeOutput to handle here any more.
    // THE HOLD, through EchoJay's own per-slot output gain - the only control it ever writes.
    if (step.writeSlotGain)
    {
        chainHost.setSlotOutGainDb(loop.slot, step.slotGainValue);
        chainHost.resetSlotShortTermStats(loop.slot, "the hold moved EchoJay's slot output gain");   // 21t-j: both legs start again
        // 21t-m item 1: BOTH figures, and the written total. The old line printed the PLUGIN's change alone, so
        // four passes each reported "6.0 dB louder out than in" while the gain went -6, -12, -18, -24 - the line
        // was true every time and the state it described was wrong.
        EchoJay_NSLog(("EJThreshold: level hold - EchoJay's slot output gain = " + juce::String(step.slotGainValue, 2)
                       + " dB on slot " + juce::String(loop.slot + 1) + " (the plugin is "
                       + juce::String(std::abs(loop.levelChangeDb), 1) + " dB "
                       + (loop.levelChangeDb > 0.0f ? "louder" : "quieter") + " out than in; residual before this "
                       "write " + juce::String(loop.levelResidualDb, 2) + " dB; written total "
                       + juce::String(loop.levelTrimmedDb, 2) + " dB)"
                       // 5 Oct 2026, Sean's budget: the make-up must land inside about 6 s of PLAYING. Audio
                       // seconds since the write, so a stopped transport is not counted against it.
                       + ((loop).secondsSinceWrite() == (loop).secondsSinceWrite()
                              ? " [" + juce::String ((loop).secondsSinceWrite(), 1) + " s of audio since the write]"
                              : juce::String (" [seconds since the write unknown]"))).toRawUTF8());
    }
    // THE STATE V2 RENDERS FROM, written by the host that measured it - every judged window, not only the ones
    // that moved the drive: the card says "working N dB" and that figure changes on windows that change nothing
    // else. Written directly rather than through publishRackSidecar, which is rate-limited on the rack picture
    // and would leave the loop state a window or two behind what this process has already decided.
    wroteSidecar = true;   // the CALLER writes once per tick, with the primary and the companions
}

void LinkProcessor::publishMeterFrame()
{
    // Active only — an inactive Link publishes nothing new, so the main
    // plugin's strip freezes and dims within a second (its seq stops moving)
    if (regMap == nullptr || regSlotIdx < 0 || !linkOn.load(std::memory_order_acquire))
        return;
    // Audio liveness: if processBlock hasn't advanced for ~1s, the host has
    // idled this channel — the engine's values are frozen mid-song. Mark the
    // frame audioStale and blank the momentary group (absent convention);
    // integrated/LRA/PLR keep their last-programme values, mirroring how
    // the Meters tab treats stopped audio.
    const uint32_t blocksNow = audioBlockCounter_.load(std::memory_order_relaxed);
    const uint32_t tNowMs    = juce::Time::getMillisecondCounter();
    if (blocksNow != lastSeenBlockCount_)
    {
        lastSeenBlockCount_  = blocksNow;
        lastBlockAdvanceMs_  = tNowMs;
    }
    const bool audioStale = (tNowMs - lastBlockAdvanceMs_) > 1000;
    if (audioStale != audioWasStale_)
    {
        audioWasStale_ = audioStale;
        EchoJay_NSLog(("EJLinkMeter: \"" + (linkName.isEmpty() ? "(untitled)" : linkName)
                       + (audioStale
                          ? "\" audio stale (blocks frozen at " + juce::String(blocksNow)
                            + ") -> momentary group absent"
                          : "\" audio resumed (blocks " + juce::String(blocksNow) + ")")).toRawUTF8());
    }

    auto md = meterEngine_.getMeterData();
    LinkMeterFrame f;   // global-scope struct, same as RegistrySlot
    f.momentary   = md.momentary;
    f.shortTerm   = md.shortTerm;
    f.integrated  = md.integrated;
    f.rmsL        = md.rmsL;
    f.rmsR        = md.rmsR;
    f.peakL       = md.peakL;
    f.peakR       = md.peakR;
    f.truePeakMax = juce::jmax(md.truePeakMaxL, md.truePeakMaxR);
    f.truePeakCur = juce::jmax(md.truePeakL, md.truePeakR);
    f.lra         = md.loudnessRange;
    f.shortTermTP = md.shortTermTruePeak;
    f.peakFastL   = md.peakFastL;
    f.peakFastR   = md.peakFastR;
    f.fieldsMask  = kFrameHasFastPeak;   // this writer populates the fast pair
    // 21t-d: WHERE these were measured, and the two fields that describe the measurement itself.
    // PRE_TRIM is unconditional for this writer - the tap is above the gain stage now, for every frame it
    // publishes. SHORTMAX and HEARD carry their own bits because a tally that has not closed a 3 s window yet
    // has no max to report, and "0 seconds heard" and "this writer does not publish HEARD" are different facts.
    f.fieldsMask |= kFrameHasPreTrim;
    {
        const auto snap = levelTally_.snapshot();
        // ---- THE LOUDEST 3 s AND THE INTEGRATED FIGURE MUST SHARE A WINDOW (19 Oct 2026) ------
        //
        // Sean, 10:55 on the Kick's Link: *"check(playback)'s loudest 3 s keeps coming out QUIETER
        // than integrated (-17.5 vs -12.2 here, same every run). Loudest short-term can't be below
        // integrated."*  It can't, and the pair was never comparable: `integrated` comes from
        // meterEngine_ (cleared only when the rack changes) and this used to publish the TALLY's
        // max-hold, which resetShortTermMax() clears for every loop window - so after a build the
        // max described the recent quiet seconds while the integrated still covered the loud
        // section. Two instruments, two windows, published as one pair.
        //
        // So the publisher keeps its own max-hold of the LATEST short-term value, on the clock the
        // integrated figure is on: cleared with the meter holds when the rack changes (above), and
        // by nothing else. The tally's own max stays exactly as it is - it belongs to the loop's
        // window and the loop is right to reset it.
        if (std::isfinite (snap.shortTermDb))
            pubShortMaxDb_ = juce::jmax (pubShortMaxDb_, snap.shortTermDb);
        if (pubShortMaxDb_ > -199.0f)
        { f.shortTermMax = pubShortMaxDb_; f.fieldsMask |= kFrameHasShortMax; }
        f.heardSeconds = snap.heardSeconds;
        f.fieldsMask |= kFrameHasHeard;
        // 21t-f item 5: SHORT90, through the codec that owns the encoding (one byte of the key group's pad -
        // the frame has no four bytes left and none could be freed without lying to an older reader). NaN
        // until a window has closed, and frameSetShort90 clears the bit in that case rather than publishing a
        // floor value.
        frameSetShort90 (f, snap.shortTermP90Db);
        // 21t-e (25 Sep 2026): PEAK COMES FROM THE TALLY, ON THE SAME CLOCK AS EVERYTHING ELSE ON THE LINE.
        // It used to be meterEngine_'s max-hold, which is only ever cleared when the RACK changes - so on
        // 25 Sep two members published -0.1 and -0.0 dBTP beside an INT of -17.0, a 17 dB gap that was a single
        // historical transient (a transport click, a load) held since the Link started, and it inflated their
        // PSR to 16.6 and 16.3. The tally's true peak is the max over the SAME heard window as INT, SHORTMAX and
        // HEARD, and it resets with them.
        if (snap.truePeakDb > -190.0f) f.truePeakMax = snap.truePeakDb;
        // 21t-i (27 Sep 2026 ruling): THE STORED RECORD, FROM THIS SAME SNAPSHOT. Fed here so the record and the
        // frame can never disagree about what was heard: they are one reading, published two ways. The frame goes
        // to the strips and decays with the programme; the record is kept.
        levelRecord_.updateFromTally(snap, juce::Time::currentTimeMillis(), true);
        // MOM and SHORT are the last LIVE values, and they are only taken while audio is flowing - the publisher
        // blanks them to -100 when it is not, and a record of a blank is not a record of anything.
        if (! audioStale)
        {
            if (md.momentary > -99.0f) levelRecord_.momDb = md.momentary;
            if (md.shortTerm > -99.0f) levelRecord_.shortDb = md.shortTerm;
            if (md.shortTermTruePeak > -99.0f) levelRecord_.shortTpDb = md.shortTermTruePeak;   // PSR's fallback term
        }
        // INT comes from the meter engine on this side (the frame's own integrated figure), because that is the
        // number the strip shows and the number Sean read off the strips on 27 Sep.
        if (md.integrated > -99.0f)
        { levelRecord_.intLufs = md.integrated; levelRecord_.valid = true;
          levelRecord_.updatedMs = juce::Time::currentTimeMillis(); }
    }
    // 21t-i: THE RECORD TO THE SIDECAR ON A SLOW CADENCE. publishRackSidecar is gated on the rack PICTURE changing,
    // and a channel that is simply being played changes no slots at all - so waiting for it would leave the record
    // only in this process. Ten seconds: often enough that a parked Link's figures are current to the minute,
    // rare enough (0.1 Hz, one small read-modify-write) to cost nothing.
    if (levelRecord_.valid && (double) tNowMs - lastLevelSidecarMs_ > 10000.0)
    {
        lastLevelSidecarMs_ = (double) tNowMs;
        if (resolvedDir.isEmpty()) { int e = 0; resolvedDir = LinkShm::resolveDir(e); }
        if (resolvedDir.isNotEmpty() && instanceUid_.isNotEmpty())
        {
            auto rc = LinkShm::readRackSidecar(resolvedDir, instanceUid_);
            if (! rc.valid) { rc.uid = instanceUid_; rc.valid = true; if (rc.revision < 0) rc.revision = 0; }
            rc.levels = levelRecord_.toVar();
            LinkShm::writeRackSidecar(resolvedDir, rc);
        }
    }
    f.audioBlocks = blocksNow;
    f.audioStale  = audioStale ? 1u : 0u;
    if (audioStale)
    {
        // Momentary group -> absent convention (dashes); rms/peak persist
        // as last-programme values and the receiver dims them
        f.momentary   = -100.0f;
        f.shortTerm   = -100.0f;
        f.shortTermTP = -100.0f;   // gates PSR to '--' like the Meters tab
        // A FAST meter showing a seconds-old peak as current is fake motion:
        // the fast pair blanks with the momentary group (rms/slow peak
        // persist dimmed, the standing convention).
        f.peakFastL   = -100.0f;
        f.peakFastR   = -100.0f;
    }
    f.crest       = md.crestFactor;
    f.correlation = md.correlation;
    f.width       = md.width;
    // Pink-referenced rels: db - mean(valid bands) — same derivation as the
    // chat JSON; macroBandDb itself holds ABSOLUTE per-octave dB
    float mean = 0.0f; int n = 0;
    for (auto db : md.macroBandDb)
        if (db > -119.0f) { mean += db; ++n; }
    if (n > 0)
    {
        mean /= (float) n;
        for (size_t i = 0; i < 6; ++i)
            f.bandRel[i] = md.macroBandDb[i] > -119.0f ? md.macroBandDb[i] - mean : 0.0f;
    }
    // Detected key (KEY_DETECTOR_SPEC.md §9): the passive duty-cycled reading,
    // published in the appended key group. Gated by kFrameHasKey so an old
    // reader ignores it and a new reader never mistakes an old writer's
    // zeroed pad for C major.
    //
    // When this Link's chain HOSTS an EchoJay Key Detector (the Tier 1 path:
    // the main plugin added it and triggered ANALYSE), the device's reading
    // is preferred — it is the explicit, committed mechanism, and preferring
    // it is what carries a Tier 1 result back to the main plugin without any
    // second transport. The passive reading remains the fallback.
    {
        auto kr = keyEngine_.getReading();
        juce::uint32 stamp = keyWorker_.lastChangeMs();
        for (int i = 0; i < chainHost.getNumSlots(); ++i)
            if (auto* kd = dynamic_cast<EedKeyDetectorProcessor*>(chainHost.getSlotProcessor(i)))
            {
                if (const auto dr = kd->engine().getReading(); dr.valid)
                {
                    kr    = dr;
                    stamp = kd->readingChangeMs();
                }
                break;   // one detector speaks for the chain
            }
        if (kr.valid)
        {
            f.keyRoot       = (int16_t) kr.root;
            f.keyIsMinor    = kr.minor ? 1 : 0;
            f.keyConfidence = kr.confidence;
            f.keyTuningHz   = kr.tuningHz;
            f.keyAgeMs = stamp != 0 ? juce::Time::getMillisecondCounter() - stamp : 0;
        }
        f.fieldsMask |= kFrameHasKey;
    }

    // Frozen-engine guard: when the transport stops, the host stops calling
    // processBlock, the engine's values freeze, and stamping them would keep
    // the strip "fresh" scrolling a constant plateau forever — fake motion.
    // Real audio NEVER produces two byte-identical frames (LUFS jitters), so
    // an identical payload means frozen: skip the publish, seq stops
    // advancing, and the receiver's staleness path freezes+dims the strip.
    //
    // keyAgeMs is EXCLUDED from the comparison (equalised before the memcmp):
    // it advances on every tick by construction, and letting it count as
    // "motion" would defeat this guard entirely — every frame would differ,
    // seq would never stop, and the receiver could no longer tell a frozen
    // engine from a live one. A stalled Link's published key age freezes with
    // the rest of the frame; the receiver's seq-staleness already covers it.
    {
        LinkMeterFrame cmp = f;
        cmp.keyAgeMs = lastPublishedFrame_.keyAgeMs;
        if (std::memcmp(reinterpret_cast<const uint8_t*>(&cmp) + sizeof(uint32_t),
                        reinterpret_cast<const uint8_t*>(&lastPublishedFrame_) + sizeof(uint32_t),
                        sizeof(LinkMeterFrame) - sizeof(uint32_t)) == 0)
            return;
    }
    lastPublishedFrame_ = f;
    LinkShm::publishMeterFrame(regMap, regSlotIdx, f);
    // Frame diagnostics: first 3 frames after (re)activation, then 1 per 10s
    ++meterFramesPublished_;
    if (meterFramesPublished_ <= 3 || meterFramesPublished_ % 100 == 0)
        EchoJay_NSLog(("EJLinkMeter: \"" + (linkName.isEmpty() ? "(untitled)" : linkName)
                       + "\" #" + juce::String(meterFramesPublished_)
                       + " mom=" + juce::String(f.momentary, 1)
                       + " rms=" + juce::String(f.rmsL, 1) + "/" + juce::String(f.rmsR, 1)
                       + " blocks=" + juce::String(f.audioBlocks)
                       + " tp=" + juce::String(f.truePeakMax, 1)
                       + " tpCur=" + juce::String(f.truePeakCur, 1)
                       + " corr=" + juce::String(f.correlation, 2)
                       + " rel=[" + juce::String(f.bandRel[0], 1) + " " + juce::String(f.bandRel[5], 1)
                       + "]").toRawUTF8());
}

void LinkProcessor::pollSessionProjectName()
{
    auto shared = ChainHost::getSessionProjectName();
    if (shared != lastSeenSharedProject_)
    {
        // Adopt when we have no name of our own; follow when our name equals
        // the PREVIOUS shared value (we were following the session). A
        // restored distinct name never gets overwritten.
        if (projectName.trim().isEmpty()
            || projectName.trim() == lastSeenSharedProject_.trim())
        {
            projectName = shared;
            updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
        }
        lastSeenSharedProject_ = shared;
    }

    // Session genre — same rules (Link has no genre default, so empty
    // genuinely means "never had one" here, unlike the main plugin)
    auto sharedG = ChainHost::getSessionGenre();
    if (sharedG != lastSeenSharedGenre_)
    {
        if (genre.trim().isEmpty()
            || genre.trim() == lastSeenSharedGenre_.trim())
        {
            genre = sharedG;
            updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
        }
        lastSeenSharedGenre_ = sharedG;
    }
}

// ---------------------------------------------------------------------------
// Remote Active control — ctrl-cmd-<instanceId>.json {v:1, seq, active}
// Authority stays HERE: the command flips linkOn exactly like the local
// toggle (same updateShmState path, same dirty-marking so it persists),
// then the ack confirms the applied state.
// ---------------------------------------------------------------------------
void LinkProcessor::pollRackLock()
{
    // ~100ms cadence, message thread — same shape as pollEditLease: the FILE
    // is the claim, freshness decides, and the transition is what acts.
    if (instanceUid_.isEmpty() || resolvedDir.isEmpty()) return;
    juce::String id, owner;
    double age = 1.0e12;
    LinkShm::readRackLockFile(resolvedDir, instanceUid_, id, owner, age);
    const auto claim = LinkShm::RackLock::read(id, age, {});   // a Link never owns
    const juce::String newOwner =
        claim == LinkShm::RackLock::Claim::Other
            ? (owner.isNotEmpty() ? owner : juce::String("another EchoJay"))
            : juce::String();
    if (newOwner == rackLockOwner_) return;
    EchoJay_NSLog((newOwner.isNotEmpty()
                       ? "EJRackLock: locked by \"" + newOwner + "\""
                       : "EJRackLock: released (was \"" + rackLockOwner_ + "\")")
                      .toRawUTF8());
    rackLockOwner_ = newOwner;
    notifyChainModel();   // the editor re-renders the rack UI locked/unlocked
}

bool LinkProcessor::rackLockGuard(const char* op)
{
    if (rackLockOwner_.isEmpty()) return false;
    // The refusal asserts itself: from outside, a guard that returned early
    // is indistinguishable from one that refused — this line is the witness.
    EchoJay_NSLog(("EJRackLock: refused " + juce::String(op)
                   + " - rack is selected on \"" + rackLockOwner_ + "\"").toRawUTF8());
    return true;
}

void LinkProcessor::pollEditLease()
{
    // ~100ms cadence, message thread. The FILE is the lease (see LinkShm.h):
    // parse what is there, let the pure LeaseGate decide, act on the verdict.
    if (instanceUid_.isEmpty() || resolvedDir.isEmpty()) return;

    juce::String fileId;
    int          fileSlot1 = 0;
    bool         fileScopeRack = false;
    double       ageMs     = 1.0e12;   // absent reads as infinitely stale
    juce::File f(LinkShm::leasePath(resolvedDir, instanceUid_));
    if (f.existsAsFile())
    {
        auto v = juce::JSON::parse(f.loadFileAsString());
        if (auto* o = v.getDynamicObject())
        {
            fileId    = o->getProperty("leaseId").toString();
            fileSlot1 = (int) o->getProperty("slot");
            fileScopeRack = o->getProperty("scope").toString() == "rack";
            // 11 Oct 2026: muteOut IS NO LONGER READ. It rode the lease because the V2 was summing a
            // processed copy of this channel into its own output and the two would have doubled. The key may
            // still be written by an older main plugin; it is ignored, which is the whole point - an old V2
            // can no longer silence a channel by selecting it. editPending still arrives and is still a
            // CONTROL fact: an edit from the holder has not acked, so the lock is held for a reason and the
            // Link's own banner says so.
            rackLeaseEditPending_.store(o->hasProperty("editPending")
                && (bool) o->getProperty("editPending"),
                std::memory_order_relaxed);
            ageMs     = (double) juce::Time::currentTimeMillis()
                          - (double) (juce::int64) o->getProperty("tMs");
        }
    }
    else
    {
        // No file. If we are not engaged either, this is the overwhelmingly
        // common case and the gate call below is a no-op; skip the work.
        if (!leaseActive_.load(std::memory_order_relaxed)) return;
    }

    switch (leaseGate_.poll(fileId, fileSlot1, ageMs))
    {
        case LinkShm::LeaseGate::Engage:
        {
            // THE SCOPE DECISION, through the shared pure helper (step 2):
            // rack scope on a capable binary engages the whole rack; on an
            // old binary the same file (slot 0, scope unparsed) falls into
            // the slot arm and refuses — proven equivalent in racklock_test.
            const auto scoped = LinkShm::LeaseScope::decide(
                fileScopeRack, /*binarySupportsRack*/ true,
                leaseGate_.activeSlot1, chainHost.getNumSlots());
            if (scoped == LinkShm::LeaseScope::Engage::Rack)
            {
                rackLeaseAddress();
                break;
            }
            const int slot0 = leaseGate_.activeSlot1 - 1;
            if (scoped == LinkShm::LeaseScope::Engage::Refuse
                || slot0 < 0 || slot0 >= chainHost.getNumSlots())
            {
                // A lease naming a slot this rack does not have engages
                // NOTHING: force the gate back to idle so a later valid
                // lease can engage, and remember nothing.
                leaseGate_.activeId.clear(); leaseGate_.activeSlot1 = 0;
                break;
            }
            leaseSlot0_ = slot0;
            // 11 Oct 2026: A SLOT-SCOPE LEASE CHANGES NOTHING ABOUT THE SLOT either. Stage 1 had already gated
            // the bypass off and recorded the reason it had existed - "the ring taps post-rack, so without this
            // the editing copy would receive audio ALREADY processed by this instance and process it again in
            // series", which is entirely about an audio detour that is gone. With the write gone, so is
            // leasePriorBypass_: there is nothing to put back.
            leaseActive_.store(true, std::memory_order_relaxed);
            notifyChainModel();   // the open editor dims + disables the slot
            EchoJay_NSLog(("EJLease: engaged slot "
                           + juce::String(slot0 + 1)).toRawUTF8());
            break;
        }
        case LinkShm::LeaseGate::Expire:
        case LinkShm::LeaseGate::Release:
        {
            // ONE restore path for every ending -- clean release, expiry
            // after a crash, a new id superseding a dead session -- and for
            // BOTH scopes: the rack arm restores every slot's saved bypass,
            // the slot arm restores its one, through this same switch case.
            // 11 Oct 2026: ONE ENDING, AND IT RESTORES NOTHING - because nothing was changed. Every ending
            // still comes through this one case, for both scopes, and it still matters: a clean release, an
            // expiry after a crash, and a new id superseding a dead session all have to clear the addressing
            // so the Link's own mutation points stop refusing and the lock overlay comes off.
            //
            // THIS IS ALSO THE MIGRATION ARM plan section 8 asks for. A project saved under the OLD behaviour
            // carries a lease FILE, and the gate's own staleness rule expires it on sight - no code here needs
            // to know it is an old one. What that project's Link must NOT do is restore a bypass from it, and
            // it cannot: the restore is gone.
            const bool wasRack = rackLeaseScope_;
            leaseActive_.store(false, std::memory_order_relaxed);
            rackLeaseEditPending_.store(false, std::memory_order_relaxed);
            rackLeaseScope_ = false;
            leaseSlot0_     = -1;
            if (wasRack)
                EchoJay_NSLog(("EJLease: RACK lease ended id \"" + leaseGate_.activeId
                               + "\" - this rack has been processing in its own place throughout, so there is "
                                 "nothing to restore").toRawUTF8());
            else
                EchoJay_NSLog("EJLease: released/expired - addressing cleared");
            notifyChainModel();
            break;
        }
        case LinkShm::LeaseGate::Hold:
        case LinkShm::LeaseGate::None:
            break;
    }
}

// ---------------------------------------------------------------------------
// Solo fabric scan (27 Aug 2026): the solo set IS the live sidecars. Walk
// the registry (shared memory, free), keep per-slot liveness, and parse a
// foreign sidecar ONLY when its file's mtime moved. A row counts toward the
// set only while proven live AND its heartbeat moved within ~3.5s —
// RegLiveness::proven is sticky, so death needs its own freshness check.
// This is what makes a deleted or crashed soloed Link self-healing: the
// want is re-derived from live evidence every tick and stored nowhere.
// ---------------------------------------------------------------------------
void LinkProcessor::soloFabricScan()
{
    if (regMap == nullptr) return;
    int err = 0;
    const juce::String dir = LinkShm::resolveDir(err);
    if (dir.isEmpty()) { soloMuteWant_.store(false, std::memory_order_relaxed); return; }
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    bool anyForeignSolo = false;
    auto* slots = LinkShm::regSlots(regMap);
    for (int i = 0; i < kRegMaxSlots; ++i)
    {
        auto& row = soloScan_[(size_t) i];
        if (LinkShm::loadAcquire(&slots[i].inUse) == 0) { row = {}; continue; }
        char ub[13] = {};
        std::memcpy(ub, slots[i].instanceUid, 12);
        const juce::String uid = juce::String::fromUTF8(ub);
        if (uid.isEmpty() || uid == instanceUid_) { row = {}; continue; }
        if (row.uid != uid) { row = {}; row.uid = uid; row.lastHbMoveMs = nowMs; }
        const uint32_t hb = LinkShm::loadRelaxed(&slots[i].heartbeat);
        if (hb != row.lastHb) { row.lastHb = hb; row.lastHbMoveMs = nowMs; }
        const bool proven = row.live.observe(hb);
        const bool fresh  = (nowMs - row.lastHbMoveMs) < 3500.0;
        if (! proven || ! fresh) continue;    // never muted by a ghost
        juce::File f(LinkShm::rackSidecarPath(dir, uid));
        const juce::int64 mt = f.existsAsFile()
            ? f.getLastModificationTime().toMilliseconds() : 0;
        if (mt != row.mtimeMs)
        {
            row.mtimeMs = mt;
            const auto rc = LinkShm::readRackSidecar(dir, uid);
            row.soloOn = (rc.uid == uid) && rc.soloOn;
        }
        anyForeignSolo = anyForeignSolo || row.soloOn;
    }
    // In the set = exempt: a soloed Link never solo-mutes itself, and
    // multi-solo means everyone in the set plays.
    soloMuteWant_.store(anyForeignSolo
                            && ! soloOn_.load(std::memory_order_relaxed),
                        std::memory_order_relaxed);
}

// ---- THE PLUGIN'S WINDOW, HERE (18 Oct 2026, Sean's item 3) -------------------------------------
// The editor comes from this Link's own ChainHost - the same createEditorForSlot the main plugin uses
// for its own rack - so what opens is THE running plugin, not a second instance of it. A slot with no
// editor to give (a built-in drawn by us, a plugin whose editor could not be created) says so, and
// the refusal reaches the user as a sentence rather than as a button that did nothing.
bool LinkProcessor::showSlotEditorWindow (int slot1Based, juce::String& why)
{
    if (slot1Based < 1) { why = "no slot was named"; return false; }
    const int idx = slot1Based - 1;
    if (idx >= chainHost.getNumSlots()) { why = "that slot is not on this rack"; return false; }
    // already open for this slot: raise it, which is what a second press should do
    if (slotEditorWin_ != nullptr && slotEditorWin_->slot1 == slot1Based)
    { slotEditorWin_->setVisible (true); slotEditorWin_->toFront (true); return true; }

    // ---- THROUGH THE LEASE (26 Oct 2026, real-editor round 1) ---------------------------------
    // The same door a V2's live view will use from round 2, used here first by the road that already
    // exists. What it buys today: one editor per slot whoever asks (the lease is exclusive and says
    // who holds it in a sentence), and a slot that is removed, replaced or renumbered closes this
    // window on the way out instead of leaving it showing a parked instance.
    slotEditorWin_.reset();          // one at a time, like the main plugin's pop-out
    auto taken = echojay::EditorLease::take (instanceUid_, slot1Based, this, echojay::EditorHolder::OwnWindow);
    // ---- THE HAND-BACK (his condition 1, 26 Oct 2026) ---------------------------------------
    // *"when I open the plugin on the Link's own window while the V2 holds it, the V2 lets go, the
    //  owner destroys the editor in the safe order, and the Link's window shows a fresh one."*
    //
    // The design said either side can ask the other to hand over; this is that ask. The revoke
    // tells the holder to let go (synchronously), then THIS object destroys the editor - the owner,
    // in the one order - and the take below builds a fresh one for this window.
    if (! taken.ok() && echojay::EditorLease::heldBy (instanceUid_, slot1Based, nullptr)
                            == echojay::EditorHolder::Borrowed)
    {
        juce::String who;
        echojay::EditorLease::heldBy (instanceUid_, slot1Based, &who);
        EchoJay_NSLog (("EJLease: slot " + juce::String (slot1Based) + " is open in " + who
                        + " and this Link's own window has been asked for it - taking it back")
                       .toRawUTF8());
        echojay::EditorLease::revokeSlot (instanceUid_, slot1Based,
                                          "the plugin was opened on the Link's own window");
        taken = echojay::EditorLease::take (instanceUid_, slot1Based, this, echojay::EditorHolder::OwnWindow);
    }
    if (! taken.ok())
    {
        // A refusal is a sentence, never a button that did nothing. "Open in EchoJay V2" is the one
        // the user will see once a V2 can hold a lease (round 3 puts those words on the card).
        why = taken.why;
        EchoJay_NSLog (("EJLease: this Link will not open slot " + juce::String (slot1Based)
                        + " in its own window: " + why).toRawUTF8());
        return false;
    }
    const auto name = chainHost.getSlotInfo (idx).name;
    slotEditorWin_ = std::make_unique<SlotEditorWindow> (name.isNotEmpty() ? name : juce::String ("Plugin"),
                                                         taken.editor, slot1Based);
    slotEditorWin_->leaseToken = taken.token;
    // Deleting the window from inside its own close callback is a use-after-free; the reset is posted.
    // THE ORDER IN HERE IS THE DESIGN'S ORDER: the window (the borrower) goes first, then the lease
    // goes back and its owner destroys the editor. Never the other way round.
    slotEditorWin_->onClosed = [this]
    {
        juce::MessageManager::callAsync ([this]
        {
            if (slotEditorWin_ == nullptr) return;
            const int slot1 = slotEditorWin_->slot1;
            const unsigned token = slotEditorWin_->leaseToken;
            slotEditorWin_.reset();
            echojay::EditorLease::giveBack (instanceUid_, slot1, token);
        });
    };
    return true;
}

// ---- echojay::EditorLender ---------------------------------------------------------------------
//
// The instance's own editor, made once and owned HERE. createEditorForSlot is the same call the
// Link's window and the main plugin's own rack have always used; what is new is that the pointer it
// returns now has one owner and one place that deletes it.
juce::AudioProcessorEditor* LinkProcessor::lenderCreateSlotEditor (int slot1Based, juce::String& why)
{
    const int idx = slot1Based - 1;
    if (idx < 0 || idx >= chainHost.getNumSlots()) { why = "that slot is not on this rack"; return nullptr; }
    if (auto it = lentEditors_.find (slot1Based); it != lentEditors_.end() && it->second != nullptr)
        return it->second.get();        // one editor per instance: the same one, never a second
    auto* ed = chainHost.createEditorForSlot (idx);
    if (ed == nullptr)
    {
        why = "that plugin would not open an editor here";
        return nullptr;
    }
    lentEditors_[slot1Based].reset (ed);
    echojay::EditorLease::noteEditorCreated (instanceUid_, slot1Based);
    EchoJay_NSLog (("EJLease: made the real editor for slot " + juce::String (slot1Based) + " of "
                    + lenderDescribe() + " - " + juce::String (echojay::EditorLease::editorsBuiltFor (instanceUid_, slot1Based))
                    + " built for that slot so far").toRawUTF8());
    return ed;
}

void LinkProcessor::lenderDestroySlotEditor (int slot1Based)
{
    auto it = lentEditors_.find (slot1Based);
    if (it == lentEditors_.end()) return;
    lentEditors_.erase (it);            // the editor dies here, and nowhere else
    EchoJay_NSLog (("EJLease: destroyed the real editor for slot " + juce::String (slot1Based) + " of "
                    + lenderDescribe()).toRawUTF8());
}

void* LinkProcessor::lenderGuestViewForSlot (int slot1Based)
{
    // THE WALK RUNS HERE, IN THIS BUNDLE, which is the whole point: dynamic_cast against
    // juce::NSViewComponent works against objects built in this image and fails across the boundary
    // to the V2's (each has its own hidden-visibility type_info - measured 26 Oct, and it put the
    // wrong view into the V2's container). EditorLender::lenderGuestViewForSlot says it in full.
    auto it = lentEditors_.find (slot1Based);
    if (it == lentEditors_.end() || it->second == nullptr) return nullptr;
    struct Find
    {
        static void* in (juce::Component* c)
        {
            if (c == nullptr) return nullptr;
            if (auto* nv = dynamic_cast<juce::NSViewComponent*> (c))
                if (void* v = nv->getView()) return v;
            for (int i = 0; i < c->getNumChildComponents(); ++i)
                if (void* v = in (c->getChildComponent (i))) return v;
            return nullptr;
        }
    };
    return Find::in (it->second.get());
}

void LinkProcessor::lenderRehomeSlotView (int slot1Based)
{
    // ---- THE VIEW COMES HOME, IN ONE MOVE (his ruling of 26 Oct 21:08) ------------------------
    // The borrower is about to let go and the editor is being KEPT, so its view must not be left in
    // the borrower's window. This puts it back inside the editor's own NSViewComponent - the wrapper
    // JUCE made for it - with a single addSubview: AppKit re-parents in place, the view never sees a
    // nil window, and nothing is removed first. That is the whole rule of this road.
    auto it = lentEditors_.find (slot1Based);
    if (it == lentEditors_.end() || it->second == nullptr) return;
    void* guest = lenderGuestViewForSlot (slot1Based);
    if (guest == nullptr)
    {
        EchoJay_NSLog (("EJLease: slot " + juce::String (slot1Based) + " of " + lenderDescribe()
                        + " has no view to take back - its editor draws nothing native").toRawUTF8());
        return;
    }
    const bool moved = NativeClip::parkGuestView (guest);
    EchoJay_NSLog (("EJLease: " + juce::String (moved ? "took the view of slot " : "could NOT take the view of slot ")
                    + juce::String (slot1Based) + " back into this Link's own wrapper (the editor itself is kept, "
                      "one per instance)").toRawUTF8());
}

void LinkProcessor::lenderDestroyAllSlotEditors()
{
    if (lentEditors_.empty()) return;
    const int n = (int) lentEditors_.size();
    lentEditors_.clear();            // every editor this instance made, leased or not
    EchoJay_NSLog (("EJLease: destroyed " + juce::String (n) + " editor(s) of " + lenderDescribe()
                    + " as it goes away - an editor may outlive a lease, never its plugin").toRawUTF8());
}

juce::String LinkProcessor::lenderDescribe() const
{
    auto n = effectiveDisplayName();
    return "EchoJay Link" + (n.isNotEmpty() ? " \"" + n + "\"" : juce::String())
         + " (uid " + instanceUid_ + ")";
}

// ---- echojay::EditorBorrower (this Link's OWN plugin window) -----------------------------------
//
// SYNCHRONOUS, by contract: the editor is destroyed as soon as this returns, so the window goes now
// rather than on a posted message. Deleting a DocumentWindow here is safe because this is never
// reached from inside that window's own callbacks - the close button's road is the posted one above.
void LinkProcessor::editorLeaseRevoked (const juce::String& uid, int slot1Based, const juce::String& why)
{
    if (uid != instanceUid_) return;
    if (slotEditorWin_ == nullptr || slotEditorWin_->slot1 != slot1Based) return;
    slotEditorWin_->editor = nullptr;   // non-owning; its owner deletes it next
    slotEditorWin_.reset();
    EchoJay_NSLog (("EJLease: closed this Link's own window for slot " + juce::String (slot1Based)
                    + " because " + why).toRawUTF8());
}

juce::String LinkProcessor::borrowerDescribe() const
{
    return "the Link's own plugin window";
}

void LinkProcessor::syncLocalDirectory (const char* why)
{
    const bool ok = echojay::LocalInstances::add (instanceUid_, this);
    EchoJay_NSLog (("EJLease: " + juce::String (ok ? "this Link is findable in this process"
                                                   : "this Link could NOT be put in this process' directory")
                    + " (uid " + instanceUid_ + ", " + why + "); the process holds "
                    + echojay::LocalInstances::describe()).toRawUTF8());
}

void LinkProcessor::pollControlCommand()
{
    auto id = chainInstanceId();
    if (id.isEmpty()) return;
    if (resolvedDir.isEmpty())
    {
        int err = 0;
        resolvedDir = LinkShm::resolveDir(err);
        if (resolvedDir.isEmpty()) return;
    }

    juce::File cmdFile(resolvedDir + "ctrl-cmd-" + id + ".json");
    if (!cmdFile.existsAsFile()) return;

    // CONSUME AND ANSWER, ALWAYS (24 Aug 2026 ruling): a command is
    // consumed exactly once and always answered — ver mismatch, duplicate
    // seq, malformed, all of them. Silence is never a valid response; a
    // refused-but-kept file wedged this channel at 10Hz and left the
    // sender's poll blind until its timeout.
    auto refuseCmd = [&](const juce::String& why, int rseq)
    {
        cmdFile.deleteFile();
        auto* r = new juce::DynamicObject();
        r->setProperty("v",       1);
        r->setProperty("seq",     rseq);
        r->setProperty("refused", why);
        juce::File(resolvedDir + "ctrl-ack-" + id + ".json")
            .replaceWithText(juce::JSON::toString(juce::var(r), true));
        EchoJay_NSLog(("EJCtrl: link REFUSED ctrl-cmd (" + why + ") seq="
            + juce::String(rseq)
            + " lastApplied=" + juce::String(lastAppliedCtrlSeq_)
            + " - consumed and answered").toRawUTF8());
    };

    auto v = juce::JSON::parse(cmdFile.loadFileAsString());
    auto* obj = v.getDynamicObject();
    if (obj == nullptr) { refuseCmd("UNPARSEABLE ctrl-cmd", 0); return; }

    int ver = (int)obj->getProperty("v");
    int seq = (int)obj->getProperty("seq");
    if (ver != 1)
    { refuseCmd("version " + juce::String(ver) + " unsupported", seq); return; }
    if (seq == 0)
    { refuseCmd("seq 0 invalid", seq); return; }
    if (seq == lastAppliedCtrlSeq_)
    { refuseCmd("duplicate seq, already applied", seq); return; }

    lastAppliedCtrlSeq_ = seq;
    cmdFile.deleteFile();   // consumed

    // Active is applied ONLY when present. The main plugin always includes
    // the current active in gain/match commands, so this is normally a
    // no-op there; guarding on presence means a hand-built gain-only command
    // can never spuriously deactivate the Link. (v stays 1: the fields are
    // additive; a pre-gain Link ignores gainDb and reads the echoed active.)
    // ---- "BRING YOUR OWN WINDOW FORWARD" (16 Oct 2026, Sean's step 6) -------------------------
    //
    // The main plugin's mirror cannot create a second editor for a third party that lives over here
    // (a licence, a single-instance UI, or simply the process boundary), so it now says where the
    // plugin IS - "Renaissance Axx is running on 88_7even Kick_2_01" - and offers to bring that
    // window forward. This is the asking half.
    //
    // WHAT IT CAN AND CANNOT DO, because the button must not promise more: nothing in either process
    // can make Logic OPEN a plugin window. If this Link's editor already exists, its top-level
    // component can be raised, and that is what happens. If it does not, the honest answer goes back
    // in the ack (`fronted:false`) and the main plugin tells him Logic owns that window. A button
    // that silently does nothing is the thing this avoids.
    // ---- SHOW THE PLUGIN'S OWN WINDOW (18 Oct 2026, Sean's item 3) -----------------------------
    //
    // Additive, and it travels WITH bringEditorToFront in the same command: a Link that predates this
    // field ignores it and answers `fronted` exactly as before, and this one answers both. The main
    // plugin reads `shown` first and falls back to `fronted`, so neither side has to know the other's
    // age - the downgrade rule, in the one place the two processes meet.
    // ---- THE SHADOW'S REQUEST ROAD (stage B of the muted shadow, his 14:19 go) ----------------
    //
    // *"B: the V2 hosts a copy of the slot in view, fed from your tap ring, output discarded, so its
    // own meters and GR move like the real one."*  The plan's own words for this road: *"Which slot
    // rides the existing onEditorRequest-shaped road: a V2 -> Link request naming the slot (or -1
    // for "stop"). The Link writes nothing while that is -1, so the cost of the feature when it is
    // not on screen is zero - not "small", zero."*
    //
    // ONE FIELD, and it is a SLOT, not a toggle: a toggle would need the two sides to agree on which
    // slot it referred to, which is a second fact to keep in step. `shadowSlot` is 1-based like every
    // other slot on this wire, and 0 or less means stop. The ack says what is streaming, so the V2
    // never has to assume its request landed - the same rule showSlotEditor follows below.
    if (obj->hasProperty ("shadowSlot"))
    {
        const int slot1 = (int) obj->getProperty ("shadowSlot");
        setShadowSlot (slot1 >= 1 ? slot1 - 1 : -1);
        auto* tap = chainHost.shadowTap().get();
        const int now = tap != nullptr ? tap->slot() : -1;
        auto* r = new juce::DynamicObject();
        r->setProperty ("v", 1);
        r->setProperty ("seq", seq);
        r->setProperty ("shadowSlot", now + 1);                      // 0 = nothing is streaming
        r->setProperty ("streaming", now >= 0 && tap != nullptr && tap->active());
        r->setProperty ("sampleRate", (double) getSampleRate());
        if (now >= 0) r->setProperty ("name", chainHost.getSlotInfo (now).name);
        // ---- THE REASON, AND THE COUNT (23 Oct 2026) ------------------------------------------
        // `streaming:false` with no reason sends the V2 guessing, and `streaming:true` with no count
        // cannot tell "running and silent" from "running and nothing is reaching it" - the two
        // states B9 could not distinguish for twenty-five seconds. Both travel with the ack now, so
        // the V2 can say which it is and a log line can be read instead of inferred. ShadowTap's own
        // header promised this count to the V2 and nothing had ever carried it.
        if (shadowWhyNot_.isNotEmpty()) r->setProperty ("why", shadowWhyNot_);
        if (tap != nullptr) r->setProperty ("frames", (double) tap->framesProduced());
        // ---- AND THE SLOT'S OWN SETTINGS, WHEN THE VIEW ASKS FOR THEM (FOLLOW layer 1, 24 Oct) --
        //
        // *"the V2's copy mirrors the real plugin ... Today the copy keeps its starting settings, so
        // its view and meters drift from the real one."*  The copy was created from a description
        // and seeded with nothing, so the picture was of a plugin set up as it left the factory.
        //
        // IT RIDES THIS SAME COMMAND, not a second one. The tap request and the settings are one
        // question - "show me that slot" - and one answer, which is this handler's own rule ("one
        // answer per command"). A second command would be a second seq to keep in step and a window
        // in which the view is up on the wrong settings.
        //
        // NO LEASE, NO BORROW, NOTHING TAKEN: this is getStateInformation on the slot's plugin and
        // nothing else, which is the same read pullSlotState has done for the edit session for
        // weeks - with that road's OWN cap, not a second one. A plugin that throws, or one whose
        // chunk is over the cap, is REFUSED WITH ITS REASON, because a view showing the wrong
        // settings silently is the fault this layer exists to close.
        if (obj->hasProperty ("shadowState") && (bool) obj->getProperty ("shadowState"))
        {
            juce::String stateWhy;
            if (now < 0)
                stateWhy = "nothing is streaming on this Link";
            else if (auto* proc = chainHost.getSlotProcessor (now))
            {
                juce::MemoryBlock mb;
                bool threw = false;
                const auto t0 = juce::Time::getMillisecondCounterHiRes();
                try { proc->getStateInformation (mb); } catch (...) { threw = true; }
                const auto tookMs = juce::Time::getMillisecondCounterHiRes() - t0;
                if (threw)
                    stateWhy = "this plugin refused to hand over its settings";
                else if ((int) mb.getSize() > LinkShm::kLinkTransferMaxSlotBytes)
                    stateWhy = "its settings are "
                             + juce::File::descriptionOfSizeInBytes ((juce::int64) mb.getSize())
                             + ", over the "
                             + juce::File::descriptionOfSizeInBytes ((juce::int64) LinkShm::kLinkTransferMaxSlotBytes)
                             + " limit, too large to carry";
                else if (mb.getSize() == 0)
                    stateWhy = "this plugin keeps no settings to send";
                else
                {
                    r->setProperty ("state", LinkShm::stateToB64 (mb));
                    r->setProperty ("stateBytes", (double) mb.getSize());
                }
                // THE TIMING IS LOGGED EVERY TIME, because FOLLOW layer 3 will want to do this once
                // a second and nobody has ever measured what it costs on a real plugin. A number in
                // the log beats an assumption in a design note.
                EchoJay_NSLog (("EJShadow: pulled slot " + juce::String (now + 1) + "'s settings for "
                                "the live view: " + juce::String ((juce::int64) mb.getSize())
                                + " bytes in " + juce::String (tookMs, 1) + " ms"
                                + (stateWhy.isNotEmpty() ? " - REFUSED: " + stateWhy
                                                         : juce::String())).toRawUTF8());
            }
            else
                stateWhy = "slot " + juce::String (now + 1) + " has no plugin";
            if (stateWhy.isNotEmpty()) r->setProperty ("stateWhy", stateWhy);
        }
        juce::File (resolvedDir + "ctrl-ack-" + id + ".json")
            .replaceWithText (juce::JSON::toString (juce::var (r), true));
        EchoJay_NSLog (("EJCtrl: shadowSlot " + juce::String (slot1) + " seq=" + juce::String (seq)
                        + " -> " + (now >= 0 ? "streaming slot " + juce::String (now + 1)
                                             : juce::String ("nothing is streaming"))).toRawUTF8());
        return;                      // one answer per command; the ack is written
    }

    if (obj->hasProperty ("showSlotEditor"))
    {
        const int slot1 = (int) obj->getProperty ("showSlotEditor");
        juce::String why;
        const bool shown = showSlotEditorWindow (slot1, why);
        EchoJay_NSLog (("EJCtrl: showSlotEditor slot " + juce::String (slot1) + " seq="
                        + juce::String (seq) + " -> "
                        + (shown ? juce::String ("opened the plugin's window here")
                                 : "not shown: " + why)).toRawUTF8());
        auto* r = new juce::DynamicObject();
        r->setProperty ("v", 1);
        r->setProperty ("seq", seq);
        r->setProperty ("shown", shown);
        if (! shown && why.isNotEmpty()) r->setProperty ("why", why);
        juce::File (resolvedDir + "ctrl-ack-" + id + ".json")
            .replaceWithText (juce::JSON::toString (juce::var (r), true));
        return;                      // one answer per command; the ack is written
    }

    if (obj->hasProperty("bringEditorToFront"))
    {
        bool fronted = false;
        if (auto* ed = getActiveEditor())
            if (auto* top = ed->getTopLevelComponent())
            { top->toFront (true); fronted = true; }
        EchoJay_NSLog (("EJCtrl: bringEditorToFront seq=" + juce::String (seq) + " -> "
                        + (fronted ? "raised this Link's editor window"
                                   : "NO editor exists here - Logic owns that window, and nothing in "
                                     "this process can open it") + " (Sean's step 6)").toRawUTF8());
        auto* r = new juce::DynamicObject();
        r->setProperty ("v", 1);
        r->setProperty ("seq", seq);
        r->setProperty ("fronted", fronted);
        juce::File (resolvedDir + "ctrl-ack-" + id + ".json")
            .replaceWithText (juce::JSON::toString (juce::var (r), true));
    }

    if (obj->hasProperty("active"))
    {
        bool wantActive = (bool)obj->getProperty("active");
        if (wantActive != linkOn.load())
            EchoJay_NSLog(("EJLinkState: remote set active=" + juce::String((int)wantActive)
                           + " (seq " + juce::String(seq) + ")").toRawUTF8());
        linkOn.store(wantActive);
        updateShmState();                   // registry flag + ring + dirty-mark
        if (onLinkStateChanged) onLinkStateChanged();   // open editor toggle sync
    }

    // Absolute gain set (from the monitor steppers OR the AI level-match
    // action, which the main plugin resolves to an absolute dB before
    // sending). Authority stays here: setGainDb clamps, mirrors to the slot,
    // dirty-marks, and syncs an open editor's slider.
    if (obj->hasProperty("gainDb"))
    {
        float g = (float)(double)obj->getProperty("gainDb");
        EchoJay_NSLog(("EJLinkState: remote set gain=" + juce::String(g, 2)
                       + " dB (seq " + juce::String(seq) + ")").toRawUTF8());
        setGainDb(g);   // snapSmoothing=false → glide (no zipper, no pop)
    }

    // Remote placement declaration (from the monitor row's placement control)
    if (obj->hasProperty("placement"))
        setPlacement((int)obj->getProperty("placement"));

    // 21t-i (27 Sep 2026 ruling): AN EXPLICIT USER RESET OF THIS CHANNEL'S LEVELS. The only thing that clears the
    // stored record. Additive field, like every other one here: an older Link never sees it, and a V2 that does not
    // send it changes nothing. The record AND the tally behind it go, or the record would be rebuilt from the same
    // history within a second and the reset would look like it had not worked.
    if (obj->hasProperty("resetLevels") && (bool)obj->getProperty("resetLevels"))
    {
        EchoJay_NSLog(("EJLevelRecord: remote reset (seq " + juce::String(seq) + ")").toRawUTF8());
        resetLevelRecord();
        // The sidecar's copy goes with it, in the same breath: V2 adopts a record from the sidecar for any uid it
        // has none for, so leaving it there would put the cleared figures straight back.
        if (resolvedDir.isEmpty()) { int e2 = 0; resolvedDir = LinkShm::resolveDir(e2); }
        if (resolvedDir.isNotEmpty() && instanceUid_.isNotEmpty())
        {
            auto rc = LinkShm::readRackSidecar(resolvedDir, instanceUid_);
            if (rc.valid && ! rc.levels.isVoid()) { rc.levels = juce::var(); LinkShm::writeRackSidecar(resolvedDir, rc); }
        }
        lastLevelSidecarMs_ = 0.0;
    }

    if (obj->hasProperty("alias"))   // 21n item 2: display-only alias from V2; "" clears
    {
        const auto a = obj->getProperty("alias").toString().trim();
        EchoJay_NSLog(("EJLinkState: remote alias \"" + a + "\" (seq " + juce::String(seq) + ") - display only, identity \""
                       + effectiveDisplayName() + "\" unchanged").toRawUTF8());
        displayAlias = a;
        if (onLinkStateChanged) onLinkStateChanged();
    }

    // Remote pre-chain gain (18 Aug 2026, from the mixer's Pre mode). A fader
    // move is a HAND set: userSet true so the next model build will not
    // overwrite it. Additive field; an old Link never sees it. updateShmState
    // republishes the sidecar (with the new value) and dirty-marks for save.
    if (obj->hasProperty("preGainDb"))
    {
        const float g = (float)(double)obj->getProperty("preGainDb");
        const bool userSet = ! obj->hasProperty("preGainUserSet")
                             || (bool)obj->getProperty("preGainUserSet");
        EchoJay_NSLog(("EJLinkState: remote set pre-gain=" + juce::String(g, 2)
                       + " dB userSet=" + juce::String((int)userSet)
                       + " (seq " + juce::String(seq) + ")").toRawUTF8());
        chainHost.setPreGainDb(g, userSet);
        updateShmState();
    }
    // Per-slot wet from the main's mix knob on a HELD rack (COMMIT 3, 17 Sep
    // 2026). idx is the ChainHost slot index — the index space the sidecar
    // publishes — and pluginId is that slot's identity (ChainHost::
    // slotIdentityHex, the same function on both ends), so a knob turned
    // against a rack that has since changed shape can never land on the
    // wrong plugin: any mismatch is REJECTED and logged, never remapped.
    // A pure value write into the object that processes audio: no addNode,
    // no removeNode, no prepare, no suspend (tools/slotwet_nomutation pins
    // it). The model copy is mirrored because it is the SESSION-SAVE source
    // (chainModelToVar). NOT rackLockGuard'ed — the sender IS the rack's lock
    // holder; NOT stampLocalRackEdit'ed — a remote edit, like preGainDb.
    // F5 (21s-b, 24 Sep 2026): the rack's OWN chain mix, set from V2 while it views this rack. A pure value write
    // into the object that processes audio - no graph mutation - and it is this Link's state from then on, so
    // releasing the rack leaves the value where it was put.
    if (obj->hasProperty("chainWet"))
    {
        const float w = juce::jlimit(0.0f, 1.0f, (float)(double) obj->getProperty("chainWet"));
        chainHost.setMasterWet(w);
        EchoJay_NSLog(("EJCtrl: chainWet applied " + juce::String(w, 3)
                       + " (seq " + juce::String(seq) + ")").toRawUTF8());
        updateShmState();      // dirty-mark for save; the sidecar republishes on the revision bump
        notifyChainModel();
    }
    if (auto* sw = obj->getProperty("slotWet").getDynamicObject())
    {
        const int          idx  = (int) sw->getProperty("idx");
        const juce::String want = sw->getProperty("pluginId").toString();
        const float        wet  = juce::jlimit(0.0f, 1.0f,
                                               (float)(double) sw->getProperty("wet"));
        const juce::String have = chainHost.slotIdentityHex(idx);   // "" out of range
        // A PARKED RACK IS NOT A REJECTION (2 Oct 2026, Sean's 14:20 finding 3) - and as of 11 Oct 2026 there
        // are no parked racks, so the finding is history rather than a live case. It was real: V2 owned the
        // instances while it held the borrow, our chain sat at 0 slots BY DESIGN, and a knob drag arrived here
        // once per tick with nothing to match it against. Sean's log carried 28 "REJECTED ... slots=0" lines in
        // three seconds, which reads like a broken rack and was not. This rack is never empty on account of a
        // lease now, so a wet command that cannot be matched is a genuine mismatch and says so with both
        // identities - which is the line the old gate was suppressing.
        // The note below stands for the general rule it states, which has not changed: a line per drag tick is how a real
        // defect hides. The verb keeps being written by V2 on purpose (slotwet_knobpath pins that).
        // 11 Oct 2026: THE PARKED ARM IS GONE. It existed because "while V2 holds the borrow our own chain is
        // parked at 0 slots BY DESIGN ... V2 has it on the borrowed host and the deselect plan carries it
        // back", so a wet command against a rack we did not hold was a fact rather than a fault, logged once
        // per lease instead of once per drag tick. This rack is never parked now - a build arrives as a
        // command and lands here - so a wet command that cannot be matched is a genuine mismatch again, and
        // it says so with the identity it wanted and the one it found.
        if (idx < 0 || idx >= chainHost.getNumSlots() || want.isEmpty() || want != have)
        {
            EchoJay_NSLog(("EJCtrl: slotWet REJECTED idx=" + juce::String(idx)
                           + " want=" + want + " have=" + have
                           + " slots=" + juce::String(chainHost.getNumSlots())
                           + " (seq " + juce::String(seq) + ")").toRawUTF8());
        }
        else
        {
            chainHost.setSlotWet(idx, wet, ChainHost::WetSource::User);
            for (auto& s : chainModel)
                if (s.hostIdx == idx) s.wet = wet;   // model copy = serialisation source
            EchoJay_NSLog(("EJCtrl: slotWet applied idx=" + juce::String(idx)
                           + " id=" + want + " wet=" + juce::String(wet, 3)
                           + " (seq " + juce::String(seq) + ")").toRawUTF8());
            updateShmState();      // dirty-mark for save; the sidecar republishes on the revision bump
            notifyChainModel();    // an open Link editor's knob follows
        }
    }
    // Mute/solo layer (27 Aug 2026), additive fields on the same cmd
    // transport. muteUser is a MIX decision: dirty-marks via
    // updateShmState so the host saves it. soloOn is monitoring state:
    // applied and published (the fabric reads the sidecar), never saved.
    if (obj->hasProperty("muteUser"))
    {
        const bool on = (bool) obj->getProperty("muteUser");
        EchoJay_NSLog(("EJLinkState: remote set muteUser="
                       + juce::String((int) on)
                       + " (seq " + juce::String(seq) + ")").toRawUTF8());
        muteUserOn_.store(on, std::memory_order_relaxed);
        updateShmState();
        if (onLinkStateChanged) onLinkStateChanged();
    }
    if (obj->hasProperty("soloOn"))
    {
        const bool on = (bool) obj->getProperty("soloOn");
        EchoJay_NSLog(("EJLinkState: remote set soloOn="
                       + juce::String((int) on)
                       + " (seq " + juce::String(seq) + ")").toRawUTF8());
        soloOn_.store(on, std::memory_order_relaxed);
        // My own membership changes my want NOW, not at the next scan:
        // soloing must never briefly mute the soloed strip itself.
        if (on) soloMuteWant_.store(false, std::memory_order_relaxed);
        updateShmState();
    }
    // Reset the pre-gain to auto (clears the hand-set flag; the next build
    // sets it again). Its own field: setPreGainDb userSet=false cannot clear.
    if (obj->hasProperty("preGainReset") && (bool)obj->getProperty("preGainReset"))
    {
        EchoJay_NSLog(("EJLinkState: remote reset pre-gain to auto (seq "
                       + juce::String(seq) + ")").toRawUTF8());
        chainHost.resetPreGainToAuto();
        updateShmState();
    }

    // ---- Remote editor open (stage 1) -------------------------------------
    // 1-BASED like every other slot reference on the wire (slot, to, after);
    // the conversion to a 0-based rack index happens here, once.
    //
    // The instance opened is THIS Link's own, in the signal path, so the user
    // hears their edits immediately. Nothing is transferred and the audio is
    // untouched: this moves a window to the front, nothing more.
    // ---- Slot state pull (stage 1 remote editing) --------------------------
    // The pull IS the size measurement: over the cap or a throwing plugin
    // fails HERE, in the ack, before the main plugin engages any lease,
    // bypass or editing instance. Nothing to unwind on refusal.
    bool pullAttempted = false;
    juce::String pulledB64, pullErr;
    if (obj->hasProperty("pullSlotState"))
    {
        pullAttempted = true;
        const int slot0 = (int) obj->getProperty("pullSlotState") - 1;
        auto* proc = chainHost.getSlotProcessor(slot0);
        if (proc == nullptr)
            pullErr = "slot " + juce::String(slot0 + 1) + " has no plugin";
        else
        {
            juce::MemoryBlock mb;
            bool threw = false;
            try { proc->getStateInformation(mb); } catch (...) { threw = true; }
            if (threw)
                pullErr = "this plugin refused to hand over its settings";
            else if ((int) mb.getSize() > LinkShm::kLinkTransferMaxSlotBytes)
                pullErr = "this plugin's settings are "
                        + juce::File::descriptionOfSizeInBytes((juce::int64) mb.getSize())
                        + ", over the "
                        + juce::File::descriptionOfSizeInBytes((juce::int64) LinkShm::kLinkTransferMaxSlotBytes)
                        + " limit, too large to carry";
            else
                pulledB64 = LinkShm::stateToB64(mb);
            // PHASE 1 BYTE ACCOUNTING (instrumentation, correlation id = seq).
            // Points 1 and 2 of four: raw size out of getStateInformation,
            // and bytes handed to the transport after encoding. The transport
            // is ONE ctrl-ack JSON file, NOT chunked, no framing beyond JSON
            // string quoting; its only cap is the raw-size gate above
            // (LinkShm::kLinkTransferMaxSlotBytes, enforced BEFORE encoding).
            EchoJay_NSLog(("EJPull[" + juce::String(seq) + "] link: raw="
                           + juce::String((juce::int64) mb.getSize())
                           + " bytes from getStateInformation; encoded="
                           + juce::String(pulledB64.length())
                           + " b64 chars; cap="
                           + juce::String(LinkShm::kLinkTransferMaxSlotBytes)
                           + " raw bytes; transport=single ctrl-ack JSON, unchunked"
                           + (pullErr.isNotEmpty() ? "; REFUSED: " + pullErr
                                                   : juce::String())).toRawUTF8());
        }
    }

    // ---- Structure plan (phase 2): verify, journal, two-phase apply --------
    bool planAttempted = false;
    ChainHost::PlanResult planResult;
    if (obj->hasProperty("structPlan"))
    {
        // RECEIPT, logged BEFORE the parse and the apply — so a run's log
        // says which side dropped a plan: no "send" line = main never sent;
        // send but no "received" = transport or this dispatcher; received
        // but no "applied" = the apply hung or died.
        EchoJay_NSLog(("EJPlan[" + juce::String(seq)
                       + "] link: plan received").toRawUTF8());
        planAttempted = true;
        LinkShm::StructureEdit::Plan plan;
        LinkShm::StructureEdit::PreImages ignored;
        if (LinkShm::StructureEdit::planFromVar(obj->getProperty("structPlan"),
                                                plan, ignored))
            // Apply + the four-step sync, one author — notify alone told
            // the editor to re-read a model the plan never wrote (the
            // two-models defect; functionally gated in linksync_test).
            planResult = applyStructurePlanAndSync(resolvedDir, plan);
        else
            planResult.reasons.add("the plan could not be read");
        EchoJay_NSLog(("EJPlan[" + juce::String(seq) + "] link: applied="
                       + juce::String(planResult.ok ? "Y" : "N")
                       + " restored=" + juce::String(planResult.restored ? "Y" : "N")
                       + (planResult.failedAt.isNotEmpty()
                              ? " failedAt=" + planResult.failedAt : juce::String()))
                          .toRawUTF8());
    }

    // ---- Commit (stage 1): the edited state comes home ---------------------
    // Guarded by baseSlots exactly like structural edits: the rack the main
    // plugin was looking at must still be THIS rack, or the state could land
    // on the wrong plugin. Stale = refused with a reason, never applied.
    bool commitAttempted = false, commitOk = false;
    juce::String commitErr;
    if (obj->hasProperty("commitSlot"))
    {
        commitAttempted = true;
        const int slot0 = (int) obj->getProperty("commitSlot") - 1;
        juce::StringArray base;
        if (auto* bs = obj->getProperty("baseSlots").getArray())
            for (auto& bv : *bs) base.add(bv.toString().trim());
        const auto infos = chainHost.getAllSlotInfos();
        bool stale = ((int) infos.size() != base.size());
        if (!stale)
            for (int i = 0; i < (int) infos.size(); ++i)
                if (infos[(size_t) i].name.trim() != base[i]) { stale = true; break; }
        if (stale)
            commitErr = "the rack changed while you were editing - nothing was applied";
        else if (auto* proc = chainHost.getSlotProcessor(slot0))
        {
            juce::MemoryBlock mb;
            const juce::String cIn = obj->getProperty("commitState").toString();
            const bool cOk = LinkShm::stateFromB64(cIn, mb);
            if (!cOk || mb.getSize() == 0)
                EchoJay_NSLog(("EJCommit[" + juce::String(seq)
                    + "] link: decode FAILED (Base64::convertFromBase64, in="
                    + juce::String(cIn.length()) + " b64 chars, out="
                    + juce::String((juce::int64) mb.getSize())
                    + " bytes)").toRawUTF8());
            if (cOk && mb.getSize() > 0)
            {
                try
                {
                    proc->setStateInformation(mb.getData(), (int) mb.getSize());
                    commitOk = true;
                }
                catch (...) { commitErr = "this plugin refused the settings"; }
            }
            else commitErr = "settings did not survive the trip (decode failed)";
        }
        else commitErr = "slot " + juce::String(slot0 + 1) + " has no plugin";
    }

    bool openAttempted = false, openSucceeded = false;
    if (obj->hasProperty("openSlot"))
    {
        openAttempted = true;
        const int slot0 = (int)obj->getProperty("openSlot") - 1;
        // A null callback means this Link has no editor open, and only a
        // window can raise a window. That is a boundary, not a failure, and
        // the ack says so rather than the main plugin guessing from silence.
        if (onOpenSlotEditor && slot0 >= 0)
            openSucceeded = onOpenSlotEditor(slot0);
    }

    auto* ack = new juce::DynamicObject();
    ack->setProperty("v",         1);
    ack->setProperty("seq",       seq);
    ack->setProperty("active",    linkOn.load());
    ack->setProperty("gainDb",    (double)gainDb_.load(std::memory_order_relaxed));
    ack->setProperty("placement", placement_.load(std::memory_order_relaxed));
    ack->setProperty("alias",     displayAlias);   // 21n item 2: echoed so V2 can read back what the Link shows
    // CROSS-VERSION: this key is what tells an "opened" apart from a Link too
    // old to know the field at all. An older build ignores openSlot and still
    // writes a perfectly normal ack, so its ABSENCE is the version signal.
    // Written only when the command actually asked, so ordinary Active and
    // gain acks stay byte-identical to what old readers expect.
    if (openAttempted)
        ack->setProperty("openedSlot", openSucceeded);
    // Same absence-is-the-version-signal contract as openedSlot: these keys
    // exist only when the command asked, so an old Link's ack simply lacks
    // them and the main plugin reports "too old" instead of claiming success.
    if (pullAttempted)
    {
        ack->setProperty("pulledState", pullErr.isEmpty());
        if (pullErr.isEmpty()) ack->setProperty("slotState", pulledB64);
        else                   ack->setProperty("slotStateErr", pullErr);
    }
    if (commitAttempted)
    {
        ack->setProperty("committedSlot", commitOk);
        if (!commitOk) ack->setProperty("commitErr", commitErr);
    }
    if (planAttempted)
    {
        ack->setProperty("planApplied",  planResult.ok);
        ack->setProperty("planRestored", planResult.restored);
        if (planResult.failedAt.isNotEmpty())
            ack->setProperty("planFailedAt", planResult.failedAt);
        juce::Array<juce::var> rs;
        for (const auto& why : planResult.reasons) rs.add(why);
        ack->setProperty("planReasons", rs);
    }
    {
        juce::File af(resolvedDir + "ctrl-ack-" + id + ".json");
        const juce::String ackText = juce::JSON::toString(juce::var(ack), true);
        af.replaceWithText(ackText);
        // v9: PER-REQUEST identity on the channel. The legacy single ack file
        // stays for older mains; a v9-aware main polls ctrl-ack-<id>-<seq>.json,
        // which no other reader deletes or overwrites (9 Sep 2026: two read
        // loops sharing one ack file threw the Link's answers away). The
        // previous per-seq file this Link wrote is removed, so an abandoned
        // request leaves at most one stale file behind per Link.
        {
            juce::File perSeq(resolvedDir + "ctrl-ack-" + id + "-" + juce::String(seq) + ".json");
            if (lastPerSeqAckFile_.isNotEmpty() && lastPerSeqAckFile_ != perSeq.getFullPathName())
                juce::File(lastPerSeqAckFile_).deleteFile();
            perSeq.replaceWithText(ackText);
            lastPerSeqAckFile_ = perSeq.getFullPathName();
        }
        if (pullAttempted)
            EchoJay_NSLog(("EJPull[" + juce::String(seq) + "] link: ack file "
                           + juce::String((juce::int64) af.getSize())
                           + " bytes on disk").toRawUTF8());
    }
}

// ---------------------------------------------------------------------------
// Passive key detection (KEY_PRECONDITION_SPEC.md §5.1) — the duty cycle.
// Message thread, ~1 Hz. Arms a committed pass when one is due and the gates
// pass; the actual analysis runs on the key worker thread.
// ---------------------------------------------------------------------------
void LinkProcessor::schedulePassiveKeyPass()
{
    const uint32_t now     = juce::Time::getMillisecondCounter();
    const bool     playing = transportPlaying_.load(std::memory_order_relaxed);

    // Invalidation events (§5.4) make the next pass due immediately —
    // computed every tick so a jump during a pass is not lost.
    bool invalidated = false;
    if (keySectionJump_.exchange(false, std::memory_order_relaxed))
        invalidated = true;
    if (playing && ! keyWasPlaying_
        && lastPlayingMs_ != 0 && now - lastPlayingMs_ > kKeyLongGapMs)
        invalidated = true;                        // came back after a long stop
    if (playing) lastPlayingMs_ = now;
    keyWasPlaying_ = playing;
    if (invalidated) lastKeyPassArmMs_ = 0;        // due now (still gated below)

    if (keyEngine_.isCollecting())
    {
        // A pass whose transport stopped under it would eventually fill its
        // window with a silent tail (or a different section) — cancel after
        // a few seconds stopped and let the cycle re-arm on the next play.
        // The engine keeps the previous reading; nothing is lost.
        if (! playing)
        {
            if (++keyStallTicks_ >= 5)
            {
                keyStallTicks_ = 0;
                keyEngine_.cancelAnalysis();
                EchoJay_NSLog("EJLinkKey: pass cancelled (transport stopped)");
            }
        }
        else keyStallTicks_ = 0;
        return;
    }
    keyStallTicks_ = 0;

    // The gates: Active (the tap is only fed while Active), rolling, signal.
    if (! linkOn.load(std::memory_order_acquire)) return;
    if (! playing) return;
    if (meterEngine_.getMeterData().momentary < kKeySignalFloorLufs) return;

    const bool due = lastKeyPassArmMs_ == 0
                  || now - lastKeyPassArmMs_ >= kKeyPassIntervalMs;
    if (! due) return;

    lastKeyPassArmMs_ = now;
    keyEngine_.startAnalysis();
    keyWorker_.notify();
    EchoJay_NSLog(("EJLinkKey: passive pass armed (" + juce::String(kKeyPassWindowS, 0)
                   + " s window)").toRawUTF8());
}

// ---------------------------------------------------------------------------
// Remote RE-ANALYSE — key-cmd-<instanceId>.json {v:1, seq}. The Meters tab's
// RE-ANALYSE button, when its key source is this Link's passive reading.
// Same transport conventions as ctrl-cmd: seq-gated, deleted on consume,
// acked. The pass is armed unconditionally (the engine's own waiting-for-
// signal state stays honest if nothing is playing yet).
// ---------------------------------------------------------------------------
void LinkProcessor::pollKeyCommand()
{
    auto id = chainInstanceId();
    if (id.isEmpty()) return;
    if (resolvedDir.isEmpty())
    {
        int err = 0;
        resolvedDir = LinkShm::resolveDir(err);
        if (resolvedDir.isEmpty()) return;
    }

    juce::File cmdFile(resolvedDir + "key-cmd-" + id + ".json");
    if (!cmdFile.existsAsFile()) return;

    auto v = juce::JSON::parse(cmdFile.loadFileAsString());
    auto* obj = v.getDynamicObject();
    if (obj == nullptr) { cmdFile.deleteFile(); return; }

    int ver = (int)obj->getProperty("v");
    int seq = (int)obj->getProperty("seq");
    if (ver != 1 || seq == lastAppliedKeySeq_ || seq == 0)
        return;

    lastAppliedKeySeq_ = seq;
    cmdFile.deleteFile();   // consumed

    lastKeyPassArmMs_ = juce::Time::getMillisecondCounter();
    keyEngine_.startAnalysis();
    keyWorker_.notify();
    EchoJay_NSLog(("EJLinkKey: remote RE-ANALYSE (seq " + juce::String(seq)
                   + ")").toRawUTF8());

    auto* ack = new juce::DynamicObject();
    ack->setProperty("v",      1);
    ack->setProperty("seq",    seq);
    ack->setProperty("status", "armed");
    juce::File(resolvedDir + "key-ack-" + id + ".json")
        .replaceWithText(juce::JSON::toString(juce::var(ack), true));
}

// =============================================================================
//  Registry helpers (message thread)
// =============================================================================
void LinkProcessor::ensureRegistryOpen()
{
    if (regMap) return;

    // Resolve directory once — persist for ring opens
    if (resolvedDir.isEmpty())
    {
        int dirErr = 0;
        resolvedDir = LinkShm::resolveDir(dirErr);
        if (resolvedDir.isEmpty())
        {
            diag.regKey    = "(no writable dir)";
            diag.regOpened = false;
            diag.regErrno  = dirErr;
            return;
        }
    }

    diag.regKey = resolvedDir;
    int err = 0;
    regMap = LinkShm::openRegistry(resolvedDir, regFd, err);
    diag.regOpened = (regMap != nullptr);
    diag.regErrno  = err;
    if (regMap == nullptr && err == EPROTO)
        EchoJay_NSLog(("EJLinkState: " + lastRegistryLayoutError()).toRawUTF8());
}

juce::String LinkProcessor::effectiveFilePart() const
{
    // UID-KEYED (6 Sep 2026, shoot-day defect): the ring file used to be
    // named from the user-typed linkName, so two Links carrying the same
    // typed name (a duplicated track or a pasted insert restores the same
    // name into both) opened the SAME ring file - openRingProducer zeroes the
    // header on open, so the second instance wiped the first's ring and both
    // rows in the main plugin read one file. Rows were already uid-keyed and
    // distinct (the uid claim gate re-mints a duplicate); only the FILE was
    // keyed on the name. instanceUid_ is unique per instance, saved and
    // restored with the state, re-minted for a proven-live duplicate before
    // this is consulted (claimRegistrySlot), and never changes on a host or
    // user rename - so the ring never renames either. The name stays purely
    // cosmetic: displayName in the registry slot.
    return instanceUid_;
}

void LinkProcessor::updateTrackProperties(const TrackProperties& props)
{
    // ANY thread (AU property listener fires off the message thread), any
    // number of times, fields optional (AU/AAX send name only; a VST3
    // colour-only update carries no name). No shm or host calls from here —
    // stash + flag, timerCallback applies on the message thread.
    if (!props.name.has_value())
        return;   // colour-only update: colour is deferred (Phase N scope)
    const juce::String n = juce::String(*props.name).trim();
    {
        const juce::ScopedLock sl(hostNameLock_);
        hostNameFromHost_ = true;   // AUTHORITATIVE from here on, even if equal to a seeded value
        if (hostTrackName_ == n) return;
        hostTrackName_ = n;
    }
    hostNameDirty_.store(true, std::memory_order_release);
    // PROVENANCE LOG (6 Sep 2026): the only line that proves the HOST delivered
    // a name; the timer's "host track name" line also fires for a seeded one.
    EchoJay_NSLog(("EJLinkState: host DELIVERED track name \"" + n + "\" (uid " + instanceUid_ + ")").toRawUTF8());
}

juce::String LinkProcessor::getHostTrackName() const
{
    const juce::ScopedLock sl(hostNameLock_);
    return hostTrackName_;
}

juce::String LinkProcessor::effectiveDisplayName() const
{
    auto user = linkName.trim();
    if (user.isNotEmpty()) return user;   // user-typed always wins
    return getHostTrackName().trim();     // "" -> main plugin shows Untitled N
}

void LinkProcessor::claimRegistrySlot()
{
    ensureRegistryOpen();
    if (!regMap || regSlotIdx >= 0) return;

    // Duplication guard, HEARTBEAT-DECIDED (25 Aug 2026 ruling): another
    // slot carrying our saved uid is one of three things, and inUse alone
    // cannot tell them apart — the old inUse-only re-mint burned an
    // identity against every GHOST (unclean kill -> frozen slot), which was
    // the uid-churn engine behind the orphaned files and journals. Now:
    // proven-live holder -> WE re-mint (real duplicate, first-alive keeps
    // the uid); observed-frozen holder -> a ghost, reap it and ADOPT our
    // own uid back; undecided -> WAIT unclaimed (the ~1s updateShmState
    // tick retries) — an unproven holder is never adopted.
    {
        RegistrySlot* slots = LinkShm::regSlots(regMap);   // global-scope struct
        int holder = -1;
        uint32_t holderHb = 0;
        for (int i = 0; i < kRegMaxSlots; ++i)
            if (LinkShm::loadAcquire(&slots[i].inUse) != 0
                && instanceUid_ == juce::String::fromUTF8(slots[i].instanceUid))
            { holder = i; holderHb = LinkShm::loadRelaxed(&slots[i].heartbeat); break; }
        if (holder >= 0)
        {
            if (uidGateHolder_ != holder) { uidGate_ = {}; uidGateHolder_ = holder; }
            // C4b (6 Sep 2026): the holder's sidecar names the process that
            // published it (round 53). A dead publisher is adopted at once;
            // an unpublished or unreadable sidecar fails CLOSED (treated as
            // alive, so the time floor applies).
            bool publisherAlive = true;
            {
                const auto rc = LinkShm::readRackSidecar(resolvedDir, instanceUid_);
                if (rc.valid && rc.uid == instanceUid_ && rc.publisherPid > 0)
                    publisherAlive = (::kill((pid_t) rc.publisherPid, 0) == 0);
            }
            switch (uidGate_.observe(holderHb, publisherAlive, juce::Time::currentTimeMillis()))
            {
                case LinkShm::UidClaimGate::Decision::Wait:
                    return;   // undecided: stay unregistered, retry next tick
                case LinkShm::UidClaimGate::Decision::Remint:
                {
                    auto old = instanceUid_;
                    instanceUid_ = juce::String::toHexString(
                        juce::Random::getSystemRandom().nextInt64()).removeCharacters("-").substring(0, 10);
                    // The directory is keyed on the uid, so a re-mint moves this instance's row with
                    // it - otherwise a V2 asking for the new uid would find nothing and a V2 asking
                    // for the old one would find a Link that no longer answers to it.
                    syncLocalDirectory ("the uid was re-minted after a registry collision");
                    // THE CHUNK WAS NOT OURS (6 Sep 2026 ruling, C1 + C2). A
                    // PROVEN-LIVE holder means the state this instance restored
                    // belongs to ANOTHER LIVE instance: Pro Tools seeds a fresh
                    // insert with the plugin's last chunk (observed 16:59:50,
                    // "setState ... post-init uid=<the first Link's> ... host
                    // track name 'bass 2'"). No field in that chunk that answers
                    // "which Link is this" is ours: the uid (re-minted above),
                    // the host track name (C1) and the typed name (C2). Cleared
                    // here, BEFORE claimSlot reads effectiveDisplayName(), so
                    // the row publishes empty and the main plugin numbers it
                    // "Untitled N" (unique by construction: getLinkDisplayList
                    // numbers untitled rows in uid order) until the host's own
                    // TrackNameChanged or the user's typing names this track.
                    // The AdoptGhost arm and a plain restore keep the seeded
                    // names: those are the cases the seeding was built for (a
                    // replacement incarnation on the same track; a session
                    // reopen in a host that never re-sends the name).
                    // the seeded names are dropped by the invariant below (uid changed)
                    EchoJay_NSLog(("EJLinkState: uid " + old + " held by a "
                        "PROVEN-LIVE instance (duplicate) -> regenerated "
                        + instanceUid_).toRawUTF8());
                    break;
                }
                case LinkShm::UidClaimGate::Decision::AdoptGhost:
                    LinkShm::reapSlot(regMap, holder);
                    EchoJay_NSLog(("EJLinkState: uid " + instanceUid_
                        + " held by a FROZEN ghost slot " + juce::String(holder)
                        + (publisherAlive ? " (publisher alive, floor elapsed)" : " (publisher pid DEAD)")
                        + " -> ghost reaped, uid adopted").toRawUTF8());
                    break;
            }
            uidGateHolder_ = -1;
        }
        else
        {
            uidGateHolder_ = -1;
            // NO HOLDER: THE UID IS KEPT. (28 Sep 2026 ruling, superseding 6 Sep's L5.)
            //
            // The retired rule re-minted whenever the chunk had been authored by THIS host process run and no
            // slot held the uid, on the reasoning that such a chunk must be a seed from a sibling that had gone.
            // It cannot tell that case from a REOPEN: Pro Tools re-instantiates every insert from its chunk when
            // a session is opened in a process that already had one open, and ejlog_lm2.txt shows the result -
            // 44 of 47 regenerations on that arm, every typed name dropped with them, and a "Main vocals (7)"
            // level match at 21:08 addressing seven uids that had been re-minted at 21:05:24.
            //
            // A uid that NOBODY HOLDS is free, and taking it back is what makes a Link the same Link across a
            // reopen - which is what every group, every stored level record and every chat target is keyed on.
            // The only case that still mints is a PROVEN-LIVE holder, handled by the gate above: that is a real
            // duplicate, and two instances cannot share one identity.
            if (chunkUid_.isNotEmpty() && instanceUid_ == chunkUid_)
                EchoJay_NSLog(("EJLinkState: uid " + chunkUid_ + " is free (no slot holds it) - KEPT, so this "
                    "Link is the same Link it was before the reopen").toRawUTF8());
        }
    }
    // THE NAMES TRAVEL WITH THE CHUNK (28 Sep 2026 ruling, superseding the 6 Sep invariant).
    //
    // The retired rule dropped the seeded typed name and host name whenever this instance ended up with a uid
    // other than the chunk's, on the reasoning that no field of a foreign chunk answers "which Link is this".
    // With the re-mint arm above reduced to the PROVEN-LIVE duplicate, the only chunk that still arrives under a
    // new uid is a COPY - and a copy keeps the name it was copied with, exactly as every other setting in the
    // chunk does. The user renames it if they want two different names; the product does not silently blank a
    // track's name to tell them apart. Sean's log shows what the old rule cost: "seeded names dropped (typed
    // \"Main vocal 2\")" on every Link of a reopened session.
    if (chunkUid_.isNotEmpty() && instanceUid_ != chunkUid_)
        EchoJay_NSLog(("EJLinkState: chunk uid " + chunkUid_ + " != this instance " + instanceUid_
            + " (a proven-live duplicate): the names TRAVEL - typed \"" + linkName + "\", host \""
            + getHostTrackName() + "\"").toRawUTF8());
    // From here on this instance IS its identity: no chunk is pending. Both
    // fields are cleared, because updateShmState releases and re-claims the
    // slot on every publish (a rename, a host-name arrival) and a re-claim
    // that still saw "authored here, uid == chunk uid" would re-mint AGAIN -
    // seen as three re-mints in a row in the first run of L5.
    chunkUid_.clear();
    const juce::String audioFilename = "audio_" + effectiveFilePart() + ".bin";

    regSlotIdx = LinkShm::claimSlot(regMap,
                                     effectiveDisplayName(),
                                     audioFilename,
                                     instanceUid_,
                                     (float)hostSampleRate,
                                     (uint32_t)hostNumChannels);
    if (regSlotIdx < 0)
    {
        // FULL (6 Sep 2026 ruling): reclaim rows whose publisher process is
        // gone, retry once, and if still full SAY SO - to the log and to the
        // user (diag.regFull, shown by the Link editor). Never silent.
        const int reaped = LinkShm::reapDeadPublisherSlots(regMap, resolvedDir);
        if (reaped > 0)
            regSlotIdx = LinkShm::claimSlot(regMap, effectiveDisplayName(), audioFilename, instanceUid_,
                                            (float)hostSampleRate, (uint32_t)hostNumChannels);
        EchoJay_NSLog(("EJLinkState: registry had no free slot; reaped " + juce::String(reaped)
            + " dead-publisher row(s); " + (regSlotIdx >= 0 ? "claimed slot " + juce::String(regSlotIdx)
            : juce::String("STILL FULL - ") + juce::String(kRegMaxSlots) + " slots all held by live publishers; this Link is NOT registered")).toRawUTF8());
    }
    diag.regFull = (regSlotIdx < 0);
    diag.slotIdx = regSlotIdx;
    // 21t-j (28 Sep 2026 ruling): REGISTRATION IS A FULL SNAPSHOT. claimSlot writes the uid, the name, the sample
    // rate and the channel count; the role and the trim were left at their claim-time defaults and reached V2 only
    // if some later event happened to publish them. A copied insert has no such event, which is why every copied
    // Link drew "SET?" while its own picker showed BUS or CHANNEL.
    publishRegistrySnapshot ("registered");
}

void LinkProcessor::releaseRegistrySlot()
{
    if (regSlotIdx >= 0)
    {
        LinkShm::releaseSlot(regMap, regSlotIdx);
        regSlotIdx = -1;
        diag.slotIdx = -1;
    }
}

// =============================================================================
//  Audio ring helpers (message thread manages lifecycle)
// =============================================================================
void LinkProcessor::openRing()
{
    if (resolvedDir.isEmpty()) return;
    const juce::String filename = "audio_" + effectiveFilePart() + ".bin";

    int fd = -1, err = 0;
    void* map = LinkShm::openRingProducer(resolvedDir, filename,
                                           (float)hostSampleRate,
                                           (uint32_t)hostNumChannels, fd, err);
    diag.ringOpened = (map != nullptr);
    diag.ringErrno  = err;
    if (!map) return;

    const juce::SpinLock::ScopedLockType sl(shmLock);
    shmMap       = map;
    shmFd        = fd;
    shmOpenedKey = resolvedDir + filename;  // full path for closeRing
}

void LinkProcessor::closeRingDeferred()
{
    void* old = nullptr;
    int   fd  = -1;
    juce::String key;
    {
        const juce::SpinLock::ScopedLockType sl(shmLock);
        old          = shmMap;
        fd           = shmFd;
        key          = shmOpenedKey;
        shmMap       = nullptr;
        shmFd        = -1;
        shmOpenedKey = {};
    }
    if (!old) return;

    // Unlink immediately → consumer's next probe sees "not found"
    LinkShm::closeRing(nullptr, -1, key, /*doUnlink=*/true);

    // Defer munmap 50 ms — audio thread may hold the old pointer inside produce()
    juce::Timer::callAfterDelay(50, [old, fd]()
    {
        LinkShm::closeRing(old, fd, {}, /*doUnlink=*/false);
    });
}

void LinkProcessor::closeRingNow()
{
    void* old = nullptr;
    int   fd  = -1;
    juce::String key;
    {
        const juce::SpinLock::ScopedLockType sl(shmLock);
        old          = shmMap;
        fd           = shmFd;
        key          = shmOpenedKey;
        shmMap       = nullptr;
        shmFd        = -1;
        shmOpenedKey = {};
    }
    if (old) LinkShm::closeRing(old, fd, key, /*doUnlink=*/true);
}

// =============================================================================
//  Public state machine (message thread)
// =============================================================================
// COMP_PROFILE_SPEC_v1 section 5, published for V2 (2 Oct 2026). Throttled hard, because this runs on every
// Link's 1 Hz tick and Sean's session carries ~60 of them: nothing is written until the reading is VALID (the
// spec's 20 s of heard audio), and then only when a figure actually moved or 3 s have passed. An idle or silent
// Link writes no file at all, which is exactly the "no reading" V2 needs to see.
void LinkProcessor::publishTrackLevel()
{
    auto r = chainHost.trackLevelReading();
    // MORE HEARD AUDIO WINS (2 Oct 2026 ruling). A live reading replaces the stored one as soon as it has heard
    // more than it; until then the stored one is what this track knows about itself, and it is what gets
    // published - which is the whole point of persisting it across a restart.
    if (r.valid && r.heardSeconds > tlStoredHeardS_)
    {
        tlStoredHeardS_ = r.heardSeconds;
        tlStoredRmsDb_  = r.loudRmsDbfs;
        tlStoredPeakDb_ = r.loudPeakDbfs;
    }
    else if (tlStoredHeardS_ > 0.0f)
    {
        r.valid = true;                        // publish the stored reading: a restart must not blank the track
        r.loudRmsDbfs  = tlStoredRmsDb_;
        r.loudPeakDbfs = tlStoredPeakDb_;
        r.heardSeconds = tlStoredHeardS_;
    }
    if (! r.valid) return;                     // nothing above the gate, ever: the spec's null
    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const bool moved = std::abs (r.loudRmsDbfs  - tlLastRmsDb_)  > 0.1f
                    || std::abs (r.loudPeakDbfs - tlLastPeakDb_) > 0.1f;
    if (! moved && nowMs - tlLastWriteMs_ < 3000.0) return;
    int err = 0;
    const juce::String dir = LinkShm::resolveDir (err);
    if (dir.isEmpty() || instanceUid_.isEmpty()) return;
    // Built from the CHOSEN reading (stored or live), in the spec's own shape and to its own precision, rather
    // than from chainHost's var - which only ever describes the live one and would contradict the figures above.
    auto* to = new juce::DynamicObject();
    to->setProperty ("loud_rms_dbfs",  juce::String (r.loudRmsDbfs, 2).getDoubleValue());
    to->setProperty ("loud_peak_dbfs", juce::String (r.loudPeakDbfs, 2).getDoubleValue());
    to->setProperty ("window",  "400ms_p95");
    to->setProperty ("heard_s", (int) std::lround (r.heardSeconds));   // stays in the payload, by ruling
    juce::var v (to);
    if (auto* o = v.getDynamicObject()) o->setProperty ("uid", instanceUid_);
    if (! juce::File (dir + "track-level-" + instanceUid_ + ".json")
            .replaceWithText (juce::JSON::toString (v, true)))
        return;
    tlLastRmsDb_ = r.loudRmsDbfs; tlLastPeakDb_ = r.loudPeakDbfs; tlLastWriteMs_ = nowMs;
    // Said at most once every 30 s per Link (16,284 lines of the measured ring). The FILE is still written on
    // every pass - V2 reads the file, never the log - so nothing downstream loses resolution.
    if (nowMs - tlLastSaidMs_ >= 30000.0)
    {
        tlLastSaidMs_ = nowMs;
        EchoJay_NSLog (("EJTrackLevel: published uid=" + instanceUid_
                        + " rms=" + juce::String (r.loudRmsDbfs, 2)
                        + " peak=" + juce::String (r.loudPeakDbfs, 2)
                        + " heard=" + juce::String ((int) r.heardSeconds) + "s"
                        + " (said at most once per 30 s; the file is written every pass)").toRawUTF8());
    }
}

void LinkProcessor::updateShmState()
{
    // Called after any linkName/linkOn change — tell the host non-parameter
    // state changed so it re-snapshots (plain updateHostDisplay() doesn't
    // signal this; a host restoring a stale blob would lose the name/toggle).
    updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));

    const bool on = linkOn.load();

    // A Link stays REGISTERED whether or not it is Active OR NAMED — the
    // main plugin must keep seeing it (heartbeat = listed): unnamed Links
    // show as "Untitled" rows and deactivated Links stay re-activatable
    // remotely. Only the audio RING (the capture/meter feed) is gated on
    // Active. Unnamed instances use a per-instance untitled file id so
    // their ring/registry filenames never collide.

    // Ensure dir is resolved and registry is open (sets resolvedDir)
    ensureRegistryOpen();

    // Registry: claim slot if not already claimed, or re-claim on name change
    if (regSlotIdx < 0)
    {
        claimRegistrySlot();
    }
    else
    {
        releaseRegistrySlot();
        claimRegistrySlot();
    }
    LinkShm::setSlotActive(regMap, regSlotIdx, on);
    // 21t-j: ONE AUTHOR for what the registry row says about this Link, from the state and not from whatever event
    // happened to run. This path was the accidental publisher of the role: toggling a Link fixed a role that a
    // copied chunk had restored and never published.
    publishRegistrySnapshot ("state changed");
    // THIS binary reads settings_structured and applies it (see
    // buildChainFromSpec), so it may say so. Published beside gain and
    // placement because it is the same kind of claim: a fact about this
    // writer, asserted by the writer, never inferred by the reader.
    LinkShm::setSlotDialCapable(regMap, regSlotIdx, true);

    if (on)
    {
        const juce::String wantedPath = resolvedDir.isEmpty() ? juce::String{}
            : resolvedDir + "audio_" + effectiveFilePart() + ".bin";

        // Ring: reopen if path changed or not yet open
        if (shmOpenedKey != wantedPath || shmMap == nullptr)
        {
            closeRingDeferred();
            openRing();
        }
    }
    else
    {
        closeRingDeferred();   // capture/meter role dormant
    }
}

// =============================================================================
//  AudioProcessor overrides
// =============================================================================
void LinkProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    rackMuteMix_.reset(sampleRate, 0.030);   // §8 mute ramp, click-free
    hostSampleRate  = sampleRate;
    // The ring is always stereo (mono is duplicated on write) so the monitor
    // reads a consistent 2-channel layout regardless of the track format.
    hostNumChannels = 2;
    const int block = samplesPerBlock > 0 ? samplesPerBlock : 512;
    meterEngine_.prepare(sampleRate, block);
    levelTally_.prepare(sampleRate);   // 21t-d: SHORTMAX + HEARD ride the same tap as the meters
    keyEngine_.prepare(sampleRate, block);   // resets its tap; continuous mode
                                             // re-accumulates from here

    // Chain hosting supports mono AND stereo. The hosted graph is stereo, so a
    // mono track is up-mixed (L duplicated to L+R) before the chain and folded
    // back down (0.5*(L+R)) after — stereo-only plugins run duplicated-mono,
    // native-mono plugins pass through unchanged. Only >2ch (surround /
    // ambisonic) layouts are unsupported and leave the chain out of circuit.
    const int nin  = getTotalNumInputChannels();
    const int nout = getTotalNumOutputChannels();
    chainSupported_.store(nin >= 1 && nin <= 2 && nout >= 1 && nout <= 2);
    chainMono_.store(nin == 1 || nout == 1);
    chainScratch_.setSize(2, block);
    chainHost.prepare(sampleRate, block);

    // Gain smoother — 30 ms ramp is short enough to feel immediate but long
    // enough to kill zipper noise on a fast drag. Snap to the current target
    // on prepare so a restored non-zero gain doesn't swell up from unity.
    gainSmoothed_.reset(sampleRate, 0.030);
    gainSmoothed_.setCurrentAndTargetValue(
        EchoJayFader::gainForDb(gainDb_.load(std::memory_order_relaxed)));
    gainSnapPending_.store(false, std::memory_order_relaxed);

    // Always sync shm state from the message thread so that:
    //  (a) a fresh session restore with Active=on registers correctly, and
    //  (b) a sample-rate change reopens the ring with the new rate.
    // updateShmState() is idempotent — if already open it compares the
    // wanted path against shmOpenedKey and skips the reopen.
    juce::MessageManager::callAsync([this, alive = std::weak_ptr<bool> (aliveToken_)]
                                   { if (! alive.expired()) updateShmState(); });
}

bool LinkProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    // Mono and stereo only (matches the main plugin). Rejects surround /
    // ambisonic so hosts don't offer exotic layouts the chain can't fold.
    auto in  = layouts.getMainInputChannelSet();
    auto out = layouts.getMainOutputChannelSet();
    if (in != out) return false;
    return in == juce::AudioChannelSet::mono()
        || in == juce::AudioChannelSet::stereo();
}

void LinkProcessor::releaseResources()
{
    chainHost.release();
}

/** EVERY FRAME A DRAIN PRODUCED, APPLIED - ONE BODY FOR BOTH CONSUMERS (26 Oct 2026).

    The audio thread calls this from processBlock; the timer calls it while the audio thread is idle,
    which is what makes a knob move land with the transport stopped (his fault of 16:35, 26 Oct).
    Two copies of this switch would be two sets of rules about what a frame does, so there is one.

    Allocation-free and lock-free, as the audio thread requires: atomics for the figures, and a
    device parameter is STAGED rather than applied, because a plugin call can never be made
    real-time safe (the 10 Oct ruling, unchanged). The caller holds the drain claim. */
void LinkProcessor::applyRingFrames (const echojay::cmdring::Drain& d)
{
        for (int i = 0; i < d.count; ++i)
        {
            const auto& f = d.kept[i];
            using K = echojay::cmdring::Kind;
            using RF = ChainHost::RingFigure;
            switch (f.kind)
            {
                case K::kSlotIn:    chainHost.applyRingValueRT (RF::SlotIn,    f.slot, (float) f.value); break;
                case K::kSlotOut:   chainHost.applyRingValueRT (RF::SlotOut,   f.slot, (float) f.value); break;
                case K::kMasterWet: chainHost.applyRingValueRT (RF::MasterWet, -1,     (float) f.value / 100.0f); break;
                case K::kPreGain:   chainHost.applyRingValueRT (RF::PreGain,   -1,     (float) f.value); break;
                case K::kLinkGain:  gainDb_.store ((float) f.value, std::memory_order_relaxed); break;
                case K::kSetParam:
                {
                    // NOT APPLIED HERE, BY RULING. A device parameter is a plugin call and can never be made
                    // real-time safe, so it is staged for the message thread and the contract says so. Counted
                    // so a frame cannot vanish silently.
                    //
                    // ---- AND NOW IT IS REALLY STAGED (CONTROL stage C1, 24 Oct 2026) -------------
                    // The ruling is unchanged; what changed is that the frame is KEPT. It used to be
                    // counted and dropped, so "staged for the message thread" was true of the count
                    // and of nothing else. One copy into a single-producer ring, no allocation, no
                    // lock, no plugin call - applyStagedSetParams does the applying on the tick.
                    const uint32_t w = stagedParamW_.load (std::memory_order_relaxed);
                    const uint32_t r = stagedParamR_.load (std::memory_order_acquire);
                    if (w - r < (uint32_t) kStagedParamCap)
                    {
                        stagedParams_[w & (uint32_t) (kStagedParamCap - 1)]
                            = echojay::stagedFromFrame (f.slot, f.paramId, f.value, f.phase, f.gesture);
                        stagedParamW_.store (w + 1, std::memory_order_release);
                    }
                    else
                        stagedParamLost_.fetch_add (1, std::memory_order_relaxed);
                    ringSetParamPending_.fetch_add (1, std::memory_order_relaxed);
                    break;
                }
                default: break;
            }
        }
        // Figures, not frames: a 400-frame drag reports as one move. Published for the message thread's log.
        if (d.count > 0 || d.dropped > 0)
        {
            ringLastKept_.store (d.count, std::memory_order_relaxed);
            ringLastDropped_.fetch_add (d.dropped, std::memory_order_relaxed);
            ringLastOverflow_.store (d.overflow, std::memory_order_relaxed);
        }
}

void LinkProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    audioBlockCounter_.fetch_add(1, std::memory_order_relaxed);   // audio liveness

    // Transport state for the passive key scheduler (§5.1): the rolling flag
    // gates arming, and a position landing far from where the last block left
    // off flags a section jump (§5.4 invalidation). One playhead query per
    // block, relaxed stores — the scheduler reads at 1 Hz.
    // Host sample position for the ring's position stamp (stage 0 of remote
    // editing). Captured from the SAME single per-block playhead query the
    // key scheduler uses; -1 = the host gave no position this block, and no
    // stamp is published, so absence stays absence (see LinkShm.h).
    int64_t stampHostPos = -1;
    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            if (auto ts = pos->getTimeInSamples())
                stampHostPos = *ts;
            const bool playingNow = pos->getIsPlaying();
            if (auto t = pos->getTimeInSeconds())
            {
                const double prev = transportTimeS_.load(std::memory_order_relaxed);
                if (playingNow && transportPlaying_.load(std::memory_order_relaxed)
                    && std::abs(*t - prev) > kKeyJumpSeconds)
                    keySectionJump_.store(true, std::memory_order_relaxed);
                transportTimeS_.store(*t, std::memory_order_relaxed);
            }
            transportPlaying_.store(playingNow, std::memory_order_relaxed);
        }
    }
    // Pass-through: silence extra output channels
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear(ch, 0, buffer.getNumSamples());

    // ---- THE VALUE RING, DRAINED HERE (stage 3, 10 Oct 2026) --------------------------------------
    // BEFORE the chain runs, so a value written by the V2 lands on THIS buffer - that is the whole bound the
    // ring exists for (one buffer: 2.7 ms at 128 samples, 10.7 ms at 512). Draining after the chain, or on a
    // timer, would put the move a buffer or a timer tick late and the drag would feel broken.
    // Allocation-free and lock-free: cmdring::drain coalesces into a fixed array and the appliers write only
    // the atomics the audio path reads. The logging, the revision bump and the undo step are downstream, on
    // the message thread (ChainHost::drainRingBookkeeping).
    // ---- ONE CONSUMER, DECIDED BY AN ATOMIC AND NEVER BY A CLOCK (26 Oct 2026) ----------------
    // The timer drains this same ring while the audio thread is idle, so a knob moved with the
    // transport stopped still reaches the plugin (his 16:35 fault; EJRingDrainClaim.h says why).
    // His condition, and it is the right one: the exclusion is an ATOMIC TRY-CLAIM, not a time
    // check. This thread never waits - if the timer holds the claim this block does not drain and
    // the frames are still there for the next one, which is a value one buffer late at worst.
    if (auto* ring = cmdRing_.load (std::memory_order_acquire))
    {
        echojay::RingDrainClaim claim (ringDrainClaim_);
        if (claim.held())
        {
            lastAudioDrainMs_.store (juce::Time::getMillisecondCounterHiRes(), std::memory_order_relaxed);
            applyRingFrames (echojay::cmdring::drain (ring, (uint32_t) chainHost.getChainRevision()));
        }
        else
            ringDrainSkipped_.fetch_add (1, std::memory_order_relaxed);
    }

    // Hosted chain runs FIRST so the ring tap (and the track) hears the
    // processed signal. Empty / all-bypassed chain = the graph stays out of
    // circuit; only >2ch layouts skip it. On a mono track the signal is
    // up-mixed into the stereo graph then folded back to mono on output.
    if (chainSupported_.load(std::memory_order_relaxed))
    {
        const int n = buffer.getNumSamples();
        if (buffer.getNumChannels() == 1 && n <= chainScratch_.getNumSamples())
        {
            float* chans[2] = { chainScratch_.getWritePointer(0),
                                chainScratch_.getWritePointer(1) };
            const float* src = buffer.getReadPointer(0);
            juce::FloatVectorOperations::copy(chans[0], src, n);
            juce::FloatVectorOperations::copy(chans[1], src, n);
            juce::AudioBuffer<float> stereoView(chans, 2, n); // wraps scratch, no alloc
            chainHost.process(stereoView, midi);
            // Measure how much the chain decorrelated the duplicated-mono input:
            // |L-R| relative to total level (0 = mono-safe, →1 = fully stereo).
            // Leaky-averaged with a threshold + reset-on-silence so a transient
            // doesn't toggle the status note.
            {
                float diff = 0.0f, sum = 0.0f;
                for (int i = 0; i < n; ++i)
                {
                    diff += std::abs(chans[0][i] - chans[1][i]);
                    sum  += std::abs(chans[0][i]) + std::abs(chans[1][i]);
                }
                if (sum > 1.0e-4f)   // only when there's audible signal
                {
                    const float ratio = diff / sum;
                    monoDivergeAvg_ += 0.05f * (ratio - monoDivergeAvg_);
                    monoFoldDiverges_.store(monoDivergeAvg_ > 0.03f,
                                            std::memory_order_relaxed);
                }
            }
            // Fold to mono by TAKING LEFT — never sum. Summing can cancel a
            // widener / mid-side plugin to a quiet, hollow track (the worst
            // silent failure). Native-mono plugins keep L==R, so take-L is
            // lossless for them; a genuinely stereo plugin loses its right
            // output, which is surfaced once in the status line.
            float* dst = buffer.getWritePointer(0);
            juce::FloatVectorOperations::copy(dst, chans[0], n);
        }
        else
        {
            chainHost.process(buffer, midi);
        }
    }

    // Built-in gain stage — POST-chain, PRE-meter-tap. This position is
    // deliberate: (1) it gains the TRACK output (the gain is a real stage on
    // the signal path, so the DAW hears it), and (2) the meter tap below
    // reads the POST-gain signal, so the LINK monitor's integrated loudness
    // reflects the gain — which is exactly what the "match level" feature
    // needs to converge (target minus post-gain integrated IS the delta to
    // apply). Bit-transparent at 0 dB (unity multiply skipped entirely).
    // 21t-d (25 Sep 2026): THE METER TAP MOVED ABOVE THE GAIN STAGE.
    // It used to sit below, deliberately, so the published integrated already included the trim and a level
    // match could converge by subtraction. That is one use; levelling a GROUP is the other, and it needs the
    // opposite - what each channel is delivering INTO its trim, so moving one member's trim does not rewrite
    // the number the next decision is made from. The frame now says which it published (kFrameHasPreTrim), and
    // a reader that wants the DAW's figure adds the trim back (frameLoudnessAsHeard). POST-chain as before:
    // this is the Link's own rack output arriving at the trim.
    if (linkOn.load(std::memory_order_acquire) && buffer.getNumChannels() >= 1)
    {
        const float* L = buffer.getReadPointer(0);
        const float* R = buffer.getNumChannels() >= 2 ? buffer.getReadPointer(1) : L;
        meterEngine_.processBlock(L, R, buffer.getNumSamples());
        // The tally rides the SAME tap: SHORTMAX and HEARD describe the same signal the loudness fields do.
        levelTally_.push(L, R, buffer.getNumSamples());
        // Key tap, same gate and tap point: lock-free ring write, analysis
        // happens on the worker thread (KEY_DETECTOR_SPEC.md §9).
        keyEngine_.pushBlock(L, R, buffer.getNumSamples());
    }

    applyGainSmoothed(buffer);

    // Write into ring buffer if active -- non-blocking tryEnter. ALSO while
    // an edit lease is held: linkOn gates the ring, and an inactive Link
    // publishes nothing, but the whole point of the lease is that the main
    // plugin is monitoring this channel through its editing copy. Forcing
    // production here (rather than refusing to open editors on inactive
    // Links) keeps the feature available exactly when someone is mid-mix
    // with Links toggled off. Meters stay gated on linkOn alone.
    if (linkOn.load(std::memory_order_acquire)
        || leaseActive_.load(std::memory_order_relaxed))
    {
        if (shmLock.tryEnter())
        {
            if (shmMap != nullptr)
            {
                // Ring is always stereo: on a mono track duplicate L into R so
                // captures record and play back as proper (dual-mono) audio
                // instead of a dead right channel.
                const float* L = buffer.getNumChannels() >= 1 ? buffer.getReadPointer(0) : nullptr;
                const float* R = buffer.getNumChannels() >= 2 ? buffer.getReadPointer(1) : L;
                const float* chPtrs[2] = { L, R };
                // The stamp pairs THIS block's first frame with the ring
                // index it is about to land on, published BEFORE the produce
                // so a reader can never see frames whose stamp has not been
                // written yet. No host position this block = no stamp.
                if (stampHostPos >= 0)
                {
                    LinkShm::ringStampPublish(shmMap,
                        LinkShm::loadRelaxed(&LinkShm::ringHeader(shmMap)->writeIdx),
                        stampHostPos);
                    // Log from the TIMER, not here: NSLog allocates and can
                    // block, and this is the audio thread. One relaxed store.
                    stampsArmed_.store(true, std::memory_order_relaxed);
                }
                LinkShm::ringProduce(shmMap, chPtrs, 2, buffer.getNumSamples());
                didWrite.store(true, std::memory_order_relaxed);
            }
            shmLock.exit();
        }
    }

    // §8 IN-CONTEXT MUTE, strictly AFTER the ring write: the ring must keep
    // carrying this signal (it is the main's injection source); only the
    // channel's contribution to the DAW mix goes silent. Ramped ~30ms so
    // engage/release never click. The want rides the lease (re-read every
    // poll; cleared by the one Release/Expire restore path), so a crash
    // un-mutes within the lease expiry exactly as it un-bypasses.
    {
        // Mute/solo layer (27 Aug 2026): THREE reasons, ONE ramp. The
        // lease mute, the user's own mute and the solo fabric compose
        // through linkMuteWanted(); each keeps its own lifetime, so a
        // session release can never clear a user mute.
        const bool muted = linkMuteWanted();
        rackMuteMix_.setTargetValue(muted ? 0.0f : 1.0f);
        if (muted || rackMuteMix_.getCurrentValue() < 0.9999f)
        {
            const int n = buffer.getNumSamples();
            for (int i = 0; i < n; ++i)
            {
                const float g = rackMuteMix_.getNextValue();
                for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                    buffer.getWritePointer(ch)[i] *= g;
            }
        }
        else
            rackMuteMix_.skip(buffer.getNumSamples());
    }
}

// Audio thread. Glides the smoothed linear gain toward the atomic target and
// multiplies it into the buffer. Returns false (and touches nothing) when the
// stage is a settled unity no-op, so 0 dB is bit-transparent.
bool LinkProcessor::applyGainSmoothed(juce::AudioBuffer<float>& buffer)
{
    const float targetLin =
        EchoJayFader::gainForDb(gainDb_.load(std::memory_order_relaxed));

    if (gainSnapPending_.exchange(false, std::memory_order_acq_rel))
        gainSmoothed_.setCurrentAndTargetValue(targetLin);
    else
        gainSmoothed_.setTargetValue(targetLin);

    // Settled at unity → nothing to do (bit-transparent bypass at 0 dB)
    if (!gainSmoothed_.isSmoothing() && gainSmoothed_.getCurrentValue() == 1.0f)
        return false;

    const int n = buffer.getNumSamples();
    const int numCh = buffer.getNumChannels();
    if (gainSmoothed_.isSmoothing())
    {
        for (int i = 0; i < n; ++i)
        {
            const float g = gainSmoothed_.getNextValue();
            for (int ch = 0; ch < numCh; ++ch)
                buffer.getWritePointer(ch)[i] *= g;
        }
    }
    else
    {
        buffer.applyGain(gainSmoothed_.getCurrentValue());
    }
    return true;
}

void LinkProcessor::setGainDb(float db, bool snapSmoothing)
{
    db = juce::jlimit(kGainMinDb, kGainMaxDb, db);
    gainDb_.store(db, std::memory_order_relaxed);
    if (snapSmoothing)
        gainSnapPending_.store(true, std::memory_order_relaxed);

    // Mirror to the registry slot so the monitor shows it (Active or not)
    if (regMap != nullptr && regSlotIdx >= 0)
        LinkShm::setSlotGain(regMap, regSlotIdx, db);

    // Non-parameter state changed — host re-snapshots (dirty-mark)
    updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));

    if (onLinkStateChanged) onLinkStateChanged();   // open editor slider sync
}

// ---- BUSY, PUBLISHED AT ONCE (18 Oct 2026, queue items 13/16/21) ---------------------------------------
// Not left to the next snapshot, and the reason is the whole point of the flag: a build is exactly the moment
// this plugin's message thread stops being punctual (each AU load blocks it), so the next heartbeat may be
// seconds away - and those are the seconds in which the V2 decides whether this track is reachable. Written
// where the state changes, so the registry is right before anyone can read it wrong.
void LinkProcessor::publishBusy (bool busy)
{
    if (regMap == nullptr || regSlotIdx < 0) return;
    LinkShm::setSlotBusy (regMap, regSlotIdx, busy);
}

void LinkProcessor::publishRegistrySnapshot (const char* why)
{
    if (regMap == nullptr || regSlotIdx < 0) return;
    const int place = placement_.load (std::memory_order_relaxed);
    LinkShm::setSlotGain      (regMap, regSlotIdx, gainDb_.load (std::memory_order_relaxed));
    LinkShm::setSlotPlacement (regMap, regSlotIdx, (uint8_t) place);
    LinkShm::setSlotDialCapable (regMap, regSlotIdx, true);
    LinkShm::setSlotActive    (regMap, regSlotIdx, linkOn.load (std::memory_order_acquire));
    LinkShm::setSlotBusy      (regMap, regSlotIdx, chainBuilding);   // 18 Oct 2026: BUSY is not DEAD
    // THE LINE THAT WAS MISSING. There was no registration line carrying a role at all, so "the role did not
    // reach V2" could not be seen in a log - only inferred from what the strip drew. Every snapshot says what it
    // published and why, so the next copied insert answers the question in one grep.
    const juce::String said = "EJLinkState: registry snapshot (" + juce::String (why) + ") uid=" + instanceUid_
                    + " name=\"" + effectiveDisplayName() + "\" role="
                    + juce::String (place == PlacementBus ? "bus" : place == PlacementInsert ? "channel"
                                    : place == PlacementSend ? "send" : "UNSET")
                    + " trim=" + juce::String (gainDb_.load (std::memory_order_relaxed), 2)
                    + " dB active=" + juce::String ((int) linkOn.load (std::memory_order_acquire));
    // ---- SAID ON CHANGE, NOT ONCE A SECOND (11 Oct 2026) -------------------------------------------------
    // The publish stays on its heartbeat - the registry must not go stale. The LINE does not. Measured on Sean's
    // session: 81 Links x 1 line/s was 50,018 of the 70,000 lines in the whole five-file ring, and it pushed the
    // window in which any bug could be diagnosed down to minutes. A line that repeats itself unchanged 81 times a
    // second is not evidence, it is what destroys evidence.
    //
    // Compared against the WHOLE sentence, so any field moving says so, and the reason is kept: a "heartbeat"
    // that reports a change the state-changed path missed is exactly the line worth having. Every non-heartbeat
    // reason is always said, because those mark events, and an event is worth a line even when nothing moved.
    const bool isHeartbeat = juce::String (why) == "heartbeat";
    if (! isHeartbeat || said != lastSaidSnapshot_)
    {
        lastSaidSnapshot_ = said;
        EchoJay_NSLog (said.toRawUTF8());
    }
}

void LinkProcessor::setPlacement(int p)
{
    // Clamp to the LAST enum value. This clamped to PlacementInsert from
    // before Send existed (49125ff), so choosing Send stored Channel: the
    // menu, the remote command and the shm mirror all passed through here.
    // The restore path (setStateInformation) already clamps to Send.
    p = juce::jlimit((int) PlacementUnset, (int) PlacementSend, p);
    placement_.store(p, std::memory_order_relaxed);
    publishRegistrySnapshot ("the picker was set");   // 21t-j: the whole snapshot, from the state
    updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
    if (onLinkStateChanged) onLinkStateChanged();
    EchoJay_NSLog(("EJLinkState: placement set to "
                   + juce::String(p == PlacementBus ? "bus"
                                  : p == PlacementInsert ? "insert"
                                  : p == PlacementSend ? "send" : "unset")).toRawUTF8());
}

// =============================================================================
//  Chain hosting (message thread)
// =============================================================================
juce::String LinkProcessor::chainFormatFilter() const
{
    switch (wrapperType)
    {
        case juce::AudioProcessor::wrapperType_AudioUnit: return "AudioUnit";
        case juce::AudioProcessor::wrapperType_VST3:      return "VST3";
        // AAX (Pro Tools, macOS only) takes the same filter as the AU host:
        // AU is the format that is both hostable and dialable on a Mac, so a
        // named case, not the old fallthrough (mirrors the main plugin's
        // chainFormatFilter_ switch in PluginEditor.cpp).
        case juce::AudioProcessor::wrapperType_AAX:       return "AudioUnit";
        default:                                          return {};
    }
}

juce::StringArray LinkProcessor::loadDisabledUids()
{
    // plugin_disabled.json — a JSON array of scanner uids
    // (lowercase name + "_" + lowercase manufacturer, spaces -> underscores).
    auto file = echojay::userAppData()
                    .getChildFile("Application Support/EchoJay/plugin_disabled.json");
    juce::StringArray uids;
    if (file.existsAsFile())
    {
        auto v = juce::JSON::parse(file.loadFileAsString());
        if (auto* arr = v.getArray())
            for (auto& u : *arr)
                uids.add(u.toString());
    }
    return uids;
}

juce::PluginDescription LinkProcessor::resolveChainPlugin(const juce::String& name) const
{
    // Shared loose resolver (identical behaviour to the main plugin):
    // exact -> parenthetical-stripped (+manufacturer tie-break) -> normalised.
    // The match path (or closest candidates) is logged per entry.
    juce::String matchLog;
    auto d = chainHost.resolveByName(name, chainFormatFilter(), &matchLog);
    EchoJay_NSLog(("EJChatLink: resolve \"" + name + "\" -- " + matchLog).toRawUTF8());
    return d;
}

void LinkProcessor::clearChainInternal()
{
    if (onChainAboutToChange) onChainAboutToChange();   // editors close first
    // Item 4: archive the Link's live rack before it is replaced, the same
    // protection the main rack gets. clearChainInternal is the whole-rack
    // replace point (per-slot edits use removeSlot directly), so this fires on
    // exactly the destructive event and no-ops on an empty rack.
    chainHost.archiveCurrentRack("chain replaced (Link)");
    for (int i = chainHost.getNumSlots() - 1; i >= 0; --i)
        chainHost.removeSlot(i);
    chainModel.clear();
}

void LinkProcessor::updateChainLatency()
{
    ejSetLatencyLogged (*this, chainHost.getTotalLatencySamples(), "LinkProcessor #1");
}

void LinkProcessor::resyncChainModelFromHost()
{
    std::vector<ChainSlotSpec> next;
    const int n = chainHost.getNumSlots();
    next.reserve((size_t)n);
    for (int i = 0; i < n; ++i)
    {
        auto info = chainHost.getSlotInfo(i);
        ChainSlotSpec s;
        s.name     = info.name;
        // ---- 11 Oct 2026: ONE STATE, SO THERE IS NOTHING TO CHOOSE BETWEEN -------------------------
        // Three arms stood here, picking between `intendedBypassed` and `bypassed` depending on whether a
        // lease was held and whether this was its slot. They existed because bypass was two values: the
        // user's intent and the lease's overlay. The history is worth keeping in one line, because it is two
        // of the bugs the plan retires - a persisted lease-bypass left a rack silently switched off after a
        // crash mid-borrow (24 Aug), and a slot ADDED during a lease had no prior and fell through to the
        // overlay, so the model persisted every new plugin as bypassed (8 Oct, Sean 11:41).
        //
        // setSlotBypassed is now the one author and intendedBypassed moves with it, so either name reads the
        // same thing. `bypassed` is used here because it is the one the audio path reads.
        s.bypassed = info.bypassed;
        s.hostIdx  = i;
        s.wet      = info.wet;
        s.settings = info.settings;
        if (s.settings.isEmpty())
            for (auto& old : chainModel)
                if (!old.missing && ChainHost::namesMatchLoose(old.name, s.name))
                { s.settings = old.settings; break; }
        next.push_back(std::move(s));
    }
    chainModel = std::move(next);
}

// ---- THE VALUE RING: mapping and bookkeeping (stage 3, 10 Oct 2026) -----------------------------
// MESSAGE THREAD ONLY. Two jobs, both deliberately off the audio thread:
//   (1) MAP the rack's ring once, publishing the pointer through an atomic so processBlock never touches a
//       mapping. A rack whose ring will not map keeps using the file channel, which is the right fallback.
//   (2) RECONCILE: run the ordinary setter for every move the audio thread already applied, so the log, the
//       value revision and the readback catch up with the audio. This is the "downstream" half of Sean's
//       ruling (a): the RT applier is the authority for the figure, this makes every other surface agree.
void LinkProcessor::ringTick()
{
    // STAGE 4: the same-process proof's own side of the comparison. This process publishes the rack's
    // sidecar, so for THIS host the peer pid is our own - and an embed request only ever reaches here from a
    // V2 that compared its pid against the one in our sidecar. Set every tick rather than once, because a
    // ChainHost can be rebuilt under us and a stale 0 would read as "no proof" and refuse a legitimate embed.
    chainHost.editorPeerPid = (int) ::getpid();
    if (! chainHost.onEditorRequest)
        chainHost.onEditorRequest = [this] (int slot, bool open, bool embed)
        {
            // RECORDED, NOT ACTED ON HERE. The window belongs to the editor, which may not exist - a Link
            // with no window open is the normal case - so the request is held and the editor takes it when it
            // next ticks. A request that nobody collects is visible in the log rather than silently lost.
            editorReqSlot_.store (slot, std::memory_order_relaxed);
            editorReqOpen_.store (open, std::memory_order_relaxed);
            editorReqEmbed_.store (embed, std::memory_order_relaxed);
            editorReqSeq_.fetch_add (1, std::memory_order_release);
            EchoJay_NSLog (("EJEditorReq: slot " + juce::String (slot + 1) + (open ? " OPEN" : " CLOSE")
                            + (embed ? " embedded" : " floating")
                            + " - held for this Link's editor").toRawUTF8());
        };

    // ---- 11 Oct 2026: A SLOW, BOUNDED RETRY --------------------------------------------------------------
    //
    // What stood here retried whenever cmdRing_ was null, and this function runs at 30 Hz (the heartbeat below
    // divides it by 30). A rack whose ring is not laid down keeps cmdRing_ null for ever, so "attempted quietly
    // and once" - my own comment - was thirty attempts a second, for the life of the session, per Link. With
    // the leak in openRingForDrain behind it, that is what exhausted the AU hosting process.
    //
    // The fix is not "try once and give up", which is what the V2's producer side correctly does: a ring
    // legitimately APPEARS later, when the V2 first addresses this rack, and that can be minutes after this
    // Link started. So the retry stays, at about 1 Hz instead of 30, and ring_leak_guard's leg (5) is the one
    // that stops a future tidy-up turning a bounded retry into no retry at all.
    if (cmdRing_.load(std::memory_order_acquire) == nullptr)
    {
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        if (nowMs >= ringNextTryMs_)
        {
            ringNextTryMs_ = nowMs + kRingRetryMs;
            int err = 0;
            const juce::String dir = LinkShm::resolveDir(err);
            const juce::String uid = instanceUid_;
            if (dir.isNotEmpty() && uid.isNotEmpty())
            {
                int fd = -1, e2 = 0;
                ++ringOpenAttempts_;
                if (auto* r = echojay::cmdring::openRingForDrain(dir, uid, fd, e2))
                {
                    cmdRingFd_ = fd;
                    cmdRing_.store(r, std::memory_order_release);
                    EchoJay_NSLog(("EJRing: mapped the value ring for \"" + uid + "\" after "
                                   + juce::String(ringOpenAttempts_) + " attempt(s)"
                                   + " - values arrive within one buffer now").toRawUTF8());
                }
                // NOT SILENT ANY MORE. This path said nothing at all, which is why the log pointed nowhere and
                // it took a kernel message ("Too many open files") to find. One line per kRingSayMs, with the
                // attempt COUNT on it - a reader needs to know whether this tried twice or twenty thousand
                // times, which is the difference between "no ring yet" and a leak.
                else if (nowMs - ringSaidMs_ >= kRingSayMs)
                {
                    ringSaidMs_ = nowMs;
                    EchoJay_NSLog(("EJRing: no value ring for \"" + uid + "\" yet (errno "
                                   + juce::String(e2) + ", " + juce::String(ringOpenAttempts_)
                                   + " attempt(s) so far, retrying about every "
                                   + juce::String((int) (kRingRetryMs / 1000.0)) + " s) - values go by the file"
                                     " channel, which is slower and still correct. This is the NORMAL state for a"
                                     " rack no main plugin has addressed.").toRawUTF8());
                }
            }
        }
    }

    const int reconciled = chainHost.drainRingBookkeeping();
    const int dropped    = ringLastDropped_.exchange(0, std::memory_order_relaxed);
    const int setParams  = ringSetParamPending_.exchange(0, std::memory_order_relaxed);
    if (reconciled > 0 || dropped > 0 || setParams > 0)
        EchoJay_NSLog(("EJRing: reconciled " + juce::String(reconciled) + " ring move(s)"
                       + (dropped > 0 ? ", DISCARDED " + juce::String(dropped)
                                        + " (stale structure revision)" : juce::String())
                       + (setParams > 0 ? ", " + juce::String(setParams)
                                          + " set_param frame(s) staged (a device parameter is a plugin call "
                                            "and is never applied on the audio thread - by ruling)"
                                        : juce::String())
                       + ", overflow " + juce::String((int) ringLastOverflow_.load(std::memory_order_relaxed))).toRawUTF8());
}

// ---- 11 Oct 2026, THE REMOVALS: ADDRESSING, NOT ENGAGING -------------------------------------------------
//
// rackLeaseEngage() and rackLeaseRelease() are replaced by this one function and no counterpart, because an
// engage no longer does anything a release has to undo.
//
// WHAT THE ENGAGE DID, in its own words: "WHOLE-RACK ENGAGE: save every slot's bypass, bypass all once, stream
// dry". Three things went with it:
//
//   * setAttachBypassed(true) + setLeaseBypass(i, true) for every slot - Sean's kick, going through its own
//     channel strip and its own bus UNPROCESSED while the processed copy was summed in at the V2's position;
//   * rackLeasePrior_, the snapshot the release restored from - and the source of the persistent bypass after
//     a hand-back, because a slot added DURING a lease has no prior;
//   * leaseBaseRev_, which existed only so the V2 could judge a copy it had staged. Nothing is staged now.
//
// What is left is a fact about WHO is addressing this rack, which is the lease as the plan keeps it: a control
// concept. The one line of work is to let the editor re-read the model, so the lock overlay appears.
void LinkProcessor::rackLeaseAddress()
{
    if (rackLeaseScope_) return;   // already addressed by this holder; nothing to restate
    rackLeaseScope_ = true;
    leaseSlot0_     = -1;
    leaseActive_.store(true, std::memory_order_relaxed);
    notifyChainModel();
    EchoJay_NSLog(("EJLease: RACK addressed by id \"" + leaseGate_.activeId + "\" rev "
                   + juce::String(chainHost.getChainRevision()) + " - " + juce::String(chainHost.getNumSlots())
                   + " slot(s), ALL STILL PROCESSING in this instance's own place in the graph. A selection is "
                     "not an audio act.").toRawUTF8());
}

void LinkProcessor::syncModelAfterStructuralChange()
{
    // THE FOUR-STEP WRITER, one author (24 Aug 2026): the chain-cmd path
    // always did these four; the plan path did only the last, so the
    // editor re-read a model nobody had changed.
    resyncChainModelFromHost();
    publishRackSidecar();
    updateChainLatency();
    notifyChainModel();
}

ChainHost::PlanResult LinkProcessor::applyStructurePlanAndSync(
    const juce::String& dir, const LinkShm::StructureEdit::Plan& plan)
{
    auto res = chainHost.applyStructurePlan(dir, plan);
    // ---- 11 Oct 2026: THE PRIOR REMAP IS GONE --------------------------------------------------------
    // A block here remapped rackLeasePrior_ through the plan's finalOrigin record and then re-asserted the
    // lease's dry rack over every slot, created ones included. Both halves were about a rack that had been
    // switched off: the remap so the release put the right bypass back on the right plugin after a reshape
    // (24 Aug, "every plugin came back bypassed"), and the re-assert so a freshly attached slot was never
    // rendered live. Stage 1 had already turned the re-assert off and linksync_test's reshape leg caught why:
    // it read [0:byp,1:byp,2:byp] where the plan asked for two live survivors and one bypassed Create - the
    // loop was quietly switching the user's rack off again after every plan.
    // UNCONDITIONAL: applied changed the shape, a rollback tore it down
    // and rebuilt it — the editor-facing model is stale either way. Runs
    // AFTER the remap so the sync records the lease's TRUE priors.
    syncModelAfterStructuralChange();
    return res;
}

void LinkProcessor::notifyChainModel()
{
    updateMonoStereoOnlyNames();
    // Fresh chain → clear the stale fold-down flag; the audio thread re-sets it
    // within a few blocks if the new chain still decorrelates a mono input.
    monoFoldDiverges_.store(false, std::memory_order_relaxed);
    if (onChainModelChanged) onChainModelChanged();
    // Chain model changed → host should re-snapshot our state
    updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
}

// Message thread. Names the non-bypassed hosted plugins whose main bus cannot
// run mono — the plausible culprits when the mono fold-down drops stereo work.
void LinkProcessor::updateMonoStereoOnlyNames()
{
    juce::StringArray names;
    const int n = chainHost.getNumSlots();
    for (int i = 0; i < n; ++i)
    {
        auto info = chainHost.getSlotInfo(i);
        if (info.bypassed) continue;
        auto* p = chainHost.getSlotProcessor(i);
        if (p == nullptr) continue;
        // Flip only the main bus to mono, preserving any aux/sidechain buses.
        auto layout = p->getBusesLayout();
        if (layout.inputBuses.size()  > 0) layout.inputBuses.getReference(0)  = juce::AudioChannelSet::mono();
        if (layout.outputBuses.size() > 0) layout.outputBuses.getReference(0) = juce::AudioChannelSet::mono();
        if (!p->checkBusesLayoutSupported(layout))
            names.add(info.name);
    }
    monoStereoOnlyNames_ = names.joinIntoString(", ");
}

juce::String LinkProcessor::getMonoFoldNote() const
{
    if (!chainMono_.load() || !monoFoldDiverges_.load(std::memory_order_relaxed))
        return {};
    if (monoStereoOnlyNames_.isEmpty())
        return "A stereo-only plugin; using its left output";
    const bool many = monoStereoOnlyNames_.contains(",");
    return monoStereoOnlyNames_ + (many ? " are stereo-only; using their left output"
                                        : " is stereo-only; using its left output");
}

void LinkProcessor::buildChainFromSpec(std::vector<ChainBuildItem> spec,
                                      ChainHost::LoadOrigin origin,
                                       std::function<void(const juce::StringArray&,
                                                          const juce::var&)> onDone)
{
    if ((int)spec.size() > kMaxChainSlots)
        spec.resize((size_t)kMaxChainSlots);

    chainBuilding = true;
    publishBusy (true);          // before a single plugin is loaded - see publishBusy
    chainBuildStartedMs_ = juce::Time::getMillisecondCounterHiRes();
    chainBusyAckedSeq_   = 0;
    clearChainInternal();
    notifyChainModel();

    // Resolve against the SAME entries list the main plugin maintains: pick
    // up the shared chain_entries.xml if another host refreshed it. Own scan
    // is only the backstop when no cache exists at all.
    chainHost.maybeReloadEntriesCache();
    if (chainHost.getNumPlugins() == 0 && !chainHost.isScanning())
        chainHost.startScan();

    // Build-time diagnostics: list size, source, freshness
    {
        auto ec = ChainHost::getEntriesCacheFile();
        EchoJay_NSLog(("EJChatLink: build start -- list=" +
            juce::String(chainHost.getNumPlugins()) + " entries, format=" +
            chainFormatFilter() + ", cache=" + ec.getFullPathName() +
            (ec.existsAsFile()
                 ? " (mtime " + ec.getLastModificationTime().toISO8601(true) + ")"
                 : " (missing)") +
            (chainHost.isScanning() ? ", scanning" : "")).toRawUTF8());
    }

    auto results  = std::make_shared<juce::StringArray>();
    auto detail   = std::make_shared<juce::Array<juce::var>>();
    auto disabled = std::make_shared<juce::StringArray>(loadDisabledUids());
    auto items    = std::make_shared<std::vector<ChainBuildItem>>(std::move(spec));
    auto idx      = std::make_shared<int>(0);
    auto self     = this;   // processor outlives message-thread callbacks in-session

    auto isDisabled = [disabled](const juce::String& name)
    {
        // Key on both the raw name and the parenthetical-stripped one — the
        // AI may send "Name (Manufacturer)" while uids derive from the plain
        // scanner name ("name_manufacturer_...").
        auto mk = [](const juce::String& n)
            { return n.trim().toLowerCase().replaceCharacter(' ', '_') + "_"; };
        auto k1 = mk(name);
        auto k2 = mk(ChainHost::stripParenthetical(name));
        for (auto& uid : *disabled)
            if (uid.startsWith(k1) || uid.startsWith(k2)) return true;
        return false;
    };

    auto addDetail = [detail](const juce::String& name, const juce::String& kind,
                              const juce::String& info, const juce::String& resolvedName)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("name", name);
        o->setProperty("kind", kind);
        if (info.isNotEmpty())         o->setProperty("detail", info);
        if (resolvedName.isNotEmpty()) o->setProperty("resolvedName", resolvedName);
        detail->add(juce::var(o));
    };

    auto stepPtr = std::make_shared<std::function<void()>>();
    *stepPtr = [self, results, detail, items, idx, isDisabled, addDetail, stepPtr, onDone, origin]()
    {
        // Wait for an in-flight scan before resolving (poll, bounded)
        if (self->chainHost.isScanning())
        {
            juce::Timer::callAfterDelay(200, [stepPtr] { (*stepPtr)(); });
            return;
        }

        if (*idx >= (int)items->size())
        {
            self->chainBuilding = false;
            self->publishBusy (false);
            self->updateChainLatency();
            self->notifyChainModel();
            // The Link reports its own dial outcome now. Before this, a
            // channel build produced no dial line anywhere: the summary was
            // wired into the main plugin's editor, which this binary does not
            // compile, so a whole build path was unobservable by construction.
            self->chainHost.reportDialWhenSettled("EJDialSummary(Link): channel build");
            if (onDone) onDone(*results, juce::var(*detail));
            return;
        }

        int i = (*idx)++;
        auto& item = (*items)[(size_t)i];

        ChainSlotSpec slot;
        slot.name     = item.name;
        slot.settings = item.settings;
        slot.bypassed = item.bypassed;
        slot.wet      = item.wet;

        if (isDisabled(item.name))
        {
            slot.missing = true;
            self->chainModel.push_back(slot);
            results->add(item.name + ": skipped (disabled in Settings)");
            addDetail(item.name, "skipped", "disabled in Settings", {});
            self->notifyChainModel();
            (*stepPtr)();
            return;
        }

        auto desc = self->resolveChainPlugin(item.name);
        if (desc.name.isEmpty())
        {
            slot.missing = true;
            self->chainModel.push_back(slot);
            results->add(item.name + ": not found");
            addDetail(item.name, "not_found", {}, {});
            self->notifyChainModel();
            (*stepPtr)();
            return;
        }

        // Re-check disabled state against the RESOLVED name too — the loose
        // resolver can match variants the raw-name key check misses.
        if (isDisabled(desc.name))
        {
            slot.missing = true;
            self->chainModel.push_back(slot);
            results->add(item.name + ": skipped (disabled in Settings)");
            addDetail(item.name, "skipped", "disabled in Settings", desc.name);
            self->notifyChainModel();
            (*stepPtr)();
            return;
        }

        juce::String stateB64 = item.stateBase64;
        bool wantBypass = item.bypassed;
        juce::var structuredForSlot = item.structured;
        // 4 Oct 2026: copied out of `item` for the same reason the three above are - the apply runs inside a lambda
        // that captures by value, and `item` is a reference into a vector the lambda does not own.
        const float restoreOutGainDb = item.outGainDb;
        const float restorePreTrimDb = item.preTrimDb;
        // Format preference applies to NEW instantiation only: restores
        // (stateBase64 present) keep the format their state was saved with —
        // AU and VST3 state blobs are not interchangeable.
        if (stateB64.isEmpty())
            desc = self->chainHost.preferInlineHostableDesc(desc);
        self->chainHost.loadPluginAsync(desc, origin,
            [self, results, addDetail, slot, stateB64, wantBypass, structuredForSlot, restoreOutGainDb, restorePreTrimDb, stepPtr,
             name = item.name, resolvedName = desc.name]
            (const juce::String& err) mutable
        {
            if (err.isNotEmpty())
            {
                slot.missing = true;
                self->chainModel.push_back(slot);
                results->add(name + ": failed (" + err + ")");
                addDetail(name, "load_failed", err, resolvedName);
            }
            else
            {
                int hostIdx = self->chainHost.getNumSlots() - 1;
                slot.hostIdx = hostIdx;
                self->chainHost.setSlotSettings(hostIdx, slot.settings);
                // Auto-apply, the same three lines the LOCAL build path has had
                // since the dial work shipped (PluginEditor loadChainFromJson).
                // setSlotStructuredSettings ignores a void/non-object, so a
                // prose-only slot is unchanged.
                if (structuredForSlot.getDynamicObject() != nullptr
                    || structuredForSlot.isArray())
                    self->chainHost.setSlotStructuredSettings(hostIdx, structuredForSlot);
                self->chainHost.setSlotWet(hostIdx, slot.wet, ChainHost::WetSource::Restore);
                if (wantBypass)
                    self->chainHost.setSlotBypassed(hostIdx, true);
                // 4 Oct 2026 (ruled): the slot's own gains, restored with it. A build command carries 0.0 for
                // both, which is the neutral it would have had anyway, so only a session restore moves them.
                // From the BUILD ITEM, not the model spec: ChainSlotSpec is the display model and deliberately
                // mirrors only what the panel draws, while the gains are restore payload.
                if (restoreOutGainDb != 0.0f) self->chainHost.setSlotOutGainDb(hostIdx, restoreOutGainDb);
                if (restorePreTrimDb != 0.0f) self->chainHost.setSlotPreTrimDb(hostIdx, restorePreTrimDb);
                // Restore the hosted plugin's saved state (session restore)
                if (stateB64.isNotEmpty())
                {
                    juce::MemoryOutputStream mo;
                    if (juce::Base64::convertFromBase64(mo, stateB64))
                        if (auto* p = self->chainHost.getSlotProcessor(hostIdx))
                            p->setStateInformation(mo.getData(), (int)mo.getDataSize());
                }
                self->chainModel.push_back(slot);
                results->add(name + ": ok");
                addDetail(name, "built", {}, resolvedName);
            }
            self->notifyChainModel();
            (*stepPtr)();
        });
    };
    (*stepPtr)();
}

bool LinkProcessor::isPluginDisabledByName(const juce::String& name) const
{
    auto disabled = loadDisabledUids();
    auto mk = [](const juce::String& n)
        { return n.trim().toLowerCase().replaceCharacter(' ', '_') + "_"; };
    auto k1 = mk(name), k2 = mk(ChainHost::stripParenthetical(name));
    for (auto& uid : disabled)
        if (uid.startsWith(k1) || uid.startsWith(k2)) return true;
    return false;
}

void LinkProcessor::addChainPluginManually(const juce::PluginDescription& desc,
                                           std::function<void(const juce::String&)> done)
{
    if (rackLockGuard("add"))
    {
        if (done) done(rackEditPendingHeld()
            ? "An edit from \"" + rackLockOwner_ + "\" is still pending - "
              "retry or end the session there to release this rack."
            : "This rack is selected on \"" + rackLockOwner_
                       + "\" - deselect there to edit here.");
        return;
    }
    if ((int)chainModel.size() >= kMaxChainSlots)
    {
        if (done) done("Chain is full (" + juce::String(kMaxChainSlots) + " slots max)");
        return;
    }
    if (chainBuilding)
    {
        if (done) done("A chain build is in progress");
        return;
    }
    // NEW instantiation — popout-only AUs may swap to their VST3 build
    auto hostDesc = chainHost.preferInlineHostableDesc(desc);
    // USER: the sole caller is the Link's own plugin picker
    // (LinkEditor.cpp:516), which is a person choosing a plugin.
    chainHost.loadPluginAsync(hostDesc, ChainHost::LoadOrigin::User,
        [this, done, name = desc.name](const juce::String& err)
    {
        if (err.isNotEmpty()) { if (done) done(err); return; }
        // No settings guidance on manual adds — the empty string persists in
        // state like AI-provided guidance does (chainModelToVar serialises
        // the slot as-is), so the placeholder survives save/reopen.
        ChainSlotSpec slot;
        slot.name    = name;
        slot.hostIdx = chainHost.getNumSlots() - 1;
        chainModel.push_back(slot);
        stampLocalRackEdit();
        // Latency: chainHost.onChainChanged already ran updateChainLatency
        // during the graph rebuild. notifyChainModel refreshes the editor
        // and marks host state dirty — the same path command builds use.
        notifyChainModel();
        if (done) done({});
    });
}

void LinkProcessor::removeChainSlot(int idx)
{
    if (rackLockGuard("remove")) return;
    if (idx < 0 || idx >= (int)chainModel.size()) return;
    if (onChainAboutToChange) onChainAboutToChange();
    int hostIdx = chainModel[(size_t)idx].hostIdx;
    if (hostIdx >= 0)
    {
        chainHost.removeSlot(hostIdx);
        for (auto& s : chainModel)
            if (s.hostIdx > hostIdx) --s.hostIdx;
    }
    chainModel.erase(chainModel.begin() + idx);
    stampLocalRackEdit();
    notifyChainModel();
}

void LinkProcessor::moveChainSlot(int idx, int dir)
{
    if (rackLockGuard("move")) return;
    int j = idx + dir;
    if (idx < 0 || idx >= (int)chainModel.size()) return;
    if (j < 0 || j >= (int)chainModel.size()) return;
    // Host order = real model slots in order, so a host move is only needed
    // when BOTH swapped slots are real (adjacent real slots are adjacent in
    // the host too — missing slots don't exist there)
    auto& a = chainModel[(size_t)idx];
    auto& b = chainModel[(size_t)j];
    if (a.hostIdx >= 0 && b.hostIdx >= 0)
    {
        chainHost.moveSlot(a.hostIdx, dir);
        std::swap(a.hostIdx, b.hostIdx);
    }
    std::swap(a, b);
    stampLocalRackEdit();
    notifyChainModel();
}

void LinkProcessor::toggleChainSlotBypass(int idx)
{
    if (rackLockGuard("bypass")) return;
    if (idx < 0 || idx >= (int)chainModel.size()) return;
    auto& s = chainModel[(size_t)idx];
    s.bypassed = !s.bypassed;
    if (s.hostIdx >= 0)
        chainHost.setSlotBypassed(s.hostIdx, s.bypassed);
    stampLocalRackEdit();
    notifyChainModel();
}

void LinkProcessor::setChainSlotWet(int idx, float wet01)
{
    if (rackLockGuard("slot wet")) return;
    if (idx < 0 || idx >= (int)chainModel.size()) return;
    auto& s = chainModel[(size_t)idx];
    s.wet = juce::jlimit(0.0f, 1.0f, wet01);   // model copy = serialisation source
    if (s.hostIdx >= 0)
        chainHost.setSlotWet(s.hostIdx, s.wet, ChainHost::WetSource::Restore);
    stampLocalRackEdit();
}

float LinkProcessor::getChainSlotWet(int idx) const
{
    if (idx < 0 || idx >= (int)chainModel.size()) return 1.0f;
    return chainModel[(size_t)idx].wet;
}

void LinkProcessor::commitChainWetChange()
{
    // One host re-snapshot per gesture — same signal the other chain
    // mutations send via notifyChainModel, minus the model rebuild.
    updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true));
}

// ---- Chain state serialise / restore ---------------------------------------
juce::var LinkProcessor::chainModelToVar() const
{
    juce::Array<juce::var> arr;
    for (auto& s : chainModel)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("name",     s.name);
        o->setProperty("settings", s.settings);
        o->setProperty("bypassed", s.bypassed);
        o->setProperty("missing",  s.missing);
        o->setProperty("wet",      (double)s.wet);
        // 4 Oct 2026 (ruled), ADDITIVE, three fields that restoreChainFromVar was already prepared for or needed:
        //
        //   settings_structured   restoreChainFromVar READS it (item.structured) and this writer never wrote it,
        //                         so every Link session restore silently degraded structured dialling to the
        //                         prose text. Written from the slot's own structured settings when it has them.
        //   outGainDb / preTrimDb the hold's write and the drive's write. In no save format at all until today, so
        //                         a reopened Link session lost its compressor builds' level match.
        //
        // Absent on an older chunk means default - prose-only settings, and 0.0 gains - which is exactly what
        // those sessions restored to before, so nothing that loads today changes.
        if (! s.missing && s.hostIdx >= 0)
        {
            const auto structured = chainHost.getSlotStructured(s.hostIdx);
            if (! structured.isVoid())
                o->setProperty("settings_structured", structured);
            o->setProperty("outGainDb", (double) chainHost.getSlotOutGainDb(s.hostIdx));
            o->setProperty("preTrimDb", (double) chainHost.getSlotPreTrimDb(s.hostIdx));
        }
        if (!s.missing && s.hostIdx >= 0)
        {
            if (auto* p = chainHost.getSlotProcessor(s.hostIdx))
            {
                juce::MemoryBlock mb;
                p->getStateInformation(mb);
                if (mb.getSize() > 0)
                    o->setProperty("state", juce::Base64::toBase64(mb.getData(), mb.getSize()));
            }
        }
        arr.add(juce::var(o));
    }
    return juce::var(arr);
}

void LinkProcessor::restoreChainFromVar(const juce::var& v)
{
    auto* arr = v.getArray();
    if (arr == nullptr || arr->isEmpty()) return;

    std::vector<ChainBuildItem> spec;
    for (auto& sv : *arr)
    {
        auto* o = sv.getDynamicObject();
        if (o == nullptr) continue;
        ChainBuildItem item;
        item.name        = o->getProperty("name").toString();
        item.settings    = o->getProperty("settings").toString();
        item.structured  = o->getProperty("settings_structured");
        item.bypassed    = (bool)o->getProperty("bypassed");
        item.wet         = o->hasProperty("wet")
                             ? juce::jlimit(0.0f, 1.0f, (float)(double)o->getProperty("wet"))
                             : 1.0f;   // pre-wet/dry sessions restore fully wet
        item.stateBase64 = o->getProperty("state").toString();
        // 4 Oct 2026: additive. Absent = 0.0, which is what every pre-today chunk restored to.
        item.outGainDb   = (float) (double) o->getProperty("outGainDb");
        item.preTrimDb   = (float) (double) o->getProperty("preTrimDb");
        if (item.name.isNotEmpty())
            spec.push_back(std::move(item));
    }
    if (!spec.empty())
        // RESTORE: rebuilding a rack from a saved var, not a build.
        buildChainFromSpec(std::move(spec), ChainHost::LoadOrigin::Restore,
                           nullptr);   // missing → named slot, no crash
}

// =============================================================================
//  Chain transport — command/ack files in the shared link directory
// =============================================================================
juce::String LinkProcessor::chainInstanceId() const
{
    // The command/ack ADDRESS is the per-instance uid — never the name
    // (unnamed/duplicate names collapsed every same-named Link onto one
    // command file, so a toggle addressed to one applied to all)
    return instanceUid_;
}

void LinkProcessor::pollChainCommand()
{
    auto id = chainInstanceId();
    if (id.isEmpty()) return;                     // unnamed Link — no identity

    if (resolvedDir.isEmpty())
    {
        int err = 0;
        resolvedDir = LinkShm::resolveDir(err);
        if (resolvedDir.isEmpty()) return;
    }

    juce::File cmdFile(resolvedDir + "chain-cmd-" + id + ".json");
    if (!cmdFile.existsAsFile()) return;

    // ---- ONE BUILD AT A TIME, BUT NEVER IN SILENCE (18 Oct 2026, queue items 21, 16 and 13) ---------------
    //
    // WHAT THIS LINE USED TO BE. `if (chainBuilding) return;` stood at the top of this function, above even the
    // test for a command file, so a command that arrived while this rack was being rebuilt was neither applied
    // nor answered nor logged. The sender waits 8 s for an ack that was never going to be written and then
    // tells the user, in the agent's own words from the gate's S6 log: *"the rack never acknowledged it within
    // 8 s - it may be gone, or its track may be disabled"*. Neither was true. The rack was mid-build, from the
    // row before, and it applied this very command a few seconds later - after its outcome had been reported
    // as a failure.
    //
    // The local path has always done this properly: the Link's own plugin picker answers "A chain build is in
    // progress" (addPluginByDesc, above). Only the remote road went quiet, and silence is the one answer this
    // transport has been ruled never to give - the O2 note further down this function says so about a seq that
    // burned, in those words, three weeks ago.
    //
    // SO: ACK "busy", AND DO NOT CONSUME. The command file stays exactly where it is, so the build that is
    // running finishes and the NEXT poll applies it for real and acks its true outcome. The busy ack is not a
    // verdict; it is this rack saying "I have your command, I am not free yet", which is the one fact the
    // waiting side needs in order to wait instead of inventing a reason. Written once per seq, not once per
    // poll (this runs ~10x a second and a build can take tens of seconds).
    if (chainBuilding)
    {
        const auto v    = juce::JSON::parse (cmdFile.loadFileAsString());
        const int  bseq = (int) v.getProperty ("seq", 0);
        const double forMs = chainBuildStartedMs_ > 0.0
                           ? juce::Time::getMillisecondCounterHiRes() - chainBuildStartedMs_ : 0.0;
        // ONLY FOR A SENDER THAT ASKED FOR IT, which is what keeps this additive. A main plugin from before
        // today treats every status that is not "ok" as a refusal and prints the reason, so an unasked-for
        // busy ack would turn a command that is about to succeed into a reported failure in an OLD V2. The
        // flag is written by writeChainEditCommand in this same round, so "absent" means "that sender cannot
        // understand this answer" - and the honest behaviour for it is the one it already has.
        const bool wantsBusy = (bool) v.getProperty ("wantBusyAck", false);
        if (wantsBusy && bseq != 0 && bseq != chainBusyAckedSeq_)
        {
            chainBusyAckedSeq_ = bseq;
            EchoJay_NSLog (("EJLink: chain-cmd seq=" + juce::String (bseq) + " arrived while this rack is still "
                            "building (" + juce::String (forMs / 1000.0, 1) + " s so far) - acked BUSY and KEPT "
                            "on disk; it is applied when the build ends").toRawUTF8());
            writeChainAck (bseq, "busy",
                           { "the rack is still building and has not read this command yet" }, {},
                           "a rebuild of this rack has been running for "
                           + juce::String (forMs / 1000.0, 1) + " s; the command is queued, not refused");
        }
        return;
    }

    auto v = juce::JSON::parse(cmdFile.loadFileAsString());
    auto* obj = v.getDynamicObject();
    if (obj == nullptr) { cmdFile.deleteFile(); return; }   // malformed — drop

    int ver = (int)obj->getProperty("v");
    int seq = (int)obj->getProperty("seq");
    // 20 Sep 2026 (idempotent): a command whose id was already applied is acked "ok" again and NOT re-applied
    const juce::String cmdId = obj->getProperty("id").toString();
    if (cmdId.isNotEmpty() && appliedChainIds_.contains(cmdId))
    {
        cmdFile.deleteFile();
        EchoJay_NSLog(("EJLink: repeat id=" + cmdId + " seq=" + juce::String(seq) + " - already applied, acked ok again, NOT re-applied").toRawUTF8());
        writeChainAck(seq, "ok", { "already applied (id " + cmdId + ")" }, {});
        return;
    }
    if (ver != 1 && ver != 2)
        return;   // unknown version — leave for inspection (older Links reject v:2 the same way: forward-safe)
    if (seq == 0)
        return;
    // O2 (21t-b, 25 Sep 2026): THIS BRANCH USED TO RETURN IN SILENCE, and that is what locked a user out of a
    // rack for six minutes. An edit that ABORTED still burned its seq here (it is stamped below, before the
    // apply runs), and the id-based ack-again path above only records ids for edits that were NOT aborted - so
    // every re-send of a refused edit matched this test, returned with no ack, and the sender waited for an
    // answer that could never come. Two corrections: the abort now CLEARS the stamp (see the callback below), so
    // a genuine retry is judged afresh; and a repeat of a seq that really was applied is ANSWERED, the same rule
    // the ctrl-cmd path has carried since 24 Aug 2026 - silence is never a valid response.
    if (seq == lastAppliedChainSeq_)
    {
        cmdFile.deleteFile();
        EchoJay_NSLog(("EJLink: repeat seq=" + juce::String(seq)
                       + " - already applied, acked ok again, NOT re-applied").toRawUTF8());
        writeChainAck(seq, "ok", { "already applied (seq " + juce::String(seq) + ")" }, {});
        return;
    }

    lastAppliedChainSeq_ = seq;
    cmdFile.deleteFile();   // consumed

    // ---- v:2 editOps (Phase 1c): structural edits on the EXISTING chain ----
    // Same shared sequencer as the main plugin (ChainHost::applyChainEdits,
    // staleness-guarded, stop-at-failure). Editors close first via
    // onChainAboutToChange, sequencer starts a beat later (AMEK discipline).
    if (ver == 2)
    {
        // O2 (21t-b): every path that does NOT apply releases the seq stamp, or the retry of a refused command
        // is mistaken for a repeat of an applied one.
        auto failChain = [this, seq](const juce::String& why)
        { lastAppliedChainSeq_ = 0; writeChainAck(seq, "failed", { why }, {}); };
        if (!obj->hasProperty("editOps"))
        { failChain("v2 command without editOps"); return; }
        // A leased rack refuses STRUCTURE. The lease saved one slot's bypass
        // state and will restore it BY INDEX; an edit that removes or moves
        // slots underneath it would make that restore hit the wrong plugin.
        // Refused with a reason, never queued.
        // 20 Sep 2026 (the deleted-chain bug): the LEASE HOLDER's own structural edits arrive here at once (chain-cmd v:2 with
        // this lease's id) and are applied under the lease; anyone else's structure command is still refused while leased.
        const juce::String cmdLease = obj->getProperty("leaseId").toString();
        const bool fromLeaseHolder = leaseActive_.load(std::memory_order_relaxed) && cmdLease.isNotEmpty() && cmdLease == leaseGate_.activeId;
        if (leaseActive_.load(std::memory_order_relaxed) && ! fromLeaseHolder)
        { failChain("this rack is being edited from the main plugin - try again after release"); return; }

        juce::StringArray baseSlots;
        if (auto* bs = obj->getProperty("baseSlots").getArray())
            for (auto& bv : *bs) baseSlots.add(bv.toString().trim());

        std::vector<ChainHost::ChainEditOp> ops;
        {
            // Reuse the parser via a synthetic payload so op-shape rules
            // stay single-source in ChainHost::parseChainEditOps
            auto* wrap = new juce::DynamicObject();
            wrap->setProperty("edit", obj->getProperty("editOps"));
            ops = ChainHost::parseChainEditOps(juce::JSON::toString(juce::var(wrap), true));
        }
        if (ops.empty())
        { failChain("editOps empty or malformed"); return; }

        // ---- 21t-j (28 Sep 2026 ruling): LEVEL MATCH IS THIS LINK'S OWN TRIM, NOT A RACK EDIT ----------------
        // Sean's 10:10:34 log: five Links answered "applied level_match count=0->0 ... ABORTED applied=0/1". Why,
        // from the code: parseChainEditOps built an op with op="level_match", slot=-1 and (since 21t-i) a member
        // COUNT but no members; ChainHost::applyChainEdits then dry-runs every op against a simulated RACK and
        // ends `else return bad("unknown operation ...")` - level_match is none of add/remove/replace/move/
        // bypass/set/set_wet - so the batch aborted before a single op ran. It never looked anything up, because
        // the thing it had to move is not a rack slot at all: it is gainDb_, this plugin's own trim.
        //
        // So the op is taken OUT of the rack batch and applied here, by this Link, to itself: find this instance's
        // uid among the members and move the trim by that member's delta. The rack sequencer's vocabulary is
        // unchanged - a level match was never a rack edit.
        int lmMoved = 0, lmSeen = 0; juce::String lmWhy; juce::StringArray lmResults;
        {
            std::vector<ChainHost::ChainEditOp> rest;
            for (auto& op : ops)
            {
                if (op.op != "level_match") { rest.push_back (op); continue; }
                ++lmSeen;
                auto* arr = op.members.getArray();
                if (arr == nullptr) { lmWhy = "the level_match op carries no members"; continue; }
                bool mine = false;
                for (const auto& mv : *arr)
                {
                    auto* mo = mv.getDynamicObject();
                    if (mo == nullptr) continue;
                    if (mo->getProperty ("uid").toString().trim() != instanceUid_) continue;
                    mine = true;
                    const double delta = (double) mo->getProperty ("delta_db");
                    const auto intV = mo->getProperty ("int_lufs");
                    if (intV.isVoid() || std::abs (delta) < 0.005)
                    {
                        lmWhy = intV.isVoid() ? "no reading for this channel, so nothing to match it to"
                                              : "delta 0.0 dB";
                        lmResults.add ("level_match: left alone (" + lmWhy + ")");
                        break;
                    }
                    const float before = gainDb_.load (std::memory_order_relaxed);
                    const float after  = juce::jlimit (kGainMinDb, kGainMaxDb, (float) (before + delta));
                    setGainDb (after);
                    ++lmMoved;
                    lmResults.add ("level_match: trim " + juce::String (before, 2) + " -> "
                                   + juce::String (after, 2) + " dB");
                    EchoJay_NSLog(("EJLink: level_match moved THIS Link's trim " + juce::String (before, 2)
                                   + " -> " + juce::String (after, 2) + " dB (delta " + juce::String (delta, 2)
                                   + ", seq " + juce::String (seq) + ")").toRawUTF8());
                    break;
                }
                if (! mine)
                {
                    lmWhy = "this Link (" + instanceUid_ + ") is not named in the members";
                    lmResults.add ("level_match: not for this channel");
                    EchoJay_NSLog(("EJLink: level_match carried " + juce::String ((int) arr->size())
                                   + " member(s), none of them this Link (" + instanceUid_ + ")").toRawUTF8());
                }
            }
            ops.swap (rest);
        }
        // NOTHING BUT LEVEL MATCH: answer here. An abort is not a staleness, and a move is not a rack change.
        if (lmSeen > 0 && ops.empty())
        {
            updateShmState();                      // the new trim reaches the registry, which is what V2 reads back
            const juce::String status = lmMoved > 0 ? "ok" : "not_applied";
            EchoJay_NSLog(("EJLink: applied level_match count=" + juce::String (chainHost.getNumSlots()) + "->"
                           + juce::String (chainHost.getNumSlots()) + " seq=" + juce::String (seq)
                           + " moved=" + juce::String (lmMoved) + "/" + juce::String (lmSeen)
                           + (lmMoved > 0 ? juce::String() : " (" + lmWhy + ")")).toRawUTF8());
            writeChainAck (seq, status, lmResults, lmMoved > 0 ? juce::String() : lmWhy);
            return;
        }

        // ---- 11 Oct 2026: THE LIST THE OPS ARE JUDGED AGAINST -------------------------------------------
        // An `add` resolves its name through resolveOfferedName -> resolveByName, which answers built-ins from
        // the compiled registry and EVERYTHING ELSE from `entries_`. On a Link, `entries_` is only ever filled
        // from the shared chain_entries.xml, and until now the ONE place that loaded it was buildChainFromSpec -
        // the v:1 chain path. So a v:2 edit naming a third-party plugin was judged against an EMPTY list, the
        // dry run refused it as "not in the loadable plugin list", and the whole batch aborted - which the
        // sender reads as `not_applied`, keeps pending, and re-sends at deselect. That is exactly Sean's report:
        // a chain built on an addressed Link does not arrive until the V2 switches racks, and only built-in
        // slots ever worked. (buildRecommendable, the other name source, is called from PluginEditor.cpp alone,
        // so a Link has no feed table at all - the cache is the whole of its knowledge.)
        //
        // mtime-gated inside, and reached only when a command has actually arrived, so this is one stat per
        // command rather than anything on a timer.
        chainHost.maybeReloadEntriesCache();
        EchoJay_NSLog (("EJEdit: judging " + juce::String ((int) ops.size()) + " op(s) against "
                        + juce::String (chainHost.getNumPlugins()) + " loadable entr(ies)"
                        + (chainHost.getNumPlugins() == 0
                               ? juce::String (" - EMPTY, so only built-ins can resolve")
                               : juce::String())).toRawUTF8());

        if (onChainAboutToChange) onChainAboutToChange();   // editors close first
        auto self = this;   // processor outlives message-thread callbacks in-session
        // ITEM 4 (14 Oct 2026): a push that IS a build says so (additive `intent`; absent on an older sender,
        // and then this behaves exactly as it did - no arming, which is today's behaviour).
        const bool pushIsBuild = obj->getProperty("intent").toString().trim().equalsIgnoreCase("build");
        // ---- ITEM 2 (19 Oct 2026): AN EXPLICIT OUT TRIM ENDS AN ARMED MATCH ---------------------
        // *"If B also sets the OUT trim from a check, B's explicit value wins; don't fight it."*  The
        // same-batch case is caught by the sender (EJAgentHome::intentForOps withdraws the intent);
        // this is the LATER command, arriving while a match armed by an earlier add is still waiting
        // for audio. Checked here, on the parsed ops, before they are moved into the applier.
        const bool namesAnOutTrim = std::any_of (ops.begin(), ops.end(),
                                                 [] (const auto& o) { return o.op == "slot_out"; });
        // ---- level_match:false ON AN ADD (B's contract of 20 Oct 2026) -----------------------
        //
        // *"When an add carries level_match: false, skip the silent slot-OUT match for that slot."*
        // Read from the raw op for the same reason dial_on_playback is: ChainEditOp has no field for
        // it and does not need one - the only thing the arm has to know is whether any add in this
        // batch said no.
        bool levelMatchOff = false;
        if (auto* ea = obj->getProperty ("editOps").getArray())
            for (const auto& ov : *ea)
                if (auto* oo = ov.getDynamicObject())
                    if (const auto lm = oo->getProperty ("level_match"); lm.isBool() && ! (bool) lm)
                    { levelMatchOff = true; break; }
        // ---- dial_on_playback (20 Oct 2026, B's CONTRACT_dial_on_playback_2026-10-20.md) --------
        //
        // *"when an add carries dial_on_playback (target dB + which control) and nothing had been
        // heard, set that slot on the next playback with no chat round, like the slot-OUT match."*
        //
        // CARRIED PAST THE PARSE, because ChainEditOp has no field for it and does not need one: the
        // raw op object is right here, and the only thing the apply has to tell us afterwards is
        // WHICH slot the add landed on - which nameTheMatchTarget already resolves by identity for
        // the level match (his 17:09 item 2).
        // ---- EVERY DIAL IN THE BATCH, NOT THE FIRST (his 17:3x item 3, 21 Oct 2026) ----------
        //
        // *"B will send dial_on_playback on every compressor after the first in a build ... Confirm
        // that works for several slots in one build (not just the last), each dialled
        // independently."*
        //
        // The `break` on this line is what it did not: one dial per batch, so a build with three
        // compressors dialled one and dropped two without a word. The reader is shared now
        // (LoudnessLoop::dialsOnPlaybackFromOps) and keeps each op's NAME, so every dial resolves to
        // its own slot by identity.
        const auto dialsInBatch = LoudnessLoop::dialsOnPlaybackFromOps (obj->getProperty ("editOps"));
        juce::var dialOnPlayback;          // the LIMITER target, if one of them is one
        for (const auto& d : dialsInBatch)
            if (LoudnessLoop::dialIsLimiterTarget (d.dial)) { dialOnPlayback = d.dial; break; }
        juce::Timer::callAfterDelay(80, [self, ops, baseSlots, seq, cmdId, pushIsBuild, namesAnOutTrim,
                                         dialOnPlayback, dialsInBatch, levelMatchOff]() mutable
        {
            const int countBefore = self->chainHost.getNumSlots();
            // A COPY BEFORE THE MOVE: applyChainEdits takes the ops by value, and the completion
            // needs to know which plugin this change added (his 17:09 item 2).
            const auto opsForTarget = ops;
            const juce::String opNames = [&ops] { juce::StringArray o; for (const auto& op : ops) o.add(op.op + (op.slot >= 0 ? "@" + juce::String(op.slot + 1) : juce::String())); return o.joinIntoString(","); }();
            self->chainHost.applyChainEdits(std::move(ops), -1, baseSlots,
                [self, seq, countBefore, opNames, cmdId, pushIsBuild, namesAnOutTrim, opsForTarget,
                 dialOnPlayback, dialsInBatch, levelMatchOff](const juce::StringArray& results, int applied, bool aborted)
            {
                if (! aborted && cmdId.isNotEmpty()) { self->appliedChainIds_.add(cmdId); while (self->appliedChainIds_.size() > 64) self->appliedChainIds_.remove(0); }
                // O2 (21t-b): an ABORTED edit releases its seq stamp. It was never applied, so a retry must be
                // judged on the rack as it stands - not swallowed as "already applied".
                if (aborted && self->lastAppliedChainSeq_ == seq) self->lastAppliedChainSeq_ = 0;
                if (! aborted) ++self->chainCmdApplied_;
                // 11 Oct 2026: nothing to keep in step. The priors followed the edited rack here so the release
                // would restore the right value to the right plugin; there is no release and no priors.
                self->syncModelAfterStructuralChange();
                EchoJay_NSLog(("EJLink: applied " + opNames + " count=" + juce::String(countBefore) + "->" + juce::String(self->chainHost.getNumSlots())
                               + " model=" + juce::String((int) self->chainModel.size()) + " lease=" + juce::String((int) self->rackLeaseScope_)
                               + " seq=" + juce::String(seq) + (aborted ? " ABORTED" : "") + " applied=" + juce::String(applied) + "/" + juce::String(results.size())).toRawUTF8());
                // 21t-j (28 Sep 2026 ruling): AN ABORT IS NOT A STALENESS. "stale" means the rack moved under the
                // edit, and the line above it in Sean's log said the staleness guards had PASSED - so the one word
                // the reader had contradicted the one before it. An aborted batch is `not_applied`, and the reason
                // the sequencer already produced travels with it instead of an empty string.
                // 11 Oct 2026: `applied == results.size()` ALSO HELD FOR 0 == 0, so an edit that ran nothing
                // and produced not one per-op result acked "ok". That is why this bug reached Sean as "it says
                // it worked": the sender rebased its mirror on a success the Link had not had. An empty result
                // set is not a success, it is an edit that never ran.
                juce::String status = aborted ? "not_applied"
                                    : results.isEmpty() ? "not_applied"
                                    : (applied == results.size() ? "ok" : "partial");
                const juce::String why = aborted
                    ? (results.isEmpty() ? juce::String ("the edit was refused before any op ran")
                                         : results.joinIntoString ("; "))
                    : juce::String();
                // THE REASON IS THE FIFTH ARGUMENT. The fourth is a per-plugin ARRAY and a string handed to it is
                // dropped without a word - which is how the first cut of this fix acked not_applied with an empty
                // reason, and the guard caught it.
                // The resulting SLOT COUNT rides the ack (additive field; an older sender ignores it). The
                // editor's note at the push site always meant to make a divergence readable "instead of
                // inferred" - but the sender could only see its own count until now.
                self->writeChainAck(seq, status, results, juce::var(), why, self->chainHost.getNumSlots());
                // ITEM 4: AFTER the ack, and only for a clean BUILD push - the rack the loop is about to
                // measure has to be the built one, and a partial or aborted batch is not it.
                if (pushIsBuild && status == "ok")
                {
                    self->nameTheMatchTarget (opsForTarget);   // AFTER the apply: the rack holds it now
                    // B SAID NO, SO NO. The field is explicit and outranks the rack-shape test inside
                    // armLoudnessAfterBuild: B knows this is a loudness build, we can only infer it.
                    if (levelMatchOff)
                        EchoJay_NSLog ("EJLoudness: NOT arming a post-add match - this add carried "
                                       "level_match:false, so the slot's OUT stays at 0 dB and the "
                                       "limiter keeps the level it set (B's contract of 20 Oct)");
                    else
                        self->armLoudnessAfterBuild ("a borrowed-rack build push (v2 editOps, intent=build)");
                }
                // ...and an explicit OUT trim withdraws one that an earlier add armed (Sean's item 2).
                else if (namesAnOutTrim && status != "not_applied")
                    self->loudnessLoop_.standDownQuietly ("the command named a slot OUT trim itself");
                // ---- AND THE AMOUNT CONTROL, ON THE NEXT PLAYBACK (B's contract, 20 Oct) -----------
                //
                // *"when an add carries dial_on_playback (target dB + which control) and nothing had
                // been heard, set that slot on the next playback with no chat round, like the slot-OUT
                // match."*
                //
                // TRANSLATED INTO A CALIBRATION BLOCK rather than given a mechanism of its own, and the
                // contract's own last line is why: *"Agent off already sends a target block
                // (gr_target_db with calibration), which the plugin dials during playback. No change is
                // needed there."*  That machinery waits for audio, seeks the band, says nothing on the
                // way in (Purpose::buildHold leaves askOwed empty, which is what startCalibFromBlock
                // forces) and closes when it lands - which is what "like the slot-OUT match" asks for.
                // A second road to the same place is how two of this week's defects happened.
                //
                // The translation itself is CalibLoop::blockFromDialOnPlayback, beside the parser that
                // reads the shape back: the three fields that are not one-to-one, and why the control's
                // direction is never guessed from its name, are written out there.
                //
                // AND ONE WRITER PER CONTROL: the calibration hold writes this slot's OUT (the 29 Sep
                // ruling - EchoJay's own OUT, never the plugin's output), and so does the slot-OUT match
                // this same build armed four statements up. Two loops writing one control, each
                // measuring the other's write, is the 21t-m failure. The match stands down for the same
                // reason an explicit slot_out command stands it down: something else owns this slot's
                // level now. The cost is that a dialled slot does not get the 09:24 measured peak
                // ceiling, which the hold has no notion of - stated here, and owed.
                if (status == "ok")
                {
                    // ---- B'S LIMITER TARGET TAKES THE OTHER ROAD (his evening contract) -----
                    //
                    // The same field carries two different jobs now: a COMPRESSOR's gain-reduction
                    // target (target_gr_db -> a calibration block) and a LIMITER's loudness target
                    // (target_lufs -> the loudness loop at the limiter's INPUT). They are told apart
                    // by which key is present, because that is what B sends, and nothing else about
                    // the two is alike: one dials a control to a GR figure on the loud phrases, the
                    // other drives level into a ceiling.
                    if (auto* dpo = dialOnPlayback.getDynamicObject())
                        if (const auto tl = dpo->getProperty ("target_lufs");
                            tl.isDouble() || tl.isInt() || tl.isInt64())
                        {
                            // ---- A DIAL RESOLVES ITS OWN SLOT (his 11:5x item 1, 21 Oct 2026) --
                            //
                            // *"Arm your limiter target loop whenever B's add carries
                            // dial_on_playback target_lufs, even after a heard build."*
                            //
                            // What stood here was `pushIsBuild ? matchTargetSlot_ : -1`, and -1 means
                            // armTargetFromDial REFUSES ("named no slot this rack has"). So a limiter
                            // added by an ordinary add - not a whole build - carried a target that
                            // could never arm. The guard against a stale member was right; the answer
                            // was not to withhold the slot but to RESOLVE IT FROM THIS BATCH, which
                            // nameTheMatchTarget does from the ops in hand.
                            if (! pushIsBuild) self->nameTheMatchTarget (opsForTarget);
                            const int added = self->matchTargetSlot_;
                            const auto ctl  = dpo->getProperty ("control").toString().trim();
                            const auto ceilV = dpo->getProperty ("ceiling");
                            const float ceil = (ceilV.isDouble() || ceilV.isInt() || ceilV.isInt64())
                                                 ? (float) (double) ceilV : -0.1f;
                            // ---- B'S LIMITING BUDGET (Sean's ruling, 21 Oct 2026) -------------
                            // *"B will send a limiting budget with the target (e.g. "avg_limit_db":
                            // 3, "peak_limit_db": 6)."*  Absent means that half is not bounded, so
                            // the fields are read as NaN rather than defaulted to a number nobody
                            // sent - the same rule every optional figure on this wire follows.
                            const auto numOr = [dpo] (const char* key)
                            {
                                const auto v = dpo->getProperty (key);
                                return (v.isDouble() || v.isInt() || v.isInt64())
                                         ? (float) (double) v : std::numeric_limits<float>::quiet_NaN();
                            };
                            const float avgLim  = numOr ("avg_limit_db");
                            const float peakLim = numOr ("peak_limit_db");
                            if (self->loudnessLoop_.armTargetFromDial ((float) (double) tl, added, ctl, ceil,
                                                                       avgLim, peakLim))
                                EchoJay_NSLog (("EJLoudness: dial_on_playback carried a LIMITER target - "
                                                + juce::String ((double) tl, 1) + " LUFS on slot "
                                                + juce::String (added + 1) + " (\"" + self->matchTargetName_
                                                + "\"), driving its INPUT and nothing after it (his "
                                                  "correction of 20 Oct), silently, on the next "
                                                  "playback").toRawUTF8());
                            else
                                EchoJay_NSLog ("EJLoudness: dial_on_playback carried a limiter target but it "
                                               "could not be armed - see the line above for why. The slot "
                                               "keeps its input at 0, which is what the contract says a build "
                                               "that does not know the field does.");
                            return;   // a limiter target is not a compressor dial: one road each
                        }
                    juce::String dialWhy;
                    // ...AND ONLY WHEN THIS BATCH RESOLVED THE SLOT. matchTargetSlot_ is a member, so
                    // without the pushIsBuild test a dial arriving on a push that is not a build would
                    // dial WHATEVER THE LAST BUILD ADDED. A stale pointer is worse than no pointer, and
                    // nameTheMatchTarget only runs for a clean build push.
                    // ---- EVERY DIAL IN THE BATCH, ONE HOLD EACH (his 17:5x ruling, 21 Oct) ----
                    //
                    // *"several compressors dialled on one Link each get their own loop"*
                    //
                    // The collector (dialsOnPlaybackFromOps, shipped earlier today) already returns
                    // every dial with the NAME its op added. Each resolves to its own slot by
                    // identity, becomes its own Config, and the whole list goes to startCalibMany at
                    // once - because one `calibLoop_` means starting them one at a time would have
                    // each replace the last, which is exactly what his two-compressor vocal chains
                    // were hitting.
                    std::vector<echojay::CalibLoop::Config> cfgs;
                    for (const auto& d : dialsInBatch)
                    {
                        if (LoudnessLoop::dialIsLimiterTarget (d.dial)) continue;   // the other road
                        const int at = LoudnessLoop::slotTheBatchAdded (self->chainHost,
                                                                       juce::StringArray { d.name });
                        if (at < 0)
                        {
                            EchoJay_NSLog (("EJThreshold(Link): a dial names \"" + d.name + "\" but no "
                                            "slot on this rack answers to it - NOT dialled (a slot is "
                                            "an identity, not a position)").toRawUTF8());
                            continue;
                        }
                        juce::String why;
                        const auto blk = echojay::CalibLoop::blockFromDialOnPlayback (d.dial, at, why);
                        if (blk.getDynamicObject() == nullptr)
                        {
                            EchoJay_NSLog (("EJThreshold(Link): slot " + juce::String (at + 1)
                                            + " carried dial_on_playback but it could not be read as a "
                                              "calibration block - " + why).toRawUTF8());
                            continue;
                        }
                        // ...and on the road that SUCCEEDS, the same line (his 08:43): the sides it
                        // will move, and whether the done line is B's own.
                        if (why.isNotEmpty())
                            EchoJay_NSLog (("EJThreshold(Link): slot " + juce::String (at + 1) + " (\""
                                            + self->chainHost.getSlotInfo (at).name
                                            + "\") is DIALLED ON PLAYBACK - " + why.trim()).toRawUTF8());
                        echojay::CalibLoop::Config cfg;
                        juce::String whyCfg;
                        if (! echojay::CalibLoop::configFromBlock (blk, self->chainHost.getNumSlots(),
                                                                   /*bus*/ false,
                                                                   self->chainHost.getSlotInfo (at).name,
                                                                   cfg, whyCfg))
                        {
                            EchoJay_NSLog (("EJThreshold(Link): slot " + juce::String (at + 1)
                                            + "'s block is not usable - " + whyCfg.trim()).toRawUTF8());
                            continue;
                        }
                        if (whyCfg.isNotEmpty())
                            EchoJay_NSLog (("EJThreshold(Link): BLOCK NOT AS CONTRACTED (slot "
                                            + juce::String (at + 1) + ") - " + whyCfg.trim()).toRawUTF8());
                        cfg.slot   = at;
                        cfg.plugin = self->chainHost.getSlotInfo (at).name;
                        cfgs.push_back (cfg);
                    }
                    if (! cfgs.empty())
                    {
                        self->loudnessLoop_.standDownQuietly ("a calibration hold owns this slot's "
                                                              "OUT after dial_on_playback");
                        const int started = self->startCalibMany (cfgs);
                        EchoJay_NSLog (("EJThreshold(Link): " + juce::String (started) + " of "
                                        + juce::String ((int) cfgs.size()) + " dial(s) from this build "
                                          "started, each on its own slot (his 17:5x ruling)").toRawUTF8());
                    }
                    else if (! dialOnPlayback.isVoid() || ! dialsInBatch.empty())
                        EchoJay_NSLog (("EJThreshold(Link): dial_on_playback arrived but nothing was "
                                        "started - " + dialWhy + ". The slot keeps the gentle start the "
                                        "add carried, which is what the contract says a build that does "
                                        "not know the field does - said out loud rather than skipped in "
                                        "silence.").toRawUTF8());
                }
            });
        });
        return;
    }

    std::vector<ChainBuildItem> spec;
    if (auto* arr = obj->getProperty("chain").getArray())
    {
        for (auto& ev : *arr)
        {
            if (auto* eo = ev.getDynamicObject())
            {
                ChainBuildItem item;
                item.name     = eo->getProperty("name").toString().trim();
                item.settings = eo->getProperty("settings").toString();
                // THE COMMAND PATH, and the one that matters (11 Aug 2026).
                // There are TWO parses of a chain array in this file and the
                // first fix went to the wrong one: restoreChainFromVar is
                // SESSION RESTORE, while THIS is what reads
                // chain-cmd-<uid>.json -- the file a Build button writes. The
                // payload arrived correct end to end (1370 ch, 5 slots, 5 with
                // settings_structured, confirmed on disk) and was dropped
                // here, one function away from the fix.
                item.structured = eo->getProperty("settings_structured");
                // Saved-chain recall to a Link (15 Aug 2026): the recall
                // sender includes per-slot hosted state and bypass - the
                // SAME fields session restore has always carried one
                // function up (restoreChainFromVar), applied by the same
                // buildChainFromSpec. Build commands never set them, so a
                // build turn parses exactly as before.
                item.stateBase64 = eo->getProperty("state").toString();
                item.bypassed    = (bool) eo->getProperty("bypassed");
                // 4/5 Oct 2026: THE SLOT'S OWN GAINS, ON THE COMMAND PATH TOO.
                //
                // There are TWO parses of a chain array in this file, and the gains were added to the other one -
                // restoreChainFromVar, which is SESSION RESTORE. The V2 hands a saved borrowed rack back by writing
                // a chain-cmd, which lands HERE, so the gains travelled in the payload and were dropped on arrival:
                // the rack came back with its level match reset to 0. The file's own comment warns about exactly
                // this pair ("the first fix went to the wrong one"), and G6 caught it reading 0.00 / 0.00 against
                // the -2.5 / 1.5 that was sent. A build command carries neither key, so it is unaffected.
                item.outGainDb   = (float)(double) eo->getProperty("outGainDb");
                item.preTrimDb   = (float)(double) eo->getProperty("preTrimDb");
                if (eo->hasProperty("wet"))
                    item.wet = juce::jlimit(0.0f, 1.0f,
                                            (float)(double) eo->getProperty("wet"));
                // The model's slot-level "wet_pct" (0..100) on a chain block
                // built to this Link (16 Aug 2026): same knob, same default;
                // a non-number is absent. "wet" (0..1, recall/restore) wins
                // if both are present, which no sender does.
                else if (eo->hasProperty("wet_pct"))
                {
                    const auto wv = eo->getProperty("wet_pct");
                    if (wv.isDouble() || wv.isInt() || wv.isInt64())
                        item.wet = juce::jlimit(0.0f, 1.0f, (float)(double) wv / 100.0f);
                }
                if (item.name.isNotEmpty())
                    spec.push_back(std::move(item));
            }
        }
    }

    // RECEIVE SIDE, mirroring the sender's line so the hop is closed at both
    // ends. The main plugin logs "sending to Link -- N slots, N with
    // settings_structured"; this says what arrived. Two numbers that disagree
    // localise the loss to the file or the parse without another round of
    // reasoning about which of them it must be.
    {
        int withStructured = 0;
        for (const auto& it : spec)
            if (it.structured.getDynamicObject() != nullptr || it.structured.isArray())
                ++withStructured;
        EchoJay_NSLog(("EJChain(Link): parsed command -- " + juce::String((int) spec.size())
                       + " slot(s), " + juce::String(withStructured)
                       + " with settings_structured").toRawUTF8());
    }

    // 21t-g item 2: THE LOOP STARTS HERE FOR A RACK THIS LINK OWNS. V2 starts it for its own rack and for a
    // LEASED one, because those are the slots V2 can measure; a parked Link's rack is measured HERE, by the
    // process that owns it, so the block has to be read on this side too. Same header, same defaults (passive
    // unless the block says listen), same actuator rules - a loop that behaved differently depending on which
    // process happened to start it would be two loops.
    const juce::var calibBlock = v.getProperty("calibration", juce::var());

    // ASSISTANT: a build command arriving from the main plugin, which is
    // an EchoJay chain being placed on this channel.
    buildChainFromSpec(std::move(spec), ChainHost::LoadOrigin::Assistant,
        [this, seq, calibBlock](const juce::StringArray& results, const juce::var& detail)
    {
        int failures = 0;
        for (auto& r : results)
            if (!r.endsWith(": ok")) ++failures;
        juce::String status = !chainSupported_.load() ? "unsupported channel layout"
                            : failures == 0  ? "ok"
                            : failures == results.size() ? "failed" : "partial";
        writeChainAck(seq, status, results, detail);
        // AFTER the ack, and only when the chain actually landed: a slot index in the block means nothing until
        // the build that created it has applied.
        if (failures == 0) startCalibFromBlock(calibBlock);

        // ---- ITEM 11 (Sean, 13 Oct 2026): ARM THE LEVELLING LOOP. NOTHING EVER DID. ----------------------
        //
        // Sean: after the vocal build the rack was louder than its input, every slot trim at +0.0, and the
        // Kramer PIE's own output gain was the cause. He expected a build to leave the rack at the same
        // loudness. A post-build volume match DOES exist and is fully wired on this side - resolveStage
        // points at this Link's OUT gain, aimIsTarget says a Link never hits a target, isPlaying and logLine
        // are bound in the constructor - and `armFromChain()` had exactly ONE caller in the whole product:
        // EchoJayEditor::armLoudnessLoopIfTargeted(), which runs when the V2 builds ITS OWN rack.
        //
        // So a chain built onto a Link armed nothing. The loop was not failing to land the level; it was
        // never asked. Same shape as the plugin inventory on 12 Oct: declared, wired, documented, never
        // called - and like that one it looked fine from every side except the one that matters.
        //
        // Arming it here and not on the Restore path above is the whole of the care needed: reopening a
        // project must not re-level a rack the user has since set by hand, and Restore is how that arrives.
        if (failures == 0) armLoudnessAfterBuild ("the Link's own chain build (v1)");
    });
}

// ---- ITEM 11 (12b) / ITEM 4 (13a): ONE ARMING, TWO ROADS IN -------------------------------------------
//
// 13a armed the loop on the Link's OWN v1 chain build and shipped. Sean's vocal build then came out 5-6 dB
// quieter with no OUT caption, and his log says why in one word: "renew lease for 4814a16004". He was
// BORROWING that Link's rack in the V2 window, so the build landed in the V2's mirror and reached the Link
// as an editOps PUSH - a road that passes through neither the V2's own-rack arming nor the v1 build arming
// this round added. Two roads existed and the fix was on one of them.
//
// So the arming lives here, called from both, and from nowhere else: a RESTORE must not re-level a rack the
// user has since set by hand, which is the one case that has to stay unarmed.
/** ---- WHICH SLOT'S OUT TRIM (19 Oct 2026, his 14:0x correction) ----------------------------
    The OUTPUT-MOST slot: its OUT trim is the last gain before the rack's own output, so correcting
    it corrects what leaves the rack rather than a stage something later re-compresses. -1 when the
    rack has no slots at all, which is the only case left for EchoJay's rack gain.

    This replaces makeupStageSlot, which looked for the PLUGIN's make-up or output role. That was my
    reading of his 12:27 instruction and he has since ruled it out in terms: a plugin's make-up is
    the user's setting, not EchoJay's to move. */
int LinkProcessor::outermostSlot() const
{
    const int n = chainHost.getNumSlots();
    return n > 0 ? n - 1 : -1;
}

// ITEM 2 (19 Oct 2026, his 17:09): the added slot names itself, AFTER the apply, by identity.
// Called from the apply's completion, where the rack already holds the new plugin - an index worked
// out from the op's `after` would be arithmetic on the wire's numbering, and the whole defect is an
// index that did not mean what it was read to mean.
void LinkProcessor::nameTheMatchTarget (const std::vector<ChainHost::ChainEditOp>& ops)
{
    matchTargetSlot_ = -1; matchTargetName_.clear();
    juce::StringArray wanted;
    for (const auto& o : ops)
        if ((o.op == "add" || o.op == "replace") && o.name.trim().isNotEmpty())
            wanted.add (o.name.trim());
    if (wanted.isEmpty()) return;
    // THE LAST one named, when a build added several: the match is one trim, and the outermost of
    // the plugins this build put in is the one whose card the whole chain's correction belongs on.
    // THE RULE ITSELF LIVES IN LoudnessLoop::slotTheBatchAdded (21 Oct 2026), because the V2's own
    // rack now has to answer the same question for B's dial and two copies of this walk would agree
    // today and drift by Friday.
    if (const int at = LoudnessLoop::slotTheBatchAdded (chainHost, wanted); at >= 0)
    {
        matchTargetSlot_ = at; matchTargetName_ = chainHost.getSlotInfo (at).name.trim();
        EchoJay_NSLog (("EJLoudness: the level match is for slot " + juce::String (at + 1)
                        + " (\"" + matchTargetName_ + "\"), the plugin this change ADDED - not whatever "
                        "sits last in the chain (his 17:09 item 2)").toRawUTF8());
        return;
    }
    EchoJay_NSLog (("EJLoudness: this change named " + wanted.joinIntoString (", ")
                    + " but no slot on the rack matches, so the match falls back to the outermost "
                      "slot and says so rather than guessing").toRawUTF8());
}

void LinkProcessor::armLoudnessAfterBuild (const juce::String& via)
{
    // ---- UNLESS SOMETHING ELSE OWNS THE LEVEL (Sean's ruling, 20 Oct 2026) ------------------
    //
    // *"No post-add level match on a mix bus or master channel, and none on any build that carries
    // a loudness target. The limiter sets the level on purpose."*
    //
    // The same one function the V2's own-rack arm uses, so both roads refuse for the same reason in
    // the same words. A LINK has no ChannelType of its own, so the channel half is answered by the
    // source type it now publishes (round (b)) - and absent means "not stated", which does not
    // refuse: a Link that cannot say what it is still gets the match it has always had.
    {
        bool hasLimiter = false;
        for (int i = 0; i < chainHost.getNumSlots() && ! hasLimiter; ++i)
            if (ChainHost::isLimiterLikeName (chainHost.getSlotInfo (i).name)) hasLimiter = true;
        juce::String kind;
        if (sourceType_ == "mix_bus" || sourceType_ == "master") kind = sourceType_;
        if (const auto why = LoudnessLoop::postAddMatchRefusal (kind, hasLimiter); why.isNotEmpty())
        {
            EchoJay_NSLog (("EJLoudness: Link NOT armed after a build via " + via + " - " + why
                            + " (his ruling of 20 Oct)").toRawUTF8());
            return;
        }
    }
    // NO INSERT ON THIS ROAD (his 15:30 ruling): every caller of armLoudnessAfterBuild is a build
    // PUSHED to this Link - the agent's editOps push and the V2's own chain build - and his ruling is
    // that a build must not stack a limiter after whatever the build put last. If the rack has no
    // limiter this build recognises, the loop says what sits last instead of adding one.
    if (loudnessLoop_.armFromChain (/*mayInsertLimiter*/ false))
        EchoJay_NSLog(("EJLoudness: Link armed after a build via " + via + " - " + loudnessLoop_.aimWords()
                       + ", stage " + loudnessLoop_.stageName() + " at "
                       + juce::String(loudnessLoop_.currentGainDb(), 1) + " dB").toRawUTF8());
    else
        EchoJay_NSLog(("EJLoudness: Link NOT armed after a build via " + via
                       + " - see the aim/arm line above for why").toRawUTF8());
}

// The block is the contract's, read exactly as V2 reads it (CONTRACT_GROUPS, "The calibration block"): mode
// (passive unless it says listen), actuator, param as a name OR an array, sense, start_db (absent when the server
// heard under 30 s and set nothing), min_db/max_db, gr_target_db. A chain with no compressor carries no block and
// starts nothing.
// ---- A PRIMARY AND ITS COMPANIONS, FROM ONE BATCH (his 17:5x ruling, 21 Oct 2026) -----------
//
// The V2 has had this since 30 Sep ("ONE HOLD PER COMPRESSOR"); a Link had one loop, so the second
// dial of a build replaced the first and his two- and three-compressor vocal chains dialled one.
// The list is taken AT ONCE for that exact reason: starting them one at a time would have each
// replace the last.
int LinkProcessor::startCalibMany (const std::vector<echojay::CalibLoop::Config>& cfgs)
{
    calibExtra_.clear();
    if (cfgs.empty()) return 0;
    int started = 0;
    for (size_t i = 0; i < cfgs.size(); ++i)
    {
        auto cfg = cfgs[i];
        if (cfg.slot < 0 || cfg.slot >= chainHost.getNumSlots())
        {
            EchoJay_NSLog (("EJThreshold(Link): a block names slot " + juce::String (cfg.slot + 1)
                            + ", which is not in this rack - not started").toRawUTF8());
            continue;
        }
        // THE SAME TERMS THE PRIMARY GETS, because a companion is not a lesser loop: a build's hold
        // (one window, one write, then it ends), the slot's own dynamics flag, and the opening value
        // read off the plugin when the block carried none.
        cfg.purpose      = echojay::CalibLoop::Purpose::buildHold;
        cfg.dynamicsSlot = chainHost.slotIsDynamics (cfg.slot);
        if (! (cfg.startDb == cfg.startDb) && ! cfg.params.isEmpty())
        {
            float nowDb = 0.0f;
            if (chainHost.readControlDb (cfg.slot, cfg.params[0], nowDb)) cfg.startDb = nowDb;
            else                                                          cfg.holdOnly = true;
            EchoJay_NSLog (("EJThreshold(Link): \"" + cfg.plugin + "\" carried no start_db for \""
                            + cfg.params[0] + "\" - "
                            + (cfg.holdOnly ? juce::String ("could not read it, HOLDING")
                                            : "read " + juce::String (nowDb, 2) + " dB off the plugin")).toRawUTF8());
        }
        echojay::CalibLoop c;
        { auto cfgH = cfg; cfgH.heardAtBeginS = chainHost.getSlotLevels (cfg.slot).in.heardSeconds; c.begin (cfgH); }
        c.slotIdent = chainHost.slotIdentityKey (cfg.slot);
        if (i == 0) calibLoop_ = c;              // the PRIMARY, where the single block always went
        else        calibExtra_.push_back (c);   // a companion, on its own slot
        ++started;
        EchoJay_NSLog (("EJThreshold(Link): " + juce::String (i == 0 ? "primary" : "companion")
                        + " hold \"" + cfg.plugin + "\" slot " + juce::String (cfg.slot + 1)
                        + ", band " + juce::String (cfg.lo, 1) + "-" + juce::String (cfg.hi, 1)
                        + " dB - each on its own slot's real input (his 17:5x ruling)").toRawUTF8());
    }
    if (started > 0)
    {
        calibLastWindowMs_ = 0.0;   // the first window starts from now, for all of them
        publishCalibState();        // ...and the state is on the sidecar from the start (CO5)
    }
    return started;
}

void LinkProcessor::startCalibFromBlock(const juce::var& block)
{
    auto* co = block.getDynamicObject();
    if (co == nullptr) return;                       // no compressor in this chain: no block, nothing to start

    // EXACTLY ONE LOOP PER BLOCK (21t-g item 6a, 26 Sep 2026), AND THIS IS THE ONE (11 Oct 2026). The rule has
    // not changed - two hosts stepping one compressor is still the failure - but the host that may step it is
    // now always the one that owns the plugins and hears the channel, which is this one. The gate that stood
    // here refused the block whenever a lease was held and left the loop to the V2's copy, and the note on
    // calibTickOwnRack says what that copy was measuring. The V2 side no longer starts a loop for a rack it
    // does not host; it sends the block, and this runs it.
    // ONE PARSER, the shared header's (21t-g item 6b) - the same literals, the same violations, the same
    // threshold-with-no-sense refusal V2 applies. A Link is a TRACK, so the band default is the track band.
    echojay::CalibLoop::Config cfg;
    juce::String why;
    const int slotFromWire = co->hasProperty("slot") ? ((int) co->getProperty("slot")) - 1 : -1;
    const juce::String pluginName = (slotFromWire >= 0 && slotFromWire < chainHost.getNumSlots())
                                      ? chainHost.getSlotInfo(slotFromWire).name : juce::String();
    if (! echojay::CalibLoop::configFromBlock(block, chainHost.getNumSlots(), false, pluginName, cfg, why))
    {
        if (why.isNotEmpty()) EchoJay_NSLog(("EJThreshold(Link): block not usable - " + why.trim()).toRawUTF8());
        return;
    }
    if (why.isNotEmpty()) EchoJay_NSLog(("EJThreshold(Link): BLOCK NOT AS CONTRACTED - " + why.trim()).toRawUTF8());
    const int slot = cfg.slot;
    // (c) A DRIVE BLOCK WITH NO start_db OPENS FROM THE STAGING ALREADY ON THE SLOT, never from zero - the same
    // rule V2 applies, for the same reason: slot_pre_gain_db is already written and opening at 0 would undo it.
    if (cfg.actuator == echojay::CalibLoop::Actuator::Drive && ! (cfg.startDb == cfg.startDb))
    {
        cfg.startDb = chainHost.getSlotInfo(slot).preTrimDb;
        EchoJay_NSLog(("EJThreshold(Link): start_db was null on a drive block - opening from the staging on the slot ("
                       + juce::String(cfg.startDb, 2) + " dB)").toRawUTF8());
    }
    // "no opening value" is NaN out of the parser (a threshold block the server set nothing for).
    const bool haveStart = cfg.startDb == cfg.startDb;

    // (f) 30 Sep 2026 ruling: A BLOCK THAT ARRIVES HERE ARRIVES WITH A BUILD, so it is a buildHold - one window,
    // one write, "Built. ...", ended - exactly as on V2. startCalibFromBlock has ONE caller: the chain-cmd apply
    // path, gated on `failures == 0`, because "a slot index in the block means nothing until the build that
    // created it has applied". So every block reaching this side follows a chain that has just landed; there is no
    // bare-comparative road into the Link's own loop, because a comparative arrives at the V2 that holds the lease.
    // configFromBlock sets no purpose at all, so this was the Config default askRung: the Link opened a promise
    // ("landing it as it plays...") and hunted a rung on a build nobody asked to have dialled - Sean's 10:45 V2
    // fault, on the other side of the transport.
    cfg.purpose = echojay::CalibLoop::Purpose::buildHold;
    cfg.dynamicsSlot = chainHost.slotIsDynamics(slot);   // (l): the input-headroom ceiling is off on a compressor
    { auto cfgH = cfg; cfgH.heardAtBeginS = chainHost.getSlotLevels(cfg.slot).in.heardSeconds; calibLoop_.begin(cfgH); }
    calibLoop_.slotIdent = chainHost.slotIdentityKey(slot);   // (n)
    const bool threshold = cfg.actuator == echojay::CalibLoop::Actuator::Threshold;
    EchoJay_NSLog(("EJThreshold(Link): \"" + cfg.plugin + "\" slot " + juce::String(slot + 1)
                   + (cfg.mode == echojay::CalibLoop::Mode::Passive ? " PASSIVE" : " LISTEN")
                   + ", band " + juce::String(cfg.lo, 1) + "-" + juce::String(cfg.hi, 1) + " dB, dialling "
                   + (threshold ? cfg.params.joinIntoString(" + ") : juce::String("the drive"))
                   + (haveStart ? " from " + juce::String(cfg.startDb, 1) + " dB"
                                : juce::String(" (no opening value sent)"))).toRawUTF8());
    // The opening value is written the same way every later step is.
    if (threshold)
    {
        // A HOLD WRITES NOTHING, HERE TOO (3 Oct 2026, 13:52 item 3). The mirror of the V2 twin: a set_directly
        // or otherwise hold-only block has nothing to open, and writing the value back goes through the readback
        // search and can land somewhere else than it was read.
        if (echojay::CalibLoop::writesOpeningValue(cfg))
            chainHost.setSlotControlsToValue(slot, cfg.params, cfg.startDb);
        else
            EchoJay_NSLog(("EJThreshold(Link): NOT writing " + cfg.params.joinIntoString(" + ") + " - "
                           + (cfg.holdOnly ? juce::String("this is a hold; the control stays where it is")
                                           : juce::String("no opening value was sent"))).toRawUTF8());
        chainHost.resetSlotShortTermStats(slot, "the build set the actuator");   // 21t-j: reset point 1
    }
    else
    {
        chainHost.setSlotPreTrimDb(slot, cfg.startDb);
        chainHost.setSlotOutGainDb(slot, -cfg.startDb);   // 21t-m: the LIVE out, not the deleted compare trim
        chainHost.resetSlotShortTermStats(slot, "the build set the drive");   // 21t-j: reset point 1, drive side
    }
    // The state rides the sidecar from the FIRST window, so V2 can render and close a loop this side started.
    // resolvedDir is set on the paths that publish, and a build command can arrive before any of them have run -
    // the first cut wrote to an empty directory string, which lands nowhere and reads back "no sidecar". Resolve
    // it here when it is empty rather than assume the order.
    {
        if (resolvedDir.isEmpty()) { int e = 0; resolvedDir = LinkShm::resolveDir(e); }
        auto rc = LinkShm::readRackSidecar(resolvedDir, instanceUid_);
        if (! rc.valid) { rc.uid = instanceUid_; rc.valid = true; if (rc.revision < 0) rc.revision = 0; }
        rc.calib = calibLoop_.toVar();
        LinkShm::writeRackSidecar(resolvedDir, rc);
    }
}

// =============================================================================================
//  WHAT THIS CHANNEL IS (Sean's round (b), 20 Oct 2026)
// =============================================================================================
//  *"A source type for each Link: the same type setting the V2 has, guessed automatically from the
//  track name ... with Sean able to change it on the Link. Sent as channelRole on both roads."*
//
//  THE RULE, in one place: user > classifier > name, and absent when unsure. The guess is made from
//  this Link's own display name and NEVER overwrites something of higher provenance - which is why
//  the provenance is stored beside the value rather than inferred from it.
void LinkProcessor::refreshSourceTypeGuess()
{
    const auto name = effectiveDisplayName();
    // A value HE set survives a rename. That asymmetry is the whole point of carrying the
    // provenance, and the comparison is the only statement of it.
    const auto held = echojay::sourcetype::fromWireName (sourceTypeFrom_);
    if (! echojay::sourcetype::mayReplace (held, echojay::sourcetype::From::Name))
        return;
    // Nothing to do while the name has not moved: this runs on every sidecar publish, and the
    // guesser tokenises a string - cheap, but not free, and a publish is not the place for work that
    // cannot change its answer.
    if (name == guessedFromName_) return;
    guessedFromName_ = name;
    const auto g = echojay::sourcetype::guessFromName (name);
    if (g.have)
    {
        const auto wire = echojay::sourcetype::wireNameFor (g.type);
        if (wire != sourceType_ || sourceTypeFrom_ != "name")
            EchoJay_NSLog (("EJSourceType: \"" + name + "\" reads as " + wire + " - " + g.why
                            + ". Published with provenance \"name\", so it is a GUESS: his own "
                              "setting on this Link outranks it and survives a rename (round (b), "
                              "20 Oct)").toRawUTF8());
        sourceType_ = wire;
        sourceTypeFrom_ = "name";
        sourceTypeName_.clear();
    }
    else
    {
        // ABSENT IS A REAL ANSWER AND THE IMPORTANT ONE. A wrong type is worse than none - the
        // server picking a vocal chain for a drum bus is a worse failure than the server asking -
        // so all three fields go out empty and channelRole carries kind and name as it always has.
        if (sourceType_.isNotEmpty() || sourceTypeFrom_.isNotEmpty())
            EchoJay_NSLog (("EJSourceType: \"" + name + "\" does NOT say what this channel is - "
                            + g.why + ". Nothing is published rather than guessed, so the server "
                              "asks instead of assuming (round (b))").toRawUTF8());
        else
            EchoJay_NSLog (("EJSourceType: \"" + name + "\" does not say what this channel is - "
                            + g.why).toRawUTF8());
        sourceType_.clear(); sourceTypeFrom_.clear(); sourceTypeName_.clear();
    }
}

void LinkProcessor::setSourceTypeFromUser (const juce::String& wire, const juce::String& customName)
{
    const auto w = wire.trim();
    if (w.isEmpty())
    {
        // CLEARED BACK TO THE GUESS, which is how a wrong setting is undone. guessedFromName_ is
        // reset too, or the guesser would see an unchanged name and decline to run.
        sourceType_.clear(); sourceTypeFrom_.clear(); sourceTypeName_.clear();
        guessedFromName_.clear();
        EchoJay_NSLog ("EJSourceType: his setting was CLEARED on this Link - back to the guess from "
                       "the track name, or to nothing if the name does not say (round (b))");
        refreshSourceTypeGuess();
        publishRackSidecar();
        return;
    }
    ChannelType parsed = ChannelType::Other;
    if (! echojay::sourcetype::channelTypeForWireName (w, parsed))
    {
        // A name that is not one of ours is NOT STATED, never a default: the one thing this must not
        // do is quietly store something the V2's own selector could not show back to him.
        EchoJay_NSLog (("EJSourceType: refusing \"" + w + "\" - it is not one of this build's "
                        "channel types, so nothing is stored (a value the selector cannot show back "
                        "is worse than none)").toRawUTF8());
        return;
    }
    sourceType_     = w;
    sourceTypeFrom_ = "user";          // OUTRANKS the classifier and any rename, for good
    sourceTypeName_ = (parsed == ChannelType::Other) ? customName.trim() : juce::String();
    EchoJay_NSLog (("EJSourceType: HE set this Link to " + w
                    + (sourceTypeName_.isNotEmpty() ? " (\"" + sourceTypeName_ + "\")" : juce::String())
                    + " - provenance \"user\", which a rename and the classifier both lose to. "
                      "Republished at once: a gesture is not a meter and must not wait for the "
                      "digest (round (b))").toRawUTF8());
    publishRackSidecar();
}

// =============================================================================================
//  THE MUTED SHADOW, THE LINK'S HALF (stage A of docs/PLAN_LIVE_SHADOW_2026-10-20.md, 21 Oct 2026)
// =============================================================================================
//  Sean's design: *"The Link streams that slot's input audio to the V2 (shared memory, as the
//  sidecar does for data) ... Only the slot in view is shadowed; stop when he leaves it."*
//
//  WHAT THIS DOES NOT DO: touch the audio path. The tap is a second sink on the pre-trim node that
//  already measures this signal (ChainHost's SlotPreTrim, and its own 28 Sep comment is the
//  argument for the tap point). Nothing is inserted, nothing is reordered, and what is heard cannot
//  change - which is the first line of his design and the only one that is not negotiable.
/** ---- CONTROL, STAGE C1: APPLY WHAT THE V2'S VIEW SENT (24 Oct 2026) ----------------------
    *"turning a knob in the V2's live view changes the real plugin on the Link at once (I hear it,
     the Link keeps it)."*

    THE MESSAGE THREAD, on the 30 Hz tick. The audio thread staged the frames; this is the half the
    10 Oct ruling promised and nobody wrote.

    COALESCED BY PARAMETER, NEVER BY FRAME, and the gesture boundaries are never dropped - a drag
    is one gesture however many frames it arrived as. The undo step is the V2's, pushed once at the
    release through ringEndGestureWithUndo, which is the same road the slot gains and the wet knob
    have used since stage 5: one Undo button walks local and remote edits in one order, and a
    second history over here would be a second answer to one question.

    EVERY REFUSAL IS COUNTED AND NAMED. A paramId past the plugin's list, a slot with no plugin, a
    ring that lapped: each is logged with what it was. A value that cannot be applied must never
    look like one that was.                                                                      */
void LinkProcessor::drainRingWhileAudioIdle()
{
    auto* ring = cmdRing_.load (std::memory_order_acquire);
    if (ring == nullptr) return;
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (! echojay::timerMayDrain (now, lastAudioDrainMs_.load (std::memory_order_relaxed))) return;

    // The claim, not the clock, is what makes this safe: if the audio thread is inside its own
    // drain at this instant, this returns at once and the next tick tries again.
    echojay::RingDrainClaim claim (ringDrainClaim_);
    if (! claim.held()) return;

    const auto d = echojay::cmdring::drain (ring, (uint32_t) chainHost.getChainRevision());
    if (d.count == 0 && d.dropped == 0) return;
    applyRingFrames (d);
    ++timerDrains_;
    if (! saidTimerDrain_)
    {
        saidTimerDrain_ = true;
        EchoJay_NSLog ("EJControl: this Link's audio is not running, so the value ring is being "
                       "drained on the message thread instead - a change made in a V2's live view "
                       "still reaches the plugin, within a tick rather than within a buffer. The "
                       "audio thread takes it back the moment the transport rolls, and only ever one "
                       "of the two drains (an atomic claim, not a clock)");
    }
}

void LinkProcessor::applyStagedSetParams()
{
    const uint32_t r = stagedParamR_.load (std::memory_order_relaxed);
    const uint32_t w = stagedParamW_.load (std::memory_order_acquire);
    if (w == r)
    {
        if (const int lost = stagedParamLost_.exchange (0, std::memory_order_relaxed); lost > 0)
            EchoJay_NSLog (("EJControl: " + juce::String (lost) + " staged parameter frame(s) were "
                            "LOST because the staging ring lapped - the view moved faster than this "
                            "tick could apply. Counted rather than silent; the last value of each "
                            "control still lands, because the V2 sends an END frame at the "
                            "release").toRawUTF8());
        return;
    }

    // Copied out first, so the producer is never waited on and the ring is freed at once.
    echojay::StagedParam local[kStagedParamCap];
    int n = 0;
    for (uint32_t i = r; i != w && n < kStagedParamCap; ++i)
        local[n++] = stagedParams_[i & (uint32_t) (kStagedParamCap - 1)];
    stagedParamR_.store (r + (uint32_t) n, std::memory_order_release);

    const auto moves = echojay::coalesceStagedParams (local, n);
    for (const auto& m : moves)
    {
        auto* proc = m.slot >= 0 ? chainHost.getSlotProcessor (m.slot) : nullptr;
        if (proc == nullptr)
        {
            EchoJay_NSLog (("EJControl: a value for slot " + juce::String (m.slot + 1)
                            + " could not be applied - that slot has no plugin on this Link")
                           .toRawUTF8());
            continue;
        }
        const auto& params = proc->getParameters();
        if ((int) m.paramId >= params.size())
        {
            EchoJay_NSLog (("EJControl: a value for parameter " + juce::String ((int) m.paramId)
                            + " of slot " + juce::String (m.slot + 1) + " could not be applied - "
                            "that plugin has " + juce::String (params.size()) + ". The V2 and this "
                            "Link disagree about the parameter list, which FOLLOW's digest is "
                            "supposed to catch before a frame is ever sent").toRawUTF8());
            continue;
        }
        auto* prm = params[(int) m.paramId];
        if (prm == nullptr) continue;
        const float want = (float) juce::jlimit (0.0, 1.0, m.value);
        prm->setValueNotifyingHost (want);
        ++stagedParamApplied_;

        // ONE LINE PER GESTURE, not per frame: the end is the fact worth a log line, and a drag
        // would otherwise write thirty lines a second into his session's log.
        if (m.sawEnd)
            EchoJay_NSLog (("EJControl: slot " + juce::String (m.slot + 1) + " \"" + prm->getName (48)
                            + "\" set to " + juce::String (want, 4) + " (\""
                            + prm->getCurrentValueAsText() + "\") from the V2's live view, gesture "
                            + juce::String ((int) m.gesture) + " - this Link now holds it")
                           .toRawUTF8());
    }

    if (! saidControlLive_ && ! moves.empty())
    {
        saidControlLive_ = true;
        EchoJay_NSLog ("EJControl: the first value from a V2's live view has been applied to a real "
                       "plugin on this Link. The ring carried it, the audio thread staged it and "
                       "this message thread applied it - the 10 Oct ruling's other half, finally "
                       "written (CONTROL stage C1)");
    }
}

/** ---- FOLLOW LAYER 2, THE LINK'S HALF (24 Oct 2026) ----------------------------------------
    *"the Link publishes the tapped slot's parameter values, the V2 writes only the changed ones
     into the copy ... change a knob on the Link's own window with the V2's view open, and the view
     should follow within a moment."*

    THE MESSAGE THREAD, on the 30 Hz tick this timer already runs. A parameter read is a plugin
    call; LinkProcessor's own ruling on kSetParam says such a call "can never be made real-time
    safe", and that is as true of getValue as it is of a write.

    WHAT IT COSTS IS MEASURED AND SAID, not assumed: the first sweep of each tap logs its own
    duration, and every frame carries it, so the V2 and the log can both see what this is costing
    rather than trusting a figure out of a design note.                                          */
/** PER-SLOT IN, OUT AND THE THIRD FIGURE, thirty times a second (his 25 Oct ask).

    THIS MEASURES NOTHING. Every number here was already taken on the audio thread for the sidecar;
    this is a faster road for figures that exist. The pass is one loop over at most fifteen slots
    copying five floats each, on a tick that already publishes a meter frame.

    THE FILE IS OPENED LAZILY AND KEPT, with the same bounded ~1 Hz retry ringTick uses - because
    "attempted quietly and once" written as `if (map == nullptr)` on a 30 Hz tick is thirty attempts
    a second for the life of the session, which is what exhausted the AU hosting process on 11 Oct. */
void LinkProcessor::publishSlotMetersTick()
{
    if (slotMeterMap_ == nullptr)
    {
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        if (nowMs < slotMeterNextTryMs_) return;
        slotMeterNextTryMs_ = nowMs + 1000.0;
        int err = 0;
        const juce::String dir = LinkShm::resolveDir (err);
        if (dir.isEmpty() || instanceUid_.isEmpty()) return;
        const auto path = dir + echojay::slotMetersFileNameFor (instanceUid_);
        int fd = -1, e2 = 0;
        auto* map = LinkShm::openFileMapped (path, echojay::kSlotMeterFileSize,
                                             /*readOnly*/ false, fd, e2);
        if (map == nullptr)
        {
            if (! slotMeterSaidOpen_)
            {
                slotMeterSaidOpen_ = true;
                EchoJay_NSLog (("EJSlotMeter: the per-slot meter file at " + path + " would not map "
                                "(errno " + juce::String (e2) + ") - nothing else is affected, and "
                                "the V2's cards will show what they show today").toRawUTF8());
            }
            return;
        }
        // ZEROED ON CREATION: seq 0 is "never published", which readSlotMeters treats as no frame
        // at all rather than as a frame of zeros - the whole blank-not-zero rule starts here.
        std::memset (map, 0, echojay::kSlotMeterFileSize);
        slotMeterMap_ = map; slotMeterFd_ = fd; slotMeterPath_ = path;
        slotMeterPublishes_ = 0; slotMeterSaidCost_ = false;
        EchoJay_NSLog (("EJSlotMeter: publishing this rack's per-slot meters at 30 Hz to "
                        + echojay::slotMetersFileNameFor (instanceUid_) + " - IN, OUT and the third "
                          "figure per slot, from the tallies the audio thread already fills. No new "
                          "measurement and no audio-thread work (his 25 Oct ask)").toRawUTF8());
    }

    const int n = juce::jmin (chainHost.getNumSlots(), echojay::kSlotMeterMax);
    if ((int) slotMeterScratch_.size() < echojay::kSlotMeterMax)
        slotMeterScratch_.resize ((size_t) echojay::kSlotMeterMax);

    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    for (int i = 0; i < n; ++i)
    {
        const auto lv  = chainHost.getSlotLevels (i);
        const auto pic = chainHost.slotPicture (i);
        // THE GR GATE IS SLOTPICTURE'S, NOT A SECOND ONE HERE. grKnown is true only for an EchoJay
        // device (which reports its own) or a third party whose map is CERTIFIED - his ruling of
        // 15 Oct - and grHeldDb is the same figure the device's own meter draws, so the card and
        // the meter have one source and cannot disagree.
        slotMeterScratch_[(size_t) i] =
            echojay::slotMeterEntry (chainHost.getSlotInfo (i).bypassed, lv.measured,
                                     lv.in.known,  lv.in.rmsDb,  lv.in.peakDb,
                                     lv.out.known, lv.out.rmsDb, lv.out.peakDb,
                                     pic.grKnown,  pic.grHeldDb,
                                     lv.in.heardSeconds);
    }
    const auto passMs = juce::Time::getMillisecondCounterHiRes() - t0;

    echojay::SlotMeterHeader h {};
    h.slotCount         = (juce::uint32) n;
    h.structureRevision = (juce::uint32) juce::jmax (0, chainHost.getChainRevision());
    h.publishCount      = ++slotMeterPublishes_;
    h.sweepMicros       = (juce::uint32) juce::jlimit (0.0, 4.0e9, passMs * 1000.0);
    echojay::publishSlotMeters (slotMeterMap_, h, slotMeterScratch_.data(), n);

    if (! slotMeterSaidCost_ && n > 0)
    {
        slotMeterSaidCost_ = true;
        EchoJay_NSLog (("EJSlotMeter: " + juce::String (n) + " slot(s), one pass = "
                        + juce::String (passMs, 3) + " ms at 30 Hz ("
                        + juce::String (passMs * 3.0, 2) + "% of this thread) - and ZERO on the audio "
                          "thread, which is identical with this on or off").toRawUTF8());
    }
}

void LinkProcessor::publishShadowValuesTick()
{
    if (shadowValMap_ == nullptr) return;
    auto* tap = chainHost.shadowTap().get();
    const int slot = tap != nullptr ? tap->slot() : -1;
    if (slot < 0) return;

    auto* proc = chainHost.getSlotProcessor (slot);
    if (proc == nullptr) return;

    // ---- OUR OWN DEVICES PUBLISH FROM THEIR SCHEMA (item 2b, 29 Oct 2026) ----------------
    //
    // This read getParameters() and returned when it was empty, which is EVERY EchoJay device:
    // they have no juce parameters at all. So the V2 got no frame, decided it was not following,
    // and put the shield up over a Compressor he was trying to turn. The schema is the ordered,
    // stable list these devices do have.
    const auto builtinMirror = echojay::builtinMirrorOf (*proc);
    const auto& params = proc->getParameters();
    const int have = ! builtinMirror.empty() ? (int) builtinMirror.size()
                                             : params.size();
    const int n    = juce::jmin (have, echojay::kShadowValMax);
    if (n <= 0) return;

    // THE DIGEST IS RECOMPUTED WHEN THE SLOT, THE RACK'S STRUCTURE OR THE PARAMETER COUNT MOVES,
    // never every tick: it walks every parameter's name, which is the one genuinely dear thing
    // here. The old key was the slot INDEX alone, and his 11:20 fault of 26 Oct is what that cost -
    // a plugin replaced in the same slot kept the old plugin's digest, the V2 refused every frame
    // and the live view said "not following" for ever. The decision is
    // echojay::shadowDigestNeedsRecompute, and it is held in both directions by the guard.
    const auto revNow = (juce::uint32) juce::jmax (0, chainHost.getChainRevision());
    if (echojay::shadowDigestNeedsRecompute (slot, shadowValDigestSlot_, revNow, shadowValDigestRev_,
                                             have, shadowValDigestCount_))
    {
        const auto wasDigest = shadowValDigest_;
        shadowValDigest_      = echojay::shadowSchemaDigest (*proc);
        shadowValDigestSlot_  = slot;
        shadowValDigestRev_   = revNow;
        shadowValDigestCount_ = have;
        shadowValSaidCost_    = false;
        if (wasDigest != 0u && wasDigest != shadowValDigest_)
            EchoJay_NSLog (("EJShadow: slot " + juce::String (slot + 1) + "'s parameter list has "
                            "CHANGED under the tap (digest " + juce::String ((int) wasDigest) + " -> "
                            + juce::String ((int) shadowValDigest_) + ", " + juce::String (have)
                            + " parameter(s), rack revision " + juce::String ((int) revNow)
                            + ") - the values published from here describe the plugin that is in "
                              "that slot NOW, which is what a replaced plugin needs and what his "
                              "11:20 fault of 26 Oct did not get").toRawUTF8());
    }

    if ((int) shadowValScratch_.size() < n) shadowValScratch_.resize ((size_t) n, 0.0f);

    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    if (! builtinMirror.empty())
        echojay::shadowReadBuiltinValues (*proc, shadowValScratch_.data(), n);
    else
        for (int i = 0; i < n; ++i)
            shadowValScratch_[(size_t) i] = params[i]->getValue();
    const auto sweepMs = juce::Time::getMillisecondCounterHiRes() - t0;

    echojay::ShadowValueHeader h {};
    h.slot              = slot;
    h.paramCount        = (juce::uint32) have;
    h.published         = (juce::uint32) n;
    h.schemaDigest      = shadowValDigest_;
    h.structureRevision = (juce::uint32) juce::jmax (0, chainHost.getChainRevision());
    h.stateGeneration   = 0;                      // layer 3 moves this
    h.sweepMicros       = (juce::uint32) juce::jlimit (0.0, 4.0e9, sweepMs * 1000.0);
    echojay::publishShadowValues (shadowValMap_, h, shadowValScratch_.data(), n);

    if (! shadowValSaidCost_)
    {
        shadowValSaidCost_ = true;
        EchoJay_NSLog (("EJShadow: following slot " + juce::String (slot + 1) + " for the live view: "
                        + juce::String (have) + " parameter(s), one full sweep = "
                        + juce::String (sweepMs, 3) + " ms, published at 30 Hz ("
                        + juce::String (sweepMs * 3.0, 2) + "% of this thread) - the V2 writes only "
                        "what moved").toRawUTF8());
    }
    if (have > echojay::kShadowValMax && ! shadowValSaidTruncated_)
    {
        shadowValSaidTruncated_ = true;
        EchoJay_NSLog (("EJShadow: slot " + juce::String (slot + 1) + " has " + juce::String (have)
                        + " parameters and a frame carries " + juce::String (echojay::kShadowValMax)
                        + ", so the live view follows the first " + juce::String (echojay::kShadowValMax)
                        + " and NOT the rest. The count travels with every frame so the V2 can say so "
                          "rather than show a half-followed picture as a followed one").toRawUTF8());
    }
}

void LinkProcessor::setShadowSlot (int slot0)
{
    auto tap = chainHost.shadowTap();
    if (tap == nullptr) return;
    const int want = (slot0 >= 0 && slot0 < chainHost.getNumSlots()) ? slot0 : -1;

    if (want < 0)
    {
        shadowWhyNot_.clear();                                     // a stop has no reason to carry
        if (shadowMap_ == nullptr && tap->slot() < 0) return;      // already off: say nothing
        // ---- THE ORDER IS THE SAFETY (see ShadowTap::stop) ------------------------------------
        // The slot goes first, so no further block can enter push(); then the pointer; then the
        // unmap is DEFERRED by a block, because a block that had already passed the slot test may
        // still be inside. This is the same discipline the drain ring's munmap uses, and for the
        // same reason - PluginProcessor's note on it reads "deferred munmap - audio thread may be
        // inside ringConsume with old pointer".
        const auto frames = tap->framesProduced();
        tap->stop();
        auto* dying = shadowMap_; const int dyingFd = shadowFd_; const auto dyingPath = shadowPath_;
        shadowMap_ = nullptr; shadowFd_ = -1; shadowPath_.clear();
        if (dying != nullptr)
            juce::Timer::callAfterDelay (200, [dying, dyingFd, dyingPath]
            {
                LinkShm::closeMapped (dying, kLinkShmSize, dyingFd, dyingPath, /*unlink*/ true);
            });
        // AND THE VALUES FILE GOES WITH IT. Only the message thread ever touches this one, in
        // either process, so there is no block to wait out - but it is closed AFTER the tap has
        // stopped, so the last frame a V2 reads is never newer than the audio it belongs to.
        if (shadowValMap_ != nullptr)
        {
            LinkShm::closeMapped (shadowValMap_, echojay::kShadowValFileSize, shadowValFd_,
                                  shadowValPath_, /*unlink*/ true);
            shadowValMap_ = nullptr; shadowValFd_ = -1; shadowValPath_.clear();
        }
        shadowValDigestSlot_ = -1; shadowValDigestRev_ = 0u; shadowValDigestCount_ = -1;
                shadowValSaidTruncated_ = false; shadowValSaidCost_ = false;
        EchoJay_NSLog (("EJShadow: STOPPED on this Link - " + juce::String (frames)
                        + " frame(s) were streamed while it ran, and the ring is unlinked. The slot's "
                          "pre-trim now does one atomic load per block and nothing else (stage A; his "
                          "\"stop when he leaves it\")").toRawUTF8());
        return;
    }

    // ---- A SLOT THAT CANNOT STREAM IS REFUSED, NOT PROMISED (23 Oct 2026) ---------------------
    // The rig's B9 read "streaming slot 1" from here and zero frames for twenty-five seconds: this
    // function tested only that the slot EXISTS, and a bypassed or unloaded slot has no pre-trim
    // node in the graph, so no block can reach the tap. echojay::slotCanStream holds that rule and
    // the words; the ack carries them to the V2, which keeps the static picture and says why.
    {
        juce::String why;
        if (! echojay::slotCanStream (chainHost.slotHasLoadedPlugin (want),
                                      chainHost.getSlotInfo (want).bypassed, &why))
        {
            shadowWhyNot_ = why;
            EchoJay_NSLog (("EJShadow: NOT streaming slot " + juce::String (want + 1) + " - " + why
                            + ". Nothing is claimed and nothing is mapped: a tap on a slot the graph "
                              "does not run would show him a picture that can never move (the rig's "
                              "B9, 23 Oct)").toRawUTF8());
            return;
        }
        shadowWhyNot_.clear();
    }

    if (tap->slot() == want && shadowMap_ != nullptr) return;      // already shadowing that slot

    // A DIFFERENT SLOT REUSES THE RING. The consumer reads frames, not slots, so the only thing
    // that must not happen is the two slots' audio interleaving - and the slot is switched under the
    // same ordered store the stop path uses, so the next block is the new slot's and nothing mixes.
    if (shadowMap_ == nullptr)
    {
        if (resolvedDir.isEmpty()) { int e = 0; resolvedDir = LinkShm::resolveDir (e); }
        if (resolvedDir.isEmpty() || instanceUid_.isEmpty())
        {
            EchoJay_NSLog ("EJShadow: cannot start - no shared directory or no instance uid yet");
            return;
        }
        const auto path = resolvedDir + echojay::shadowRingFileNameFor (instanceUid_);
        int fd = -1, err = 0;
        auto* map = LinkShm::openFileMapped (path, kLinkShmSize, /*readOnly*/ false, fd, err);
        if (map == nullptr)
        {
            EchoJay_NSLog (("EJShadow: cannot start - the ring at " + path + " would not map (errno "
                            + juce::String (err) + ")").toRawUTF8());
            return;
        }
        // The header, as the drain ring's producer writes it: the consumer checks the magic and the
        // rate, and a rate it does not expect is a REFUSAL on its side rather than a resample here
        // (the plan's rule: compare and refuse, never paper over).
        auto* hdr = LinkShm::ringHeader (map);
        hdr->magic          = kLinkMagic;
        hdr->version        = kLinkVersion;
        hdr->sampleRate     = (float) getSampleRate();
        hdr->numChannels    = 2;
        hdr->capacityFrames = kLinkRingFrames;
        LinkShm::storeRelease (&hdr->writeIdx, 0u);
        LinkShm::storeRelease (&hdr->readIdx,  0u);
        shadowMap_ = map; shadowFd_ = fd; shadowPath_ = path;
        tap->resetFrames();
        tap->setRing (map);

        // ---- FOLLOW LAYER 2'S FILE, MAPPED BESIDE THE RING (24 Oct 2026) ---------------------
        // One name from the uid, used by both processes (EJShadowValues.h) - not two derivations
        // of one fact, which is what the audio ring's name is and what nobody should copy. A
        // failure here is NOT a failure of the view: the picture and the audio are the ring's, and
        // a V2 that finds no values file simply says the knobs are not being followed.
        {
            const auto vpath = resolvedDir + echojay::shadowValuesFileNameFor (instanceUid_);
            int vfd = -1, verr = 0;
            auto* vmap = LinkShm::openFileMapped (vpath, echojay::kShadowValFileSize,
                                                  /*readOnly*/ false, vfd, verr);
            if (vmap == nullptr)
                EchoJay_NSLog (("EJShadow: the live view's VALUES file at " + vpath + " would not "
                                "map (errno " + juce::String (verr) + ") - the picture and the "
                                "audio are unaffected, but its knobs will not follow this Link")
                               .toRawUTF8());
            else
            {
                // ZEROED ON CREATION, so a stale frame from a previous tap on another slot can
                // never be read as this slot's. seq 0 is "never published", which readShadowValues
                // treats as no frame at all rather than as a frame of zeros.
                std::memset (vmap, 0, echojay::kShadowValFileSize);
                shadowValMap_ = vmap; shadowValFd_ = vfd; shadowValPath_ = vpath;
                shadowValDigestSlot_ = -1; shadowValDigestRev_ = 0u; shadowValDigestCount_ = -1;
                shadowValSaidTruncated_ = false; shadowValSaidCost_ = false;
            }
        }
    }
    tap->setSlot (want);
    EchoJay_NSLog (("EJShadow: streaming slot " + juce::String (want + 1) + " (\""
                    + chainHost.getSlotInfo (want).name + "\") on this Link to the main plugin at "
                    + juce::String ((int) getSampleRate()) + " Hz - its INPUT, taken after the slot's "
                      "pre-trim, which is the signal the plugin itself hears (ChainHost's own 28 Sep "
                      "rule). Nothing in the audio path changed: what is heard is unaffected (stage "
                      "A of his muted-shadow design).").toRawUTF8());
}

void LinkProcessor::writeChainAck(int seq, const juce::String& status,
                                  const juce::StringArray& results,
                                  const juce::var& detail,
                                  const juce::String& reason,
                                  int slotsAfter)
{
    // PHASE 1b instrumentation: every chain ack says what it answered.
    EchoJay_NSLog(("EJChainAck[" + juce::String(seq) + "] link: status="
                   + status).toRawUTF8());
    if (resolvedDir.isEmpty()) return;
    auto id = chainInstanceId();
    if (id.isEmpty()) return;

    auto* obj = new juce::DynamicObject();
    obj->setProperty("v",      1);
    obj->setProperty("seq",    seq);
    obj->setProperty("status", status);
    juce::Array<juce::var> arr;
    for (auto& r : results) arr.add(r);
    obj->setProperty("perPluginResults", juce::var(arr));
    if (detail.isArray())
        obj->setProperty("perPluginDetail", detail);
    if (reason.isNotEmpty()) obj->setProperty("reason", reason);   // 21t-j: why, in words, for a refusal
    if (slotsAfter >= 0) obj->setProperty("slotsAfter", slotsAfter);   // 11 Oct: what this rack holds NOW

    juce::File(resolvedDir + "chain-ack-" + id + ".json")
        .replaceWithText(juce::JSON::toString(juce::var(obj), true));
}

juce::AudioProcessorEditor* LinkProcessor::createEditor() { return new LinkEditor(*this); }

void LinkProcessor::getStateInformation(juce::MemoryBlock& dest)
{
#if ECHOJAY_LINK_STATE_DIAG
    EchoJay_NSLog(("EJLinkState: getState linkOn=" + juce::String((int)linkOn.load())
                   + " name=\"" + linkName + "\"").toRawUTF8());
#endif
    juce::DynamicObject* obj = new juce::DynamicObject();
    obj->setProperty("linkName", linkName);
    obj->setProperty("alias",    displayAlias);   // 21n item 2
    // ---- ONLY HIS OWN CHOICE PERSISTS (round (b), 20 Oct 2026) -----------------------------
    // A guess is re-made from the name every session and must not be saved: a project reopened
    // after a rename would otherwise carry the OLD guess for ever, looking exactly like a decision.
    // So the saved state holds the "user" value alone, which is also the only one that outranks a
    // rename. Written only when there is one, so an older build's project is unchanged.
    if (sourceTypeFrom_ == "user" && sourceType_.isNotEmpty())
    {
        obj->setProperty ("srcType", sourceType_);
        if (sourceTypeName_.isNotEmpty()) obj->setProperty ("srcTypeName", sourceTypeName_);
    }
    obj->setProperty("linkOn",   (bool)linkOn.load());
    obj->setProperty("gainDb",   (double)gainDb_.load(std::memory_order_relaxed));
    // COMP_PROFILE_SPEC_v1 section 5, PERSISTED (2 Oct 2026 ruling). This blob is per-instance, so it is already
    // keyed to the track: Logic hands the same chunk back to the Link on that channel. Without this, a restart
    // threw away everything the track had been heard to do and the next build sent no level at all until it had
    // played again. The reading written out is the BETTER of what is stored and what this session has heard -
    // "more heard audio wins" - so reopening a session cannot quietly downgrade a longer reading to a shorter one.
    {
        const auto live = chainHost.trackLevelReading();
        const bool liveWins = live.valid && live.heardSeconds > tlStoredHeardS_;
        const float rms  = liveWins ? live.loudRmsDbfs  : tlStoredRmsDb_;
        const float peak = liveWins ? live.loudPeakDbfs : tlStoredPeakDb_;
        const float hrd  = liveWins ? live.heardSeconds : tlStoredHeardS_;
        if (hrd > 0.0f)
        {
            obj->setProperty("tlRmsDbfs",  (double) rms);
            obj->setProperty("tlPeakDbfs", (double) peak);
            obj->setProperty("tlHeardS",   (double) hrd);
        }
    }
    obj->setProperty("placement", placement_.load(std::memory_order_relaxed));
    obj->setProperty("projectName", projectName);
    obj->setProperty("genre",       genre);
    obj->setProperty("editorW",  editorW);
    obj->setProperty("editorH",  editorH);
    obj->setProperty("instanceUid", instanceUid_);
    // 21t-i (27 Sep 2026 ruling): the stored level record survives a session save and reopen.
    if (levelRecord_.valid) obj->setProperty("levelRecord", levelRecord_.toVar());
    {   // the author: this host process run (pid + start time), so a restore can
        // tell a from-disk reopen from a chunk the host re-applied in this run
        const auto& h = ChainHost::getHostIdentity();
        obj->setProperty("authorPid",       (int) ::getpid());
        obj->setProperty("authorStartSec",  (double) h.startSec);
        obj->setProperty("authorStartUsec", (double) h.startUsec);
    }
    // muteUser is channel mix identity and persists. soloOn is DELIBERATELY
    // ABSENT and must stay absent: a saved solo is how a project opens
    // silent and nobody knows why (MUTE_SOLO_SPEC §4; the gate pins this).
    obj->setProperty("muteUser", muteUserOn_.load(std::memory_order_relaxed));
    // COMMIT 4 (17 Sep 2026): keyShowRelative - the process-wide key display
    // pref, persisted here beside muteUser (the nearest boolean's pattern).
    obj->setProperty("keyShowRelative", echojay::KeyDisplayPrefs::showRelative().load());
    obj->setProperty("hostTrackName", getHostTrackName());
    // Full hosted chain: identities, order, bypass flags, wet mixes,
    // per-plugin state
    const auto chainVar = chainModelToVar();
    obj->setProperty("chain",    chainVar);
    obj->setProperty("chainMasterWet", (double)chainHost.getMasterWet());
    // 4 Oct 2026 (ruled): THE CHAIN REVISION, so V2's saved borrowed copy can be judged against it on reopen.
    // The rule is "apply V2's copy only if the Link has not moved since the borrow started", and a revision is
    // the only thing that answers that. Additive: absent on an older chunk, and then restoredChainRev_ stays -1
    // and V2's copy is never treated as newer - the safe direction.
    obj->setProperty("chainRev", chainHost.getChainRevision());
    // ...and the revision a borrow started from. THIS IS NOW A PASS-THROUGH, and it is the migration (11 Oct
    // 2026). No borrow stages a copy any more, so this build never has a live number to write - but a project
    // saved under the OLD behaviour does, and that number is half of the tie-break that decides whether the
    // V2's saved copy is restored or dropped. Writing a live -1 here would make every such copy read "no
    // borrow was open", which is the value that DROPS it: Sean's 4 Oct bug, reintroduced by a deletion.
    //
    // So what the chunk said, the chunk says again, until a V2 that has handed its copy back stops sending it.
    obj->setProperty("leaseBaseRev", restoredLeaseBaseRev_);
    juce::String json = juce::JSON::toString(juce::var(obj), true);
    dest.replaceAll(json.toRawUTF8(), json.getNumBytesAsUTF8());
    // 20 Sep 2026 (the deleted-chain-came-back observation): what the SAVED chunk carries, always logged
    // ---- AND THE COUNT IN THE CHUNK ITSELF (approved 25 Oct 2026, from the empty-rack diagnosis) ----
    //
    // His 22:08 Link came back with an empty rack, and the restore line said it faithfully:
    // "setState uid=4814a16004 (foreign/seed) chain=0 slot(s) in the chunk". The chunk predated the
    // add - but the log could not prove that, because the only save-side count was chainModel's.
    // chainModel is the SOURCE of the chunk and not the chunk: chainModelToVar can write fewer
    // entries than the model holds, and the difference between those two numbers is exactly the
    // difference between "he did not save" and "the save dropped the slot". So BOTH are printed,
    // and the restore side's count can now be compared with the save side's.
    const int inChunk = chainVar.isArray() ? chainVar.getArray()->size() : 0;
    EchoJay_NSLog(("EJLinkState: getState uid=" + instanceUid_ + " chainModel=" + juce::String((int) chainModel.size())
                   + " slot(s), IN THE CHUNK=" + juce::String(inChunk)
                   + " slot(s), host=" + juce::String(chainHost.getNumSlots()) + " slot(s), lease=" + juce::String((int) rackLeaseScope_)
                   + ", " + juce::String((juce::int64) dest.getSize()) + " bytes"
                   + (inChunk != (int) chainModel.size()
                          ? juce::String(" !! the chunk carries a DIFFERENT count from the model it was "
                                         "written from - that difference is the fault, not the save")
                          : juce::String())).toRawUTF8());
}

void LinkProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    juce::String json = juce::String::fromUTF8(static_cast<const char*>(data), sizeInBytes);
    auto v = juce::JSON::parse(json);
    if (auto* obj = v.getDynamicObject())
    {
        // COMP_PROFILE_SPEC_v1 section 5, RESTORED (2 Oct 2026 ruling). Absent fields leave the stored reading
        // alone, so an older chunk restores exactly as it does today. A reading is NEVER downgraded here: the
        // chunk only wins if it heard more than whatever this instance already holds, which is the same
        // "more heard audio wins" rule the save side and the publish use.
        if (obj->hasProperty("tlHeardS"))
        {
            const float hrd = (float)(double) obj->getProperty("tlHeardS");
            if (hrd > tlStoredHeardS_)
            {
                tlStoredHeardS_  = hrd;
                tlStoredRmsDb_   = (float)(double) obj->getProperty("tlRmsDbfs");
                tlStoredPeakDb_  = (float)(double) obj->getProperty("tlPeakDbfs");
                EchoJay_NSLog(("EJTrackLevel: restored from saved state rms="
                               + juce::String(tlStoredRmsDb_, 2) + " peak=" + juce::String(tlStoredPeakDb_, 2)
                               + " heard=" + juce::String((int) tlStoredHeardS_)
                               + "s - a restart no longer resets this track's level").toRawUTF8());
            }
        }
        // OUR OWN CHUNK RE-APPLIED (6 Sep 2026, the v6 regression): Pro Tools
        // calls SetChunk on a live instance repeatedly with that instance's own
        // current chunk (212 setState lines for ~40 instances in one session).
        // A chunk carrying the uid THIS instance already holds a registry slot
        // for is not a seed and not a restore: it is us. Identity stays settled
        // (no re-arm of chunkUid_ - the next re-claim would
        // find no holder for our own uid and re-mint, which burned 89 identities
        // in twenty minutes and dropped every host-delivered name), and name
        // PROVENANCE is not downgraded: a host-delivered or user-typed name
        // stays authoritative; the chunk's copy of a name fills in only where
        // we have none.
        const juce::String chunkUidIn = obj->getProperty("instanceUid").toString();
        const bool ownChunk = chunkUidIn.isNotEmpty() && chunkUidIn == instanceUid_ && regSlotIdx >= 0;
        if (obj->hasProperty("linkName"))
        {
            const auto n = obj->getProperty("linkName").toString();
            // C2 (7 Sep 2026): a FOREIGN chunk fills the typed name ONLY WHEN WE HAVE
            // NONE, exactly as the own-chunk arm does. It never overwrites, and never
            // downgrades the provenance of, a name the user typed. Pro Tools applies a
            // gone sibling's chunk AFTER the user (or the host) has already spoken.
            if (typedNameFromUser_)          { /* authoritative: keep name and provenance */ }
            else if (! ownChunk)             { linkName = n; typedNameFromUser_ = false; }   // seeded: provisional
            else if (linkName.isEmpty())     linkName = n;                                  // ours: fill only
        }
        if (obj->hasProperty("linkOn"))   linkOn.store((bool)obj->getProperty("linkOn"));
        // 21t-i: the stored level record, back as it was saved. A chunk WITHOUT one leaves any record this instance
        // has already built alone - a re-applied chunk (Pro Tools does this on a live instance) must not erase what
        // has been heard since the save.
        if (obj->hasProperty("levelRecord"))
        {
            const auto rec = echojay::LevelRecord::fromVar(obj->getProperty("levelRecord"));
            if (rec.valid && rec.heardSeconds >= levelRecord_.heardSeconds)
            {
                levelRecord_ = rec;
                EchoJay_NSLog(("EJLevelRecord: restored from the session - INT "
                               + juce::String(rec.intLufs, 1) + ", SHORTMAX " + juce::String(rec.shortMaxDb, 1)
                               + ", HEARD " + juce::String((int) rec.heardSeconds) + " s, age "
                               + juce::String(rec.ageSeconds(juce::Time::currentTimeMillis())) + " s").toRawUTF8());
            }
        }
        if (obj->hasProperty("alias"))    displayAlias = obj->getProperty("alias").toString().trim();   // 21n item 2
        // round (b): a saved source type is HIS, by construction - nothing else is written - so it
        // comes back with provenance "user" and keeps outranking the guesser.
        if (obj->hasProperty ("srcType"))
        {
            const auto w = obj->getProperty ("srcType").toString().trim();
            ChannelType parsed = ChannelType::Other;
            if (echojay::sourcetype::channelTypeForWireName (w, parsed))
            {
                sourceType_     = w;
                sourceTypeFrom_ = "user";
                sourceTypeName_ = obj->getProperty ("srcTypeName").toString().trim();
                EchoJay_NSLog (("EJSourceType: restored his setting for this Link - " + w
                                + ", provenance \"user\" (round (b))").toRawUTF8());
            }
            else
                EchoJay_NSLog (("EJSourceType: a saved source type \"" + w + "\" is not one of this "
                                "build's channel types - ignored, and the guesser decides instead "
                                "(a value the selector cannot show back is worse than none)").toRawUTF8());
        }
        if (obj->hasProperty("gainDb"))
            gainDb_.store(juce::jlimit(kGainMinDb, kGainMaxDb,
                                       (float)(double)obj->getProperty("gainDb")),
                          std::memory_order_relaxed);
        gainSnapPending_.store(true, std::memory_order_relaxed);  // restore: jump, don't swell
        if (obj->hasProperty("placement"))
            placement_.store(juce::jlimit((int)PlacementUnset, (int)PlacementSend,
                                          (int)obj->getProperty("placement")),
                             std::memory_order_relaxed);
        if (obj->hasProperty("projectName"))
            projectName = obj->getProperty("projectName").toString();
        if (obj->hasProperty("genre"))
            genre = obj->getProperty("genre").toString();
        if (! ownChunk)
        {
            if (chunkUidIn.isNotEmpty()) instanceUid_ = chunkUidIn;
            chunkUid_ = instanceUid_;
            syncLocalDirectory ("the uid came from restored state");
            // 28 Sep 2026: THE "AUTHORED IN THIS HOST RUN" TEST IS RETIRED. It used to be computed here from
            // authorPid / authorStartSec / authorStartUsec and read by the claim path to decide a re-mint; it
            // could not tell a gone sibling's seed from a session REOPEN, and re-minted both. The three fields
            // are still WRITTEN into the chunk as provenance a log reader can use; nothing decides on them.
        }
        // ownChunk: identity untouched, nothing re-armed
        if (obj->hasProperty("muteUser"))
            muteUserOn_.store((bool) obj->getProperty("muteUser"),
                              std::memory_order_relaxed);
        if (obj->hasProperty("keyShowRelative"))   // COMMIT 4 (absent on older saves = off)
            echojay::KeyDisplayPrefs::showRelative().store((bool) obj->getProperty("keyShowRelative"));
        if (obj->hasProperty("hostTrackName"))
        {
            // Seed the stash + dirty flag so the restored name shows and
            // publishes immediately, even in a host that never re-fires the
            // callback. setStateInformation is not guaranteed message-thread,
            // so this takes the same stash path as the live callback.
            {
                const juce::ScopedLock sl(hostNameLock_);
                // C1 (7 Sep 2026, from the v7 Pro Tools log): a FOREIGN chunk fills the host
                // name ONLY WHEN WE HAVE NONE. Pro Tools DELIVERS the track name to the fresh
                // instance first and applies a foreign chunk (the seed / a gone sibling's,
                // carrying ITS track name) ~85 ms later; this line used to overwrite the
                // delivered name and downgrade its provenance, and the invariant below then
                // dropped it as seeded. 81 deliveries, 72 empty rows, in one session.
                if (hostNameFromHost_)                  { /* authoritative: keep name and provenance */ }
                else if (! ownChunk)                    { hostTrackName_ = obj->getProperty("hostTrackName").toString(); hostNameFromHost_ = false; }   // seeded: PROVISIONAL
                else if (hostTrackName_.isEmpty())      hostTrackName_ = obj->getProperty("hostTrackName").toString();                               // ours: fill only
            }
            hostNameDirty_.store(true, std::memory_order_release);
        }
        if (obj->hasProperty("chainMasterWet"))
            chainHost.setMasterWet((float)(double)obj->getProperty("chainMasterWet"));
#if ECHOJAY_LINK_STATE_DIAG
        EchoJay_NSLog(("EJLinkState: setState linkOn=" + juce::String((int)linkOn.load())
                       + " (hadProp=" + juce::String((int)obj->hasProperty("linkOn"))
                       + ") name=\"" + linkName + "\"").toRawUTF8());
#endif
        // An OPEN editor must sync its toggle to the restored value — a
        // stale ON toggle writing itself back into the processor is a
        // re-activation path. Message thread deferred: setStateInformation's
        // calling thread is host-defined.
        juce::MessageManager::callAsync([this, alive = std::weak_ptr<bool> (aliveToken_)]
        {
            if (alive.expired()) return;
            updateShmState();
            if (onLinkStateChanged) onLinkStateChanged();
        });
        if (obj->hasProperty("editorW"))  editorW = juce::jlimit(900, 1800, (int)obj->getProperty("editorW"));
        if (obj->hasProperty("editorH"))  editorH = juce::jlimit(580, 1200, (int)obj->getProperty("editorH"));
        // 4 Oct 2026: the SAVED revision is kept as its own value and NEVER written into chainRevision_.
        // chainRevision_ is the live structural counter the preflight staleness guards compare against, and
        // overwriting it would make an edit proposed against the restored rack look stale (or fresh) for reasons
        // that have nothing to do with the rack. This is a record of what the chunk said, for the tie-break only.
        if (obj->hasProperty("chainRev"))
        {
            restoredChainRev_ = (int) obj->getProperty("chainRev");
            restoredLeaseBaseRev_ = obj->hasProperty("leaseBaseRev") ? (int) obj->getProperty("leaseBaseRev") : -1;
            EchoJay_NSLog(("EJLinkState: chunk carried chainRev=" + juce::String(restoredChainRev_)
                           + " (kept for the borrowed-rack tie-break; the live revision is untouched at "
                           + juce::String(chainHost.getChainRevision()) + ")").toRawUTF8());
        }
        if (obj->hasProperty("chain"))
        {
            EchoJay_NSLog(("EJLinkState: setState uid=" + chunkUidIn + (ownChunk ? " (own)" : " (foreign/seed)") + " chain=" + juce::String(obj->getProperty("chain").isArray() ? obj->getProperty("chain").getArray()->size() : -1)
                           + " slot(s) in the chunk; host has " + juce::String(chainHost.getNumSlots()) + " slot(s) now").toRawUTF8());   // 20 Sep 2026: what a restore is about to build
            // Restore on the message thread — sequential async instantiation;
            // missing plugins become named empty slots, the rest still load.
            auto chainVar = obj->getProperty("chain");
            juce::MessageManager::callAsync([this, chainVar, alive = std::weak_ptr<bool> (aliveToken_)]
            {
                if (alive.expired()) return;
                restoreChainFromVar(chainVar);
            });
        }
    }
    // Schedule registration on the message thread. If prepareToPlay has already
    // run (some hosts call it before setStateInformation), this triggers
    // registration immediately. If prepareToPlay comes later, the prepareToPlay
    // callAsync above will also call updateShmState — both are idempotent.
    juce::MessageManager::callAsync([this, alive = std::weak_ptr<bool> (aliveToken_)]
                                   { if (! alive.expired()) updateShmState(); });
    // 21t-j (28 Sep 2026 ruling): A RESTORED ROLE IS PUBLISHED. This function had stored placement_ in memory and
    // published nothing, so a copied insert (and a reopened session, which restores through this same function)
    // carried its role only inside the plugin. The snapshot goes out from here too, from the state this restore
    // just put in place.
    publishRegistrySnapshot ("state restored");
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new LinkProcessor(); }
