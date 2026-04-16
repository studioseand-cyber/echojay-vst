#include "ParticleVisual.h"
#include <cmath>

using namespace juce::gl;

// ============================================================================
// Shaders
// ============================================================================

// Vertex shader — transforms 3D particle positions, passes colour/size to fragment
static const char* vertexShaderSrc = R"(
    #version 120
    attribute vec3 aPos;
    attribute vec4 aColour;
    attribute float aSize;
    uniform mat4 uMVP;
    uniform float uScale;
    varying vec4 vColour;
    void main() {
        gl_Position = uMVP * vec4(aPos, 1.0);
        gl_PointSize = aSize * uScale / max(gl_Position.w, 0.1);
        vColour = aColour;
    }
)";

// Fragment shader — sharp crisp dots with thin glowing edge
static const char* fragmentShaderSrc = R"(
    #version 120
    varying vec4 vColour;
    void main() {
        vec2 coord = gl_PointCoord - vec2(0.5);
        float dist = length(coord);
        if (dist > 0.5) discard;
        // Sharp edge: full brightness inside, hard cutoff with thin glow rim
        float alpha;
        if (dist < 0.35) {
            alpha = 1.0; // solid core
        } else {
            alpha = 1.0 - smoothstep(0.35, 0.5, dist); // thin soft rim
        }
        gl_FragColor = vec4(vColour.rgb, vColour.a * alpha);
    }
)";

// ============================================================================
// Construction / Destruction
// ============================================================================

ParticleVisual::ParticleVisual()
    : rng(std::random_device{}())
{
    setOpaque(true);
    
    glContext.setRenderer(this);
    glContext.setContinuousRepainting(true);
    glContext.setComponentPaintingEnabled(false);
    glContext.attachTo(*this);

    startTimerHz(60);
}

ParticleVisual::~ParticleVisual()
{
    stopTimer();
    glContext.detach();
}

void ParticleVisual::timerCallback()
{
    // Timer drives component repaint which triggers renderOpenGL via continuous repainting
}

float ParticleVisual::randFloat(float lo, float hi)
{
    std::uniform_real_distribution<float> dist(lo, hi);
    return dist(rng);
}

// ============================================================================
// Particle initialisation
// ============================================================================

void ParticleVisual::initParticles()
{
    particles.clear();
    particles.reserve((size_t)(kMainParticles + kTrailParticles));

    int bassCount = kMainParticles * 25 / 100;
    int midCount  = kMainParticles * 35 / 100;
    int highCount = kMainParticles - bassCount - midCount;

    // All main particles: home position is a direction unit vector * max radius
    // At rest they sit in a tight ball near center; energy pushes them outward along their direction
    auto makeParticle = [&](Band band) {
        Particle p;
        p.band = band;
        p.phase = randFloat(0.0f, 6.2832f);

        // Random direction on unit sphere
        float theta = randFloat(0.0f, 6.2832f);
        float phi   = randFloat(-1.0f, 1.0f);
        float sinPhi = std::sqrt(1.0f - phi * phi);
        float dirX = sinPhi * std::cos(theta);
        float dirY = phi;
        float dirZ = sinPhi * std::sin(theta);

        // homeX/Y/Z = the fully expanded position (direction * max reach)
        float maxReach;
        switch (band) {
            case Band::Bass: maxReach = randFloat(0.4f, 0.7f); break;
            case Band::Mid:  maxReach = randFloat(0.3f, 0.6f); break;
            case Band::High: maxReach = randFloat(0.25f, 0.5f); break;
            default:         maxReach = 1.0f; break;
        }
        p.homeX = dirX * maxReach;
        p.homeY = dirY * maxReach;
        p.homeZ = dirZ * maxReach;

        // Start at center (tight orb)
        float restRadius = randFloat(0.02f, 0.15f);
        p.x = dirX * restRadius;
        p.y = dirY * restRadius;
        p.z = dirZ * restRadius;
        p.vx = p.vy = p.vz = 0.0f;

        switch (band) {
            case Band::Bass:
                p.size = randFloat(0.6f, 1.1f);
                p.brightness = randFloat(0.2f, 0.4f);
                p.orbitRadius = 0; p.orbitSpeed = 0; p.orbitAngle = 0;
                break;
            case Band::Mid:
                p.size = randFloat(0.6f, 1.1f);
                p.brightness = randFloat(0.15f, 0.35f);
                p.orbitRadius = randFloat(0.1f, 0.3f);
                p.orbitSpeed = randFloat(0.8f, 2.5f) * (randFloat(0, 1) > 0.5f ? 1.0f : -1.0f);
                p.orbitAngle = randFloat(0, 6.2832f);
                break;
            case Band::High:
                p.size = randFloat(0.4f, 0.9f);
                p.brightness = randFloat(0.12f, 0.3f);
                p.orbitRadius = 0; p.orbitSpeed = 0; p.orbitAngle = 0;
                break;
            default:
                break;
        }
        return p;
    };

    for (int i = 0; i < bassCount; ++i)  particles.push_back(makeParticle(Band::Bass));
    for (int i = 0; i < midCount;  ++i)  particles.push_back(makeParticle(Band::Mid));
    for (int i = 0; i < highCount; ++i)  particles.push_back(makeParticle(Band::High));

    // Trail particles — outer shell, always dim
    for (int i = 0; i < kTrailParticles; ++i) {
        Particle p;
        p.band = Band::Trail;
        p.phase = randFloat(0.0f, 6.2832f);
        float theta = randFloat(0.0f, 6.2832f);
        float phi   = randFloat(-1.0f, 1.0f);
        float sinPhi = std::sqrt(1.0f - phi * phi);
        float maxReach = randFloat(0.4f, 0.8f);
        p.homeX = sinPhi * std::cos(theta) * maxReach;
        p.homeY = phi * maxReach;
        p.homeZ = sinPhi * std::sin(theta) * maxReach;
        // Trails also start near center
        float restR2 = randFloat(0.05f, 0.2f);
        p.x = sinPhi * std::cos(theta) * restR2;
        p.y = phi * restR2;
        p.z = sinPhi * std::sin(theta) * restR2;
        p.vx = p.vy = p.vz = 0.0f;
        p.size = randFloat(0.3f, 0.5f);
        p.brightness = randFloat(0.06f, 0.12f);
        p.orbitRadius = 0; p.orbitSpeed = 0; p.orbitAngle = 0;
        particles.push_back(p);
    }
}

