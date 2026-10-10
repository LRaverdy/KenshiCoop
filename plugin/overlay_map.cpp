// The overlay's map layer: markers on the game's map screen (legend, tooltips), the minimap, the
// markers above the players' heads, the squad bar frames and the pings, plus their mouse input.
// Everything is drawn from the MapScene the game thread built this frame (plugin/map.cpp).
#include "overlay_map.h"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "kc/colors.h"
#include "map.h"
#include "util.h"

namespace kcp {

namespace {

constexpr float kPi = 3.14159265f;
constexpr ImU32 kEnemy = IM_COL32(235, 45, 40, 255);
constexpr ImU32 kShadow = IM_COL32(0, 0, 0, 200);
constexpr float kHeadHeight = 22.0f;      // world units above a character's feet (~2.2 m)
constexpr float kMinZoom = 300.0f, kMaxZoom = 30000.0f;
constexpr float kUnitsPerMetre = 10.0f;   // docs/MOTEUR.md: 1 world unit ~ 10 cm

ImU32 Col(uint8_t owner, float alpha = 1.0f) { return kc::PlayerColorAbgr(owner, uint8_t(std::clamp(alpha, 0.0f, 1.0f) * 255.0f)); }
ImU32 WithAlpha(ImU32 c, float a) { return (c & 0x00FFFFFF) | (ImU32(std::clamp(a, 0.0f, 1.0f) * 255.0f) << 24); }
float Dist2D(const kc::Vec3& a, const kc::Vec3& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.z - b.z) * (a.z - b.z)); }
std::string Metres(float units) {
    char b[32];
    const float m = units / kUnitsPerMetre;
    if (m >= 1000.0f) snprintf(b, sizeof(b), "%.1f km", double(m / 1000.0f));
    else snprintf(b, sizeof(b), "%.0f m", double(m));
    return b;
}
const char* PingLetter(int kind) {
    switch (kind) {
    case 1: return "!";
    case 2: return "$";
    case 3: return "?";
    }
    return ">";
}
float PingAlpha(float age) { return age < 7.0f ? 1.0f : std::max(0.0f, 1.0f - (age - 7.0f) / 3.0f); }   // fades over its last 3 s

void TextShadow(ImDrawList* dl, ImVec2 p, ImU32 col, const char* text) {
    dl->AddText(ImVec2(p.x + 1, p.y + 1), WithAlpha(kShadow, float((col >> 24) & 0xFF) / 255.0f), text);
    dl->AddText(p, col, text);
}
void TextCentred(ImDrawList* dl, ImVec2 p, ImU32 col, const char* text) {
    const ImVec2 sz = ImGui::CalcTextSize(text);
    TextShadow(dl, ImVec2(p.x - sz.x * 0.5f, p.y - sz.y * 0.5f), col, text);
}

// A tooltip that does not need ImGui's mouse (the game keeps the mouse).
void Tooltip(float x, float y, const std::vector<std::string>& lines) {
    ImGui::SetNextWindowPos(ImVec2(x + 16, y + 8), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::Begin("##kcmaptip", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i == 0) ImGui::TextColored(ImVec4(1, 0.85f, 0.4f, 1), "%s", lines[i].c_str());
        else ImGui::TextUnformatted(lines[i].c_str());
    }
    ImGui::End();
}

// ---- shared with the window procedure (main thread) and the DirectInput filter
std::mutex g_mx;
MapScene g_inScene;                  // the scene last drawn
bool g_mmShown = false;
MinimapFrame g_mm;
ImVec4 g_plus, g_minus;              // the minimap's +/- buttons (x0, y0, x1, y1)
float g_screenW = 0, g_screenH = 0;
std::atomic<float> g_curX{-1}, g_curY{-1};
std::atomic<bool> g_overMinimap{false};
std::atomic<bool> g_pingsArmed{false};   // pings are on and a world shows: Alt+clicks are ours
std::atomic<double> g_drawnAt{-1e9};       // the layer's last frame (a stale one holds nothing back)
float g_zoomPending = 0;             // a zoom change not yet back from the game thread's settings
double g_zoomPendingAt = 0;
int g_midDownX = -1, g_midDownY = -1;
MapDrawn g_drawn;                    // what the last frame drew (tests: mapconv), under g_mx
int g_frameHeads = 0, g_frameMarkers = 0, g_frameMarkersTotal = 0;   // counted while drawing (render thread)
std::string g_lastMarkersKey;        // map screen markers log: on change, every 2 s at most
double g_lastMarkersAt = -1e9;
double g_midDownAt = 0;

