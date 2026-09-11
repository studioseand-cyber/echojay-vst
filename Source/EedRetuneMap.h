/*
    EedRetuneMap.h - the RETUNE dial's transfer function (UI_SIMPLIFICATION.md
    rounds 38-40, built in round 46).

    One 0-400 dial, Antares-calibrated, drives the corrector's two internal
    controls: retune_speed_ms (the glide time constant) and depth (how much
    of the correction is applied). The curve is what the round-39 hard
    requirement selected - median activity strictly decreasing across the
    whole dial - measured at 18 positions on the reference take (v3, log
    tools/pitch_activity/logs_2026-09-03/transfer_tf4_2026-09-05.txt):

        dial d in [0, 50]:   t = d / 50
                             retune_ms = 6 + 74 t^2        (slow start)
                             depth     = 1 - 0.65 t^0.7    (depth leads)
        dial d in [50, 400]: linear in d between the fitted Antares anchors
                             (50: 80 ms, 0.35) (100: 150, 0.25) (200: 150, 0.15) (400: 150, 0.10)

    The exponents (2 and 0.7) are a regime constant: v1 linear failed
    monotonicity (activity rose to dial 10), v2 with linear depth had a
    0.03c uptick at dial 15 and worse tails, v4 (t^0.5) tied at dials 25/30.
    Re-verify on any material change. Header-only, shared by the processor
    and the measurement tools so the shipped mapping IS the measured one.
*/
#pragma once
#include <cmath>
#include <algorithm>

namespace echojay
{
struct RetuneMap
{
    static constexpr float kMaxDial = 400.0f;

    static void dialTo (float dial, float& retuneMs, float& depth) noexcept
    {
        const double d = std::min ((double) kMaxDial, std::max (0.0, (double) dial));
        if (d <= 50.0)
        {
            const double t = d / 50.0;
            retuneMs = (float) (6.0 + 74.0 * t * t);
            depth    = (float) (1.0 - 0.65 * std::pow (t, 0.7));
            return;
        }
        struct Knot { double d, ms, depth; };
        static constexpr Knot k[] = { { 50.0, 80.0, 0.35 }, { 100.0, 150.0, 0.25 },
                                      { 200.0, 150.0, 0.15 }, { 400.0, 150.0, 0.10 } };
        for (int i = 0; i < 3; ++i)
            if (d <= k[i + 1].d)
            {
                const double u = (d - k[i].d) / (k[i + 1].d - k[i].d);
                retuneMs = (float) (k[i].ms + (k[i + 1].ms - k[i].ms) * u);
                depth    = (float) (k[i].depth + (k[i + 1].depth - k[i].depth) * u);
                return;
            }
        retuneMs = 150.0f; depth = 0.10f;
    }

    // Round 51 (ruling withdrawn: no off-curve state): the dial position whose
    // curve point is NEAREST to an arbitrary (retune_ms, depth), in a space
    // where the full retune range (150 ms) and the full depth range (1.0)
    // are the same size. A loaded state that is not on the curve snaps here.
    static float nearestDial (float retuneMs, float depth) noexcept
    {
        const double pm = std::min (1.0, std::max (0.0, (double) retuneMs / 150.0));
        const double pd = std::min (1.0, std::max (0.0, (double) depth));
        double bestD = 1.0e9; float best = 0.0f;
        for (float d = 0.0f; d <= kMaxDial; d += 0.25f)
        {
            float ms = 0.0f, dp = 1.0f; dialTo (d, ms, dp);
            const double dx = ms / 150.0 - pm, dy = dp - pd;
            const double dist = dx * dx + dy * dy;
            if (dist < bestD) { bestD = dist; best = d; }
        }
        return best;
    }

    // The inverse of the retune-ms branch alone: where a mode's retune sits
    // on the dial. Past 150 ms (the corrector's cap) the dial is at 100.
    static float dialForRetuneMs (float ms) noexcept
    {
        const double m = (double) ms;
        if (m <= 6.0)   return 0.0f;
        if (m <= 80.0)  return (float) (50.0 * std::sqrt ((m - 6.0) / 74.0));
        if (m <= 150.0) return (float) (50.0 + 50.0 * (m - 80.0) / 70.0);
        return 100.0f;
    }
};
} // namespace echojay