void ParticleVisual::initVeins()
{
    veins.clear();
    veins.reserve(kVeinCount);
    for (int i = 0; i < kVeinCount; ++i) {
        Vein v;
        float theta = randFloat(0.0f, 6.2832f);
        float phi = randFloat(-0.8f, 0.8f);
        float sinPhi = std::sqrt(1.0f - phi * phi);
        v.dirX = sinPhi * std::cos(theta);
        v.dirY = phi;
        v.dirZ = sinPhi * std::sin(theta);
        v.length = randFloat(0.6f, 1.1f);
        v.phase = randFloat(0.0f, 6.2832f);
        v.thickness = randFloat(0.3f, 0.7f);
        veins.push_back(v);
    }
}

void ParticleVisual::nextPreset()
{
    int p = (int)currentPreset;
    p = (p + 1) % kPresetCount;
    currentPreset = (Preset)p;
}

void ParticleVisual::prevPreset()
{
    int p = (int)currentPreset;
    p = (p - 1 + kPresetCount) % kPresetCount;
    currentPreset = (Preset)p;
}

void ParticleVisual::nextTheme()
{
    int t = (int)currentTheme;
    t = (t + 1) % kThemeCount;
    currentTheme = (Theme)t;
}

void ParticleVisual::prevTheme()
{
    int t = (int)currentTheme;
    t = (t - 1 + kThemeCount) % kThemeCount;
    currentTheme = (Theme)t;
}

// ============================================================================
// Audio data feed
// ============================================================================

