// Pre-gain SEND-half harness (16 Sep 2026). The end-to-end readback from the Link's
// rack sidecar is NOT bench-reachable: LinkProcessor lives only in the EchoJayLink
// target, and the main + Link are separate plugins with overlapping shared code, so
// one binary cannot host both real instances (they would double-define). The true
// end-to-end is IN-HOST (Sean sees the knob move). What IS falsifiable offline is
// the send half of the fix, against the main lib: on a Link build, the main must
// write ctrl-cmd-<uid>.json carrying the ONE clamped pre-gain (== stated), and must
// NOT write its OWN ChainHost pre-gain (the old bug). The Link's CONSUME half
// (preGainDb ctrl-cmd -> setPreGainDb -> sidecar) is shipping, unchanged behaviour
// (the mixer's Pre mode) and is verified separately against the Link target.
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

    // Compose for the Link -> pendingLinkPreGain_ set (keyed by uid).
    StringArray mf; ed->testAssembleChainInjections("build me a mastering chain", String(kUid), &mf);
    const float stated = ed->testPendingLinkPreGainDb();
    const float mainPreBefore = proc.getChainHost().getPreGainDb();
    std::fprintf(stderr,"stated pre-gain=%.2f dB (expect +2.0); main ChainHost pre-gain before=%.2f\n", stated, mainPreBefore);
    check("pre-gain computed for the Link (== +2.0, one clamp)", std::abs(stated - 2.0f) < 0.01f);

    // THE FIX: send it. Must write ctrl-cmd, must NOT touch the main's ChainHost.
    ed->applyPendingLinkPreGain();

    File cmd(dir + "ctrl-cmd-" + String(kUid) + ".json");
    float sentPreGain = std::numeric_limits<float>::quiet_NaN(); bool sentUserSet = true;
    if (cmd.existsAsFile())
    {
        var v = JSON::parse(cmd.loadFileAsString());
        if (auto* o = v.getDynamicObject())
        {
            if (o->hasProperty("preGainDb"))      sentPreGain = (float)(double)o->getProperty("preGainDb");
            if (o->hasProperty("preGainUserSet")) sentUserSet = (bool)o->getProperty("preGainUserSet");
        }
    }
    std::fprintf(stderr,"ctrl-cmd exists=%d  preGainDb=%.2f  preGainUserSet=%d  |  main ChainHost pre-gain after=%.2f\n",
                 (int)cmd.existsAsFile(), sentPreGain, (int)sentUserSet, proc.getChainHost().getPreGainDb());

    check("ctrl-cmd-<uid>.json WRITTEN (routed to the Link, not the main)", cmd.existsAsFile());
    check("ctrl-cmd preGainDb == stated (== the printed value, one number)", std::abs(sentPreGain - stated) < 0.01f);
    check("ctrl-cmd preGainUserSet == false (auto, not hand-set)", sentUserSet == false);
    check("main plugin's OWN ChainHost pre-gain UNTOUCHED (the old bug is gone)",
          std::abs(proc.getChainHost().getPreGainDb() - mainPreBefore) < 0.001f);
    check("pendingLinkPreGain_ consumed (fires exactly once)",
          ! (ed->testPendingLinkPreGainDb() == ed->testPendingLinkPreGainDb()) /*NaN now*/);

    std::fprintf(stderr,"\n==== PRE-GAIN SEND HALF: %s (%d failed) ====\n", g_fails==0?"GREEN":"RED", g_fails);
    std::fprintf(stderr,"NOTE: end-to-end sidecar readback is IN-HOST only (Link is a separate target); the Link's\n");
    std::fprintf(stderr,"      preGainDb consume is shipping behaviour (mixer Pre mode), verified against the Link target.\n");
    return g_fails==0?0:1;
}
