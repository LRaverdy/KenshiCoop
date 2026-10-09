#include "kc/protocol.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace kc {

const char* ToString(RejectReason r) {
    switch (r) {
    case RejectReason::BadProtocol: return "incompatible KenshiCoop version";
    case RejectReason::GameMismatch: return "different Kenshi build (kenshi_x64.exe)";
    case RejectReason::ModsMismatch: return "different active mod list";
    case RejectReason::WorldMismatch: return "different save loaded";
    case RejectReason::Full: return "server full";
    case RejectReason::BadName: return "invalid player name";
    case RejectReason::NotReady: return "host has not loaded a world yet";
    case RejectReason::HostSaveFailed: return "the host could not save its world, try again";
    case RejectReason::Timeout: return "joining took too long";
    case RejectReason::Kicked: return "removed by the host";
    }
    return "unknown";
}

namespace {

void PutVec(Writer& w, const Vec3& v) { w.f32(v.x); w.f32(v.y); w.f32(v.z); }
Vec3 GetVec(Reader& r) { Vec3 v; v.x = r.f32(); v.y = r.f32(); v.z = r.f32(); return v; }

void PutHandle(Writer& w, const Handle& h) {
    w.varint(h.type); w.varint(h.container); w.varint(h.containerSerial); w.varint(h.index); w.varint(h.serial);
}
Handle GetHandle(Reader& r) {
    Handle h;
    auto v32 = [&r]() { const uint64_t v = r.varint(); if (v > 0xFFFFFFFFull) r.fail(); return static_cast<uint32_t>(v); };
    h.type = v32(); h.container = v32(); h.containerSerial = v32(); h.index = v32(); h.serial = v32();
    return h;
}

uint32_t GetU32Var(Reader& r) {
    const uint64_t v = r.varint();
    if (v > 0xFFFFFFFFull) r.fail();
    return static_cast<uint32_t>(v);
}

bool Done(Reader& r) { return r.ok() && r.atEnd(); }

void PutEntity(Writer& w, const EntityState& e) {
    w.varint(e.netId);
    PutVec(w, e.pos);
    w.u32(PackQuat(e.rot));
    PutVec(w, e.dest);
    w.u8(e.flags);
    w.varint(e.combatTarget);
    w.u8(e.gait);
    w.u16(uint16_t(std::lround(std::clamp(e.pace, 0.0f, 6553.5f) * 10.0f)));
}
constexpr size_t kMinEntityBytes = 1 + 12 + 4 + 12 + 1 + 1 + 3;

} // namespace

void Encode(Writer& w, const Hello& m) {
    w.u8(uint8_t(Msg::Hello));
    w.u32(kMagic);
    w.u16(m.protocol);
    w.u64(m.gameBuild);
    w.u64(m.modsHash);
    w.u64(m.worldHash);
    w.str(m.name);
}
bool Decode(Reader& r, Hello& m) {
    if (r.u32() != kMagic) return false;
    m.protocol = r.u16();
    m.gameBuild = r.u64();
    m.modsHash = r.u64();
    m.worldHash = r.u64();
    m.name = r.str(kMaxNameLen);
    return Done(r);
}

void Encode(Writer& w, const Welcome& m) {
    w.u8(uint8_t(Msg::Welcome));
    w.u8(m.yourId);
    w.u64(m.worldHash);
    w.f64(m.hostTime);
    w.varint(m.players.size());
    for (const auto& p : m.players) { w.u8(p.id); w.str(p.name); }
}
bool Decode(Reader& r, Welcome& m) {
    m.yourId = r.u8();
    m.worldHash = r.u64();
    m.hostTime = r.f64();
    const uint32_t n = r.count(kMaxPlayers, 2);
    m.players.resize(n);
    for (auto& p : m.players) { p.id = r.u8(); p.name = r.str(kMaxNameLen); }
    return Done(r);
}

void Encode(Writer& w, const Reject& m) { w.u8(uint8_t(Msg::Reject)); w.u8(uint8_t(m.reason)); }
bool Decode(Reader& r, Reject& m) {
    const uint8_t v = r.u8();
    if (v < 1 || v > 10) return false;
    m.reason = RejectReason(v);
    return Done(r);
}

void Encode(Writer& w, const PlayerInfo& m) { w.u8(uint8_t(Msg::PlayerJoined)); w.u8(m.id); w.str(m.name); }
bool Decode(Reader& r, PlayerInfo& m) { m.id = r.u8(); m.name = r.str(kMaxNameLen); return Done(r); }