void ParticleVisual::updateMeterData(const MeterData& md)
{
    // The visual should react to DYNAMICS not absolute level.
    // On a mix bus at -8 LUFS, a kick transient might peak at -3 while 
    // the sustained level is -10. The crest factor captures this.
    // Also use the difference between peak and RMS directly.
    
    auto dbToNorm = [](float db, float floor, float ceiling) {
        return juce::jlimit(0.0f, 1.0f, (db - floor) / (ceiling - floor));
    };
    
    float peakDb = (md.peakL + md.peakR) * 0.5f;
    float rmsDb = (md.rmsL + md.rmsR) * 0.5f;
    
    // Crest = peak - rms in dB. Higher = more transient. 
    // Typical: 6-10 for drums, 2-4 for sustained synths, 12+ for sparse percussive hits
    float crest = juce::jmax(0.0f, peakDb - rmsDb);
    // Normalise: 0 dB crest = no dynamics, 15+ dB = very dynamic
    float dynamics = juce::jlimit(0.0f, 1.0f, crest / 12.0f);
    
    // Also use peak level itself but only for "is anything playing at all"
    float peakNorm = dbToNorm(peakDb, -50.0f, -6.0f);
    // Gate: if peak is very low, suppress everything (silence = tight orb)
    float gate = juce::jlimit(0.0f, 1.0f, (peakDb + 40.0f) / 20.0f);
    
    // Reactivity: dynamics * gate. High crest + signal present = big expansion
    float reactivity = dynamics * gate;
    
    // Peak delta for transients
    float peakDelta = juce::jmax(0.0f, peakDb - prevPeak);
    prevPeak = peakDb;
    // Normalise delta: a 3dB jump is noticeable, 6dB+ is a big hit
    float transientHit = juce::jlimit(0.0f, 1.0f, peakDelta / 2.5f);
    
    // Band hints from spectrum
    float bHint = 0, mHint = 0, hHint = 0;
    for (int i = 0; i < 16; ++i)
        bHint += juce::jmax(0.0f, (md.spectrum[(size_t)i] + 60.0f) / 60.0f);
    for (int i = 16; i < 39; ++i)
        mHint += juce::jmax(0.0f, (md.spectrum[(size_t)i] + 60.0f) / 60.0f);
    for (int i = 39; i < 64; ++i)
        hHint += juce::jmax(0.0f, (md.spectrum[(size_t)i] + 60.0f) / 60.0f);
    bHint /= 16.0f; mHint /= 23.0f; hHint /= 25.0f;

    float be = bassEnergy.load();
    float me = midEnergy.load();
    float he = highEnergy.load();
    float bt = bassTransient.load();
    float te = totalEnergy.load();
    float eh = energyHeat.load();
    
    // Band targets: ONLY driven by transient hits (peak delta), NOT sustained crest
    // transientHit is 0 between hits, spikes on kick/snare/etc
    float bTarget = juce::jlimit(0.0f, 1.0f, transientHit * (1.2f + bHint * 0.8f));
    float mTarget = juce::jlimit(0.0f, 1.0f, transientHit * (1.2f + mHint * 0.8f));
    float hTarget = juce::jlimit(0.0f, 1.0f, transientHit * (1.0f + hHint * 1.0f));
    
    // Pure transient mode: values spike on hits and decay hard every frame
    // No sustained level — between hits everything goes to zero
    float decayRate = 0.65f; // lose 45% per frame at 20Hz = gone in ~4 frames (200ms)
    
    be = juce::jmax(bTarget, be * decayRate);
    me = juce::jmax(mTarget, me * decayRate);
    he = juce::jmax(hTarget, he * decayRate);

    // Bass transient from peak delta
    if (transientHit > 0.1f) bt = juce::jmax(bt, transientHit * 2.0f);
    bt *= 0.4f;
    prevBassEnergy = be;

    // Total energy — same pure transient decay
    te = juce::jmax(transientHit, te * decayRate);
    
    // Energy heat — slow colour shift based on overall loudness
    float absLevel = juce::jlimit(0.0f, 1.0f, peakNorm);
    eh += (absLevel - eh) * 0.08f;
    
    // Breath: gentle RMS follower — always slightly moving when any sound plays
    float rmsNorm = juce::jlimit(0.0f, 1.0f, (rmsDb + 40.0f) / 35.0f);
    float br = breath.load();
    br += (rmsNorm - br) * 0.3f; // moderate smoothing
    breath.store(br);
    
    // LUFS-driven size: maps momentary LUFS to contraction/expansion
    // -8 LUFS (loud master) = slightly expanded
    // -14 LUFS = rest size
    // -24 LUFS = noticeably contracted  
    // -40+ = very small
    float momLufs = md.momentary;
    float ls;
    if (momLufs <= -60.0f) {
        ls = 0.0f; // true silence = rest size
    } else {
        // Aggressive contraction: divide by 8 instead of 16 = 2x steeper
        // Midpoint at -10 LUFS so even moderate levels contract
        ls = juce::jlimit(-1.0f, 0.1f, (momLufs + 10.0f) / 8.0f);
    }
    float prevLs = lufsSize.load();
    prevLs += (ls - prevLs) * 0.45f; // fast response to dips
    lufsSize.store(prevLs);
    
    // Micro-pump: catches the small LUFS fluctuations from kick sidechain/compression
    // On a house track, momentary LUFS might swing 2-4 dB on each kick
    // This is too small for the transient detector but very visible rhythmically
    float momDelta = momLufs - prevMom;
    prevMom = momLufs;
    float pu = pump.load();
    if (momDelta > 0.3f) {
        // LUFS went UP — kick recovery, volume pumping back up
        pu = juce::jmax(pu, juce::jlimit(0.0f, 1.0f, momDelta / 3.0f));
    }
    pu *= 0.55f; // decay per frame
    pump.store(pu);
    
    bassEnergy.store(be);
    midEnergy.store(me);
    highEnergy.store(he);
    bassTransient.store(bt);
    totalEnergy.store(te);
    energyHeat.store(eh);
    
    // Spectrum balance for colour — relative weight of each band
    float specTotal = bHint + mHint + hHint + 0.001f; // avoid div zero
    specBass.store(bHint / specTotal);
    specMid.store(mHint / specTotal);
    specHigh.store(hHint / specTotal);
    
    // Stereo width for vein visibility (0-1, typically 0-100 mapped to 0-1)
    float sw = juce::jlimit(0.0f, 1.0f, md.width / 100.0f);
    float prevSw = stereoWidth.load();
    prevSw += (sw - prevSw) * 0.2f;
    stereoWidth.store(prevSw);
}

// ============================================================================
// Particle physics update
// ============================================================================

