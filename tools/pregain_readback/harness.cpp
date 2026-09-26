// Pre-gain SEND half - INVERTED 26 Sep 2026 (21t-e item 2, Sean's ruling).
//
// WHAT THIS GUARD USED TO ASSERT, and why it no longer can: composing for a Link staged a
// pre-gain from that Link's integrated loudness and Build sent it as
// ctrl-cmd {preGainDb, preGainUserSet:false}. That is retired. A "harder" turn on a
// one-slot TUNER chain came back with +4.4 dB of gain nobody asked for, because the carry
// was keyed on the compose, not on what the chain contains. Gain now moves only from a
// response that carries it for a compressor chain.
//
// So the same fixture asserts the opposite, and the assertions are the same shape: a real
// registry, a real Link slot, a real frame whose integrated (-20) would have produced
// exactly +2.0 dB, the real editor's compose path, and the real ctrl-cmd directory.
// On the pre-21t-e tree every check here fails - it stated +2.0 and wrote the file.
//
// The FIGURE is not retired: it is a real reading and [CHAIN LEVELS] still prints it for the
// model to use. What is retired is carrying it to the Link. linkmeter_harness holds the
// printed half.
//
// End-to-end readback from the Link's sidecar stays IN-HOST: LinkProcessor lives only in the
// EchoJayLink target, and one binary cannot host both real instances.
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "LinkShm.h"
#include <cstdio>
#include <cstring>
using namespace juce;

static void* g_rmap = nullptr; static int g_slot = -1; static uint32_t g_hb = 100;
static int g_fails = 0;
static void check(const char* l, bool ok){ std::fprintf(stderr,"   %-58s %s\n",l,ok?"PASS":"FAIL  <-- RED"); if(!ok)++g_fails; }

static void writeFrame(int slot, const LinkMeterFrame& f)
{
    LinkMeterFrame* dst = LinkShm::meterFrames(g_rmap) + slot;
    const uint32_t s0 = LinkShm::loadRelaxed(&dst->seq) & ~1u;
    LinkShm::storeRelease(&dst->seq, s0+1);
    std::memcpy((uint8_t*)dst+sizeof(uint32_t),(const uint8_t*)&f+sizeof(uint32_t),sizeof(LinkMeterFrame)-sizeof(uint32_t));
    LinkShm::storeRelease(&dst->seq, s0+2);
}
static const char* kUid = "TLINK01";

int main()
{
    ScopedJuceInitialiser_GUI gui;
    File tmp = File::getSpecialLocation(File::tempDirectory).getChildFile("ej_pregain_send_"+String(Time::getMillisecondCounter()));
    tmp.createDirectory(); setenv("ECHOJAY_STATE_HOME", tmp.getFullPathName().toRawUTF8(), 1);
    int derr=0; const String dir = LinkShm::resolveDir(derr);

    // Seed a live Link with a real frame (integrated -20 -> pre-gain clamp(-18 - -20) = +2.0).
    int fd=-1,oerr=0; g_rmap = LinkShm::openRegistry(dir, fd, oerr);
    g_slot = LinkShm::claimSlot(g_rmap, "Nafe Lead Vocal", "", kUid, 48000.0f, 2u);
    auto* slots = LinkShm::regSlots(g_rmap);
    slots[g_slot].placement=1; slots[g_slot].dialCapable=1;
    LinkShm::storeRelease(&slots[g_slot].heartbeat,100u); LinkShm::storeRelease(&slots[g_slot].inUse,1u);
    LinkMeterFrame f{};
    f.momentary=-18.5f; f.shortTerm=-19.2f; f.integrated=-20.0f; f.truePeakMax=-1.3f; f.crest=8.4f;
    f.correlation=0.55f; f.width=0.72f; f.lra=6.1f; f.shortTermTP=-2.4f;
    f.bandRel[0]=1.4f; f.bandRel[1]=0.3f; f.bandRel[2]=-0.6f; f.bandRel[3]=-0.9f; f.bandRel[4]=0.7f; f.bandRel[5]=3.4f;
    f.audioBlocks=1000; f.audioStale=0;
    writeFrame(g_slot, f);

    EchoJayProcessor proc;
    for (int k=0;k<8;++k){ LinkShm::storeRelease(&slots[g_slot].heartbeat,++g_hb); proc.refreshLinkRegistry(); proc.updateLinkAudioRecency(); }

    std::unique_ptr<AudioProcessorEditor> edBase(proc.createEditor());
    auto* ed = dynamic_cast<EchoJayEditor*>(edBase.get());
    if(!ed){ std::fprintf(stderr,"HARNESS ERROR: no editor\n"); return 2; }

    // A ctrl-cmd from an earlier run would answer for us; start from nothing.
    File cmd(dir + "ctrl-cmd-" + String(kUid) + ".json");
    cmd.deleteFile();

    // Compose for the Link. On the old tree this staged +2.0 dB for that uid.
    StringArray mf;
    const String linkOut = ed->testAssembleChainInjections("build me a mastering chain", String(kUid), &mf);
    const float staged = ed->testPendingLinkPreGainDb();          // NaN when nothing is staged
    const float mainPreBefore = proc.getChainHost().getPreGainDb();
    std::fprintf(stderr,"staged pre-gain=%.2f dB (NaN expected); main ChainHost pre-gain before=%.2f\n", staged, mainPreBefore);

    check("NOTHING is staged for the Link at compose time (the +4.4 dB on a tuner chain)",
          ! (staged == staged));          // NaN != NaN
    // The figure itself is still computed and PRINTED - that is the reading, not an action.
    check("...while [CHAIN LEVELS] still states the figure it measured (+2.0 dB from int -20)",
          linkOut.contains("pre-gain to +2.0 dB"));

    // Build's send site, driven exactly as Build drives it. Nothing may go out.
    ed->applyPendingLinkPreGain();

    bool cmdHasPreGain = false;
    float sentPreGain = std::numeric_limits<float>::quiet_NaN();
    if (cmd.existsAsFile())
    {
        const var v = JSON::parse(cmd.loadFileAsString());
        if (auto* o = v.getDynamicObject())
            if (o->hasProperty("preGainDb"))
            { cmdHasPreGain = true; sentPreGain = (float)(double)o->getProperty("preGainDb"); }
    }
    std::fprintf(stderr,"ctrl-cmd exists=%d  carries preGainDb=%d (%.2f)  |  main ChainHost pre-gain after=%.2f\n",
                 (int)cmd.existsAsFile(), (int)cmdHasPreGain, sentPreGain, proc.getChainHost().getPreGainDb());

    check("no ctrl-cmd preGainDb is written for the Link (RED on the old tree: it carried +2.0)",
          ! cmdHasPreGain);
    check("main plugin's OWN ChainHost pre-gain UNTOUCHED (the older bug, still gone)",
          std::abs(proc.getChainHost().getPreGainDb() - mainPreBefore) < 0.001f);
    check("and a second compose does not stage one either (the carry is gone, not consumed)",
          [&]{ StringArray mf2; ed->testAssembleChainInjections("harder", String(kUid), &mf2);
               const float again = ed->testPendingLinkPreGainDb(); return ! (again == again); }());

    std::fprintf(stderr,"\n==== PRE-GAIN SEND HALF (no compose-time carry): %s (%d failed) ====\n", g_fails==0?"GREEN":"RED", g_fails);
    std::fprintf(stderr,"NOTE: the Link's preGainDb CONSUME path is untouched and still shipping (the mixer's Pre mode\n");
    std::fprintf(stderr,"      and a response that carries gain for a compressor chain); what is gone is the compose-time carry.\n");
    return g_fails==0?0:1;
}
