// What the map screen, the minimap and the markers above the heads show, and the projections that
// place it on the screen. Built on the game thread each frame (plugin/map.cpp), drawn by the
// overlay (plugin/overlay.cpp); the debug channel reads the same scene back (exp_map).
#pragma once
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "kc/protocol.h"

namespace kcp {

struct SceneChar {
    uint32_t netId = 0;
    uint8_t owner = 0;
    std::string name;
    kc::Vec3 pos;
    bool avatar = false, down = false, dead = false;
    bool head = false;          // shown above its head (an avatar standing in this machine's world)
};
struct SceneThreat {
    kc::Vec3 pos;
    int count = 0;
    int kind = 1;               // kc::ThreatKind
    std::string label;
};
struct ScenePing {
    uint32_t id = 0;
    uint8_t owner = 0;
    std::string who;
    int kind = 0;               // kc::PingKind
    kc::Vec3 pos;
    float age = 0;              // seconds
};
struct SceneRect {
    float x = 0, y = 0, w = 0, h = 0;
    uint8_t owner = 0;
};

struct MapScene {
    bool live = false;          // a world is loaded and running: anything may be drawn
    bool covered = false;       // the management screen or the character editor is up: no 3D markers, no minimap
    uint8_t me = 0;             // the local player
    std::vector<std::pair<uint8_t, std::string>> players;   // legend: id, name
    // the game's map screen, when it shows: the map image's rectangle (it pans and zooms with it),
    // the visible part of it, and the world rectangle the image covers
    bool mapOpen = false;
    float imgX = 0, imgY = 0, imgW = 0, imgH = 0;
    float clipX0 = 0, clipY0 = 0, clipX1 = 0, clipY1 = 0;
    bool boundsOk = false;
    float minX = 0, minZ = 0, sizeX = 0, sizeZ = 0;
    // the 3D camera: Ogre projection * view (row-major), and where it looks (x, z)
    bool camOk = false;
    float viewProj[16] = {};
    float camFwdX = 0, camFwdZ = -1;
    // the minimap's centre: the selected character, else the player's own
    bool centreOk = false;
    kc::Vec3 centre;
    std::vector<SceneChar> chars;
    std::vector<SceneThreat> threats;
    std::vector<ScenePing> pings;
    std::vector<SceneRect> portraits;   // squad bar frames of the players' own characters
    // settings
    bool showMap = true, showHeads = true, showPortraits = true, showMinimap = true, minimapRotate = false, showPings = true;
    int minimapCorner = 1;              // 0 top left, 1 top right, 2 bottom left, 3 bottom right
    float minimapZoom = 1500.0f;        // world units from the centre to the rim
};

// ---- projections (pure: the overlay and the tests use the same ones)

// The map screen: MapScreen::worldToMapCoords puts (x, z) at (x - minX) / sizeX * image width,
// (z - minZ) / sizeZ * image height inside the map image; the image sits at imgX, imgY.
inline bool WorldToMapScreen(const MapScene& s, const kc::Vec3& p, float& sx, float& sy) {
    if (!s.boundsOk || s.sizeX <= 0 || s.sizeZ <= 0) return false;
    sx = s.imgX + (p.x - s.minX) / s.sizeX * s.imgW;
    sy = s.imgY + (p.z - s.minZ) / s.sizeZ * s.imgH;
    return std::isfinite(sx) && std::isfinite(sy);
}
inline bool MapScreenToWorld(const MapScene& s, float sx, float sy, kc::Vec3& out) {
    if (!s.boundsOk || s.imgW <= 0 || s.imgH <= 0) return false;
    out.x = s.minX + (sx - s.imgX) / s.imgW * s.sizeX;
    out.z = s.minZ + (sy - s.imgY) / s.imgH * s.sizeZ;
    out.y = 0;
    return true;
}
inline bool InMapClip(const MapScene& s, float sx, float sy) {
    return sx >= s.clipX0 && sx <= s.clipX1 && sy >= s.clipY0 && sy <= s.clipY1;
}

// The 3D view: false when behind the camera.
inline bool WorldToScreen(const MapScene& s, const kc::Vec3& p, float w, float h, float& sx, float& sy) {
    if (!s.camOk) return false;
    const float* m = s.viewProj;
    const float cx = m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3];
    const float cy = m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7];
    const float cw = m[12] * p.x + m[13] * p.y + m[14] * p.z + m[15];
    if (!(cw > 1e-3f)) return false;
    sx = (cx / cw * 0.5f + 0.5f) * w;
    sy = (0.5f - cy / cw * 0.5f) * h;
    return std::isfinite(sx) && std::isfinite(sy);
}

