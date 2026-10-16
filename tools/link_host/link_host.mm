// =============================================================================
//  link_host - ONE EchoJay Link, IN ITS OWN PROCESS (Sean, 16 Oct 2026)
// =============================================================================
//
//  WHY. Until today every Link in a test lived in the rig's own process. The
//  CHANNEL between the V2 and a Link was real - it is files and a shared-memory
//  ring, and it does not care how many processes there are - but the ADDRESS
//  SPACE crossing was not, and three of this week's faults lived exactly there:
//  a Link with no road to ask for a parameter map, a sidecar field the Link
//  never wrote, and a borrowed mirror the V2 preferred over the Link's own
//  published rack. Each of those would have been caught a day earlier by a Link
//  the V2 could not see inside.
//
//  Sean's words: *"Run each Link in its own process if at all possible, the way
//  Logic hosts AUs out-of-process (AUHostingService), so channel timing between
//  V2 and Link matches Logic. If it can't be done, say why and what timing
//  difference remains."*
//
//  IT CAN BE DONE, and this is it. What it does NOT reproduce is written down in
//  docs/RIG_LINK_CAPABILITY.md and repeated here because it matters: Logic hosts
//  AUs out of process but still renders them from ONE audio graph, so V2 block N
//  and Link block N belong to the same render pass. Two processes pump their own
//  blocks on their own clocks, so anything that depends on the two racks seeing
//  the same audio instant (a level match across the pair, a meter comparison, a
//  latency report) is NOT block-locked here the way it is in Logic. The matrix
//  therefore runs the channel/lock/window/view scenarios on this setup and the
//  audio/meter ones in process, and every row says which it ran on.
//
//  ISOLATION, the standing rule: ECHOJAY_STATE_HOME must be set, or this
//  refuses to start. It shares that root with the rig, which is exactly how two
//  Logic instances find each other through the shared directory.
//
//  THE CHANNEL (this process's own, not the plugin's):
//      <state home>/rig/linkhost-<label>-cmd.json    the runner writes
//      <state home>/rig/linkhost-<label>-ack.json    this writes back
//      <state home>/rig/linkhost-<label>.json        what this is holding, 2 Hz
//
//  COMMANDS: open_editor, close_editor, build (a chain of plugin names, sent
//  down the product's OWN chain-cmd road), report, quit.
//
//  USAGE  link_host <Link.component> <label> [--prebuild "Name,Name"] [--open-editor]
// =============================================================================
#include <CoreFoundation/CoreFoundation.h>   // before JUCE: MacTypes Point (the rig's own note)
#include <JuceHeader.h>
#include "EJStateRoot.h"
#include "LinkShm.h"
#include <cstdio>
#include <unistd.h>

namespace
{

juce::File rigDir()
{
    auto d = echojay::userStateHome().getChildFile ("rig");
    d.createDirectory();
    return d;
}

/** THIS PROCESS'S OWN LINK, BY PID. Two link_hosts share one directory, so a uid
    cannot be found by "the only rack sidecar there is". The sidecar carries the
    publisher's pid (LinkShm::RackSidecar::publisherPid, written by the Link
    itself), so each host identifies its own instance by asking the OS who it is -
    an identity, not a guess, which is the same rule the rest of this tree follows. */
juce::String ownUid()
{
    int err = 0;
    const auto dir = LinkShm::resolveDir (err);
    if (dir.isEmpty()) return {};
    const auto me = (int) ::getpid();
    // The uid is in the FILE NAME ("rack-<uid>.json"), so the reader the product
    // uses can be called with it - no second parser for the same file.
    for (juce::DirectoryEntry e : juce::RangedDirectoryIterator (juce::File (dir), false, "rack-*.json"))
    {
        const auto uid = e.getFile().getFileNameWithoutExtension().fromFirstOccurrenceOf ("rack-", false, false);
        if (uid.isEmpty()) continue;
        const auto rc = LinkShm::readRackSidecar (dir, uid);
        if (rc.valid && rc.publisherPid == me) return rc.uid;
    }
    return {};
}

struct Host
{
    juce::String label;
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    std::unique_ptr<juce::DocumentWindow> window;
    juce::AudioBuffer<float> buf { 2, 512 };
    juce::MidiBuffer midi;
    double phase = 0.0;
    juce::String uid;
    int lastSeq = -1;
    juce::String lastNote;

