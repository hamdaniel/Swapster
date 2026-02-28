#pragma once
#include <windows.h>

namespace swapster {

/**
 * Initializes optional process-wide settings used by SwapAllWindows().
 *
 * Currently:
 * - Best-effort enables Per-Monitor DPI Awareness V2 when available.
 *
 * Call once at startup (safe to call multiple times).
 */
void Initialize();

/**
 * Swaps all eligible top-level windows between the monitor they are currently on
 * and the "other" monitor (first monitor returned by EnumDisplayMonitors that is
 * different from the window's current monitor).
 *
 * Behavior (matches your current server):
 * - Collects eligible windows
 * - Freezes redraw for all
 * - Forces snapped windows into a normal rect in-place (rcNormalPosition trick, inset=0)
 * - Computes target rect by relative transform between monitor rectangles
 * - Applies all moves via DeferWindowPos (batch) with fallback to sequential
 * - Restores maximized state
 * - Unfreezes + redraws, then applies a small "kick" to Explorer/Electron windows
 * - Restores original foreground window (best-effort)
 */
void SwapAllWindows();

} // namespace swapster