// The ground under a screen point, taken as the horizontal plane at height `groundY` (Kenshi's
// ground is not flat: close enough for a ping near the characters).
inline bool ScreenToGround(const MapScene& s, float sx, float sy, float w, float h, float groundY, kc::Vec3& out);

// The minimap: world offset from the centre -> screen offset from the circle's centre (pixels),
// north up (screen up = -z, as on the map screen) or turned so the camera looks up.
struct MinimapFrame {
    float cx = 0, cy = 0, radius = 0;   // screen
    float scale = 0;                    // pixels per world unit
    float cosA = 1, sinA = 0;           // rotation applied to north-up offsets
};
inline MinimapFrame MakeMinimapFrame(const MapScene& s, float cx, float cy, float radius) {
    MinimapFrame f;
    f.cx = cx; f.cy = cy; f.radius = radius;
    f.scale = radius / (s.minimapZoom > 1 ? s.minimapZoom : 1);
    if (s.minimapRotate) {
        const float len = std::sqrt(s.camFwdX * s.camFwdX + s.camFwdZ * s.camFwdZ);
        if (len > 1e-4f) {
            // the camera's facing, (fx, fz) in north-up screen axes, must point straight up
            const float a = std::atan2(s.camFwdZ, s.camFwdX);
            const float t = -1.5707963f - a;
            f.cosA = std::cos(t); f.sinA = std::sin(t);
        }
    }
    return f;
}
inline void MinimapProject(const MinimapFrame& f, const kc::Vec3& centre, const kc::Vec3& p, float& sx, float& sy) {
    const float dx = (p.x - centre.x) * f.scale, dy = (p.z - centre.z) * f.scale;
    sx = f.cx + dx * f.cosA - dy * f.sinA;
    sy = f.cy + dx * f.sinA + dy * f.cosA;
}
inline kc::Vec3 MinimapUnproject(const MinimapFrame& f, const kc::Vec3& centre, float sx, float sy) {
    const float rx = sx - f.cx, ry = sy - f.cy;
    const float dx = rx * f.cosA + ry * f.sinA, dy = -rx * f.sinA + ry * f.cosA;
    return {centre.x + dx / f.scale, centre.y, centre.z + dy / f.scale};
}

inline bool ScreenToGround(const MapScene& s, float sx, float sy, float w, float h, float groundY, kc::Vec3& out) {
    if (!s.camOk || w <= 0 || h <= 0) return false;
    // Solve viewProj * (x, groundY, z, 1) ~ (ndcX * cw, ndcY * cw, ., cw) for x, z: two linear equations.
    const float nx = sx / w * 2.0f - 1.0f, ny = 1.0f - sy / h * 2.0f;
    const float* m = s.viewProj;
    // row_i . p - nd_i * row_3 . p = 0  ->  a_i x + b_i z = c_i
    const float a0 = m[0] - nx * m[12], b0 = m[2] - nx * m[14], c0 = -((m[1] - nx * m[13]) * groundY + (m[3] - nx * m[15]));
    const float a1 = m[4] - ny * m[12], b1 = m[6] - ny * m[14], c1 = -((m[5] - ny * m[13]) * groundY + (m[7] - ny * m[15]));
    const float det = a0 * b1 - a1 * b0;
    if (std::fabs(det) < 1e-9f) return false;
    out.x = (c0 * b1 - c1 * b0) / det;
    out.z = (a0 * c1 - a1 * c0) / det;
    out.y = groundY;
    // it must be in front of the camera
    const float cw = m[12] * out.x + m[13] * out.y + m[14] * out.z + m[15];
    return cw > 1e-3f && std::isfinite(out.x) && std::isfinite(out.z);
}

const char* PingKindName(int kind);     // French, for the players
const char* ThreatKindName(int kind);   // French

} // namespace kcp
