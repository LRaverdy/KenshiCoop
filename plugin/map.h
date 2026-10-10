// Map markers, minimap, markers above the heads, squad bar frames and pings: the game side.
// Each frame (game thread) the scene is built from the session's map feed and the game's own map
// screen, camera and squad bar, then handed to the overlay, which draws it.
#pragma once
#include <string>

#include "kc/session.h"
#include "map_view.h"
#include "util.h"
#include "world.h"

namespace kcp {

// Game thread, every tick. live: the world may be touched this frame.
void UpdateMapScene(kc::Session& session, KenshiWorld& world, const Config& cfg, bool live);
const MapScene& LastMapScene();   // what the overlay was last given (game thread)
// Tests: what this machine draws, one line ("carte" / "minicarte" / "tetes" / "barre" / "pings").
std::string DescribeMapScene(const std::string& what);
// Tests: the game's worldToMapCoords against our projection for (x, z): "game=x,y ours=x,y".
std::string CheckMapProjection(float x, float z);
// The overlay, in its frame: reads the GUI rectangles (map image, squad bar frames, covered) again,
// when it runs on the thread of the game's GUI. False when it cannot (the tick's values stay).
bool MapRefreshGui(MapScene& s);
// Tests ("mapconv"): MyGUI view, back buffer, window client and DPI, the factors, the portrait
// frames read now (raw MyGUI and converted) and the ones the overlay last drew.
std::string DescribeMapConversion();
// Tests ("mapui open|close|maptab|state"): clicks the game's MAP button through MyGUI's input.
std::string MapUi(const std::string& what);

// Hooks on the squad bar's portraits (installed with the others in hooks.cpp).
namespace mapmarks {
using PortraitUpdateFn = void (*)(void* cell, const void* info, void* data);
using PortraitDtorFn = void (*)(void* cell);
extern PortraitUpdateFn o_portraitUpdate;
extern PortraitDtorFn o_portraitDtor;
void hk_portraitUpdate(void* cell, const void* info, void* data);
void hk_portraitDtor(void* cell);
} // namespace mapmarks

} // namespace kcp