void Encode(Writer& w, const PlayerLeft& m) { w.u8(uint8_t(Msg::PlayerLeft)); w.u8(m.id); }
bool Decode(Reader& r, PlayerLeft& m) { m.id = r.u8(); return Done(r); }

void Encode(Writer& w, const Chat& m) { w.u8(uint8_t(Msg::Chat)); w.u8(m.from); w.str(m.text); }
bool Decode(Reader& r, Chat& m) { m.from = r.u8(); m.text = r.str(kMaxChatLen * 4); return Done(r); }

void Encode(Writer& w, const Bind& m) {
    w.u8(uint8_t(Msg::Bind));
    w.varint(m.netId);
    w.u8(uint8_t(m.kind));
    PutHandle(w, m.handle);
    w.u8(m.owner);
    w.boolean(m.squad);
    w.boolean(m.hasSpawn);
    if (m.hasSpawn) {
        w.str(m.spawn.templateSid);
        w.str(m.spawn.factionSid);
        w.str(m.spawn.name);
        w.f32(m.spawn.age);
    }
    PutHandle(w, m.previous);
}
bool Decode(Reader& r, Bind& m) {
    m.netId = GetU32Var(r);
    const uint8_t k = r.u8();
    if (k != uint8_t(EntityKind::Character)) return false;
    m.kind = EntityKind(k);
    m.handle = GetHandle(r);
    m.owner = r.u8();
    m.squad = r.boolean();
    m.hasSpawn = r.boolean();
    if (m.hasSpawn) {
        m.spawn.templateSid = r.str(kMaxSidLen);
        m.spawn.factionSid = r.str(kMaxSidLen);
        m.spawn.name = r.str(kMaxNameLen * 4);
        m.spawn.age = r.f32();
    }
    m.previous = GetHandle(r);
    return Done(r) && m.netId != 0;
}

void Encode(Writer& w, const Unbind& m) { w.u8(uint8_t(Msg::Unbind)); w.varint(m.netId); }
bool Decode(Reader& r, Unbind& m) { m.netId = GetU32Var(r); return Done(r) && m.netId != 0; }

std::vector<std::vector<uint8_t>> EncodeSnapshot(const Snapshot& s, size_t budget) {
    std::vector<std::vector<uint8_t>> out;
    size_t i = 0;
    do {
        Writer w(budget + 64);
        w.u8(uint8_t(Msg::Snapshot));
        w.u32(s.tick);
        w.f64(s.hostTime);
        // Count is a fixed u16 here so it can be patched after filling the packet.
        const size_t countAt = w.size();
        w.u16(0);
        uint16_t n = 0;
        Writer tmp(64);
        while (i < s.entities.size() && n < kMaxEntitiesPerMsg) {
            tmp.clear();
            PutEntity(tmp, s.entities[i]);
            if (n > 0 && w.size() + tmp.size() > budget) break;
            w.bytes(tmp.data(), tmp.size());
            ++n; ++i;
        }
        std::memcpy(w.vec().data() + countAt, &n, 2);
        out.push_back(std::move(w.vec()));
    } while (i < s.entities.size());
    return out;
}
bool Decode(Reader& r, Snapshot& m) {
    m.tick = r.u32();
    m.hostTime = r.f64();
    const uint16_t n = r.u16();
    if (n > kMaxEntitiesPerMsg || size_t(n) * kMinEntityBytes > r.remaining()) return false;
    m.entities.resize(n);
    for (auto& e : m.entities) {
        e.netId = GetU32Var(r);
        e.pos = GetVec(r);
        e.rot = UnpackQuat(r.u32());
        e.dest = GetVec(r);
        e.flags = r.u8();
        e.combatTarget = GetU32Var(r);
        e.gait = r.u8();
        e.pace = float(r.u16()) / 10.0f;
        if (!r.ok() || e.netId == 0) return false;
    }
    return Done(r);
}

void Encode(Writer& w, const Command& m) {
    w.u8(uint8_t(Msg::Command));
    w.varint(m.seq);
    w.varint(m.netId);
    w.u8(uint8_t(m.kind));
    PutVec(w, m.pos);
    w.boolean(m.run);
}
bool Decode(Reader& r, Command& m) {
    m.seq = GetU32Var(r);
    m.netId = GetU32Var(r);
    const uint8_t k = r.u8();
    if (k < 1 || k > 2) return false;
    m.kind = CommandKind(k);
    m.pos = GetVec(r);
    m.run = r.boolean();
    return Done(r) && m.netId != 0;
}

