// Messages and pure logic of kc/actions.h (0.3.5, lot 5: slave shops, action progress, disguise,
// inventories asked again).
#include "kc/actions.h"

#include <algorithm>
#include <cmath>

namespace kc {

namespace {
uint32_t U32Var(Reader& r) {
    const uint64_t v = r.varint();
    if (v > 0xFFFFFFFFull) r.fail();
    return static_cast<uint32_t>(v);
}
bool End(Reader& r) { return r.ok() && r.atEnd(); }
std::string Cut(const std::string& s, size_t n) { return s.size() > n ? s.substr(0, n) : s; }
} // namespace

// ---- slave shops
void Encode(Writer& w, const SlaveShopMsg& m) {
    w.u8(uint8_t(Msg::SlaveShop));
    w.varint(m.offerId);
    w.u8(m.kind);
    w.varint(m.sellerNetId);
    w.varint(m.buyerNetId);
    w.i32(m.basePrice);
    const size_t n = std::min<size_t>(m.slaves.size(), kMaxShopSlaves);
    w.varint(n);
    for (size_t i = 0; i < n; ++i) {
        w.varint(m.slaves[i]);
        w.i32(i < m.prices.size() ? m.prices[i] : 0);
    }
    w.str(Cut(m.note, 200));
}
bool Decode(Reader& r, SlaveShopMsg& m) {
    m = SlaveShopMsg{};
    m.offerId = U32Var(r);
    m.kind = r.u8();
    m.sellerNetId = U32Var(r);
    m.buyerNetId = U32Var(r);
    m.basePrice = r.i32();
    const uint32_t n = r.count(kMaxShopSlaves, 5);
    m.slaves.resize(n);
    m.prices.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        m.slaves[i] = U32Var(r);
        m.prices[i] = r.i32();
        if (!m.slaves[i] || m.prices[i] < 0) return false;
    }
    m.note = r.str(200);
    return End(r) && m.offerId != 0 && m.buyerNetId != 0 && m.basePrice >= 0 && m.kind <= kSlaveSell;
}
void Encode(Writer& w, const SlaveTradeMsg& m) {
    w.u8(uint8_t(Msg::SlaveTrade));
    w.varint(m.offerId);
    w.u8(m.kind);
    w.varint(m.buyerNetId);
    const size_t n = std::min<size_t>(m.slaves.size(), kMaxShopSlaves);
    w.varint(n);
    for (size_t i = 0; i < n; ++i) w.varint(m.slaves[i]);
    w.i32(m.shownPrice);
}
bool Decode(Reader& r, SlaveTradeMsg& m) {
    m = SlaveTradeMsg{};
    m.offerId = U32Var(r);
    m.kind = r.u8();
    m.buyerNetId = U32Var(r);
    const uint32_t n = r.count(kMaxShopSlaves, 1);
    m.slaves.resize(n);
    for (auto& s : m.slaves) {
        s = U32Var(r);
        if (!s) return false;
    }
    m.shownPrice = r.i32();
    return End(r) && m.offerId != 0 && m.buyerNetId != 0 && m.kind <= kSlaveSell;
}

// ---- action progress
void Encode(Writer& w, const ActionProgressMsg& m) {
    w.u8(uint8_t(Msg::ActionProgress));
    const size_t n = std::min<size_t>(m.entries.size(), kMaxProgressEntries);
    w.varint(n);
    for (size_t i = 0; i < n; ++i) {
        w.varint(m.entries[i].netId);
        w.u16(m.entries[i].permille);
    }
}
bool Decode(Reader& r, ActionProgressMsg& m) {
    m = ActionProgressMsg{};
    const uint32_t n = r.count(kMaxProgressEntries, 3);
    m.entries.resize(n);
    for (auto& e : m.entries) {
        e.netId = U32Var(r);
        e.permille = r.u16();
        if (!e.netId || (e.permille > 1000 && e.permille != kProgressEnded)) return false;
    }
    return End(r);
}

// ---- disguise
void Encode(Writer& w, const DisguiseMsg& m) {
    w.u8(uint8_t(Msg::Disguise));
    const size_t n = std::min<size_t>(m.entries.size(), kMaxDisguiseEntries);
    w.varint(n);
    for (size_t i = 0; i < n; ++i) {
        const auto& e = m.entries[i];
        w.varint(e.netId);
        w.str(Cut(e.d.factionSid, 120));
        w.str(Cut(e.d.factionName, 80));
        w.f32(std::isfinite(e.d.level) ? e.d.level : 0.0f);
        w.f32(std::isfinite(e.d.blown) ? e.d.blown : 0.0f);
    }
}
bool Decode(Reader& r, DisguiseMsg& m) {
    m = DisguiseMsg{};
    const uint32_t n = r.count(kMaxDisguiseEntries, 11);
    m.entries.resize(n);
    for (auto& e : m.entries) {
        e.netId = U32Var(r);
        e.d.factionSid = r.str(120);
        e.d.factionName = r.str(80);
        e.d.level = r.f32();
        e.d.blown = r.f32();
        if (!e.netId || e.d.level < -1.0f || e.d.level > 10.0f || e.d.blown < 0.0f || e.d.blown > 100000.0f) return false;
    }
    return End(r);
}

// ---- inventories asked again
void Encode(Writer& w, const InvResendMsg& m) {
    w.u8(uint8_t(Msg::InvResend));
    const size_t n = std::min<size_t>(m.netIds.size(), kMaxInvResend);
    w.varint(n);
    for (size_t i = 0; i < n; ++i) w.varint(m.netIds[i]);
}
bool Decode(Reader& r, InvResendMsg& m) {
    m = InvResendMsg{};
    const uint32_t n = r.count(kMaxInvResend, 1);
    m.netIds.resize(n);
    for (auto& id : m.netIds) {
        id = U32Var(r);
        if (!id) return false;
    }
    return End(r) && n > 0;
}

namespace actions {

bool DisguiseChanged(const DisguiseInfo& before, const DisguiseInfo& now) {
    if (before.factionSid != now.factionSid || Disguised(before) != Disguised(now)) return true;
    if ((before.blown > 0) != (now.blown > 0)) return true;
    return std::fabs(before.level - now.level) >= 0.05f;
}

float ProgressToSend(ProgressTrack& t, double now) {
    const bool live = now - t.seenAt <= kProgressStale;
    if (!live) {
        if (t.sentValue < 0) return -1.0f;   // ended already (or never sent)
        t.sentValue = -1;
        t.sentAt = now;
        return kSendEnded;
    }
    if (now - t.sentAt < kProgressSendInterval) return -1.0f;
    const float v = std::clamp(t.value, 0.0f, 1.0f);
    if (t.sentValue >= 0 && ToPermille(v) == ToPermille(t.sentValue)) return -1.0f;
    t.sentValue = v;
    t.sentAt = now;
    return v;
}

bool EndsCurrentTask(const JobRef& removed, const std::vector<JobRef>& remaining, const JobRef& current) {
    if (removed.type < 0 || current.type != removed.type) return false;
    // a subject on both sides must be the same one (a job without subject matches any of its kind)
    if (removed.subject.valid() && current.subject.valid() && !(removed.subject == current.subject)) return false;
    for (const JobRef& j : remaining)
        if (j.type == removed.type && (!j.subject.valid() || !removed.subject.valid() || j.subject == removed.subject)) return false;
    return true;
}

} // namespace actions
} // namespace kc
