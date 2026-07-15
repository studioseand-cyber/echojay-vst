#pragma once

// Single source of truth for text-input field corner rounding, shared by the
// main plugin's LookAndFeel (Project name box, Mix Bus / genre dropdowns) and
// the Link name box, so their corners stay identical and can never drift.
namespace EchoJayChrome
{
    inline constexpr float kFieldCorner = 8.0f;
}