void ParticleVisual::updateParticles(float dt)
{
    if (dt <= 0 || dt > 0.1f) dt = 1.0f / 60.0f;

    // Load atomic energy values into locals for this frame
    float be = bassEnergy.load();
    float me = midEnergy.load();
    float he = highEnergy.load();
    float te = totalEnergy.load();
    float bt = bassTransient.load();
    float eh = energyHeat.load();
    float br = breath.load();
    float ls = lufsSize.load();
    float pu = pump.load();

    // Camera slow orbit
    cameraAngle += dt * 0.12f;
    cameraElevation = 0.15f + 0.05f * std::sin(cameraAngle * 0.3f);

    // Capture animation state machine
    if (captureAnim != CaptureAnim::None) {
        captureAnimTime += dt;
        if (captureAnim == CaptureAnim::Imploding && captureAnimTime >= kImplodeDuration) {
            captureAnim = CaptureAnim::Holding;
            captureAnimTime = 0.0f;
        }
        else if (captureAnim == CaptureAnim::Releasing && captureAnimTime >= kReleaseDuration) {
            captureAnim = CaptureAnim::None;
            captureAnimTime = 0.0f;
        }
    }

    float captureImplodeFactor = 0.0f;
    float captureGlow = 0.0f;
    if (captureAnim == CaptureAnim::Imploding) {
        captureImplodeFactor = captureAnimTime / kImplodeDuration;
    } else if (captureAnim == CaptureAnim::Holding) {
        captureImplodeFactor = 1.0f;
        captureGlow = 0.3f + 0.2f * std::sin(captureAnimTime * 4.0f);
    } else if (captureAnim == CaptureAnim::Releasing) {
        float t = captureAnimTime / kReleaseDuration;
        captureImplodeFactor = (1.0f - t) * (1.0f - t);
    }

    for (auto& p : particles) {
        // Core concept: particles live in a tight orb at center when silent,
        // expand outward along their home direction when energy increases.
        // home = fully expanded position, restR = radius of the silent orb.
        float homeDist = std::sqrt(p.homeX*p.homeX + p.homeY*p.homeY + p.homeZ*p.homeZ);
        float dirX = (homeDist > 0.01f) ? p.homeX / homeDist : 0.0f;
        float dirY = (homeDist > 0.01f) ? p.homeY / homeDist : 0.0f;
        float dirZ = (homeDist > 0.01f) ? p.homeZ / homeDist : 0.0f;
        // Base size from LUFS
        float baseR = 1.2f + ls * 1.0f;
        
        // Pump modulation: when loud (ls > -0.3), the orb rhythmically shrinks/swells
        // Between beats: restR contracts slightly. On beat: restR swells back.
        // The shrink amount scales with loudness — louder = more pump visible
        float loudness = juce::jlimit(0.0f, 1.0f, (ls + 1.0f) / 1.1f); // 0=quiet, 1=loud
        float pumpShrink = loudness * 0.25f; // max 25% shrink between beats when loud
        float restR = baseR - pumpShrink + pu * (pumpShrink + 0.15f); // pump restores + overshoots
        if (restR < 0.15f) restR = 0.15f;

        float expand = 0.0f;
        float targetX, targetY, targetZ;
        
        // Preset-specific position modifier applied to all bands
        float presetOffX = 0, presetOffY = 0, presetOffZ = 0;
        float presetRadiusMod = 1.0f;
        
        switch (currentPreset) {
            case Preset::Orb:
                // Default sphere — no modification
                break;
                
            case Preset::Ring: {
                // Flatten into a ring/disc — squash Y axis, particles form a torus
                float ringAngle = std::atan2(dirZ, dirX);
                float ringR = std::sqrt(dirX * dirX + dirZ * dirZ);
                // Squash Y toward 0, push outward in XZ
                presetOffY = -dirY * 0.7f; // flatten
                presetRadiusMod = 0.8f + ringR * 0.4f;
                // Add subtle rotation
                float rot = cameraAngle * 0.3f;
                float cosR = std::cos(rot), sinR = std::sin(rot);
                float newDirX = dirX * cosR - dirZ * sinR;
                float newDirZ = dirX * sinR + dirZ * cosR;
                presetOffX = (newDirX - dirX) * restR * 0.3f;
                presetOffZ = (newDirZ - dirZ) * restR * 0.3f;
                break;
            }
                
            case Preset::Helix: {
                // Double helix / DNA shape — particles spiral around Y axis
                float helixAngle = p.phase * 3.0f + dirY * 6.2832f * 2.0f + cameraAngle * 0.5f;
                float helixR = 0.3f + br * 0.15f;
                presetOffX = std::cos(helixAngle) * helixR - dirX * restR * 0.5f;
                presetOffZ = std::sin(helixAngle) * helixR - dirZ * restR * 0.5f;
                // Stretch along Y
                presetOffY = dirY * restR * 0.5f;
                presetRadiusMod = 0.5f;
                break;
            }
                
            case Preset::Scatter: {
                // Scattered cloud — particles spread into random clusters
                // Use phase to create 5-6 cluster centers
                float clusterAngle = std::floor(p.phase / 1.2f) * 1.2f;
                float cx = std::cos(clusterAngle * 2.1f) * 0.5f;
                float cy = std::sin(clusterAngle * 1.7f) * 0.4f;
                float cz = std::cos(clusterAngle * 3.3f) * 0.5f;
                presetOffX = cx * (0.3f + br * 0.2f);
                presetOffY = cy * (0.3f + br * 0.2f);
                presetOffZ = cz * (0.3f + br * 0.2f);
                presetRadiusMod = 0.6f;
                break;
            }
        }

        switch (p.band) {
            case Band::Bass: {
                float breathWobble = br * 0.15f * (1.0f + 0.3f * std::sin(p.phase + cameraAngle * 1.5f));
                expand = juce::jlimit(0.0f, 1.0f, te * 1.2f + bt * 1.5f + breathWobble + pu * 0.5f);
                float r = restR * presetRadiusMod + expand * homeDist;
                targetX = dirX * r + presetOffX;
                targetY = dirY * r + presetOffY;
                targetZ = dirZ * r + presetOffZ;

                float spring = 14.0f;
                float damp = 6.0f;
                p.vx += -spring * (p.x - targetX) * dt;
                p.vy += -spring * (p.y - targetY) * dt;
                p.vz += -spring * (p.z - targetZ) * dt;

                if (bt > 0.08f) {
                    float kick = bt * 3.0f;
                    p.vx += dirX * kick * dt;
                    p.vy += dirY * kick * dt;
                    p.vz += dirZ * kick * dt;
                }

                p.vx *= (1.0f - damp * dt);
                p.vy *= (1.0f - damp * dt);
                p.vz *= (1.0f - damp * dt);

                p.size = juce::jlimit(0.8f, 2.0f, 0.8f + te * 1.5f + bt * 1.0f + br * 0.3f);
                p.brightness = juce::jlimit(0.2f, 0.7f, 0.25f + eh * 0.4f + bt * 0.2f + pu * 0.2f);
                break;
            }

            case Band::Mid: {
                float breathWobble = br * 0.12f * (1.0f + 0.4f * std::sin(p.phase + cameraAngle * 2.0f));
                expand = juce::jlimit(0.0f, 1.0f, te * 1.1f + breathWobble + pu * 0.4f);
                float midR = restR * presetRadiusMod + expand * homeDist;
                p.orbitAngle += p.orbitSpeed * dt * (1.0f + me * 5.0f + br * 2.0f);
                float orbitR = p.orbitRadius * (0.2f + te * 1.5f + br * 0.5f);

                targetX = dirX * midR + std::cos(p.orbitAngle) * orbitR * 0.3f + presetOffX;
                targetY = dirY * midR + std::sin(p.orbitAngle * 0.7f + p.phase) * orbitR * 0.2f + presetOffY;
                targetZ = dirZ * midR + std::sin(p.orbitAngle) * orbitR * 0.3f + presetOffZ;

                float spring = 10.0f;
                float damp = 4.5f;
                p.vx += -spring * (p.x - targetX) * dt;
                p.vy += -spring * (p.y - targetY) * dt;
                p.vz += -spring * (p.z - targetZ) * dt;
                p.vx *= (1.0f - damp * dt);
                p.vy *= (1.0f - damp * dt);
                p.vz *= (1.0f - damp * dt);

                p.size = juce::jlimit(0.6f, 1.5f, 0.6f + te * 1.0f + br * 0.2f);
                p.brightness = juce::jlimit(0.15f, 0.6f, 0.2f + eh * 0.35f + pu * 0.15f);
                break;
            }

            case Band::High: {
                float breathWobble = br * 0.1f * (1.0f + 0.5f * std::sin(p.phase * 1.3f + cameraAngle * 2.5f));
                expand = juce::jlimit(0.0f, 1.0f, te * 1.0f + breathWobble + pu * 0.35f);
                float hiR = restR * presetRadiusMod + expand * homeDist;
                targetX = dirX * hiR + presetOffX;
                targetY = dirY * hiR + presetOffY;
                targetZ = dirZ * hiR + presetOffZ;

                float jitter = (te + he) * 0.4f + br * 0.15f;
                p.vx += randFloat(-jitter, jitter);
                p.vy += randFloat(-jitter, jitter);
                p.vz += randFloat(-jitter, jitter);

                float spring = 8.0f;
                float damp = 4.0f;
                p.vx += -spring * (p.x - targetX) * dt;
                p.vy += -spring * (p.y - targetY) * dt;
                p.vz += -spring * (p.z - targetZ) * dt;
                p.vx *= (1.0f - damp * dt);
                p.vy *= (1.0f - damp * dt);
                p.vz *= (1.0f - damp * dt);

                p.brightness = juce::jlimit(0.12f, 0.55f, 0.15f + eh * 0.3f + pu * 0.12f + randFloat(-0.05f, 0.05f));
                p.size = juce::jlimit(0.4f, 1.2f, 0.4f + te * 0.8f + br * 0.15f);
                break;
            }

            case Band::Trail: {
                float breathWobble = br * 0.08f;
                expand = juce::jlimit(0.0f, 0.7f, te * 0.9f + breathWobble);
                float t2 = p.phase + cameraAngle * 0.15f;
                float trR = restR * 1.8f * presetRadiusMod + expand * homeDist;
                targetX = dirX * trR + std::sin(t2) * 0.05f + presetOffX;
                targetY = dirY * trR + std::cos(t2 * 0.7f) * 0.04f + presetOffY;
                targetZ = dirZ * trR + std::sin(t2 * 0.5f) * 0.05f + presetOffZ;

                float spring = 3.0f;
                float damp = 2.0f;
                p.vx += -spring * (p.x - targetX) * dt;
                p.vy += -spring * (p.y - targetY) * dt;
                p.vz += -spring * (p.z - targetZ) * dt;
                p.vx *= (1.0f - damp * dt);
                p.vy *= (1.0f - damp * dt);
                p.vz *= (1.0f - damp * dt);

                p.brightness = juce::jlimit(0.06f, 0.25f, 0.08f + eh * 0.15f);
                break;
            }
        }

        // Capture animation — pull everything to center
        if (captureImplodeFactor > 0.01f) {
            p.vx += -captureImplodeFactor * 15.0f * p.x * dt;
            p.vy += -captureImplodeFactor * 15.0f * p.y * dt;
            p.vz += -captureImplodeFactor * 15.0f * p.z * dt;
        }

        // Capture glow boost
        if (captureGlow > 0.01f && p.band != Band::Trail) {
            p.brightness = juce::jmin(1.0f, p.brightness + captureGlow);
        }

        // Integrate velocity
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        p.z += p.vz * dt;
    }
}

