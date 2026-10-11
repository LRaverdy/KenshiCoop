// The overlay's map layer (inside overlay.cpp's frame): markers on the game's map screen with their
// legend and tooltips, the minimap, the markers above the players' heads, the squad bar frames and
// the pings, plus the mouse input that goes with them (minimap wheel, buttons, drag and resize, placing pings).
#pragma once
#include <windows.h>

#include <d3d11.h>

#include "map_view.h"
#include "overlay.h"

struct ImDrawList;

namespace kcp {

// Render thread, between ImGui::NewFrame and ImGui::Render.
void MapOverlayDraw(const MapScene& s, float w, float h, ID3D11Device* device);
// Window procedure (main thread): true when the message is ours (a ping, the minimap's wheel,
// buttons, drag or resize grip) and must not reach the game. sx, sy: the cursor in back buffer pixels.
bool MapOverlayMessage(UINT msg, WPARAM wp, float sx, float sy);
// DirectInput filter: the mouse is over the minimap / a ping click is under way (no game clicks).
bool MapOverlayHoldsMouse();
// The minimap is being moved or resized with the mouse (the button is still down).
bool MapOverlayDragging();
// Back buffer pixels per window client pixel (set by the overlay each frame).
void MapOverlaySetCursorScale(float sx, float sy);
void MapOverlayRelease();   // the D3D texture
MapDrawn MapOverlayLastDrawn();   // what the last frame drew (tests)

} // namespace kcp
