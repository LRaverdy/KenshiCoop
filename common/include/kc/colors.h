// One colour per player, the same on every machine and everywhere it shows (world map, minimap,
// marker above the head, squad bar frame, pings). Players are numbered from 1 (the host) to
// kMaxPlayers; the palette avoids red, which the map keeps for enemies.
#pragma once
#include <cstdint>

namespace kc {

struct PlayerColour {
    uint8_t r, g, b;
    const char* name;   // in French, without accents (logs, tests)
};

inline constexpr PlayerColour kPlayerPalette[8] = {
    {255, 200, 50, "or"},       // 1: the host
    {60, 160, 255, "bleu"},     // 2
    {80, 220, 80, "vert"},      // 3
    {230, 80, 230, "magenta"},  // 4
    {255, 140, 40, "orange"},   // 5
    {60, 230, 220, "cyan"},     // 6
    {240, 240, 240, "blanc"},   // 7
    {160, 120, 255, "violet"},  // 8
};

// Player 0 (no one) and ids past the palette wrap around it.
inline constexpr PlayerColour PlayerColor(uint8_t playerId) {
    return kPlayerPalette[(playerId == 0 ? 0 : (playerId - 1)) % 8];
}

// 0xAABBGGRR (Dear ImGui's packed colour)
inline constexpr uint32_t PlayerColorAbgr(uint8_t playerId, uint8_t alpha = 255) {
    const PlayerColour c = PlayerColor(playerId);
    return uint32_t(alpha) << 24 | uint32_t(c.b) << 16 | uint32_t(c.g) << 8 | c.r;
}

} // namespace kc