// ============================================================================
// Colour mapping
// ============================================================================

juce::Colour ParticleVisual::getParticleColour(const Particle& p) const
{
    float eh = energyHeat.load();
    float sb = specBass.load();
    float sm = specMid.load();
    float sh = specHigh.load();
    float te = totalEnergy.load();
    float br = breath.load();
    
    float bassDom = juce::jlimit(0.0f, 1.0f, (sb - sm) * 8.0f);
    float highDom = juce::jlimit(0.0f, 1.0f, (sh - sm) * 8.0f);
    float midDom  = juce::jlimit(0.0f, 1.0f, (sm - (sb + sh) * 0.5f) * 8.0f);
    
    juce::Colour base;
    
    switch (currentTheme) {
        case Theme::Nebula: {
            // Purple/pink — reacts to spectrum balance
            juce::Colour c1(0xff6C5CE7); // deep purple
            juce::Colour c2(0xffA29BFE); // light purple
            juce::Colour c3(0xff60A5FA); // blue
            juce::Colour warm(0xffFF6B9D); // pink
            
            juce::Colour spec = c1;
            spec = spec.interpolatedWith(warm, bassDom * 0.6f);
            spec = spec.interpolatedWith(c3, midDom * 0.5f);
            spec = spec.interpolatedWith(juce::Colour(0xff06D6A0), highDom * 0.5f);
            
            switch (p.band) {
                case Band::Bass:  base = c1.interpolatedWith(spec, 0.5f); break;
                case Band::Mid:   base = c2.interpolatedWith(c3, (std::sin(p.orbitAngle)+1)*0.5f).interpolatedWith(spec, 0.4f); break;
                case Band::High:  base = c3.interpolatedWith(juce::Colours::white, te * 0.3f).interpolatedWith(spec, 0.3f); break;
                case Band::Trail: base = c1.interpolatedWith(spec, 0.3f); break;
            }
            if (eh > 0.01f && p.band != Band::Trail)
                base = base.interpolatedWith(warm, eh * 0.25f);
            break;
        }
        
        case Theme::Aurora: {
            // Green/cyan/teal — reacts to correlation (stereo coherence)
            // Wide stereo = more colour variation, narrow = uniform green
            juce::Colour green(0xff06D6A0);   // base green
            juce::Colour teal(0xff00B4D8);    // teal
            juce::Colour lime(0xff7BED9F);    // bright lime
            juce::Colour violet(0xff9B59B6);  // purple accent for contrast
            float sw = stereoWidth.load();
            
            switch (p.band) {
                case Band::Bass:  base = green.interpolatedWith(teal, sw * 0.6f); break;
                case Band::Mid:   base = teal.interpolatedWith(lime, (std::sin(p.orbitAngle * 2.0f)+1)*0.5f); break;
                case Band::High:  base = lime.interpolatedWith(juce::Colours::white, te * 0.4f); break;
                case Band::Trail: base = green.darker(0.3f); break;
            }
            // Correlation drives violet accent — low correlation = more violet shimmer
            float corr = juce::jlimit(0.0f, 1.0f, 1.0f - sw * 0.5f);
            if (p.band != Band::Trail)
                base = base.interpolatedWith(violet, (1.0f - corr) * 0.3f);
            break;
        }
        
        case Theme::Solar: {
            // Orange/gold/red — reacts to RMS level (breath), brighter = hotter
            juce::Colour amber(0xffF59E0B);   // warm amber
            juce::Colour orange(0xffFF6348);  // hot orange
            juce::Colour gold(0xffFFD700);    // bright gold
            juce::Colour red(0xffEE5A24);     // deep red
            juce::Colour white(0xffFFF8E7);   // warm white
            
            switch (p.band) {
                case Band::Bass:  base = red.interpolatedWith(orange, br * 0.5f); break;
                case Band::Mid:   base = amber.interpolatedWith(gold, (std::sin(p.orbitAngle)+1)*0.5f); break;
                case Band::High:  base = gold.interpolatedWith(white, te * 0.5f + br * 0.2f); break;
                case Band::Trail: base = red.darker(0.4f); break;
            }
            // Louder = hotter shift toward white-gold
            if (p.band != Band::Trail)
                base = base.interpolatedWith(white, eh * 0.3f);
            break;
        }
        
        case Theme::Crystal: {
            // Ice blue/white/silver — reacts to true peak (sharper = brighter flashes)
            juce::Colour ice(0xff74B9FF);     // ice blue
            juce::Colour frost(0xffDFE6E9);   // silver/frost
            juce::Colour deepBlue(0xff0984E3);// deep blue
            juce::Colour white(0xffF8F9FA);   // pure white
            juce::Colour mint(0xff55EFC4);    // mint accent
            
            switch (p.band) {
                case Band::Bass:  base = deepBlue.interpolatedWith(ice, te * 0.4f); break;
                case Band::Mid:   base = ice.interpolatedWith(frost, (std::sin(p.orbitAngle * 1.5f)+1)*0.5f); break;
                case Band::High:  base = frost.interpolatedWith(white, te * 0.6f); break;
                case Band::Trail: base = deepBlue.darker(0.3f).interpolatedWith(mint, 0.1f); break;
            }
            // High crest/transient = bright white flash
            float bt2 = bassTransient.load();
            if (bt2 > 0.05f && p.band != Band::Trail)
                base = base.interpolatedWith(white, bt2 * 0.5f);
            break;
        }
    }
    
    return base.withAlpha(p.brightness);
}