void Encode(Writer& w, const TimeState& m) {
    w.u8(uint8_t(Msg::TimeState));
    w.f32(m.speed);
    w.boolean(m.paused);
    w.f64(m.gameHours);
}
bool Decode(Reader& r, TimeState& m) {
    m.speed = r.f32();
    m.paused = r.boolean();
    m.gameHours = r.f64();
    return Done(r) && m.speed >= 0.0f && m.speed <= 16.0f && m.gameHours >= 0.0;
}

namespace {
void PutVitals(Writer& w, const EntityVitals& e) {
    w.varint(e.netId);
    w.f32(e.blood);
    w.f32(e.koTimer);
    w.u8(e.flags);
    w.u8(uint8_t(e.parts.size()));
    for (const auto& p : e.parts) { w.f32(p.flesh); w.f32(p.stun); w.f32(p.bandage); }
}
constexpr size_t kMinVitalsBytes = 1 + 4 + 4 + 1 + 1;
} // namespace

std::vector<std::vector<uint8_t>> EncodeVitals(const VitalsMsg& v, size_t budget) {
    std::vector<std::vector<uint8_t>> out;
    size_t i = 0;
    do {
        Writer w(budget + 64);
        w.u8(uint8_t(Msg::Vitals));
        w.u32(v.tick);
        const size_t countAt = w.size();
        w.u16(0);
        uint16_t n = 0;
        Writer tmp(256);
        while (i < v.entities.size() && n < kMaxEntitiesPerMsg) {
            tmp.clear();
            PutVitals(tmp, v.entities[i]);
            if (n > 0 && w.size() + tmp.size() > budget) break;
            w.bytes(tmp.data(), tmp.size());
            ++n; ++i;
        }
        std::memcpy(w.vec().data() + countAt, &n, 2);
        out.push_back(std::move(w.vec()));
    } while (i < v.entities.size());
    return out;
}

bool Decode(Reader& r, VitalsMsg& m) {
    m.tick = r.u32();
    const uint16_t n = r.u16();
    if (n > kMaxEntitiesPerMsg || size_t(n) * kMinVitalsBytes > r.remaining()) return false;
    m.entities.resize(n);
    for (auto& e : m.entities) {
        e.netId = GetU32Var(r);
        e.blood = r.f32();
        e.koTimer = r.f32();
        e.flags = r.u8();
        const uint8_t np = r.u8();
        if (np > kMaxBodyParts || size_t(np) * 12 > r.remaining()) return false;
        e.parts.resize(np);
        for (auto& p : e.parts) { p.flesh = r.f32(); p.stun = r.f32(); p.bandage = r.f32(); }
        if (!r.ok() || e.netId == 0) return false;
    }
    return Done(r);
}

void EncodePing(Writer& w, const Ping& m, bool pong) { w.u8(uint8_t(pong ? Msg::Pong : Msg::Ping)); w.f64(m.t); }
bool Decode(Reader& r, Ping& m) { m.t = r.f64(); return Done(r); }

std::optional<Msg> PeekType(Reader& r) {
    const uint8_t t = r.u8();
    if (!r.ok() || t < uint8_t(Msg::Hello) || t > uint8_t(Msg::Ground)) return std::nullopt;
    return Msg(t);
}

void Encode(Writer& w, const WorldBegin& m) { w.u8(uint8_t(Msg::WorldBegin)); w.u64(m.totalBytes); w.varint(m.fileCount); }
bool Decode(Reader& r, WorldBegin& m) {
    m.totalBytes = r.u64();
    m.fileCount = GetU32Var(r);
    return Done(r) && m.totalBytes <= kMaxWorldBytes && m.fileCount <= kMaxWorldFiles;
}

