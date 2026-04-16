#pragma once
#include <JuceHeader.h>
#include "MeterEngine.h"
#include <vector>
#include <random>
#include <atomic>

// ============================================================================
// ParticleVisual — OpenGL-rendered reactive 3D particle swarm
// Reads live meter/spectrum data and renders a swarm of frequency-band orbs
// ============================================================================

class ParticleVisual : public juce::Component,
                       public juce::OpenGLRenderer,
                       private juce::Timer
{
public:
    ParticleVisual();
    ~ParticleVisual() override;

    // Feed live meter data each frame (called from editor timer)
    void updateMeterData(const MeterData& md);

    // Capture animation triggers
    void triggerCaptureImplode();
    void triggerCaptureRelease();
    
    // Visual presets — changes particle shape/behaviour/physics
    enum class Preset { Orb, Ring, Helix, Scatter };
    static constexpr int kPresetCount = 4;
    Preset currentPreset = Preset::Orb;
    void nextPreset();
    void prevPreset();
    
    static const char* getPresetName(Preset p) {
        switch (p) {
            case Preset::Orb:     return "Orb";
            case Preset::Ring:    return "Ring";
            case Preset::Helix:   return "Helix";
            case Preset::Scatter: return "Scatter";
        }
        return "Orb";
    }
    
    // Colour themes — changes the palette applied to any visual preset
    enum class Theme { Nebula, Aurora, Solar, Crystal };
    static constexpr int kThemeCount = 4;
    Theme currentTheme = Theme::Aurora;
    void nextTheme();
    void prevTheme();
    
    static const char* getThemeName(Theme t) {
        switch (t) {
            case Theme::Nebula:  return "Nebula";
            case Theme::Aurora:  return "Aurora";
            case Theme::Solar:   return "Solar";
            case Theme::Crystal: return "Crystal";
        }
        return "Nebula";
    }

    // OpenGLRenderer
    void newOpenGLContextCreated() override;
    void renderOpenGL() override;
    void openGLContextClosing() override;

    // Component
    void paint(juce::Graphics&) override {}
    void resized() override;

    // Paint the compact number strip (called externally on the Graphics of parent)
    void paintNumberStrip(juce::Graphics& g, juce::Rectangle<int> area, const MeterData& md);

private:
    void timerCallback() override;

    juce::OpenGLContext glContext;

    // === Particle data ===
    enum class Band { Bass, Mid, High, Trail };

    struct Particle {
        float x, y, z;             // current position
        float homeX, homeY, homeZ; // rest position
        float vx, vy, vz;          // velocity
        float size;
        float brightness;
        Band band;
        float phase;                // individual animation phase offset
        float orbitRadius;          // for mid orbs
        float orbitSpeed;           // for mid orbs
        float orbitAngle;           // current orbit angle
    };

    std::vector<Particle> particles;
    static constexpr int kMainParticles = 8000;
    static constexpr int kTrailParticles = 2000;

    void initParticles();
    void updateParticles(float dt);

    // Veins — thin white lines radiating from center like an iris
    struct Vein {
        float dirX, dirY, dirZ;   // direction from center
        float length;              // max length
        float phase;               // animation offset
        float thickness;           // base brightness
    };
    static constexpr int kVeinCount = 24;
    std::vector<Vein> veins;
    void initVeins();
    GLuint veinVbo = 0;

    // Width for veins
    std::atomic<float> stereoWidth { 0.0f };

    // Camera
    float cameraAngle = 0.0f;
    float cameraElevation = 0.15f;
    float cameraDistance = 3.5f;

    // Audio-reactive state (smoothed) — atomic because written on message thread, read on GL thread
    std::atomic<float> bassEnergy { 0.0f };
    std::atomic<float> midEnergy { 0.0f };
    std::atomic<float> highEnergy { 0.0f };
    std::atomic<float> totalEnergy { 0.0f };
    std::atomic<float> bassTransient { 0.0f };
    float prevBassEnergy = 0.0f;
    float baseline = 0.0f;
    float prevPeak = 0.0f;
    
    // Gentle breathing — RMS-driven, always moving slightly when sound plays
    std::atomic<float> breath { 0.0f };
    
    // LUFS-driven size offset: negative = contract, positive = expand, 0 = rest
    std::atomic<float> lufsSize { 0.0f };
    
    // Micro-pump: detects small rhythmic fluctuations (kick pumps in house/EDM)
    float prevMom = -100.0f;
    std::atomic<float> pump { 0.0f };

    // Colour shift
    std::atomic<float> energyHeat { 0.0f };
    
    // Spectrum balance for colour (0-1 each, relative weight)
    std::atomic<float> specBass { 0.33f };
    std::atomic<float> specMid { 0.33f };
    std::atomic<float> specHigh { 0.33f };

    // Capture animation state
    enum class CaptureAnim { None, Imploding, Holding, Releasing };
    CaptureAnim captureAnim = CaptureAnim::None;
    float captureAnimTime = 0.0f;
    static constexpr float kImplodeDuration = 0.4f;
    static constexpr float kHoldMinDuration = 0.5f;
    static constexpr float kReleaseDuration = 0.6f;

    // Shader program
    std::unique_ptr<juce::OpenGLShaderProgram> shader;
    GLuint vbo = 0;

    bool shadersCompiled = false;
    bool contextActive = false;

    // Frame timing
    double lastFrameTime = 0.0;

    std::mt19937 rng;

    // Colour palette
    struct Palette {
        static constexpr uint32_t deepPurple  = 0xff06b6d4;
        static constexpr uint32_t lightPurple = 0xff67e8f9;
        static constexpr uint32_t blueAccent  = 0xff60A5FA;
        static constexpr uint32_t energyPink  = 0xff22d3ee;
        static constexpr uint32_t cyanAccent  = 0xff06D6A0;
        static constexpr uint32_t bgDark      = 0xff070810;
    };

    // Helper
    float randFloat(float lo, float hi);
    juce::Colour getParticleColour(const Particle& p) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ParticleVisual)
};
