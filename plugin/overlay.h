// Read-only status overlay drawn with Dear ImGui on top of Kenshi's D3D11 swap chain.
// It takes no input (Kenshi reads input through DirectInput, which a window-message based UI
// cannot intercept); all interaction goes through hotkeys.
#pragma once
#include <string>
#include <vector>

namespace kcp {

struct OverlayModel {
    bool visible = true;
    std::string title;
    std::vector<std::string> lines;     // status block
    std::vector<std::string> chat;      // recent events
    std::vector<std::string> toasts;    // transient warnings (already filtered by age)
};

bool OverlayInstall(std::string* err);      // hooks IDXGISwapChain::Present / ResizeBuffers
void OverlayPublish(OverlayModel model);    // game thread -> render thread
void OverlayShutdown();

} // namespace kcp