void Encode(Writer& w, const WorldChunk& m) {
    w.u8(uint8_t(Msg::WorldChunk));
    w.varint(m.file);
    w.varint(m.offset);
    if (m.offset == 0) { w.str(m.path); w.varint(m.fileSize); }
    w.varint(m.data.size());
    w.bytes(m.data.data(), m.data.size());
}
bool Decode(Reader& r, WorldChunk& m) {
    m.file = GetU32Var(r);
    m.offset = r.varint();
    if (m.offset == 0) {
        m.path = r.str(kMaxWorldPathLen);
        m.fileSize = r.varint();
        if (!r.ok() || !ValidWorldPath(m.path) || m.fileSize > kMaxWorldBytes) return false;
    }
    const uint64_t n = r.varint();
    if (!r.ok() || n > kWorldChunkSize || n > r.remaining()) return false;
    m.data.resize(size_t(n));
    r.bytes(m.data.data(), size_t(n));
    return Done(r) && m.file < kMaxWorldFiles && m.offset <= kMaxWorldBytes;
}

void Encode(Writer& w, const WorldEnd& m) { w.u8(uint8_t(Msg::WorldEnd)); w.u64(m.worldHash); }
bool Decode(Reader& r, WorldEnd& m) { m.worldHash = r.u64(); return Done(r); }

void Encode(Writer& w, const ReadyMsg& m) { w.u8(uint8_t(Msg::Ready)); w.u64(m.worldHash); }
bool Decode(Reader& r, ReadyMsg& m) { m.worldHash = r.u64(); return Done(r); }

void Encode(Writer& w, const WeatherMsg& m) {
    w.u8(uint8_t(Msg::Weather));
    w.varint(m.regions.size());
    for (const auto& r : m.regions) {
        w.str(r.regionSid); w.str(r.seasonSid); w.i32(r.seasonEnd); w.str(r.weatherSid);
        w.f32(r.effectStrength); w.f32(r.strength); w.f32(r.windSpeed); PutVec(w, r.windDir);
        w.boolean(r.windBuildUpEnded); w.i32(r.windBuildUpStart); w.i32(r.windBuildUpEnd);
        w.f32(r.windBuildUpSpeedStart); w.f32(r.windBuildUpSpeedEnd); w.f32(r.windBuildUpAngleStart); w.f32(r.windBuildUpAngleEnd);
        w.i32(r.startMinutes); w.i32(r.endMinutes); w.i32(r.updateWindMinutes); w.f32(r.time);
    }
}
bool Decode(Reader& r, WeatherMsg& m) {
    const uint32_t n = r.count(kMaxWeatherRegions, 8);
    m.regions.resize(n);
    for (auto& x : m.regions) {
        x.regionSid = r.str(kMaxSidLen); x.seasonSid = r.str(kMaxSidLen); x.seasonEnd = r.i32(); x.weatherSid = r.str(kMaxSidLen);
        x.effectStrength = r.f32(); x.strength = r.f32(); x.windSpeed = r.f32(); x.windDir = GetVec(r);
        x.windBuildUpEnded = r.boolean(); x.windBuildUpStart = r.i32(); x.windBuildUpEnd = r.i32();
        x.windBuildUpSpeedStart = r.f32(); x.windBuildUpSpeedEnd = r.f32(); x.windBuildUpAngleStart = r.f32(); x.windBuildUpAngleEnd = r.f32();
        x.startMinutes = r.i32(); x.endMinutes = r.i32(); x.updateWindMinutes = r.i32(); x.time = r.f32();
        if (!r.ok()) return false;
    }
    return Done(r);
}

namespace {
constexpr size_t kMaxSectionLen = 64;
void PutItem(Writer& w, const ItemState& i) {
    w.str(i.templateSid); w.str(i.materialSid); w.str(i.manufacturerSid); w.str(i.section);
    w.i32(i.quantity); w.u16(uint16_t(i.x)); w.u16(uint16_t(i.y)); w.boolean(i.equipped); w.i32(i.level);
    w.f32(i.quality); w.f32(i.charges);
}
bool GetItem(Reader& r, ItemState& i) {
    i.templateSid = r.str(kMaxSidLen); i.materialSid = r.str(kMaxSidLen); i.manufacturerSid = r.str(kMaxSidLen);
    i.section = r.str(kMaxSectionLen);
    i.quantity = r.i32(); i.x = int16_t(r.u16()); i.y = int16_t(r.u16()); i.equipped = r.boolean(); i.level = r.i32();
    i.quality = r.f32(); i.charges = r.f32();
    return r.ok() && !i.templateSid.empty() && i.quantity > 0 && i.quantity < 1000000;
}
} // namespace