// ============================================================================
// Capture animation triggers
// ============================================================================

void ParticleVisual::triggerCaptureImplode()
{
    captureAnim = CaptureAnim::Imploding;
    captureAnimTime = 0.0f;
}

void ParticleVisual::triggerCaptureRelease()
{
    if (captureAnim == CaptureAnim::Holding || captureAnim == CaptureAnim::Imploding) {
        captureAnim = CaptureAnim::Releasing;
        captureAnimTime = 0.0f;
        // Add burst velocity — outward from center
        for (auto& p : particles) {
            if (p.band == Band::Trail) continue;
            float dx = p.x;
            float dy = p.y;
            float dz = p.z;
            float dist = std::sqrt(dx*dx + dy*dy + dz*dz);
            if (dist < 0.01f) dist = 0.01f;
            float burst = 4.0f;
            p.vx += (dx / dist) * burst;
            p.vy += (dy / dist) * burst;
            p.vz += (dz / dist) * burst;
        }
    }
}

// ============================================================================
// OpenGL lifecycle
// ============================================================================

void ParticleVisual::newOpenGLContextCreated()
{
    shader = std::make_unique<juce::OpenGLShaderProgram>(glContext);

    if (shader->addVertexShader(vertexShaderSrc)
        && shader->addFragmentShader(fragmentShaderSrc)
        && shader->link())
    {
        shadersCompiled = true;
        DBG("ParticleVisual: shaders compiled successfully");
    }
    else
    {
        shadersCompiled = false;
        DBG("ParticleVisual: shader compilation failed: " + shader->getLastError());
        return;
    }

    initParticles();
    initVeins();

    auto& gl = glContext.extensions;
    gl.glGenBuffers(1, &vbo);
    gl.glGenBuffers(1, &veinVbo);
    contextActive = true;
    lastFrameTime = juce::Time::getMillisecondCounterHiRes() * 0.001;
}