    void block (double sr)
    {
        const int n = buf.getNumSamples();
        for (int i = 0; i < n; ++i)
        {
            // The same -12 dBFS tone the rig feeds, for the same reason: a loop that
            // has heard nothing never arms, so silence would prove nothing.
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
        if (editor != nullptr) { lastNote = "already open"; return; }
        editor.reset (plugin->createEditorIfNeeded());
        if (editor == nullptr) { lastNote = "the Link gave no editor"; return; }
        window.reset (new juce::DocumentWindow (label, juce::Colours::black,
                                                juce::DocumentWindow::closeButton, true));
        window->setUsingNativeTitleBar (true);
        window->setContentNonOwned (editor.get(), true);
        window->setVisible (true);
        lastNote = "opened";
    }

    void closeEditor()
    {
        // The host's own order: the window first, then the editor - which is the
        // order a real host tears them down in.
        if (window != nullptr) { window->setVisible (false); window->clearContentComponent(); window.reset(); }
        editor.reset();
        lastNote = "closed";
    }

    /** A rack of real plugins, sent down the PRODUCT'S OWN road: the same
        chain-cmd-<uid>.json file the V2's Build button writes. Not a private
        back door - a second build path is the thing this codebase has already
        paid for twice. */
    void build (const juce::StringArray& names)
    {
        if (uid.isEmpty()) { lastNote = "no uid yet, so there is nothing to send to"; return; }
        int err = 0;
        const auto dir = LinkShm::resolveDir (err);
        if (dir.isEmpty()) { lastNote = "no shared directory"; return; }
        juce::Array<juce::var> chain;
        for (const auto& n : names)
        {
            if (n.trim().isEmpty()) continue;
            auto* e = new juce::DynamicObject();
            e->setProperty ("name", n.trim());
            chain.add (juce::var (e));
        }
        auto* cmd = new juce::DynamicObject();
        cmd->setProperty ("v", 1);
        cmd->setProperty ("seq", LinkShm::nextCtrlSeq());
        cmd->setProperty ("chain", juce::var (chain));
        cmd->setProperty ("sourceNote", "link_host prebuild");
        juce::File (dir + "chain-ack-" + uid + ".json").deleteFile();
        juce::File (dir + "chain-cmd-" + uid + ".json")
            .replaceWithText (juce::JSON::toString (juce::var (cmd), true));
        lastNote = "sent a build of " + juce::String (chain.size()) + " slot(s)";
    }

    juce::var stateVar() const
    {
        auto* o = new juce::DynamicObject();
        o->setProperty ("label", label);
        o->setProperty ("pid", (int) ::getpid());
        o->setProperty ("uid", uid);
        o->setProperty ("editorOpen", editor != nullptr);
        o->setProperty ("lastNote", lastNote);
        int err = 0;
        const auto dir = LinkShm::resolveDir (err);
        if (dir.isNotEmpty() && uid.isNotEmpty())
        {
            const auto rc = LinkShm::readRackSidecar (dir, uid);
            o->setProperty ("sidecarValid", rc.valid);
            if (rc.valid)
            {
                o->setProperty ("slotCount", (int) rc.slots.size());
                juce::StringArray names, landed;
                for (const auto& s : rc.slots)
                {
                    names.add (s.name);
                    if (s.landedValues.isNotEmpty()) landed.add (s.name + "{" + s.landedValues + "}");
                }
                o->setProperty ("slots", names.joinIntoString (", "));
                o->setProperty ("landed", landed.joinIntoString (" | "));
                o->setProperty ("revision", rc.revision);
            }
        }
        return juce::var (o);
    }
};

} // namespace

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    if (! echojay::stateIsIsolated())
    {
        std::printf ("link_host REFUSED: ECHOJAY_STATE_HOME is not set. This process shares a state root\n"
                     "with the rig and must never touch ~/Library/EchoJay (the 6 Sep ruling).\n");
        return 2;
    }
    if (argc < 3)
    {
        std::printf ("usage: link_host <Link.component> <label> [--prebuild \"Name,Name\"] "
                     "[--track-name \"88_7even Kick_2_01\"] [--open-editor]\n");
        return 2;
    }