void Encode(Writer& w, const GroundMsg& m) {
    w.u8(uint8_t(Msg::Ground));
    w.varint(m.events.size());
    for (const auto& e : m.events) {
        w.u8(uint8_t(e.kind));
        PutHandle(w, e.item);
        PutItem(w, e.state);
        PutVec(w, e.pos);
    }
}
bool Decode(Reader& r, GroundMsg& m) {
    const uint32_t n = r.count(kMaxGroundEvents, 2);   // a pickup is just a kind and a handle
    m.events.resize(n);
    for (auto& e : m.events) {
        const uint8_t k = r.u8();
        if (k != uint8_t(GroundKind::Dropped) && k != uint8_t(GroundKind::PickedUp)) return false;
        e.kind = GroundKind(k);
        e.item = GetHandle(r);
        if (!GetItem(r, e.state)) return false;
        e.pos = GetVec(r);
        if (!r.ok()) return false;
    }
    return Done(r);
}

void Encode(Writer& w, const InventoryMsg& m) {
    w.u8(uint8_t(Msg::Inventory));
    w.varint(m.netId);
    w.varint(m.items.size());
    for (const auto& i : m.items) PutItem(w, i);
}
bool Decode(Reader& r, InventoryMsg& m) {
    m.netId = GetU32Var(r);
    const uint32_t n = r.count(kMaxItemsPerInventory, 20);
    m.items.resize(n);
    for (auto& i : m.items) if (!GetItem(r, i)) return false;
    return Done(r) && m.netId != 0;
}

void Encode(Writer& w, const InvOp& m) {
    w.u8(uint8_t(Msg::InvOp));
    w.u8(uint8_t(m.kind));
    w.varint(m.fromNetId);
    w.varint(m.toNetId);
    PutItem(w, m.item);
    w.str(m.toSection);
    w.u16(uint16_t(m.toX));
    w.u16(uint16_t(m.toY));
}
bool Decode(Reader& r, InvOp& m) {
    const uint8_t k = r.u8();
    if (k < 1 || k > 2) return false;
    m.kind = InvOpKind(k);
    m.fromNetId = GetU32Var(r);
    m.toNetId = GetU32Var(r);
    if (!GetItem(r, m.item)) return false;
    m.toSection = r.str(kMaxSectionLen);
    m.toX = int16_t(r.u16());
    m.toY = int16_t(r.u16());
    return Done(r) && m.fromNetId != 0;
}

bool ValidWorldPath(const std::string& p) {
    if (p.empty() || p.size() > kMaxWorldPathLen || p.front() == '/' || p.back() == '/') return false;
    size_t segStart = 0;
    for (size_t i = 0; i <= p.size(); ++i) {
        if (i < p.size()) {
            const unsigned char c = static_cast<unsigned char>(p[i]);
            // no control chars, no Windows-reserved characters, no backslash or drive/stream colon
            if (c < 0x20 || c == 0x7F || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
                c == '>' || c == '|')
                return false;
            if (c != '/') continue;
        }
        const std::string seg = p.substr(segStart, i - segStart);
        if (seg.empty() || seg == "." || seg == ".." || seg.back() == '.' || seg.back() == ' ' || seg.front() == ' ') return false;
        // Windows device names (CON, NUL, COM1, ...) are refused whatever their extension
        std::string base = seg.substr(0, seg.find('.'));
        for (char& ch : base) ch = char(std::toupper(static_cast<unsigned char>(ch)));
        static const char* kDevices[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
                                         "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
        for (const char* d : kDevices) if (base == d) return false;
        segStart = i + 1;
    }
    return true;
}

bool ValidName(const std::string& s) {
    if (s.empty() || s.size() > kMaxNameLen || s.front() == ' ' || s.back() == ' ') return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return c >= 0x20 && c < 0x7F; });
}

std::string SanitizeChat(const std::string& s) {
    std::string out;
    out.reserve(std::min(s.size(), kMaxChatLen));
    for (unsigned char c : s) {
        if (out.size() >= kMaxChatLen) break;
        if (c >= 0x20 && c != 0x7F) out.push_back(char(c));
    }
    return out;
}