// ---- the world map texture (GUI_Map.dds, what the map screen shows): one mip of 2048 px or less
ID3D11ShaderResourceView* g_mapSrv = nullptr;
bool g_texTried = false;

bool LoadMapTexture(ID3D11Device* dev) {
    const std::wstring path = GameDir() + L"data\\gui\\gfx\\GUI_Map.dds";
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) { Log("minimap: %ls not found, grid background", path.c_str()); return false; }
    uint8_t hdr[128];
    bool ok = fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) && std::memcmp(hdr, "DDS ", 4) == 0;
    uint32_t height = 0, width = 0, mips = 0;
    char four[5] = {};
    if (ok) {
        std::memcpy(&height, hdr + 12, 4);
        std::memcpy(&width, hdr + 16, 4);
        std::memcpy(&mips, hdr + 28, 4);
        std::memcpy(four, hdr + 84, 4);
    }
    const bool dxt1 = std::strcmp(four, "DXT1") == 0, dxt5 = std::strcmp(four, "DXT5") == 0;
    ok = ok && (dxt1 || dxt5) && width >= 4 && height >= 4 && width <= 32768 && height <= 32768;
    if (!ok) { fclose(f); Log("minimap: GUI_Map.dds not a DXT1/DXT5 texture (%s), grid background", four); return false; }
    const uint32_t block = dxt1 ? 8 : 16;
    auto mipBytes = [&](uint32_t w, uint32_t h) { return uint64_t(std::max(1u, (w + 3) / 4)) * std::max(1u, (h + 3) / 4) * block; };
    uint64_t offset = 128;
    uint32_t w = width, h = height, level = 0;
    while (w > 2048 && level + 1 < std::max(1u, mips)) {
        offset += mipBytes(w, h);
        w = std::max(1u, w / 2); h = std::max(1u, h / 2); ++level;
    }
    std::vector<uint8_t> data(size_t(mipBytes(w, h)));
    ok = _fseeki64(f, int64_t(offset), SEEK_SET) == 0 && fread(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    if (!ok) { Log("minimap: cannot read GUI_Map.dds"); return false; }
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = dxt1 ? DXGI_FORMAT_BC1_UNORM : DXGI_FORMAT_BC3_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = data.data();
    sd.SysMemPitch = std::max(1u, (w + 3) / 4) * block;
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(dev->CreateTexture2D(&td, &sd, &tex)) || !tex) { Log("minimap: cannot create the map texture"); return false; }
    const HRESULT hr = dev->CreateShaderResourceView(tex, nullptr, &g_mapSrv);
    tex->Release();
    if (FAILED(hr)) { g_mapSrv = nullptr; return false; }
    Log("minimap: world map texture %ux%u (mip %u of GUI_Map.dds %ux%u)", w, h, level, width, height);
    return true;
}

