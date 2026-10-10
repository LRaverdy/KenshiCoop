// Map markers and pings.
//
// Markers: three times a second the host sends every player where every squad character is (with
// its owner, whether it is a player's own character, down or dead) and the hostile squads near the
// players (raid parties after the player faction, squads fighting a player character, squads the
// game marks as enemies close to one). Each player's world map, minimap, markers above the heads
// and squad bar frames are drawn from that, the host's included: one source, everyone sees the
// same. Characters far from a client (outside its streaming radius) are on its map too.
//
// Pings: a player marks a spot. A client asks the host, the host checks the rate (one every 0.5 s
// per player, 5 alive at most per player: the oldest goes) and shows it to everyone, itself
// included. A ping lives 10 s. It is only a marker: nothing changes in the game world.
#include <algorithm>
#include <cctype>

#include "kc/session.h"

namespace kc {

namespace {
constexpr double kMapMarkersInterval = 1.0 / 3.0;
constexpr float kThreatRadius = 2500.0f;   // hostile squads this close to a player character are shown

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

double Session::mapMarkersAge() const { return clock_() - mapMarkersAt_; }

double Session::pingAge(const LivePing& p) const { return clock_() - p.at; }

bool Session::netIdHandle(uint32_t netId, Handle& out) const {
    auto it = entities_.find(netId);
    if (it == entities_.end()) return false;
    out = it->second.handle;
    return true;
}

void Session::ResetMap() {
    mapMarkers_ = MapMarkersMsg{};
    mapMarkersAt_ = -1e9;
    nextMapMarkers_ = 0;
    pings_.clear();
    lastPingAt_.clear();
    nextPingId_ = 1;
}

void Session::HostMapMarkers(double now) {
    PrunePings(now);
    if (now < nextMapMarkers_) return;
    nextMapMarkers_ = now + kMapMarkersInterval;
    MapMarkersMsg m;
    m.players.push_back({localId_, cfg_.name});
    for (const auto& [id, p] : players_)
        if (m.players.size() < kMaxMapPlayers) m.players.push_back({id, p.name});
    auto nameOf = [&](uint8_t id) -> std::string {
        for (const auto& p : m.players) if (p.id == id) return p.name;
        return {};
    };
    // every squad character, in squad order
    world_.PlayerCharacters(scratchHandles_);
    std::map<uint8_t, size_t> avatars;   // player -> its characters marked as its own
    scratchCenters_.clear();
    for (const Handle& h : scratchHandles_) {
        auto b = byHandle_.find(h);
        if (b == byHandle_.end()) continue;
        const Entity& e = entities_[b->second];
        if (!e.squad || m.chars.size() >= kMaxMapChars) continue;
        EntityState st;
        if (!world_.Read(h, st)) continue;
        MapChar c;
        c.netId = e.netId;
        c.owner = e.owner ? e.owner : hostId_;
        c.name = world_.CharacterNameOf(h);
        c.pos = st.pos;
        if (st.flags & kFlagDead) c.flags |= kMapDead;
        else if (st.flags & kFlagDown) c.flags |= kMapDown;
        // a player's own character: the one made for them, or the one with their name
        bool own = !c.name.empty() && Lower(c.name) == Lower(nameOf(c.owner));
        if (auto s = sync_.find(c.owner); s != sync_.end() && s->second.own.valid() && s->second.own == h) own = true;
        if (own) { c.flags |= kMapAvatar; ++avatars[c.owner]; }
        if (!(c.flags & kMapDead)) scratchCenters_.push_back(c.pos);
        m.chars.push_back(std::move(c));
    }
    // a player with none of those: their first character stands for them
    for (const auto& p : m.players) {
        if (avatars[p.id]) continue;
        for (auto& c : m.chars)
            if (c.owner == p.id && !(c.flags & kMapDead)) { c.flags |= kMapAvatar; break; }
    }
    world_.ReadMapThreats(scratchCenters_, kThreatRadius, m.threats);
    if (m.threats.size() > kMaxMapThreats) m.threats.resize(kMaxMapThreats);
    mapMarkers_ = m;
    mapMarkersAt_ = now;
    Writer w;
    Encode(w, m);
    BroadcastReliable(w, true);
}

void Session::AddPing(const MapPingMsg& m) {
    // at most kPingsPerPlayer alive per player: the oldest goes
    size_t mine = 0;
    for (const auto& p : pings_) mine += p.ping.owner == m.owner ? 1 : 0;
    if (mine >= kPingsPerPlayer) {
        auto oldest = std::find_if(pings_.begin(), pings_.end(), [&](const LivePing& p) { return p.ping.owner == m.owner; });
        if (oldest != pings_.end()) pings_.erase(oldest);
    }
    pings_.push_back({m, clock_()});
}

void Session::PrunePings(double now) {
    pings_.erase(std::remove_if(pings_.begin(), pings_.end(), [&](const LivePing& p) { return now - p.at > kPingLife; }), pings_.end());
}

bool Session::PlaceMapPing(const Vec3& pos, PingKind kind) {
    const double now = clock_();
    if (!isHost() && state_ != SessionState::Connected) return false;
    const uint8_t me = isHost() ? localId_ : 0;
    if (auto it = lastPingAt_.find(me); it != lastPingAt_.end() && now - it->second < kPingInterval) return false;
    lastPingAt_[me] = now;
    MapPingMsg m;
    m.kind = kind;
    m.pos = pos;
    if (isHost()) {
        m.owner = localId_;
        m.id = nextPingId_++;
        AddPing(m);
        Writer w;
        Encode(w, m);
        BroadcastReliable(w, true);
        log_("ping by " + cfg_.name + " (kind " + std::to_string(int(kind)) + ")");
        return true;
    }
    Writer w;
    Encode(w, m);
    SendReliable(net_.serverPeer(), w);
    return true;
}

void Session::HostPingPacket(uint8_t from, Reader& r) {
    MapPingMsg m;
    if (!Decode(r, m)) return;
    const double now = clock_();
    if (auto it = lastPingAt_.find(from); it != lastPingAt_.end() && now - it->second < kPingInterval) return;   // too often
    lastPingAt_[from] = now;
    m.owner = from;
    m.id = nextPingId_++;
    AddPing(m);
    Writer w;
    Encode(w, m);
    BroadcastReliable(w, true);
    auto p = players_.find(from);
    log_("ping by " + (p != players_.end() ? p->second.name : std::string("?")) + " (kind " + std::to_string(int(m.kind)) + ")");
}

void Session::ClientMapPacket(Msg type, Reader& r) {
    if (state_ != SessionState::Connected) return;
    if (type == Msg::MapMarkers) {
        MapMarkersMsg m;
        if (!Decode(r, m)) return;
        mapMarkers_ = std::move(m);
        mapMarkersAt_ = clock_();
        return;
    }
    MapPingMsg m;
    if (!Decode(r, m) || m.id == 0) return;
    for (const auto& p : pings_) if (p.ping.id == m.id) return;
    AddPing(m);
}

} // namespace kc