uint32_t PackQuat(const Quat& qin) {
    float q[4] = {qin.w, qin.x, qin.y, qin.z};
    const float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(len > 1e-6f) || !std::isfinite(len)) { q[0] = 1; q[1] = q[2] = q[3] = 0; }
    else for (float& c : q) c /= len;
    int largest = 0;
    for (int i = 1; i < 4; ++i) if (std::fabs(q[i]) > std::fabs(q[largest])) largest = i;
    const float sign = q[largest] < 0 ? -1.0f : 1.0f;
    constexpr float kRange = 0.70710678f;  // 1/sqrt(2): bound of the three smaller components
    uint32_t packed = uint32_t(largest) << 30;
    int shift = 20;
    for (int i = 0; i < 4; ++i) {
        if (i == largest) continue;
        const float v = std::clamp(q[i] * sign, -kRange, kRange);
        const uint32_t qv = uint32_t(std::lround((v + kRange) / (2 * kRange) * 1023.0f));
        packed |= (qv & 0x3FF) << shift;
        shift -= 10;
    }
    return packed;
}

Quat UnpackQuat(uint32_t v) {
    constexpr float kRange = 0.70710678f;
    const int largest = int(v >> 30);
    float q[4];
    float sum = 0;
    int shift = 20;
    for (int i = 0; i < 4; ++i) {
        if (i == largest) continue;
        q[i] = float((v >> shift) & 0x3FF) / 1023.0f * (2 * kRange) - kRange;
        sum += q[i] * q[i];
        shift -= 10;
    }
    q[largest] = std::sqrt(std::max(0.0f, 1.0f - sum));
    return Quat{q[0], q[1], q[2], q[3]};
}

uint64_t Fnv1a64(const void* data, size_t n, uint64_t h) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

void Encode(Writer& w, const EffectsMsg& m) {
    w.u8(uint8_t(Msg::Effects));
    w.boolean(m.full);
    w.varint(m.spawned.size());
    for (const auto& e : m.spawned) {
        w.u32(e.id); w.u8(uint8_t(e.kind)); w.str(e.regionSid); w.str(e.effectSid); w.u8(e.ordinal);
        PutVec(w, e.pos); w.f32(e.age); w.f32(e.life); w.boolean(e.endless); w.f32(e.strength);
        if (e.kind == EffectKind::Point) {
            w.boolean(e.struck); w.f32(e.strikeIn);
        } else {
            PutVec(w, e.dir); PutVec(w, e.turnTo);
        }
    }
    w.varint(m.moved.size());
    for (const auto& s : m.moved) { w.u32(s.id); PutVec(w, s.pos); PutVec(w, s.dir); PutVec(w, s.turnTo); }
    w.varint(m.ended.size());
    for (uint32_t id : m.ended) w.u32(id);
}
bool Decode(Reader& r, EffectsMsg& m) {
    m.full = r.boolean();
    const uint32_t n = r.count(kMaxEffectsPerMsg, 30);
    m.spawned.resize(n);
    for (auto& e : m.spawned) {
        e.id = r.u32();
        const uint8_t kind = r.u8();
        if (kind != uint8_t(EffectKind::Point) && kind != uint8_t(EffectKind::Wandering)) return false;
        e.kind = EffectKind(kind);
        e.regionSid = r.str(kMaxSidLen); e.effectSid = r.str(kMaxSidLen); e.ordinal = r.u8();
        e.pos = GetVec(r); e.age = r.f32(); e.life = r.f32(); e.endless = r.boolean(); e.strength = r.f32();
        if (e.kind == EffectKind::Point) {
            e.struck = r.boolean(); e.strikeIn = r.f32();
        } else {
            e.dir = GetVec(r); e.turnTo = GetVec(r);
        }
        if (!r.ok()) return false;
    }
    const uint32_t nm = r.count(kMaxEffectsPerMsg, 40);
    m.moved.resize(nm);
    for (auto& s : m.moved) { s.id = r.u32(); s.pos = GetVec(r); s.dir = GetVec(r); s.turnTo = GetVec(r); }
    const uint32_t ne = r.count(kMaxEffectsPerMsg, 4);
    m.ended.resize(ne);
    for (auto& id : m.ended) id = r.u32();
    return Done(r);
}

