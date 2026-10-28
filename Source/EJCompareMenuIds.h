#pragma once

/** ---- THE COMPARE/MATCH MENU'S ID BANDS AND NAME LENGTH (A0, 28 Oct 2026) --------------------

    These three were file-statics in PluginEditor.cpp, read by the Compare menu there AND by the
    Match page's mix picker - which moved into EJMatchTab.cpp with the rest of the page. One number
    with two definitions is the fault this codebase keeps writing notes about, so they live here
    instead and both files include it.

    The id BAND is what matters: a menu id is decoded by which band it falls in, so the band size is
    shared arithmetic, not a style choice. 100.. snapshots, 300.. references. */
namespace echojay
{
inline constexpr int kCompareMenuBandSize   = 100;
inline constexpr int kCompareMenuRefCap     = kCompareMenuBandSize - 1;   // 300..398
inline constexpr int kCompareMenuRefNameLen = 44;
}
