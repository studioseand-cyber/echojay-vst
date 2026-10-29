#pragma once

#include <JuceHeader.h>
#include "EJStateRoot.h"
#include "EJRemoteControl.h"   // echojay::embedAllowed - the same pid question the agent's embed op asks

/** THE SWITCH OVER THE REAL-EDITOR LIVE VIEW (his condition, 26 Oct 2026).

    *"Put the real editor behind a switch (default off until I've tested it), never remove or change
     the copy road in these rounds, and if round 2 doesn't look and feel at least as good as the copy
     ... we switch back and park it."*

    So there are two live views in the build and ONE place that decides between them. The copy-leads
    view (the one that works today) is what every build takes until the switch is turned on, and
    turning it off again is one act with nothing else to undo.

    TWO THINGS MUST BOTH SAY YES:
      1. the switch, which is the user's, and is OFF unless he turned it on;
      2. echojay::embedAllowed, which is the measurement - the Link's own published pid against ours.
         Unknown is not permission (an older Link publishes no pid), so that road falls back too.

    And whichever road is taken, the reason is a sentence: a live view that silently became the other
    kind is the class of thing that gets found months later in a bug report about something else. */
namespace echojay
{

/** The file the switch lives in: present = on. A file rather than a preference because it has to be
    readable from both bundles and settable with one line while he is testing; round 2 puts a toggle
    in Settings on top of this same file, so there is still one authority. */
inline juce::File realEditorFlagFile()
{
    return echojay::userAppData().getChildFile ("EchoJay").getChildFile ("real_editor_live_view");
}

/** The switch alone, with no opinion about processes. OFF by default, by absence. */
inline bool realEditorSwitchOn()
{
    return realEditorFlagFile().existsAsFile();
}

/** Turn it on or off. Round 2's Settings toggle calls this; nothing in the product calls it today. */
inline bool setRealEditorSwitch (bool on)
{
    auto f = realEditorFlagFile();
    if (! on) return f.deleteFile() || ! f.existsAsFile();
    f.getParentDirectory().createDirectory();
    return f.replaceWithText ("on") || f.existsAsFile();
}

/** Which live view this Link gets. PURE, so it can be driven exactly in a guard. */
inline bool realEditorRoad (bool switchOn, int linkPublisherPid, int ourPid) noexcept
{
    return switchOn && echojay::embedAllowed (linkPublisherPid, ourPid);
}

/** ---- AND OUR OWN DEVICES TAKE THE COPY ROAD WHATEVER THE SWITCH SAYS (his Test 5, 29 Oct) ---

    *"Test 5 (switch ON, EchoJay Compressor on the Kick Link, opened in the V2): the meter doesn't
    move, the knob turns but nothing changes in the sound while the V2 is open, and when Sean
    closed the V2 the compression appeared ... So with the switch on, EchoJay devices fall to the
    edit-here, write-on-leave road."*

    THE REASON IS STRUCTURAL, not a bug in that fallback. The real-editor road shows the LINK'S OWN
    editor, embedded from the other instance in this process - and for a hosted plugin that is the
    real thing, live, because the audio is the Link's. An EchoJay device has no separate editor to
    embed: its view is OUR code either way, so the "real editor" for one is just this plugin's own
    instance of it, which is a different device in a different rack - no Link audio, and settings
    that land when he leaves.

    The copy road is the live one for our devices, and it is live in both directions now: the Link
    publishes their values through their schema (item 2b) and applies ours through it (his R4). So
    this is not a downgrade, it is the road that does what the switch was turned on to get.

    `slotIsOneOfOurs` is ChainHost::isBuiltinName of what the rack says is in that slot. */
inline bool realEditorRoadForSlot (bool switchOn, int linkPublisherPid, int ourPid,
                                   bool slotIsOneOfOurs) noexcept
{
    if (slotIsOneOfOurs) return false;
    return realEditorRoad (switchOn, linkPublisherPid, ourPid);
}

/** The sentence for that case, so a road taken for this reason is never a silent one. */
inline juce::String ourDeviceRoadWhy (const juce::String& name)
{
    return "\"" + name + "\" is one of EchoJay's own devices, so the live view is its copy on this "
           "rack, fed by that Link's audio, with its knobs travelling to the Link as you turn them. "
           "The real-editor switch applies to hosted plugins, which have an editor of their own to "
           "show.";
}

/** Why that road, in words, for the log and for the card. Never silent, and it names WHICH of the
    two tests said no - "the switch is off" and "that Link is in another process" are different
    facts and he must not have to guess which he is looking at. */
inline juce::String realEditorRoadWhy (bool switchOn, int linkPublisherPid, int ourPid)
{
    if (! switchOn)
        return "the live view shows its own copy of the plugin (the real-editor switch is off)";
    if (! echojay::embedAllowed (linkPublisherPid, ourPid))
        return linkPublisherPid <= 0
             ? "the live view shows its own copy of the plugin (that Link does not publish which "
               "process it is in, and unknown is not permission)"
             : "the live view shows its own copy of the plugin (that Link runs in another process, "
               "pid " + juce::String (linkPublisherPid) + " against ours " + juce::String (ourPid) + ")";
    return "the live view will show the Link's own plugin editor (the switch is on and that Link is "
           "in this process, pid " + juce::String (ourPid) + ")";
}

} // namespace echojay