void ParticleVisual::renderOpenGL()
{
    if (!shadersCompiled || !contextActive) return;

    double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    float dt = (float)(now - lastFrameTime);
    lastFrameTime = now;

    updateParticles(dt);

    auto& gl = glContext.extensions;
    auto desktopScale = (float)glContext.getRenderingScale();
    int w = (int)(getWidth() * desktopScale);
    int h = (int)(getHeight() * desktopScale);
    
    if (w <= 0 || h <= 0) return;

    juce::OpenGLHelpers::clear(juce::Colour(Palette::bgDark));
    juce::gl::glViewport(0, 0, w, h);
    juce::gl::glEnable(GL_BLEND);
    juce::gl::glBlendFunc(GL_SRC_ALPHA, GL_ONE); // Additive blending for dense orb glow
    juce::gl::glEnable(GL_POINT_SPRITE);
    juce::gl::glEnable(GL_VERTEX_PROGRAM_POINT_SIZE);

    shader->use();

    // === Build MVP matrix ===

    // Perspective projection
    float aspect = (float)w / (float)juce::jmax(1, h);
    float fov = 1.0f; // ~57 degrees
    float nearZ = 0.1f, farZ = 50.0f;
    float f = 1.0f / std::tan(fov * 0.5f);

    float proj[16] = {};
    proj[0]  = f / aspect;
    proj[5]  = f;
    proj[10] = (farZ + nearZ) / (nearZ - farZ);
    proj[11] = -1.0f;
    proj[14] = (2.0f * farZ * nearZ) / (nearZ - farZ);

    // Camera position — orbits around origin
    float camX = cameraDistance * std::cos(cameraElevation) * std::sin(cameraAngle);
    float camY = cameraDistance * std::sin(cameraElevation);
    float camZ = cameraDistance * std::cos(cameraElevation) * std::cos(cameraAngle);

    // lookAt: forward vector = normalize(-cam)
    float fx = -camX, fy = -camY, fz = -camZ;
    float fLen = std::sqrt(fx*fx + fy*fy + fz*fz);
    fx /= fLen; fy /= fLen; fz /= fLen;

    // right = cross(forward, worldUp)
    float upX = 0, upY = 1, upZ = 0;
    float rx = fy * upZ - fz * upY;
    float ry = fz * upX - fx * upZ;
    float rz = fx * upY - fy * upX;
    float rLen = std::sqrt(rx*rx + ry*ry + rz*rz);
    rx /= rLen; ry /= rLen; rz /= rLen;

    // recalculated up = cross(right, forward)
    upX = ry * fz - rz * fy;
    upY = rz * fx - rx * fz;
    upZ = rx * fy - ry * fx;

    // View matrix (column-major)
    float view[16] = {
        rx, upX, -fx, 0,
        ry, upY, -fy, 0,
        rz, upZ, -fz, 0,
        -(rx*camX + ry*camY + rz*camZ),
        -(upX*camX + upY*camY + upZ*camZ),
        -(-fx*camX + (-fy)*camY + (-fz)*camZ),
        1
    };

    // MVP = proj * view (column-major multiplication)
    float mvp[16] = {};
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            for (int k = 0; k < 4; ++k)
                mvp[row + col * 4] += proj[row + k * 4] * view[k + col * 4];

    GLint mvpLoc   = gl.glGetUniformLocation(shader->getProgramID(), "uMVP");
    GLint scaleLoc = gl.glGetUniformLocation(shader->getProgramID(), "uScale");
    gl.glUniformMatrix4fv(mvpLoc, 1, GL_FALSE, mvp);
    gl.glUniform1f(scaleLoc, (float)h * 0.02f);


    // === Build interleaved vertex data ===
    struct VertexData { float x, y, z, r, g, b, a, size; };
    std::vector<VertexData> verts(particles.size());

    for (size_t i = 0; i < particles.size(); ++i) {
        auto& p = particles[i];
        auto col = getParticleColour(p);
        verts[i] = {
            p.x, p.y, p.z,
            col.getFloatRed(), col.getFloatGreen(), col.getFloatBlue(), col.getFloatAlpha(),
            p.size
        };
    }

    gl.glBindBuffer(GL_ARRAY_BUFFER, vbo);
    gl.glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts.size() * sizeof(VertexData)),
                    verts.data(), GL_DYNAMIC_DRAW);

    GLint posAttr  = gl.glGetAttribLocation(shader->getProgramID(), "aPos");
    GLint colAttr  = gl.glGetAttribLocation(shader->getProgramID(), "aColour");
    GLint sizeAttr = gl.glGetAttribLocation(shader->getProgramID(), "aSize");

    gl.glEnableVertexAttribArray((GLuint)posAttr);
    gl.glVertexAttribPointer((GLuint)posAttr, 3, GL_FLOAT, GL_FALSE,
                             sizeof(VertexData), nullptr);

    gl.glEnableVertexAttribArray((GLuint)colAttr);
    gl.glVertexAttribPointer((GLuint)colAttr, 4, GL_FLOAT, GL_FALSE,
                             sizeof(VertexData), (void*)(3 * sizeof(float)));

    gl.glEnableVertexAttribArray((GLuint)sizeAttr);
    gl.glVertexAttribPointer((GLuint)sizeAttr, 1, GL_FLOAT, GL_FALSE,
                             sizeof(VertexData), (void*)(7 * sizeof(float)));

    juce::gl::glDrawArrays(GL_POINTS, 0, (GLsizei)verts.size());

    gl.glDisableVertexAttribArray((GLuint)posAttr);
    gl.glDisableVertexAttribArray((GLuint)colAttr);
    gl.glDisableVertexAttribArray((GLuint)sizeAttr);
    gl.glBindBuffer(GL_ARRAY_BUFFER, 0);


    juce::gl::glDisable(GL_BLEND);
    juce::gl::glDisable(GL_VERTEX_PROGRAM_POINT_SIZE);
    juce::gl::glDisable(GL_POINT_SPRITE);
}

