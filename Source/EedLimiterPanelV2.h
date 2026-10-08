/*
    EedLimiterPanelV2.h (session L, 8 Oct 2026; revised 8 Oct evening to Sean's notes): the redesigned limiter panel -
    the device SHOWS what it is doing. A new component beside the shipping EedLimiterEditor (untouched until the
    go-ahead). Copies no FabFilter layout, colours, graphics or wording: EchoJay palette, EchoJay filmstrip dials,
    EchoJay type.

      header     LIMITER  (subtitle when it fits)  <style>  TRUE PK  BYPASS
      picture    THE CENTREPIECE: a scrolling display over the last 1 / 3 / 10 s (speed buttons inside its corner):
                 the INPUT waveform dim behind the OUTPUT waveform, both halves, on a dB amplitude scale (+12 at the
                 edges, -36 at the centre); the gain line (amber) on top, 0 at the top down to -12 dB, its own scale
      meters     slim bars: IN (left), OUT and GR (right), each with a dB scale and its number below; IN/OUT in the
                 normal palette, red only for the part above 0 dBFS, with a true-peak max-hold; GR fills DOWN from 0
                 with a max-hold line and the deepest value
      dials      GAIN (input_db) and CEILING, large; a LUFS column: short-term bar, the integrated value large, M and
                 S small, its own RESET LUFS button; the ADVANCED button and the latency line
      advanced   LOOKAHEAD, ATTACK, RELEASE, LINK, SC HPF - Pro-L 2's Default Setting as the defaults (0.18 ms, 275 ms,
                 400 ms, 75 %)

    Nothing truncates: every piece of text is chosen from a list of shorter forms until one fits its box (fitText).

    DATA. The panel owns nothing live: it reads a limv2::MeterTap (lock-free, fed by the engine per sample; the tap
    delays IN by the latency so it sits under OUT) on a 30 Hz timer, folds the columns into its own picture ring and
    the hops into a LoudnessReader. The host (the editor in the plugin, the preview app offline) gives it a Model of
    the dial values and a callback for changes.
*/
#pragma once
#include "EJLimiterPanelPalette.h"
#include "EJLimiterMeterTap.h"
#include <functional>
#include <vector>

class EedLimiterPanelV2 : public juce::Component, private juce::Timer
{
public:
    struct Model
    {
        double gainDb = 0.0, ceilingDb = 0.0; int style = 0; bool truePeak = true, bypassed = false;
        double lookaheadMs = 0.18, attackMs = 275.0, releaseMs = 400.0, linkPct = 75.0, scHpfHz = 0.0; int latencySamples = 0; double sampleRate = 48000.0;
        std::function<void (const juce::String& id, double value)> onChange;   // "input_db", "ceiling_db", "mode", "true_peak", "lookahead_ms", "attack_ms", "release_ms", "link_pct", "sc_hpf_hz", "bypass"
    };
    static constexpr int kDefaultW = 480, kDefaultH = 338, kMinW = 340, kMinH = 260;

    explicit EedLimiterPanelV2 (echojay::limv2::MeterTap& tap);
    ~EedLimiterPanelV2() override;

    void setModel (const Model& m);
    void setKnobFilmstrip (const juce::Image& strip);   // 128 frames of 300x300, vertical (Assets/wet_knob_filmstrip.png); empty = vector dial
    void setSpeedIndex (int i);                         // 0 = 1 s, 1 = 3 s, 2 = 10 s across the picture
    void refreshNow();                                   // what the timer does, callable by an offline driver
    void setAdvancedOpen (bool open);

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override { refreshNow(); }
    void paintPicture (juce::Graphics&, juce::Rectangle<int>);
    void paintLevelMeter (juce::Graphics&, juce::Rectangle<int>, float peakDb, float holdDb, const juce::String& caption, bool scaleOnRight);
    void paintGrMeter (juce::Graphics&, juce::Rectangle<int>);
    void paintLufs (juce::Graphics&, juce::Rectangle<int>);
    void layoutDial (juce::Slider&, juce::Label&, juce::Rectangle<int>);
    bool small() const { return getWidth() < 420 || getHeight() < 300; }

    struct Dial : public juce::LookAndFeel_V4
    {
        juce::Image strip;
        void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float, float, juce::Slider&) override;
    };

    echojay::limv2::MeterTap& tap_;
    echojay::limv2::LoudnessReader loud_;
    Model model_;
    Dial dialLnf_;
    juce::ComboBox styleBox_; juce::TextButton truePeakBtn_ { "TRUE PK" }, bypassBtn_ { "BYPASS" }, lufsResetBtn_ { "RESET LUFS" }, advBtn_ { "ADVANCED" };
    juce::TextButton speed1_ { "1s" }, speed3_ { "3s" }, speed10_ { "10s" }; juce::TextButton* speedBtn_[3] { &speed1_, &speed3_, &speed10_ };
    juce::Slider gainDial_, ceilingDial_, lookaheadDial_, attackDial_, releaseDial_, linkDial_, hpfDial_;
    juce::Label gainCap_, ceilingCap_, lookaheadCap_, attackCap_, releaseCap_, linkCap_, hpfCap_;
    bool advancedOpen_ = false; int speed_ = 1; bool suppress_ = false;

    // the picture: the last kPictureCols columns, newest last
    static constexpr int kPictureCols = 1024;
    std::vector<echojay::limv2::ColumnRecord> picture_; int pictureHead_ = 0; int tapCursor_ = 0;
    float inPeakDb_ = -200, outPeakDb_ = -200, grNowDb_ = 0, grHoldDb_ = 0; int holdAge_ = 0;
    juce::Rectangle<int> headerArea_, pictureArea_, inMeterArea_, outMeterArea_, grMeterArea_, lufsArea_, latencyArea_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EedLimiterPanelV2)
};