// ---- the map screen
void DrawMapScreen(const MapScene& s, float w, float h) {
    (void)w; (void)h;
    g_frameMarkers = 0;
    g_frameMarkersTotal = int(s.threats.size() + s.chars.size() + (s.showPings ? s.pings.size() : 0));
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->PushClipRect(ImVec2(s.clipX0, s.clipY0), ImVec2(s.clipX1, s.clipY1), true);
    const float cx = g_curX.load(), cy = g_curY.load();
    float bestD = 12.0f;
    std::vector<std::string> tip;
    for (const auto& t : s.threats) {
        float x, y;
        if (!WorldToMapScreen(s, t.pos, x, y) || !InMapClip(s, x, y)) continue;
        ++g_frameMarkers;
        const float r = t.kind == 3 ? 7.0f : 6.0f;
        if (t.kind == 3) dl->AddCircle(ImVec2(x, y), r + 3.0f + 2.0f * std::sin(float(ImGui::GetTime()) * 5.0f), kEnemy, 20, 2.0f);
        dl->AddCircleFilled(ImVec2(x, y), r, kEnemy, 16);
        dl->AddCircle(ImVec2(x, y), r, kShadow, 16, 1.5f);
        if (t.count > 1) { char b[8]; snprintf(b, sizeof(b), "%d", t.count); TextCentred(dl, ImVec2(x, y - r - 8), kEnemy, b); }
        const float d = std::hypot(x - cx, y - cy);
        if (d < bestD) {
            bestD = d;
            tip = {t.label.empty() ? "Hostiles" : t.label, std::to_string(t.count) + " perso(s), " + ThreatKindName(t.kind)};
            if (s.centreOk) tip.push_back("Distance : " + Metres(Dist2D(t.pos, s.centre)));
        }
    }
    for (const auto& c : s.chars) {
        float x, y;
        if (!WorldToMapScreen(s, c.pos, x, y) || !InMapClip(s, x, y)) continue;
        ++g_frameMarkers;
        const float r = c.avatar ? 6.0f : 4.0f;
        dl->AddCircleFilled(ImVec2(x, y), r, Col(c.owner, c.dead ? 0.5f : 1.0f), 16);
        dl->AddCircle(ImVec2(x, y), r, kShadow, 16, 1.5f);
        if (c.dead || c.down) dl->AddLine(ImVec2(x - r, y - r), ImVec2(x + r, y + r), kShadow, 2.0f);
        const float d = std::hypot(x - cx, y - cy);
        if (d < bestD) {
            bestD = d;
            std::string who = "?";
            for (const auto& p : s.players) if (p.first == c.owner) who = p.second;
            tip = {c.name.empty() ? "?" : c.name, "Joueur : " + who + (c.avatar ? "" : " (recrue)")};
            if (c.dead) tip.push_back("Mort");
            else if (c.down) tip.push_back("Inconscient");
            if (s.centreOk) tip.push_back("Distance : " + Metres(Dist2D(c.pos, s.centre)));
        }
    }
    if (s.showPings) {
        for (const auto& p : s.pings) {
            float x, y;
            if (!WorldToMapScreen(s, p.pos, x, y) || !InMapClip(s, x, y)) continue;
            ++g_frameMarkers;
            const float a = PingAlpha(p.age);
            const float pulse = std::fmod(p.age, 1.0f);
            dl->AddCircle(ImVec2(x, y), 6.0f + 14.0f * pulse, Col(p.owner, a * (1.0f - pulse)), 24, 2.0f);
            dl->AddCircleFilled(ImVec2(x, y), 7.0f, Col(p.owner, a), 16);
            TextCentred(dl, ImVec2(x, y), WithAlpha(kShadow, a), PingLetter(p.kind));
            const float d = std::hypot(x - cx, y - cy);
            if (d < bestD) { bestD = d; tip = {std::string("Ping : ") + PingKindName(p.kind), "de " + p.who}; }
        }
    }
    dl->PopClipRect();
    // legend, in the map's top left corner
    ImGui::SetNextWindowPos(ImVec2(s.clipX0 + 10, s.clipY0 + 10), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.7f);
    ImGui::Begin("##kcmaplegend", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    ImGui::TextColored(ImVec4(1, 0.75f, 0.3f, 1), "Légende");
    ImDrawList* wl = ImGui::GetWindowDrawList();
    auto swatch = [&](ImU32 col) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float hh = ImGui::GetTextLineHeight();
        wl->AddCircleFilled(ImVec2(p.x + hh * 0.5f, p.y + hh * 0.5f), hh * 0.35f, col, 12);
        ImGui::Dummy(ImVec2(hh, hh));
        ImGui::SameLine();
    };
    for (const auto& [id, name] : s.players) {
        swatch(Col(id));
        ImGui::Text("%s%s", name.c_str(), id == s.me ? " (toi)" : "");
    }
    swatch(kEnemy);
    ImGui::TextUnformatted("Ennemis qui nous visent");
    ImGui::TextDisabled("Gros point : perso du joueur, petit : recrue");
    if (s.showPings) ImGui::TextDisabled("Ping : clic molette ou Alt+clic");
    ImGui::End();
    if (!tip.empty() && cx >= s.clipX0 && cx <= s.clipX1 && cy >= s.clipY0 && cy <= s.clipY1) Tooltip(cx, cy, tip);
}

