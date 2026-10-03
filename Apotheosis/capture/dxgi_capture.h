#pragma once
#include "capture.h"
#include "dxgi_capture_logic.h"

#include <memory>
#include <string>
#include <vector>

// Screen capture through the Windows desktop duplication API (DXGI): the picture the
// GPU is presenting on a monitor, without a capture card. The frame delivered to the
// detector is the centred square of the chosen monitor, like the other sources.
//
// Limits to be aware of (also shown in the settings page):
//  - Games in exclusive fullscreen may not be duplicable; borderless / windowed is.
//  - HDR desktops and rotated monitors are not supported (reported, not guessed).
//  - The picture comes from the GPU the monitor is attached to; the mouse pointer is
//    not part of it.
namespace dxgi_capture {

// Monitors that can be captured, in a stable order. Never throws; on failure the list
// is empty and `error` says why.
std::vector<OutputInfo> EnumerateOutputs(std::string& error);

// `outputName` is OutputInfo::deviceName; empty (or no longer present) selects the
// primary monitor. The capture runs on its own thread and recovers from lock screens
// and display mode changes; HasStopped() turns true only when it cannot work at all.
std::unique_ptr<IScreenCapture> Create(const std::string& outputName, int outputSide);

} // namespace dxgi_capture