    const juce::File comp { juce::String (argv[1]) };
    Host host; host.label = juce::String (argv[2]);
    juce::StringArray prebuild;
    juce::String trackName;
    bool openAtStart = false;
    for (int i = 3; i < argc; ++i)
    {
        const juce::String a { argv[i] };
        if (a == "--prebuild" && i + 1 < argc) prebuild.addTokens (juce::String (argv[++i]), ",", "");
        else if (a == "--track-name" && i + 1 < argc) trackName = juce::String (argv[++i]);
        else if (a == "--open-editor")         openAtStart = true;
    }

    juce::AudioPluginFormatManager fm;
    // The FREE function, not the member: in the headless audio-processors module the
    // member is deleted, and the rig says so in its own comment at the same line.
    juce::addDefaultFormatsToManager (fm);
    juce::OwnedArray<juce::PluginDescription> found;
    for (auto* f : fm.getFormats())
        if (f->getName() == "AudioUnit") f->findAllTypesForFile (found, comp.getFullPathName());
    if (found.isEmpty())
    {
        // By path, never a scan: the standing rule. A bundle that will not enumerate is a refusal.
        std::printf ("link_host REFUSED: no AU types in %s\n", comp.getFullPathName().toRawUTF8());
        return 2;
    }
    juce::String err;
    host.plugin = fm.createPluginInstance (*found[0], 48000.0, 512, err);
    if (host.plugin == nullptr)
    {
        std::printf ("link_host REFUSED: %s would not instantiate: %s\n",
                     comp.getFileName().toRawUTF8(), err.toRawUTF8());
        return 2;
    }
    const double sr = 48000.0;
    host.plugin->setPlayConfigDetails (2, 2, sr, 512);
    host.plugin->prepareToPlay (sr, 512);
    // ---- THE TRACK NAME, THROUGH THE DOOR LOGIC USES (16 Oct 2026, Sean) ------------------------
    //
    // Every Link in the rig was called "Untitled". The live pass showed what that costs: asked to
    // "make the kick punchier", the model answered "I don't see a track named Kick - did you mean one
    // of these linked tracks?" and offered Untitled, Untitled 2, Untitled 3, Untitled 4. The rows were
    // testing a project with no track names in it, which is not the project anyone has.
    //
    // updateTrackProperties is the HOST's own door - the one Logic and every AU host calls, and the
    // one LinkProcessor already treats as authoritative (hostNameFromHost_). A name delivered here is
    // a name delivered the way Logic delivers it, so what the rows exercise is the real path.
    // --track-name overrides; the default is a name shaped like one of Sean's own.
    {
        juce::AudioProcessor::TrackProperties tp;
        tp.name = trackName.isNotEmpty() ? trackName : host.label;
        host.plugin->updateTrackProperties (tp);
        std::printf ("link_host \"%s\": track name delivered as \"%s\" (the host's own door)\n",
                     host.label.toRawUTF8(), tp.name->toRawUTF8());
    }
    {
        // THE DIRECTORY THIS PROCESS RESOLVED, on its own line. A Link that ends up on a different
        // shared directory from the V2 is invisible to it, and the only way to tell that from "it
        // never registered" is to have both processes say where they are looking.
        int derr = 0;
        const auto sharedDir = LinkShm::resolveDir (derr);
        std::printf ("link_host \"%s\": %s up, pid %d, state root %s, shared dir %s\n",
                     host.label.toRawUTF8(), found[0]->name.toRawUTF8(), (int) ::getpid(),
                     echojay::userStateHome().getFullPathName().toRawUTF8(),
                     sharedDir.isEmpty() ? "(none)" : sharedDir.toRawUTF8());
    }
    std::fflush (stdout);