// ---- above the heads, pings on the ground, squad bar frames
void DrawWorldMarkers(const MapScene& s, float w, float h) {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (s.showHeads && s.camOk && !s.covered) {
        for (const auto& c : s.chars) {
            if (!c.head) continue;
            float x, y;
            if (!WorldToScreen(s, {c.pos.x, c.pos.y + kHeadHeight, c.pos.z}, w, h, x, y)) continue;
            if (x < -50 || y < -50 || x > w + 50 || y > h + 50) continue;
            ++g_frameHeads;
            const ImU32 col = Col(c.owner, c.down ? 0.6f : 1.0f);
            // a small cursor pointing down at the head, the name above it
            dl->AddTriangleFilled(ImVec2(x - 6, y - 10), ImVec2(x + 6, y - 10), ImVec2(x, y), col);
            dl->AddTriangle(ImVec2(x - 6, y - 10), ImVec2(x + 6, y - 10), ImVec2(x, y), kShadow, 1.2f);
            std::string who = c.name;
            for (const auto& p : s.players) if (p.first == c.owner) who = p.second;
            TextCentred(dl, ImVec2(x, y - 20), col, who.c_str());
        }
    }
    if (s.showPings && s.camOk && !s.covered) {
        for (const auto& p : s.pings) {
            float x, y, x0, y0;
            const float a = PingAlpha(p.age);
            if (!WorldToScreen(s, {p.pos.x, p.pos.y + 30.0f, p.pos.z}, w, h, x, y)) continue;
            const bool ground = WorldToScreen(s, p.pos, w, h, x0, y0);
            if (x < -50 || y < -50 || x > w + 50 || y > h + 50) continue;
            const ImU32 col = Col(p.owner, a);
            if (ground) {
                dl->AddLine(ImVec2(x0, y0), ImVec2(x, y), col, 2.0f);
                const float pulse = std::fmod(p.age, 1.0f);
                dl->AddCircle(ImVec2(x0, y0), 4.0f + 12.0f * pulse, Col(p.owner, a * (1.0f - pulse)), 20, 2.0f);
            }
            dl->AddQuadFilled(ImVec2(x, y - 9), ImVec2(x + 9, y), ImVec2(x, y + 9), ImVec2(x - 9, y), col);
            TextCentred(dl, ImVec2(x, y), WithAlpha(kShadow, a), PingLetter(p.kind));
            std::string label = p.who + " : " + PingKindName(p.kind);
            if (s.centreOk) label += "  " + Metres(Dist2D(p.pos, s.centre));
            TextCentred(dl, ImVec2(x, y - 20), col, label.c_str());
        }
    }
    if (s.showPortraits) {
        for (const auto& r : s.portraits) {
            dl->AddRect(ImVec2(r.x - 1, r.y - 1), ImVec2(r.x + r.w + 1, r.y + r.h + 1), Col(r.owner), 3.0f, 0, 3.0f);
            dl->AddRectFilled(ImVec2(r.x, r.y + r.h - 5), ImVec2(r.x + r.w, r.y + r.h), Col(r.owner, 0.9f));
        }
    }
}

// ---- the minimap
bool ClipSegmentToCircle(ImVec2 c, float r, ImVec2& a, ImVec2& b) {
    const ImVec2 d(b.x - a.x, b.y - a.y);
    const ImVec2 f(a.x - c.x, a.y - c.y);
    const float A = d.x * d.x + d.y * d.y, B = 2 * (f.x * d.x + f.y * d.y), C = f.x * f.x + f.y * f.y - r * r;
    const float disc = B * B - 4 * A * C;
    if (A < 1e-6f || disc <= 0) return false;
    const float sq = std::sqrt(disc);
    const float t0 = std::max(0.0f, (-B - sq) / (2 * A)), t1 = std::min(1.0f, (-B + sq) / (2 * A));
    if (t1 <= t0) return false;
    const ImVec2 a2(a.x + d.x * t0, a.y + d.y * t0), b2(a.x + d.x * t1, a.y + d.y * t1);
    a = a2; b = b2;
    return true;
}