void ParticleVisual::openGLContextClosing()
{
    contextActive = false;
    if (vbo != 0) {
        glContext.extensions.glDeleteBuffers(1, &vbo);
        vbo = 0;
    }
    if (veinVbo != 0) {
        glContext.extensions.glDeleteBuffers(1, &veinVbo);
        veinVbo = 0;
    }
    shader.reset();
    shadersCompiled = false;
}

void ParticleVisual::resized()
{
    // OpenGL viewport is set in renderOpenGL — nothing to do here
}

// ============================================================================
// Number strip — painted by parent onto its Graphics context
// ============================================================================

void ParticleVisual::paintNumberStrip(juce::Graphics& g, juce::Rectangle<int> area, const MeterData& md)
{
    // Semi-transparent dark background
    g.setColour(juce::Colour(0xff0C0E1A).withAlpha(0.85f));
    g.fillRect(area);
    g.setColour(juce::Colour(0xff1A1D30));
    g.drawHorizontalLine(area.getY(), (float)area.getX(), (float)area.getRight());

    struct Metric {
        const char* label;
        float value;
        const char* unit;
        bool isDb;
    };

    Metric metrics[] = {
        { "INT",   md.integrated,    " LUFS", true  },
        { "ST",    md.shortTerm,     " LUFS", true  },
        { "MOM",   md.momentary,     " LUFS", true  },
        { "LRA",   md.loudnessRange, " LU",   false },
        { "TP L",  md.truePeakL,     " dB",   true  },
        { "TP R",  md.truePeakR,     " dB",   true  },
        { "CORR",  md.correlation,   "",       false },
        { "WIDTH", md.width,         "",       false },
        { "CREST", md.crestFactor,   " dB",   false },
    };

    int count = 9;
    float cellW = (float)area.getWidth() / (float)count;

    g.setFont(juce::Font(juce::FontOptions(9.0f)));

    for (int i = 0; i < count; ++i) {
        float x = area.getX() + i * cellW;

        // Label
        g.setColour(juce::Colour(0xff8890B0));
        g.drawText(metrics[i].label, (int)x, area.getY() + 2, (int)cellW, 12,
                   juce::Justification::centred);

        // Value
        juce::String valStr;
        if (metrics[i].isDb && metrics[i].value <= -100.0f)
            valStr = "-inf";
        else if (metrics[i].unit[0] == '\0')
            valStr = juce::String(metrics[i].value, 2);
        else
            valStr = juce::String(metrics[i].value, 1) + metrics[i].unit;

        g.setColour(juce::Colour(0xffE2E4F0));
        g.drawText(valStr, (int)x, area.getY() + 13, (int)cellW, 13,
                   juce::Justification::centred);

        // Cell divider
        if (i > 0) {
            g.setColour(juce::Colour(0xff1A1D30));
            g.drawVerticalLine((int)x, (float)area.getY() + 2, (float)area.getBottom() - 2);
        }
    }
}