void Encode(Writer& w, const AnimMsg& m) {
    w.u8(uint8_t(Msg::Anim));
    w.varint(m.events.size());
    for (const auto& e : m.events) {
        w.varint(e.netId); w.u8(uint8_t(e.kind)); w.str(e.name); w.f32(e.a); w.f32(e.b); w.u8(e.flags);
    }
}
bool Decode(Reader& r, AnimMsg& m) {
    const uint32_t n = r.count(kMaxAnimEvents, 12);
    m.events.resize(n);
    for (auto& e : m.events) {
        e.netId = GetU32Var(r);
        const uint8_t k = r.u8();
        if (k < uint8_t(AnimKind::Combat) || k > uint8_t(AnimKind::FloaterColor)) return false;
        e.kind = AnimKind(k);
        e.name = r.str(kMaxAnimNameLen); e.a = r.f32(); e.b = r.f32(); e.flags = r.u8();
        if (!r.ok() || e.netId == 0) return false;
    }
    return Done(r);
}

// Each packet is self-contained: a string table (animation and data names) then the characters.
std::vector<std::vector<uint8_t>> EncodeAnimFrames(const AnimFrameMsg& m, size_t budget) {
    std::vector<std::vector<uint8_t>> out;
    size_t i = 0;
    while (i < m.chars.size()) {
        std::vector<std::string> table;
        auto index = [&](const std::string& s) {
            for (size_t k = 0; k < table.size(); ++k)
                if (table[k] == s) return uint32_t(k);
            table.push_back(s);
            return uint32_t(table.size() - 1);
        };
        Writer body(budget + 256);
        uint32_t n = 0;
        size_t tableBytes = 0;
        while (i < m.chars.size()) {
            const AnimFrame& f = m.chars[i];
            const size_t tableBefore = table.size();
            Writer one(256);
            one.varint(f.netId);
            one.f32(f.masterTime);
            one.f32(f.masterSpeed);
            const size_t na = std::min<size_t>(f.anims.size(), kMaxAnimsPerChar);
            one.u8(uint8_t(na));
            for (size_t a = 0; a < na; ++a) {
                const AnimEntry& e = f.anims[a];
                one.u8(uint8_t((e.layer & 0x3F) | (e.looped ? 0x40 : 0) | (e.fadingOut ? 0x80 : 0)));
                one.varint(index(e.anim));
                one.varint(index(e.data));
                one.f32(e.time); one.f32(e.speed);
                one.u8(uint8_t(std::lround(std::clamp(e.weight, 0.0f, 1.0f) * 255)));
                one.u8(uint8_t(std::lround(std::clamp(e.desired, 0.0f, 1.0f) * 255)));
            }
            size_t added = 0;
            for (size_t k = tableBefore; k < table.size(); ++k) added += table[k].size() + 2;
            if (n > 0 && 1 + 8 + 4 + tableBytes + added + body.size() + one.size() > budget) {
                table.resize(tableBefore);
                break;
            }
            tableBytes += added;
            body.bytes(one.data(), one.size());
            ++n;
            ++i;
        }
        Writer w(budget + 512);
        w.u8(uint8_t(Msg::AnimFrame));
        w.f64(m.hostTime);
        w.varint(table.size());
        for (const auto& s : table) w.str(s);
        w.varint(n);
        w.bytes(body.data(), body.size());
        out.push_back(std::move(w.vec()));
    }
    return out;
}
bool Decode(Reader& r, AnimFrameMsg& m) {
    m.hostTime = r.f64();
    const uint32_t nt = r.count(4096, 1);
    std::vector<std::string> table(nt);
    for (auto& s : table) s = r.str(kMaxAnimNameLen);
    const uint32_t n = r.count(kMaxEntitiesPerMsg, 2);
    m.chars.resize(n);
    for (auto& f : m.chars) {
        f.netId = GetU32Var(r);
        f.masterTime = r.f32();
        f.masterSpeed = r.f32();
        const uint8_t na = r.u8();
        if (na > kMaxAnimsPerChar) return false;
        f.anims.resize(na);
        for (auto& e : f.anims) {
            const uint8_t fl = r.u8();
            e.layer = fl & 0x3F; e.looped = (fl & 0x40) != 0; e.fadingOut = (fl & 0x80) != 0;
            const uint32_t ia = GetU32Var(r), id = GetU32Var(r);
            if (ia >= table.size() || id >= table.size()) return false;
            e.anim = table[ia]; e.data = table[id];
            e.time = r.f32(); e.speed = r.f32();
            e.weight = r.u8() / 255.0f; e.desired = r.u8() / 255.0f;
        }
        if (!r.ok() || f.netId == 0) return false;
    }
    return Done(r);
}

} // namespace kc