void DrawMinimap(const MapScene& s, float w, float h, ID3D11Device* dev) {
    const float R = std::clamp(h * 0.11f, 70.0f, 160.0f);
    const float margin = 18.0f;
    const int corner = std::clamp(s.minimapCorner, 0, 3);
    const float cx = (corner % 2 == 0) ? margin + R : w - margin - R;
    // top right: below KenshiCoop's status panel
    const float cy = corner >= 2 ? h - margin - R - 30.0f : margin + R + (corner == 1 ? 150.0f : 0.0f);
    MapScene sz = s;
    float zoom = s.minimapZoom;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_zoomPending > 0 && NowSeconds() - g_zoomPendingAt < 1.0) zoom = g_zoomPending;
    }
    sz.minimapZoom = std::clamp(zoom, kMinZoom, kMaxZoom);
    const MinimapFrame f = MakeMinimapFrame(sz, cx, cy, R);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 C(cx, cy);
    // background: the world map texture inside the circle, else a dark disc with a grid
    if (!g_texTried && dev) { g_texTried = true; LoadMapTexture(dev); }
    constexpr int kSeg = 64;
    if (g_mapSrv && s.boundsOk) {
        dl->PushTextureID(ImTextureID(reinterpret_cast<intptr_t>(g_mapSrv)));
        dl->PrimReserve(kSeg * 3, kSeg + 1);
        const ImDrawIdx base = ImDrawIdx(dl->_VtxCurrentIdx);
        auto uvOf = [&](float sx, float sy) {
            const kc::Vec3 wp = MinimapUnproject(f, s.centre, sx, sy);
            return ImVec2((wp.x - s.minX) / s.sizeX, (wp.z - s.minZ) / s.sizeZ);
        };
        dl->PrimWriteVtx(C, uvOf(cx, cy), IM_COL32(255, 255, 255, 235));
        for (int i = 0; i < kSeg; ++i) {
            const float a = 2 * kPi * float(i) / kSeg;
            const float px = cx + std::cos(a) * R, py = cy + std::sin(a) * R;
            dl->PrimWriteVtx(ImVec2(px, py), uvOf(px, py), IM_COL32(255, 255, 255, 235));
        }
        for (int i = 0; i < kSeg; ++i) {
            dl->PrimWriteIdx(base);
            dl->PrimWriteIdx(ImDrawIdx(base + 1 + i));
            dl->PrimWriteIdx(ImDrawIdx(base + 1 + (i + 1) % kSeg));
        }
        dl->PopTextureID();
    } else {
        dl->AddCircleFilled(C, R, IM_COL32(20, 22, 26, 220), kSeg);
        // a grid every round number of metres, about four lines across
        float step = 10.0f * kUnitsPerMetre;
        while (step * 4 < sz.minimapZoom * 2) step *= (std::fmod(std::log10(step), 1.0f) < 0.5f ? 5.0f : 2.0f);
        const float gx0 = std::floor((s.centre.x - sz.minimapZoom) / step) * step, gz0 = std::floor((s.centre.z - sz.minimapZoom) / step) * step;
        for (int i = 0; i <= 12; ++i) {
            for (int axis = 0; axis < 2; ++axis) {
                kc::Vec3 p0 = s.centre, p1 = s.centre;
                if (axis == 0) { p0.x = p1.x = gx0 + i * step; p0.z -= sz.minimapZoom * 2; p1.z += sz.minimapZoom * 2; }
                else { p0.z = p1.z = gz0 + i * step; p0.x -= sz.minimapZoom * 2; p1.x += sz.minimapZoom * 2; }
                ImVec2 a, b;
                MinimapProject(f, s.centre, p0, a.x, a.y);
                MinimapProject(f, s.centre, p1, b.x, b.y);
                if (ClipSegmentToCircle(C, R, a, b)) dl->AddLine(a, b, IM_COL32(90, 95, 105, 140), 1.0f);
            }
        }
    }
    dl->AddCircle(C, R, IM_COL32(10, 10, 10, 230), kSeg, 3.0f);
    dl->AddCircle(C, R - 2.0f, IM_COL32(200, 190, 160, 160), kSeg, 1.0f);
    const float mx = g_curX.load(), my = g_curY.load();
    float bestD = 10.0f;
    std::vector<std::string> tip;
    auto inside = [&](float x, float y) { return std::hypot(x - cx, y - cy) <= R - 4.0f; };
    for (const auto& t : s.threats) {
        float x, y;
        MinimapProject(f, s.centre, t.pos, x, y);
        if (!inside(x, y)) continue;
        dl->AddCircleFilled(ImVec2(x, y), 5.0f, kEnemy, 12);
        dl->AddCircle(ImVec2(x, y), 5.0f, kShadow, 12, 1.2f);
        if (const float d = std::hypot(x - mx, y - my); d < bestD) {
            bestD = d;
            tip = {t.label.empty() ? "Hostiles" : t.label, std::to_string(t.count) + " perso(s), " + ThreatKindName(t.kind), "Distance : " + Metres(Dist2D(t.pos, s.centre))};
        }
    }
    for (const auto& c : s.chars) {
        float x, y;
        MinimapProject(f, s.centre, c.pos, x, y);
        if (!inside(x, y)) continue;
        const float r = c.avatar ? 4.5f : 3.0f;
        dl->AddCircleFilled(ImVec2(x, y), r, Col(c.owner, c.dead ? 0.5f : 1.0f), 12);
        dl->AddCircle(ImVec2(x, y), r, kShadow, 12, 1.2f);
        if (const float d = std::hypot(x - mx, y - my); d < bestD) {
            bestD = d;
            std::string who = "?";
            for (const auto& p : s.players) if (p.first == c.owner) who = p.second;
            tip = {c.name.empty() ? "?" : c.name, "Joueur : " + who + (c.avatar ? "" : " (recrue)"), "Distance : " + Metres(Dist2D(c.pos, s.centre))};
        }
    }
    if (s.showPings) {
        for (const auto& p : s.pings) {
            float x, y;
            MinimapProject(f, s.centre, p.pos, x, y);
            const float a = PingAlpha(p.age);
            if (inside(x, y)) {
                const float pulse = std::fmod(p.age, 1.0f);
                dl->AddCircle(ImVec2(x, y), 4.0f + 10.0f * pulse, Col(p.owner, a * (1.0f - pulse)), 20, 2.0f);
                dl->AddCircleFilled(ImVec2(x, y), 5.0f, Col(p.owner, a), 12);
                if (const float d = std::hypot(x - mx, y - my); d < bestD) {
                    bestD = d;
                    tip = {std::string("Ping : ") + PingKindName(p.kind), "de " + p.who, "Distance : " + Metres(Dist2D(p.pos, s.centre))};
                }
            } else {
                // an arrow on the rim, pointing at it
                const float ang = std::atan2(y - cy, x - cx);
                const ImVec2 tipP(cx + std::cos(ang) * (R - 2), cy + std::sin(ang) * (R - 2));
                const ImVec2 l(cx + std::cos(ang - 0.12f) * (R - 14), cy + std::sin(ang - 0.12f) * (R - 14));
                const ImVec2 rr(cx + std::cos(ang + 0.12f) * (R - 14), cy + std::sin(ang + 0.12f) * (R - 14));
                dl->AddTriangleFilled(tipP, l, rr, Col(p.owner, a));
                dl->AddTriangle(tipP, l, rr, WithAlpha(kShadow, a), 1.0f);
            }
        }
    }
    // us, at the centre: an arrow along the camera's facing
    {
        float fx = 0, fy = -1;
        if (!s.minimapRotate) {
            const float len = std::hypot(s.camFwdX, s.camFwdZ);
            if (len > 1e-4f) { fx = s.camFwdX / len; fy = s.camFwdZ / len; }
        }
        const ImVec2 a(cx + fx * 9, cy + fy * 9), b(cx - fx * 6 - fy * 5, cy - fy * 6 + fx * 5), c(cx - fx * 6 + fy * 5, cy - fy * 6 - fx * 5);
        dl->AddTriangleFilled(a, b, c, IM_COL32(255, 255, 255, 240));
        dl->AddTriangle(a, b, c, kShadow, 1.2f);
    }
    // north
    {
        float nx, ny;
        MinimapProject(f, s.centre, {s.centre.x, s.centre.y, s.centre.z - sz.minimapZoom}, nx, ny);
        const float ang = std::atan2(ny - cy, nx - cx);
        const ImVec2 p(cx + std::cos(ang) * (R + 1), cy + std::sin(ang) * (R + 1));
        dl->AddCircleFilled(p, 9.0f, IM_COL32(20, 20, 20, 230), 16);
        TextCentred(dl, p, IM_COL32(240, 80, 70, 255), "N");
    }
    // scale: a round number of metres, at most 60 % of the radius
    {
        static const float nice[] = {5, 10, 20, 25, 50, 100, 200, 250, 500, 1000, 2000, 2500, 5000};
        float metres = nice[0];
        for (float n : nice) if (n * kUnitsPerMetre * f.scale <= R * 0.6f) metres = n;
        const float px = metres * kUnitsPerMetre * f.scale;
        const float y = cy + R + 12.0f, x0 = cx - R;
        dl->AddLine(ImVec2(x0, y), ImVec2(x0 + px, y), IM_COL32(240, 240, 240, 230), 2.0f);
        dl->AddLine(ImVec2(x0, y - 4), ImVec2(x0, y + 4), IM_COL32(240, 240, 240, 230), 2.0f);
        dl->AddLine(ImVec2(x0 + px, y - 4), ImVec2(x0 + px, y + 4), IM_COL32(240, 240, 240, 230), 2.0f);
        TextShadow(dl, ImVec2(x0 + px + 6, y - 8), IM_COL32(240, 240, 240, 230), Metres(metres * kUnitsPerMetre).c_str());
    }
    // + / - buttons
    const ImVec4 plus(cx + R - 22, cy + R + 2, cx + R, cy + R + 24), minus(cx + R - 48, cy + R + 2, cx + R - 26, cy + R + 24);
    for (const ImVec4* b : {&plus, &minus}) {
        const bool hot = mx >= b->x && mx <= b->z && my >= b->y && my <= b->w;
        dl->AddRectFilled(ImVec2(b->x, b->y), ImVec2(b->z, b->w), hot ? IM_COL32(70, 70, 80, 240) : IM_COL32(30, 30, 36, 220), 4.0f);
        dl->AddRect(ImVec2(b->x, b->y), ImVec2(b->z, b->w), IM_COL32(200, 190, 160, 200), 4.0f);
        TextCentred(dl, ImVec2((b->x + b->z) * 0.5f, (b->y + b->w) * 0.5f), IM_COL32(240, 240, 240, 255), b == &plus ? "+" : "-");
    }
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_mm = f;
        g_mmShown = true;
        g_plus = plus;
        g_minus = minus;
    }
    if (!tip.empty()) Tooltip(mx, my, tip);
}

