/*  BORROWED-HOST DIAL LEG (10 Sep 2026 ruling). Main archive. Isolated root required.
    A THIRD-PARTY-style probe (a real AudioPluginInstance with one Gain parameter, NOT a builtin) is
    loaded into a BORROWED ChainHost. Its param map is on disk (written by a Main host). The leg asserts
    the probe's parameter actually MOVES to the dialled value - proof the borrowed host received its map
    and applied its settings. A builtin cannot move this probe, so builtin-only success fails the leg.
    Negative control: the pre-dial Build G archive, where a Borrowed host does not load the map -> the
    parameter stays at its default -> FAIL. Scale: N spectator slots so the host runs at Sean's size.  */
#include <CoreFoundation/CoreFoundation.h>
#include <JuceHeader.h>
#include "EJStateRoot.h"
#include "ChainHost.h"
#include <cstdio>

static const char* kProbeId = "legs:thirdparty:gain";
static juce::PluginDescription probeDesc()
{
    juce::PluginDescription d; d.name = "Legs Gain"; d.pluginFormatName = "AudioUnit";
    d.manufacturerName = "Legs"; d.fileOrIdentifier = kProbeId; d.uniqueId = 0x1E6D9A1; d.version = "1.0";
    return d;
}
struct GainParam final : public juce::HostedAudioProcessorParameter
{
    float v = 0.5f;
    float getValue() const override { return v; }
    void setValue (float nv) override { v = juce::jlimit (0.0f, 1.0f, nv); }
    float getDefaultValue() const override { return 0.5f; }
    juce::String getName (int) const override { return "Gain"; }
    juce::String getLabel() const override { return {}; }
    float getValueForText (const juce::String& t) const override { return t.getFloatValue(); }
    juce::String getText (float value, int) const override { return juce::String (value, 3); }
    juce::String getParameterID() const override { return "gain"; }
};
struct GainProbe final : public juce::AudioPluginInstance
{
    GainProbe() : juce::AudioPluginInstance (BusesProperties().withInput ("In", juce::AudioChannelSet::stereo(), true)
                                                              .withOutput ("Out", juce::AudioChannelSet::stereo(), true))
    { addHostedParameter (std::make_unique<GainParam>()); }
    void fillInPluginDescription (juce::PluginDescription& d) const override { d = probeDesc(); }
    const juce::String getName() const override { return "Legs Gain"; }
    void prepareToPlay (double, int) override {} void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; } bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; } bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; } int getCurrentProgram() override { return 0; } void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; } void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {} void setStateInformation (const void*, int) override {}
};

// { "gain_db": { index:0, kind:"anchored", anchors:[[-24,0],[24,1]] } } -> -6 dB = norm 0.375
static juce::var gainMap (const juce::String& fp)
{
    auto* entry = new juce::DynamicObject();
    entry->setProperty ("index", 0); entry->setProperty ("kind", "anchored"); entry->setProperty ("trust", "setread");
    juce::var anchors;   // gaps 0.375 / 0.625 are not integer multiples, so this is a continuous curve
    { const double pts[3][2] = {{-24,0.0},{-6,0.375},{24,1.0}};   // -6 dB maps EXACTLY to norm 0.375
      for (auto& pr : pts) { juce::var a; a.append (pr[0]); a.append (pr[1]); anchors.append (a); } }
    entry->setProperty ("anchors", anchors);
    auto* params = new juce::DynamicObject(); params->setProperty ("gain_db", juce::var (entry));
    auto* map = new juce::DynamicObject();
    map->setProperty ("fp", fp); map->setProperty ("dialable", true); map->setProperty ("category", "dynamics");
    map->setProperty ("params", juce::var (params));
    return juce::var (map);
}
static juce::var settings() { auto* o = new juce::DynamicObject(); o->setProperty ("gain_db", -6.0); return juce::var (o); }

static float probeValueAfterBorrowedDial (int spectators, juce::String& statusOut)
{
    // 1) learn the fp a Borrowed host assigns this descriptor (deterministic per mode).
    juce::String fp;
    { ChainHost tmp (ChainHost::Mode::Borrowed); tmp.prepare (48000.0, 512);
      tmp.completeLoad (std::make_unique<GainProbe>(), probeDesc(), ChainHost::LoadOrigin::Restore);
      fp = tmp.getSlotIdentity (0).fp; }

    // 2) a PRIMARY host writes that fp's map to disk (fresh fpFetchedAt), BEFORE the real borrowed
    //    host is built - the borrowed host loads the maps on construction (the FIX).
    { ChainHost writer (ChainHost::Mode::Primary); writer.prepare (48000.0, 512);
      auto* maps = new juce::DynamicObject(); maps->setProperty (juce::Identifier (fp), gainMap (fp));
      writer.storeParamMaps (juce::var (maps)); }

    // 3) the real BORROWED host: on construction it loads param_maps.json (the FIX; pre-dial it does
    //    not), then dials the probe. A third-party map present -> the probe's Gain MOVES to the target.
    ChainHost borrowed (ChainHost::Mode::Borrowed);
    borrowed.prepare (48000.0, 512);
    for (int i = 0; i < spectators; ++i)   // scale: extra slots so the host runs at size
        borrowed.completeLoad (std::make_unique<GainProbe>(), probeDesc(), ChainHost::LoadOrigin::Restore);
    borrowed.completeLoad (std::make_unique<GainProbe>(), probeDesc(), ChainHost::LoadOrigin::Restore);
    const int slot = borrowed.getNumSlots() - 1;
    borrowed.setSlotStructuredSettings (slot, settings());
    borrowed.logDialSummary ("leg: borrowed-host third-party dial");
    auto* pr = borrowed.getSlotProcessor (slot);
    float v = -1.0f;
    if (pr != nullptr && pr->getParameters().size() > 0) v = pr->getParameters()[0]->getValue();
    statusOut = "fp=" + fp.substring (0, 12) + " slot=" + juce::String (slot);
    return v;
}

int main (int argc, char** argv)
{
    echojay::requireIsolationOrDie ("borrowed_dial_legs_test.cpp");
    std::setvbuf (stdout, nullptr, _IONBF, 0); juce::ScopedJuceInitialiser_GUI gui;
    echojay::userAppData().getChildFile ("EchoJay").createDirectory();   // the app makes this; the harness must too
    const int spectators = argc > 1 ? atoi (argv[1]) : 40;
    juce::String status; const float v = probeValueAfterBorrowedDial (spectators, status);
    const float want = 0.375f;   // -6 dB on the [-24,24] anchored map
    const bool applied = std::abs (v - want) < 0.02f;
    std::printf ("borrowed-dial: %s  probe Gain norm = %.3f (default 0.500, dialled target %.3f) -> %s\n",
                 status.toRawUTF8(), v, want, applied ? "PASS (third-party dialled in the borrowed host)"
                                                      : "FAIL (third-party did NOT dial - default unchanged)");
    return applied ? 0 : 1;
}
