#pragma once
#include <JuceHeader.h>

// Shared mixer-fader taper for the Link mixer strips and the main plugin's
// own bus fader. The two apply paths, LinkProcessor::applyGainSmoothed and
// EchoJayProcessor::applyBusGainSmoothed, are verbatim twins, so the mute has
// to live in one place or it fixes only one surface.
//
// The fader value is dB across [kMixerFaderMinDb .. hi]. Plain
// decibelsToGain(kMixerFaderMinDb) is 0.063, which is -24 dBFS: audible. The
// bottom of the throw was quiet, not muted. Here the bottom rail is a TRUE
// zero, and the bottom kMixerFaderFadeDb of the range crossfades the (already
// small) gain down to it, so silence is reached over the last stretch of the
// travel instead of in one pixel. Above the fade zone the taper is unchanged:
// linear in dB, decibelsToGain, so 0 dB stays bit-transparent unity and
// gain-staging near the top is untouched.
namespace EchoJayFader
{
    constexpr float kMixerFaderMinDb  = -24.0f;   // bottom rail, == the editor's faderLo()
    constexpr float kMixerFaderFadeDb = 6.0f;     // fade-to-zero zone above the rail

    inline float gainForDb (float db) noexcept
    {
        if (db <= kMixerFaderMinDb) return 0.0f;                     // true zero at the bottom
        const float g    = juce::Decibels::decibelsToGain (db);
        const float zTop = kMixerFaderMinDb + kMixerFaderFadeDb;
        if (db < zTop)
            return g * ((db - kMixerFaderMinDb) / kMixerFaderFadeDb); // crossfade to 0, no cliff
        return g;
    }
}