// ---- input
kc::PingKind KindFromModifiers() {
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0, ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    if (shift && ctrl) return kc::PingKind::Help;
    if (shift) return kc::PingKind::Danger;
    if (ctrl) return kc::PingKind::Loot;
    return kc::PingKind::Go;
}

// Where a click at (x, y) points in the world: the minimap, else the map screen, else the ground.
bool PointedSpot(float x, float y, kc::Vec3& out) {
    std::lock_guard<std::mutex> lk(g_mx);
    const MapScene& s = g_inScene;
    if (!s.live) return false;
    if (g_mmShown && std::hypot(x - g_mm.cx, y - g_mm.cy) <= g_mm.radius) {
        out = MinimapUnproject(g_mm, s.centre, x, y);
        return true;
    }
    if (s.mapOpen && s.showMap && InMapClip(s, x, y)) {
        if (!MapScreenToWorld(s, x, y, out)) return false;
        out.y = s.centreOk ? s.centre.y : 0;
        return true;
    }
    if (!s.covered && s.camOk) return ScreenToGround(s, x, y, g_screenW, g_screenH, s.centreOk ? s.centre.y : 0, out);
    return false;
}

void PlacePing(float x, float y) {
    kc::Vec3 at;
    if (!PointedSpot(x, y, at)) return;
    OverlayAction a;
    a.kind = OverlayAction::Kind::Ping;
    a.index = int(KindFromModifiers());
    a.x = at.x; a.y = at.y; a.z = at.z;
    OverlayPushAction(std::move(a));
}