    const auto cmdFile   = rigDir().getChildFile ("linkhost-" + host.label + "-cmd.json");
    const auto ackFile   = rigDir().getChildFile ("linkhost-" + host.label + "-ack.json");
    const auto stateFile = rigDir().getChildFile ("linkhost-" + host.label + ".json");
    cmdFile.deleteFile(); ackFile.deleteFile();

    bool quit = false;
    bool prebuilt = false;
    juce::int64 lastPublish = 0;
    const auto startedMs = juce::Time::currentTimeMillis();

    while (! quit)
    {
        // AUDIO FIRST, always: the registry pass, the sidecar publish and every
        // timer on this plugin only happen once it has processed.
        for (int i = 0; i < 8; ++i) host.block (sr);
        // The message thread, the same idiom the rig uses (no bare sleep, because
        // the plugin's own timers live here).
        juce::Timer::callPendingTimersSynchronously();
        CFRunLoopRunInMode (kCFRunLoopDefaultMode, 0.01, false);

        if (host.uid.isEmpty())
        {
            host.uid = ownUid();
            if (host.uid.isNotEmpty())
                { std::printf ("link_host \"%s\": uid %s\n", host.label.toRawUTF8(), host.uid.toRawUTF8());
                  std::fflush (stdout); }
        }

        // The prebuilt rack, once the instance has a uid to send to.
        if (! prebuilt && host.uid.isNotEmpty() && ! prebuild.isEmpty()
            && juce::Time::currentTimeMillis() - startedMs > 2000)
        {
            host.build (prebuild);
            prebuilt = true;
            std::printf ("link_host \"%s\": %s\n", host.label.toRawUTF8(), host.lastNote.toRawUTF8());
            std::fflush (stdout);
        }
        if (openAtStart && host.editor == nullptr && host.uid.isNotEmpty())
        {
            host.openEditor();
            openAtStart = false;
        }

        if (cmdFile.existsAsFile())
        {
            const auto v = juce::JSON::parse (cmdFile.loadFileAsString());
            if (auto* o = v.getDynamicObject())
            {
                const int seq = (int) o->getProperty ("seq");
                if (seq != host.lastSeq)
                {
                    host.lastSeq = seq;
                    const auto cmd = o->getProperty ("cmd").toString();
                    if      (cmd == "open_editor")  host.openEditor();
                    else if (cmd == "close_editor") host.closeEditor();
                    else if (cmd == "report")       host.lastNote = "reported";
                    else if (cmd == "quit")       { quit = true; host.lastNote = "quitting"; }
                    else if (cmd == "build")
                    {
                        juce::StringArray names;
                        names.addTokens (o->getProperty ("names").toString(), ",", "");
                        host.build (names);
                    }
                    else host.lastNote = "unknown command \"" + cmd + "\"";

                    auto* ack = new juce::DynamicObject();
                    ack->setProperty ("seq", seq);
                    ack->setProperty ("cmd", cmd);
                    ack->setProperty ("note", host.lastNote);
                    ack->setProperty ("state", host.stateVar());
                    ackFile.replaceWithText (juce::JSON::toString (juce::var (ack), true));
                    std::printf ("link_host \"%s\": %s -> %s\n", host.label.toRawUTF8(),
                                 cmd.toRawUTF8(), host.lastNote.toRawUTF8());
                    std::fflush (stdout);
                }
            }
        }

        const auto now = juce::Time::currentTimeMillis();
        if (now - lastPublish >= 500)
        {
            lastPublish = now;
            stateFile.replaceWithText (juce::JSON::toString (host.stateVar(), true));
        }
    }

    host.closeEditor();
    // The instance goes before the process does, so the Link's own clean-exit
    // hygiene runs (it deletes its uid-keyed files) rather than leaving litter
    // for the reaper.
    host.plugin->releaseResources();
    host.plugin.reset();
    stateFile.deleteFile();
    std::printf ("link_host \"%s\": gone\n", host.label.toRawUTF8());
    return 0;
}
