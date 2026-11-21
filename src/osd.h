// Halo - on-screen display: a small pill near the bottom of the screen when DPI or the profile
// changes from a hotkey, the tray or an automatic switch. Click-through, never takes focus, fades out.
#pragma once
#include "common.h"
#include "gfx.h"

namespace osd {

// `level` 0-1 draws a ring (DPI within the presets, or battery); -1 draws none
void Show(const std::wstring& title, const std::wstring& sub, float level, D2D1_COLOR_F accent);
void Destroy();

} // namespace osd