void Zoom(float factor) {
    std::lock_guard<std::mutex> lk(g_mx);
    const float cur = (g_zoomPending > 0 && NowSeconds() - g_zoomPendingAt < 1.0) ? g_zoomPending : g_inScene.minimapZoom;
    g_zoomPending = std::clamp(cur * factor, kMinZoom, kMaxZoom);
    g_zoomPendingAt = NowSeconds();
    OverlayAction a;
    a.kind = OverlayAction::Kind::SetOption;
    a.text = "minimap_zoom";
    a.value = g_zoomPending;
    OverlayPushAction(std::move(a));
}

bool OverMinimap(float x, float y) {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_mmShown) return false;
    auto in = [&](const ImVec4& b) { return x >= b.x && x <= b.z && y >= b.y && y <= b.w; };
    return std::hypot(x - g_mm.cx, y - g_mm.cy) <= g_mm.radius || in(g_plus) || in(g_minus);
}

} // namespace

void MapOverlayDraw(const MapScene& in, float w, float h, ID3D11Device* device) {
    // The scene's GUI rectangles are MyGUI view pixels read by the game thread: read them again now
    // when this frame runs on that thread (no lag behind a window being dragged), then convert them
    // to this frame's display pixels (the back buffer: io.DisplaySize). Nothing is kept from a
    // previous frame.
    MapScene s = in;
    const bool refreshed = MapRefreshGui(s);
    SceneToDisplay(s, w, h);
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_inScene = s;
        g_screenW = w;
        g_screenH = h;
        g_mmShown = false;
    }
    g_drawnAt = NowSeconds();
    g_pingsArmed = s.live && s.showPings;
    g_frameHeads = 0;
    g_frameMarkers = 0;
    bool minimap = false;
    if (s.live) {
        if (s.mapOpen && s.showMap) DrawMapScreen(s, w, h);
        DrawWorldMarkers(s, w, h);
        minimap = s.showMinimap && !s.covered && s.centreOk;
        if (minimap) DrawMinimap(s, w, h, device);
        g_overMinimap = OverMinimap(g_curX.load(), g_curY.load());
    } else {
        g_overMinimap = false;
    }
    MapDrawn d;
    d.at = NowSeconds();
    d.w = w; d.h = h;
    d.refreshed = refreshed;
    d.heads = g_frameHeads;
    d.minimap = minimap;
    d.mapOpen = s.live && s.mapOpen && s.showMap;
    d.mapMarkers = d.mapOpen ? g_frameMarkers : 0;
    d.imgX = s.imgX; d.imgY = s.imgY; d.imgW = s.imgW; d.imgH = s.imgH;
    if (s.live && s.showPortraits) d.frames = s.portraits;
    // the map screen's markers, in the log: how many, or why none (on change, 2 s apart at most)
    if (d.mapOpen) {
        char k[64];
        snprintf(k, sizeof(k), "%d/%d", d.mapMarkers, g_frameMarkersTotal);
        if (g_lastMarkersKey != k && NowSeconds() - g_lastMarkersAt >= 2.0) {
            g_lastMarkersKey = k;
            g_lastMarkersAt = NowSeconds();
            if (d.mapMarkers > 0)
                Log("map screen: drew %d of %d marker(s) (chars %zu, hostile squads %zu, pings %zu) in (%.0f,%.0f)-(%.0f,%.0f), display %.0fx%.0f",
                    d.mapMarkers, g_frameMarkersTotal, s.chars.size(), s.threats.size(), s.showPings ? s.pings.size() : size_t(0), double(s.clipX0),
                    double(s.clipY0), double(s.clipX1), double(s.clipY1), double(w), double(h));
            else
                Log("map screen: open but no marker drawn (%s)", g_frameMarkersTotal == 0 ? "nothing in the map feed"
                                                                                      : "every marker is outside the visible part of the map");
        }
    } else {
        g_lastMarkersKey.clear();
    }
    std::lock_guard<std::mutex> lk(g_mx);
    g_drawn = std::move(d);
}

MapDrawn MapOverlayLastDrawn() {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_drawn;
}

bool MapOverlayMessage(UINT msg, WPARAM wp, float x, float y) {
    if (msg != WM_MOUSEWHEEL && msg != WM_MOUSEHWHEEL) { g_curX = x; g_curY = y; }
    if (NowSeconds() - g_drawnAt.load() > 0.5) return false;   // the layer is not on screen
    const bool over = OverMinimap(x, y);
    if (msg == WM_MOUSEMOVE) { g_overMinimap = over; return false; }
    bool pings = false;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        pings = g_inScene.live && g_inScene.showPings;
    }
    switch (msg) {
    case WM_MOUSEWHEEL:
        if (!over) return false;
        Zoom(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 0.8f : 1.25f);
        return true;
    case WM_LBUTTONDOWN: {
        const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        if (alt && pings) { PlacePing(x, y); return true; }
        if (!over) return false;
        ImVec4 plus, minus;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            plus = g_plus; minus = g_minus;
        }
        if (x >= plus.x && x <= plus.z && y >= plus.y && y <= plus.w) Zoom(0.8f);
        else if (x >= minus.x && x <= minus.z && y >= minus.y && y <= minus.w) Zoom(1.25f);
        return true;
    }
    case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        return over;
    case WM_MBUTTONDOWN: {
        if (!pings) return false;
        bool onMap = false;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            onMap = g_inScene.mapOpen && g_inScene.showMap && InMapClip(g_inScene, x, y);
        }
        if (over || onMap) { PlacePing(x, y); return true; }
        // in the 3D view the middle button also turns the camera: a ping only for a short click
        g_midDownX = int(x); g_midDownY = int(y);
        g_midDownAt = NowSeconds();
        return false;
    }
    case WM_MBUTTONUP: {
        if (!pings || g_midDownX < 0) return over;
        const bool click = std::abs(int(x) - g_midDownX) <= 5 && std::abs(int(y) - g_midDownY) <= 5 && NowSeconds() - g_midDownAt < 0.35;
        g_midDownX = -1;
        if (click) PlacePing(x, y);
        return false;
    }
    default:
        return false;
    }
}

bool MapOverlayHoldsMouse() {
    if (NowSeconds() - g_drawnAt.load() > 0.5) return false;   // the overlay stopped drawing (failed, minimised)
    if (g_overMinimap.load()) return true;
    // Alt+click places a ping: the game must not see that click
    return g_pingsArmed.load() && (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
}

void MapOverlaySetCursorScale(float, float) {}

void MapOverlayRelease() {
    if (g_mapSrv) { g_mapSrv->Release(); g_mapSrv = nullptr; }
}

} // namespace kcp
