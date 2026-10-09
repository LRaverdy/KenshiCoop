// KenshiCoop tests: protocol robustness + full host/client sessions over real loopback ENet,
// using a fake world. Exit code 0 = all passed.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "kc/protocol.h"
#include "kc/session.h"

using namespace kc;

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) { ++g_failed; std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

static double Now() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return duration<double>(steady_clock::now() - t0).count();
}

static float Dist(const Vec3& a, const Vec3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

// ---------------------------------------------------------------- fake world
struct FakeChar {
    Vec3 pos, dest;
    Quat rot;
    bool squad = true;
    EntityVitals vit;   // netId unused
    uint32_t fights = 0;   // serial of the character it fights
    std::vector<ItemState> items;
};

struct FakeWorld : IWorld {
    bool ready = true;
    uint32_t gen = 1;
    uint64_t build = 111, mods = 222, fp = 333;
    std::map<uint32_t, FakeChar> chars;  // key = handle.serial
    std::vector<std::pair<Handle, Command>> localOrders;
    TimeState time;
    bool client = false, active = false, holding = false;
    std::vector<Handle> controllable;
    float speed = 50.0f;  // units per second
    // world transfer simulation
    int exportFrames = 3, exportCountdown = -1;
    int importFrames = 5, importCountdown = 0;
    std::vector<WorldFile> pendingImport;
    bool corruptImport = false;
    int imports = 0;
    int spawns = 0, despawns = 0;

    static Handle H(uint32_t serial) { Handle h; h.type = 3; h.index = serial; h.serial = serial; return h; }

    // A save: a header file with the characters, plus a large blob to exercise chunking.
    std::vector<WorldFile> Serialize() const {
        Writer w;
        w.u64(fp);
        w.varint(chars.size());
        for (auto& [s, c] : chars) {
            w.u32(s); w.boolean(c.squad);
            w.f32(c.pos.x); w.f32(c.pos.y); w.f32(c.pos.z);
            w.f32(c.dest.x); w.f32(c.dest.y); w.f32(c.dest.z);
            w.f32(c.vit.blood); w.u8(c.vit.flags);
        }
        WorldFile header{"quick.save", w.vec()};
        WorldFile blob{"zone/zone.37.34.zone", std::vector<uint8_t>(70000)};
        for (size_t i = 0; i < blob.data.size(); ++i) blob.data[i] = uint8_t(i * 31 + 7);
        WorldFile accent{"platoon/Ma\xc3\xaetres des pantins_0.platoon", {1, 2, 3}};
        return {header, blob, accent};
    }
    bool Deserialize(const std::vector<WorldFile>& files) {
        const WorldFile* header = nullptr;
        for (auto& f : files) if (f.path == "quick.save") header = &f;
        if (!header) return false;
        for (auto& f : files) if (f.path == "zone/zone.37.34.zone")
            for (size_t i = 0; i < f.data.size(); ++i) if (f.data[i] != uint8_t(i * 31 + 7)) return false;
        Reader r(header->data.data(), header->data.size());
        fp = r.u64();
        const uint32_t n = r.count(100000);
        chars.clear();
        for (uint32_t i = 0; i < n; ++i) {
            FakeChar c;
            const uint32_t s = r.u32(); c.squad = r.boolean();
            c.pos.x = r.f32(); c.pos.y = r.f32(); c.pos.z = r.f32();
            c.dest.x = r.f32(); c.dest.y = r.f32(); c.dest.z = r.f32();
            c.vit.blood = r.f32(); c.vit.flags = r.u8();
            chars[s] = c;
        }
        if (corruptImport) fp ^= 1;
        return r.ok();
    }

    bool Ready() override { return ready; }
    uint32_t WorldGeneration() override { return gen; }
    uint64_t Fingerprint() override { return fp; }
    uint64_t GameBuild() override { return build; }
    uint64_t ModsHash() override { return mods; }
    Handle Hc(uint32_t s) const {   // the handle as the game currently gives it
        Handle h = H(s);
        if (auto it = container.find(s); it != container.end()) h.container = it->second;
        return h;
    }
    void PlayerCharacters(std::vector<Handle>& out) override { for (auto& [s, c] : chars) if (c.squad) out.push_back(Hc(s)); }
    void NearbyCharacters(const std::vector<Vec3>& centers, float radius, std::vector<Handle>& out) override {
        for (auto& [s, c] : chars) {
            if (c.squad) continue;
            if (radius <= 0) { out.push_back(Hc(s)); continue; }   // 0 = every active character
            for (auto& ctr : centers) if (Dist(c.pos, ctr) <= radius) { out.push_back(Hc(s)); break; }
        }
    }
    bool Exists(const Handle& h) override { return chars.count(h.serial) != 0; }
    bool Read(const Handle& h, EntityState& out) override {
        auto it = chars.find(h.serial);
        if (it == chars.end()) return false;
        out.pos = it->second.pos; out.rot = it->second.rot; out.dest = it->second.dest; out.flags = 0;
        if (Dist(it->second.pos, it->second.dest) > 1e-3f) out.flags |= kFlagMoving;
        return true;
    }
    bool ReadVitals(const Handle& h, EntityVitals& out) override {
        auto it = chars.find(h.serial);
        if (it == chars.end()) return false;
        out = it->second.vit;
        return true;
    }
    void Apply(const Handle& h, const EntityState& target, const EntityState& latest) override {
        auto it = chars.find(h.serial);
        if (it == chars.end()) return;
        it->second.pos = target.pos; it->second.rot = target.rot; it->second.dest = latest.dest;
    }
    bool ReadCombat(const Handle& h, Handle& target) override {
        auto it = chars.find(h.serial);
        if (it == chars.end() || !it->second.fights) return false;
        target = H(it->second.fights);
        return true;
    }
    void ApplyCombat(const Handle& h, bool fight, const Handle& target) override {
        auto it = chars.find(h.serial);
        if (it != chars.end()) it->second.fights = fight ? target.serial : 0;
    }
    uint64_t Identity(const Handle& h) override { return chars.count(h.serial) ? 0x1000 + h.serial : 0; }
    std::vector<std::pair<Handle, Handle>> rehandles;
    std::map<uint32_t, uint32_t> container;   // serial -> current container (part of the handle)
    void Rehandle(const Handle& from, const Handle& to) override { rehandles.emplace_back(from, to); }
    bool ReadSpawnInfo(const Handle& h, SpawnInfo& out) override {
        auto it = chars.find(h.serial);
        if (it == chars.end()) return false;
        out.templateSid = "tmpl-" + std::to_string(h.serial);
        out.factionSid = "bandits";
        out.name = "Npc";
        out.age = 30;
        return true;
    }
    bool Spawn(const Handle& h, const SpawnInfo& info, const EntityState& at) override {
        if (chars.count(h.serial) || info.templateSid != "tmpl-" + std::to_string(h.serial)) return false;
        FakeChar c;
        c.squad = false;
        c.pos = at.pos;
        c.dest = at.pos;
        chars[h.serial] = c;
        ++spawns;
        return true;
    }
    void Despawn(const Handle& h) override {
        if (chars.erase(h.serial)) ++despawns;
    }
    std::map<std::string, uint32_t> named;   // player characters by name
    bool EnsurePlayerCharacter(const std::string& name, uint64_t steamId, Handle& out, bool& created) override {
        (void)steamId;
        auto it = named.find(name);
        created = it == named.end();
        if (it == named.end()) {
            const uint32_t s = 5000 + uint32_t(named.size());
            FakeChar c;
            c.squad = true;
            c.pos = {10.0f * float(named.size()), 0, -20};
            c.dest = c.pos;
            chars[s] = c;
            it = named.emplace(name, s).first;
            fp ^= s * 0x9E3779B97F4A7C15ull;   // the world changed: so does its fingerprint
        }
        out = H(it->second);
        return true;
    }
    // client: characters the local game created by itself (key = local serial)
    std::map<uint32_t, double> strangerSince;
    std::map<uint32_t, std::string> templateOf;   // strangers' template, when not "tmpl-<serial>"
    int adoptions = 0, culled = 0;
    void Reconcile(const std::vector<Handle>& known, const std::vector<MissingChar>& missing, double now,
                   std::vector<Handle>& adopted) override {
        std::set<uint32_t> k;
        for (auto& h : known) k.insert(h.serial);
        std::vector<uint32_t> strangers;
        for (auto& [s, c] : chars) if (!c.squad && !k.count(s)) strangers.push_back(s);
        for (auto it = strangerSince.begin(); it != strangerSince.end();)
            it = std::find(strangers.begin(), strangers.end(), it->first) == strangers.end() ? strangerSince.erase(it) : std::next(it);
        for (uint32_t s : strangers) strangerSince.emplace(s, now);
        for (const auto& m : missing) {
            for (uint32_t s : strangers) {
                if (!chars.count(s) || chars.count(m.handle.serial)) continue;
                auto t = templateOf.find(s);
                if (t == templateOf.end() || t->second != m.spawn.templateSid) continue;
                // the fake world has no handle indirection: the stranger takes the host's serial
                chars[m.handle.serial] = chars[s];
                chars.erase(s);
                strangerSince.erase(s);
                adopted.push_back(m.handle);
                ++adoptions;
                break;
            }
        }
        for (auto it = strangerSince.begin(); it != strangerSince.end();) {
            if (chars.count(it->first) && now - it->second >= 5.0) { chars.erase(it->first); ++culled; it = strangerSince.erase(it); }
            else ++it;
        }
    }
    void ApplyVitals(const Handle& h, const EntityVitals& v) override {
        auto it = chars.find(h.serial);
        if (it != chars.end()) { it->second.vit = v; it->second.vit.netId = 0; }
    }
    bool Order(const Handle& h, const Command& c) override {
        auto it = chars.find(h.serial);
        if (it == chars.end()) return false;
        if (c.kind == CommandKind::MoveTo) it->second.dest = c.pos;
        else it->second.dest = it->second.pos;
        return true;
    }
    void TakeLocalOrders(std::vector<std::pair<Handle, Command>>& out) override { out.swap(localOrders); localOrders.clear(); }
    bool ReadInventory(const Handle& h, std::vector<ItemState>& out) override {
        auto it = chars.find(h.serial);
        if (it == chars.end()) return false;
        out = it->second.items;
        return true;
    }
    bool ApplyInventory(const Handle& h, const std::vector<ItemState>& items) override {
        auto it = chars.find(h.serial);
        if (it == chars.end()) return false;
        it->second.items = items;
        return true;
    }
    bool ExecuteInvOp(const Handle& from, const Handle& to, const InvOp& op) override {
        auto a = chars.find(from.serial), b = chars.find(to.serial);
        if (a == chars.end() || b == chars.end()) return false;
        for (size_t i = 0; i < a->second.items.size(); ++i) {
            ItemState& it = a->second.items[i];
            if (!it.sameKind(op.item) || it.quantity < op.item.quantity) continue;
            ItemState moved = it;
            moved.quantity = op.item.quantity;
            moved.section = op.toSection; moved.x = op.toX; moved.y = op.toY;
            it.quantity -= op.item.quantity;
            if (it.quantity == 0) a->second.items.erase(a->second.items.begin() + ptrdiff_t(i));
            if (op.kind == InvOpKind::Move) b->second.items.push_back(moved);
            return true;
        }
        return false;
    }
    std::vector<RegionWeather> weather;
    void ReadWeather(std::vector<RegionWeather>& out) override { out = weather; }
    void ApplyWeather(const std::vector<RegionWeather>& r) override { weather = r; }
    EffectsMsg fxPending;                  // host: what the game placed/moved/removed since the last read
    std::vector<WeatherEffect> fxLive;     // host: every live effect
    std::vector<EffectsMsg> fxApplied;     // client: what arrived from the host
    void ReadEffects(EffectsMsg& out, bool full) override {
        out = std::move(fxPending);
        fxPending = EffectsMsg{};
        if (full) { out.full = true; out.spawned = fxLive; }
    }
    void ApplyEffects(const EffectsMsg& m) override { fxApplied.push_back(m); }
    std::vector<std::pair<Handle, AnimEvent>> animPending;   // host: what its characters started
    std::vector<std::pair<uint32_t, AnimEvent>> animApplied; // client: (serial of our character, event)
    void TakeAnimEvents(std::vector<std::pair<Handle, AnimEvent>>& out, bool) override { out.swap(animPending); animPending.clear(); }
    void ApplyAnim(const Handle& h, const AnimEvent& e) override { animApplied.emplace_back(h.serial, e); }
    TimeState GetTime() override { return time; }
    void SetTime(const TimeState& t) override { time = t; }
    void HoldForJoin(bool h) override { holding = h; }
    bool BeginWorldExport(std::string*) override { exportCountdown = exportFrames; return true; }
    ExportStatus PollWorldExport(std::vector<WorldFile>& files, std::string*) override {
        if (exportCountdown < 0) return ExportStatus::Failed;
        if (exportCountdown-- > 0) return ExportStatus::Pending;
        files = Serialize();
        return ExportStatus::Done;
    }
    bool BeginWorldImport(const std::vector<WorldFile>& files, std::string*) override {
        for (auto& f : files) if (!ValidWorldPath(f.path)) return false;
        pendingImport = files;
        importCountdown = importFrames;
        ready = false;   // loading screen
        ++imports;
        return true;
    }
    void SetRole(bool c, bool a) override { client = c; active = a; }
    void SetControllable(const std::vector<Handle>& h) override { controllable = h; }

    void Simulate(float dt) {
        if (importCountdown > 0 && --importCountdown == 0) {
            Deserialize(pendingImport);
            ready = true;
            ++gen;
        }
        if (client || holding || !ready) return;  // clients never simulate; a held host stands still
        for (auto& [s, c] : chars) {
            const float d = Dist(c.pos, c.dest);
            const float step = speed * dt;
            if (d <= step || d < 1e-4f) c.pos = c.dest;
            else {
                c.pos.x += (c.dest.x - c.pos.x) / d * step;
                c.pos.y += (c.dest.y - c.pos.y) / d * step;
                c.pos.z += (c.dest.z - c.pos.z) / d * step;
            }
        }
    }
};

// Runs sessions for `seconds` of real time at ~100 fps.
static void Run(std::vector<std::pair<Session*, FakeWorld*>> s, double seconds, std::function<bool()> until = nullptr) {
    const double end = Now() + seconds;
    double last = Now();
    while (Now() < end) {
        const double n = Now();
        for (auto& [sess, w] : s) { w->Simulate(float(n - last)); sess->Tick(w->ready); }
        last = n;
        if (until && until()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

static uint16_t g_port = 31337;

static void SetupHost(FakeWorld& host) {
    for (uint32_t i = 1; i <= 3; ++i) {
        FakeChar c;
        c.pos = {float(i) * 100, 0, 0};
        c.dest = c.pos;
        c.vit.blood = 100;
        host.chars[i] = c;
    }
}
// A client sitting in the main menu: no world at all.
static void AtMenu(FakeWorld& cli) { cli.ready = false; cli.chars.clear(); cli.fp = 0; }

static Session::LogFn Quiet(const char* tag, bool verbose = false) {
    return [tag, verbose](const std::string& s) { if (verbose) std::printf("    [%s] %s\n", tag, s.c_str()); };
}

static bool JoinAndWait(Session& host, FakeWorld& hw, Session& cli, FakeWorld& cw, uint16_t port, size_t entities) {
    std::string err;
    if (!cli.Join("127.0.0.1", port, &err)) return false;
    Run({{&host, &hw}, {&cli, &cw}}, 10.0, [&] { return cli.state() == SessionState::Connected && cli.entityCount() == entities; });
    return cli.state() == SessionState::Connected && cli.entityCount() == entities;
}

// ---------------------------------------------------------------- tests
static void TestWire() {
    std::printf("wire/protocol roundtrips\n");
    Writer w;
    w.varint(0); w.varint(127); w.varint(128); w.varint(0xFFFFFFFFFFFFFFFFull); w.str("h\xc3\xa9llo"); w.f32(1.5f);
    Reader r(w.data(), w.size());
    CHECK(r.varint() == 0); CHECK(r.varint() == 127); CHECK(r.varint() == 128);
    CHECK(r.varint() == 0xFFFFFFFFFFFFFFFFull); CHECK(r.str(100) == "h\xc3\xa9llo"); CHECK(r.f32() == 1.5f);
    CHECK(r.ok() && r.atEnd());

    Reader r2(w.data(), 2);
    r2.u64();
    CHECK(!r2.ok());
    Writer w3; w3.varint(1000000); Reader r3(w3.data(), w3.size()); CHECK(r3.str(10).empty()); CHECK(!r3.ok());
    Writer w4; w4.f32(std::nanf("")); Reader r4(w4.data(), w4.size()); r4.f32(); CHECK(!r4.ok());

    Hello h; h.gameBuild = 5; h.modsHash = 6; h.name = "Beep";
    Writer hw; Encode(hw, h);
    Reader hr(hw.data(), hw.size());
    CHECK(PeekType(hr) == Msg::Hello);
    Hello h2; CHECK(Decode(hr, h2)); CHECK(h2.name == "Beep" && h2.modsHash == 6);

    Snapshot s; s.tick = 9; s.hostTime = 12.5;
    for (uint32_t i = 1; i <= 300; ++i) {
        EntityState e; e.netId = i * 1000; e.pos = {float(i), -float(i), 0.5f}; e.dest = e.pos; e.flags = uint8_t(i);
        s.entities.push_back(e);
    }
    auto pkts = EncodeSnapshot(s);
    CHECK(pkts.size() > 1);
    size_t total = 0;
    for (auto& p : pkts) {
        CHECK(p.size() <= kSnapshotBudget);
        Reader pr(p.data(), p.size());
        CHECK(PeekType(pr) == Msg::Snapshot);
        Snapshot d; CHECK(Decode(pr, d));
        CHECK(d.tick == 9 && d.hostTime == 12.5);
        for (auto& e : d.entities) { const uint32_t i = e.netId / 1000; CHECK(e.pos.x == float(i) && e.flags == uint8_t(i)); }
        total += d.entities.size();
    }
    CHECK(total == 300);

    EffectsMsg fx; fx.full = true;
    WeatherEffect bolt; bolt.id = 41; bolt.kind = EffectKind::Point; bolt.regionSid = "r-1"; bolt.effectSid = "lightning"; bolt.ordinal = 1;
    bolt.pos = {1.5f, -2, 3}; bolt.age = 0.05f; bolt.life = 0.9f; bolt.strength = 2; bolt.strikeIn = 0.05f;
    WeatherEffect storm; storm.id = 42; storm.kind = EffectKind::Wandering; storm.regionSid = "r-1"; storm.effectSid = "dust"; storm.endless = true;
    storm.pos = {100, 5, -100}; storm.dir = {1, 0, 0}; storm.turnTo = {0, 0, 1};
    fx.spawned = {bolt, storm};
    fx.moved.push_back({42, {101, 5, -100}, {0.7f, 0, 0.7f}, {0, 0, 1}});
    fx.ended = {40};
    Writer fw; Encode(fw, fx);
    Reader fr(fw.data(), fw.size()); CHECK(PeekType(fr) == Msg::Effects);
    EffectsMsg fx2; CHECK(Decode(fr, fx2));
    CHECK(fx2.full && fx2.spawned.size() == 2 && fx2.moved.size() == 1 && fx2.ended == std::vector<uint32_t>{40});
    CHECK(fx2.spawned[0].kind == EffectKind::Point && fx2.spawned[0].ordinal == 1 && fx2.spawned[0].strikeIn == 0.05f &&
          fx2.spawned[0].pos.y == -2 && fx2.spawned[0].life == 0.9f && fx2.spawned[0].effectSid == "lightning");
    CHECK(fx2.spawned[1].kind == EffectKind::Wandering && fx2.spawned[1].endless && fx2.spawned[1].turnTo.z == 1 && fx2.spawned[1].pos.x == 100);
    CHECK(fx2.moved[0].id == 42 && fx2.moved[0].dir.x == 0.7f);
    {   // an unknown kind is refused
        std::vector<uint8_t> bad(fw.data(), fw.data() + fw.size());
        bad[3 + 4] = 9;   // id byte, full, count, u32 id -> kind
        Reader br(bad.data(), bad.size()); PeekType(br);
        EffectsMsg fx3; CHECK(!Decode(br, fx3));
    }

    AnimMsg am;
    AnimEvent ae; ae.netId = 12; ae.kind = AnimKind::Combat; ae.name = "attack_chop_long_name_over_15"; ae.a = 1.25f;
    am.events = {ae};
    ae.kind = AnimKind::State; ae.name = ""; ae.flags = 5; am.events.push_back(ae);
    Writer aw; Encode(aw, am);
    Reader ar(aw.data(), aw.size()); CHECK(PeekType(ar) == Msg::Anim);
    AnimMsg am2; CHECK(Decode(ar, am2));
    CHECK(am2.events.size() == 2 && am2.events[0].kind == AnimKind::Combat && am2.events[0].name == "attack_chop_long_name_over_15" &&
          am2.events[0].a == 1.25f && am2.events[1].kind == AnimKind::State && am2.events[1].flags == 5 && am2.events[1].netId == 12);

    {   // items on the ground: a pickup alone (short) and a drop (with the item)
        GroundMsg g;
        GroundEvent up; up.kind = GroundKind::PickedUp; up.item.index = 467; up.item.serial = 9;
        up.state.templateSid = "209-gamedata.base"; up.state.quantity = 1; up.pos = {4, 5, 6};
        g.events = {up};
        Writer gw; Encode(gw, g);
        Reader gr(gw.data(), gw.size()); CHECK(PeekType(gr) == Msg::Ground);
        GroundMsg g2; CHECK(Decode(gr, g2));
        CHECK(g2.events.size() == 1 && g2.events[0].kind == GroundKind::PickedUp && g2.events[0].item.index == 467);
        GroundEvent dn; dn.kind = GroundKind::Dropped; dn.item.index = 5; dn.state.templateSid = "209-gamedata.base"; dn.state.quantity = 2;
        dn.pos = {1, 2, 3};
        g.events = {dn, up};
        Writer gw2; Encode(gw2, g);
        Reader gr2(gw2.data(), gw2.size()); PeekType(gr2);
        GroundMsg g3; CHECK(Decode(gr2, g3));
        CHECK(g3.events.size() == 2 && g3.events[0].state.quantity == 2 && g3.events[0].pos.z == 3 && g3.events[1].kind == GroundKind::PickedUp);
    }
    {   // skill levels and money
        ProgressMsg pm; pm.hasMoney = true; pm.money = 12345;
        CharProgress cp; cp.netId = 9; cp.stats.assign(kStatCount, 0.0f); cp.stats[0] = 17.25f; cp.stats[kStatCount - 1] = 3.5f;
        pm.chars = {cp};
        Writer pw; Encode(pw, pm);
        Reader pr(pw.data(), pw.size()); CHECK(PeekType(pr) == Msg::Progress);
        ProgressMsg pm2; CHECK(Decode(pr, pm2));
        CHECK(pm2.hasMoney && pm2.money == 12345 && pm2.chars.size() == 1 && pm2.chars[0].netId == 9 &&
              pm2.chars[0].stats.size() == kStatCount && pm2.chars[0].stats[0] == 17.25f && pm2.chars[0].stats[kStatCount - 1] == 3.5f);
        pm.chars[0].stats[3] = -5.0f;   // nonsense levels are refused
        Writer pw2; Encode(pw2, pm);
        Reader pr2(pw2.data(), pw2.size()); PeekType(pr2);
        ProgressMsg pm3; CHECK(!Decode(pr2, pm3));
    }
    {   // a player task with its subject
        Command tc; tc.netId = 3; tc.kind = CommandKind::Task; tc.via = TaskVia::TaskNearest; tc.task = 25; tc.shift = true;
        tc.subject.type = 1; tc.subject.index = 77; tc.building.index = 4; tc.pos = {1, 2, 3};
        Writer tw; Encode(tw, tc);
        Reader tr(tw.data(), tw.size()); CHECK(PeekType(tr) == Msg::Command);
        Command tc2; CHECK(Decode(tr, tc2));
        CHECK(tc2.kind == CommandKind::Task && tc2.via == TaskVia::TaskNearest && tc2.task == 25 && tc2.shift && !tc2.add &&
              tc2.subject.index == 77 && tc2.building.index == 4 && tc2.pos.y == 2);
    }
    {   // speech bubbles and a conversation window
        DialogMsg dm;
        DialogEvent say; say.kind = DialogKind::Say; say.netId = 5; say.text = "Hey you! Stop right there."; say.shout = true;
        DialogEvent txt; txt.kind = DialogKind::Text; txt.dialogId = 3; txt.netId = 5; txt.text = "What do you want?"; txt.replies = {"Nothing", "Join me"};
        dm.events = {say, txt};
        Writer dw; Encode(dw, dm);
        Reader dr(dw.data(), dw.size()); CHECK(PeekType(dr) == Msg::Dialog);
        DialogMsg dm2; CHECK(Decode(dr, dm2));
        CHECK(dm2.events.size() == 2 && dm2.events[0].shout && dm2.events[0].text == say.text && dm2.events[1].dialogId == 3 &&
              dm2.events[1].replies.size() == 2 && dm2.events[1].replies[1] == "Join me");
        DialogReply rep; rep.dialogId = 3; rep.index = 1;
        Writer rw; Encode(rw, rep);
        Reader rr(rw.data(), rw.size()); CHECK(PeekType(rr) == Msg::DialogReply);
        DialogReply rep2; CHECK(Decode(rr, rep2) && rep2.dialogId == 3 && rep2.index == 1);
    }
    {   // a character's looks
        AppearanceMsg look; look.netId = 7; look.name = "Nassim";
        AppearanceField f1; f1.type = AppearanceType::Float; f1.key = "Age"; f1.f[0] = 33.5f;
        AppearanceField f2; f2.type = AppearanceType::Refs; f2.key = "race"; f2.refs = {"17-gamedata.quack"};
        AppearanceField f3; f3.type = AppearanceType::Quat; f3.key = "q"; f3.f[0] = 1; f3.f[3] = 0.5f;
        AppearanceField f4; f4.type = AppearanceType::String; f4.key = "head"; f4.s = "1234-gamedata.base";
        look.fields = {f1, f2, f3, f4};
        Writer aw2; Encode(aw2, look);
        Reader ar2(aw2.data(), aw2.size()); CHECK(PeekType(ar2) == Msg::Appearance);
        AppearanceMsg look2; CHECK(Decode(ar2, look2));
        CHECK(look2.netId == 7 && look2.name == "Nassim" && look2.fields.size() == 4 && look2.fields[0] == f1 && look2.fields[1] == f2 && look2.fields[2] == f3 && look2.fields[3] == f4);
    }
    {   // client log lines, sync reports, editor state
        ClientLog cl; cl.lines = {"une ligne", std::string(1000, 'x')};
        Writer lw; Encode(lw, cl);
        Reader lr(lw.data(), lw.size()); CHECK(PeekType(lr) == Msg::ClientLog);
        ClientLog cl2; CHECK(Decode(lr, cl2) && cl2.lines.size() == 2 && cl2.lines[0] == "une ligne" && cl2.lines[1].size() == kMaxLogLine);
        ClientReport rep; rep.entities = 96; rep.missingNpcs = 3; rep.farOff = 1; rep.maxErr = 2.5f; rep.fps = 58;
        Writer rw2; Encode(rw2, rep);
        Reader rr2(rw2.data(), rw2.size()); CHECK(PeekType(rr2) == Msg::ClientReport);
        ClientReport rep2; CHECK(Decode(rr2, rep2) && rep2.entities == 96 && rep2.missingNpcs == 3 && rep2.farOff == 1 && rep2.maxErr == 2.5f && rep2.fps == 58);
        Writer ew; Encode(ew, EditState{true});
        Reader er(ew.data(), ew.size()); CHECK(PeekType(er) == Msg::EditState);
        EditState es; CHECK(Decode(er, es) && es.editing);
        CHECK(std::string(TaskLabel(258)) == "dormir" && std::string(TaskLabel(9999)) == "?");
    }
    {   // hunger travels with the vitals
        VitalsMsg vm; vm.entities.resize(1); vm.entities[0].netId = 2; vm.entities[0].hunger = 250.5f;
        auto pk = EncodeVitals(vm)[0];
        Reader vr(pk.data(), pk.size()); PeekType(vr);
        VitalsMsg vm2; CHECK(Decode(vr, vm2) && vm2.entities.size() == 1 && vm2.entities[0].hunger == 250.5f);
    }

    WorldChunk c; c.file = 2; c.path = "zone/zone.1.2.zone"; c.fileSize = 5; c.data = {1, 2, 3};
    Writer cw; Encode(cw, c);
    Reader cr(cw.data(), cw.size()); CHECK(PeekType(cr) == Msg::WorldChunk);
    WorldChunk c2; CHECK(Decode(cr, c2)); CHECK(c2.path == c.path && c2.data == c.data && c2.fileSize == 5);

    // save paths coming from the network
    CHECK(ValidWorldPath("quick.save"));
    CHECK(ValidWorldPath("platoon/Ma\xc3\xaetres des pantins_0.platoon"));
    CHECK(ValidWorldPath("zone/zone.37.34.zone"));
    for (const char* bad : {"", "..", "../x", "a/../b", "/abs", "C:/x", "a\\b", "a//b", "a/", "x:stream", "CON", "zone/nul.txt",
                            "a/./b", "trailing.", "sp ", "ctl\x01"})
        CHECK(!ValidWorldPath(bad));

    std::mt19937 rng(1);
    std::uniform_real_distribution<float> u(-1, 1);
    float worst = 0;
    for (int i = 0; i < 2000; ++i) {
        Quat q{u(rng), u(rng), u(rng), u(rng)};
        const float l = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
        if (l < 1e-3f) continue;
        q.w /= l; q.x /= l; q.y /= l; q.z /= l;
        Quat p = UnpackQuat(PackQuat(q));
        worst = std::max(worst, 1.0f - std::fabs(q.w * p.w + q.x * p.x + q.y * p.y + q.z * p.z));
    }
    CHECK(worst < 1e-4f);
}

static void TestFuzz() {
    std::printf("decoder fuzzing (random + mutated packets)\n");
    std::mt19937 rng(42);
    std::vector<std::vector<uint8_t>> seeds;
    auto add = [&](auto&& fn) { Writer w; fn(w); seeds.push_back(w.vec()); };
    add([](Writer& w) { Hello h; h.name = "abc"; Encode(w, h); });
    add([](Writer& w) { Welcome m; m.players = {{1, "a"}, {2, "b"}}; Encode(w, m); });
    add([](Writer& w) { Encode(w, Reject{RejectReason::Full}); });
    add([](Writer& w) { Encode(w, PlayerInfo{3, "x"}); });
    add([](Writer& w) { Encode(w, Chat{1, "hi"}); });
    add([](Writer& w) { Bind b; b.netId = 5; b.handle.serial = 9; Encode(w, b); });
    add([](Writer& w) { Bind b; b.netId = 5; b.hasSpawn = true; b.spawn.templateSid = "123-x.mod"; b.spawn.factionSid = "f"; b.spawn.name = "Bob"; Encode(w, b); });
    add([](Writer& w) { Encode(w, Unbind{5}); });
    add([](Writer& w) { Command c; c.netId = 3; Encode(w, c); });
    add([](Writer& w) { Encode(w, TimeState{2.0f, true, 10.0}); });
    add([](Writer& w) { Encode(w, WorldBegin{1000, 3}); });
    add([](Writer& w) { WorldChunk c; c.path = "a/b"; c.fileSize = 4; c.data = {1, 2}; Encode(w, c); });
    add([](Writer& w) { WorldChunk c; c.offset = 100; c.data = {1, 2}; Encode(w, c); });
    add([](Writer& w) { Encode(w, WorldEnd{7}); });
    add([](Writer& w) { Encode(w, ReadyMsg{7}); });
    add([](Writer& w) { WeatherMsg m; m.regions.resize(2); m.regions[0].regionSid = "a"; m.regions[1].weatherSid = "b"; Encode(w, m); });
    add([](Writer& w) { InventoryMsg m; m.netId = 3; m.items.resize(2); for (auto& i : m.items) i.templateSid = "x"; Encode(w, m); });
    add([](Writer& w) { InvOp m; m.fromNetId = 1; m.toNetId = 2; m.item.templateSid = "x"; m.toSection = "main"; Encode(w, m); });
    add([](Writer& w) { AnimMsg m; m.events.resize(2); m.events[0].netId = 3; m.events[0].name = "x"; m.events[1].netId = 4; Encode(w, m); });
    add([](Writer& w) {
        EffectsMsg m; m.spawned.resize(2); m.spawned[1].kind = EffectKind::Wandering; m.spawned[0].regionSid = "r";
        m.moved.resize(1); m.ended = {3}; Encode(w, m);
    });
    { DialogMsg dm; DialogEvent e; e.netId = 1; e.text = "hi"; e.replies = {"a", "b"}; dm.events = {e}; add([&](Writer& w) { Encode(w, dm); }); }
    { ProgressMsg pm; pm.hasMoney = true; CharProgress cp; cp.netId = 1; cp.stats.assign(kStatCount, 1.0f); pm.chars = {cp}; add([&](Writer& w) { Encode(w, pm); }); }
    { VitalsMsg v; v.entities.resize(2); for (auto& e : v.entities) { e.netId = 4; e.parts.resize(3); } seeds.push_back(EncodeVitals(v)[0]); }
    { Snapshot s; s.entities.resize(3); for (auto& e : s.entities) e.netId = 7; seeds.push_back(EncodeSnapshot(s)[0]); }

    auto decodeAll = [](const std::vector<uint8_t>& p) {
        Reader r(p.data(), p.size());
        auto t = PeekType(r);
        if (!t) return;
        switch (*t) {
        case Msg::Hello: { Hello m; Decode(r, m); break; }
        case Msg::Welcome: { Welcome m; Decode(r, m); break; }
        case Msg::Reject: { Reject m; Decode(r, m); break; }
        case Msg::PlayerJoined: { PlayerInfo m; Decode(r, m); break; }
        case Msg::PlayerLeft: { PlayerLeft m; Decode(r, m); break; }
        case Msg::Chat: { Chat m; Decode(r, m); break; }
        case Msg::Bind: { Bind m; Decode(r, m); break; }
        case Msg::Unbind: { Unbind m; Decode(r, m); break; }
        case Msg::Snapshot: { Snapshot m; Decode(r, m); break; }
        case Msg::Command: { Command m; Decode(r, m); break; }
        case Msg::TimeState: { TimeState m; Decode(r, m); break; }
        case Msg::Ping: case Msg::Pong: { Ping m; Decode(r, m); break; }
        case Msg::Vitals: { VitalsMsg m; Decode(r, m); break; }
        case Msg::WorldBegin: { WorldBegin m; Decode(r, m); break; }
        case Msg::WorldChunk: { WorldChunk m; if (Decode(r, m) && m.offset == 0 && !ValidWorldPath(m.path)) std::abort(); break; }
        case Msg::WorldEnd: { WorldEnd m; Decode(r, m); break; }
        case Msg::Ready: { ReadyMsg m; Decode(r, m); break; }
        case Msg::Weather: { WeatherMsg m; Decode(r, m); break; }
        case Msg::Inventory: { InventoryMsg m; Decode(r, m); break; }
        case Msg::InvOp: { InvOp m; Decode(r, m); break; }
        case Msg::Effects: { EffectsMsg m; Decode(r, m); break; }
        case Msg::Anim: { AnimMsg m; Decode(r, m); break; }
        case Msg::AnimFrame: { AnimFrameMsg m; Decode(r, m); break; }
        case Msg::Ground: { GroundMsg m; Decode(r, m); break; }
        case Msg::Progress: { ProgressMsg m; Decode(r, m); break; }
        case Msg::Dialog: { DialogMsg m; Decode(r, m); break; }
        case Msg::Appearance: { AppearanceMsg m; Decode(r, m); break; }
        case Msg::Squads: { SquadsMsg m; Decode(r, m); break; }
        case Msg::ClientLog: { ClientLog m; Decode(r, m); break; }
        case Msg::ClientReport: { ClientReport m; Decode(r, m); break; }
        case Msg::EditState: { EditState m; Decode(r, m); break; }
        case Msg::DialogReply: { DialogReply m; Decode(r, m); break; }
        }
    };
    for (int i = 0; i < 300000; ++i) {
        std::vector<uint8_t> p;
        if (i % 2) {
            p.resize(rng() % 64);
            for (auto& b : p) b = uint8_t(rng());
            if (!p.empty()) p[0] = uint8_t(1 + rng() % 21);
        } else {
            p = seeds[rng() % seeds.size()];
            const int muts = 1 + rng() % 4;
            for (int m = 0; m < muts && !p.empty(); ++m) {
                switch (rng() % 3) {
                case 0: p[rng() % p.size()] = uint8_t(rng()); break;
                case 1: p.resize(rng() % (p.size() + 1)); break;
                case 2: p.push_back(uint8_t(rng())); break;
                }
            }
        }
        decodeAll(p);
    }
    CHECK(true);  // reaching here without a crash / sanitizer report is the test
}

static void TestJoinFromMenu() {
    std::printf("session: join from the main menu receives the host's world\n");
    FakeWorld hw, cw;
    SetupHost(hw);
    hw.chars[2].pos = {222, 0, 9}; hw.chars[2].dest = hw.chars[2].pos;   // the host played since its last save
    AtMenu(cw);
    SessionConfig hc; hc.characterPerPlayer = false; hc.name = "Host"; hc.port = ++g_port;
    SessionConfig cc; cc.name = "Client"; cc.port = hc.port;
    Session host(hw, hc, Now, Quiet("host"));
    Session cli(cw, cc, Now, Quiet("cli"));
    std::string err;
    CHECK(host.Host(&err));
    CHECK(cli.Join("127.0.0.1", hc.port, &err));
    bool sawHold = false, sawLoading = false;
    Run({{&host, &hw}, {&cli, &cw}}, 10.0, [&] {
        sawHold |= hw.holding;
        sawLoading |= cli.state() == SessionState::Loading;
        return cli.state() == SessionState::Connected && cli.entityCount() == 3;
    });
    CHECK(cli.state() == SessionState::Connected);
    CHECK(cli.localId() == 2);
    CHECK(sawHold && sawLoading);
    CHECK(cw.imports == 1);
    CHECK(cw.fp == hw.fp);                                     // same world
    CHECK(cw.chars.size() == 3);
    CHECK(Dist(cw.chars[2].pos, {222, 0, 9}) < 1e-3f);          // the host's *current* state, not an old save
    CHECK(cw.client && cw.active);
    Run({{&host, &hw}, {&cli, &cw}}, 0.5);
    CHECK(!hw.holding);                                        // released once everyone is in
    CHECK(host.joiningPlayers() == 0);
}

static void TestOwnCharacter() {
    std::printf("session: a joining player gets a character of their own, kept across rejoins\n");
    FakeWorld hw, cw;
    SetupHost(hw);
    AtMenu(cw);
    SessionConfig hc; hc.name = "Host"; hc.port = ++g_port;   // characterPerPlayer: the default
    SessionConfig cc; cc.name = "Client"; cc.port = hc.port;
    Session host(hw, hc, Now, Quiet("host"));
    std::string err;
    CHECK(host.Host(&err));
    for (int round = 0; round < 2; ++round) {
        Session cli(cw, cc, Now, Quiet("cli"));
        CHECK(cli.Join("127.0.0.1", hc.port, &err));
        Run({{&host, &hw}, {&cli, &cw}}, 10.0, [&] {
            return cli.state() == SessionState::Connected && cli.entityCount() == 4 && cw.controllable.size() == 1;
        });
        CHECK(cli.state() == SessionState::Connected);
        CHECK(hw.named.size() == 1);                    // created once, found again on the rejoin
        const uint32_t mine = hw.named.count("Client") ? hw.named["Client"] : 0;
        CHECK(cw.chars.count(mine) == 1);               // it was in the world the client received
        CHECK(cw.controllable.size() == 1 && !cw.controllable.empty() && cw.controllable[0].serial == mine);
        CHECK(hw.controllable.size() == 3);             // the host keeps its own three
        cli.Leave();
        Run({{&host, &hw}, {&cli, &cw}}, 1.0);
        AtMenu(cw);
    }
    // A second player joins while the first plays: the first has no copy of the newcomer's new
    // character in its world, so it gets a stand-in; each controls only their own.
    FakeWorld bw;
    AtMenu(bw);
    Session cliA(cw, cc, Now, Quiet("cliA"));
    SessionConfig bc; bc.name = "Second"; bc.port = hc.port;
    Session cliB(bw, bc, Now, Quiet("cliB"));
    CHECK(cliA.Join("127.0.0.1", hc.port, &err));
    Run({{&host, &hw}, {&cliA, &cw}, {&cliB, &bw}}, 10.0, [&] { return cliA.state() == SessionState::Connected && cw.controllable.size() == 1; });
    CHECK(cliB.Join("127.0.0.1", hc.port, &err));
    Run({{&host, &hw}, {&cliA, &cw}, {&cliB, &bw}}, 15.0, [&] {
        return cliB.state() == SessionState::Connected && bw.controllable.size() == 1 && hw.named.count("Second") &&
               cw.chars.count(hw.named["Second"]) == 1;
    });
    CHECK(cliB.state() == SessionState::Connected);
    CHECK(hw.named.size() == 2);
    const uint32_t second = hw.named.count("Second") ? hw.named["Second"] : 0;
    CHECK(cw.chars.count(second) == 1);                 // stand-in on the first player's side
    CHECK(bw.chars.count(hw.named["Client"]) == 1);     // the first player's character came with the save
    CHECK(bw.controllable.size() == 1 && !bw.controllable.empty() && bw.controllable[0].serial == second);
    CHECK(cw.controllable.size() == 1 && !cw.controllable.empty() && cw.controllable[0].serial == hw.named["Client"]);

    std::printf("session: a character whose handle changes (death, new squad) stays the same entity\n");
    const uint32_t mine = hw.named["Client"];
    size_t entitiesBefore = cliA.entityCount();
    hw.container[mine] = 77;   // e.g. the host moved it to another squad
    Run({{&host, &hw}, {&cliA, &cw}, {&cliB, &bw}}, 3.0, [&] { return !cw.rehandles.empty() && !bw.rehandles.empty(); });
    CHECK(cw.rehandles.size() == 1 && bw.rehandles.size() == 1);
    if (!cw.rehandles.empty()) {
        CHECK(cw.rehandles[0].first.container == 0 && cw.rehandles[0].second.container == 77);
        CHECK(cw.rehandles[0].second.serial == mine);
    }
    CHECK(cliA.entityCount() == entitiesBefore);        // no unbind / new entity
    Run({{&host, &hw}, {&cliA, &cw}, {&cliB, &bw}}, 1.0);
    CHECK(cw.controllable.size() == 1 && !cw.controllable.empty() && cw.controllable[0].container == 77);   // still ours
}

static void TestSessionReplication() {
    std::printf("session: ownership, commands, convergence, chat, leave\n");
    FakeWorld hw, cw;
    SetupHost(hw);
    AtMenu(cw);
    SessionConfig hc; hc.characterPerPlayer = false; hc.name = "Host"; hc.port = ++g_port;
    SessionConfig cc; cc.name = "Client"; cc.port = hc.port;
    Session host(hw, hc, Now, Quiet("host"));
    Session cli(cw, cc, Now, Quiet("cli"));
    std::string err;
    CHECK(host.Host(&err));
    CHECK(JoinAndWait(host, hw, cli, cw, hc.port, 3));
    CHECK(cli.missingSquad() == 0);
    CHECK(host.players().size() == 1);
    CHECK(cw.controllable.empty());
    Run({{&host, &hw}, {&cli, &cw}}, 0.3);
    CHECK(hw.controllable.size() == 3);

    host.Assign(FakeWorld::H(2), 2);
    Run({{&host, &hw}, {&cli, &cw}}, 2.0, [&] { return cw.controllable.size() == 1; });
    CHECK(cw.controllable.size() == 1 && cw.controllable[0].serial == 2);
    CHECK(hw.controllable.size() == 2);

    Command c; c.kind = CommandKind::MoveTo; c.pos = {200, 0, 80};
    cw.localOrders.push_back({FakeWorld::H(2), c});
    Command bad; bad.kind = CommandKind::MoveTo; bad.pos = {-500, 0, 0};
    cw.localOrders.push_back({FakeWorld::H(1), bad});
    hw.chars[3].dest = {300, 0, -60};

    Run({{&host, &hw}, {&cli, &cw}}, 6.0, [&] {
        return Dist(hw.chars[2].pos, c.pos) < 1e-3f && Dist(hw.chars[3].pos, hw.chars[3].dest) < 1e-3f &&
               Dist(cw.chars[2].pos, hw.chars[2].pos) < 1e-3f && Dist(cw.chars[3].pos, hw.chars[3].pos) < 1e-3f;
    });
    CHECK(Dist(hw.chars[2].pos, c.pos) < 1e-3f);
    CHECK(Dist(hw.chars[1].pos, {100, 0, 0}) < 1e-3f);
    for (uint32_t i = 1; i <= 3; ++i) CHECK(Dist(cw.chars[i].pos, hw.chars[i].pos) < 1e-3f);

    hw.time = TimeState{3.0f, false, 42.0};
    Run({{&host, &hw}, {&cli, &cw}}, 2.0, [&] { return cw.time.speed == 3.0f; });
    CHECK(cw.time.speed == 3.0f && cw.time.gameHours == 42.0);

    cli.SendChat("hello\x01 there");
    Run({{&host, &hw}, {&cli, &cw}}, 2.0, [&] { return !host.chatLog().empty() && host.chatLog().back() == "Client: hello there"; });
    CHECK(!host.chatLog().empty() && host.chatLog().back() == "Client: hello there");

    cli.Leave();
    CHECK(!cw.active);
    Run({{&host, &hw}}, 3.0, [&] { return host.players().empty(); });
    CHECK(host.players().empty());
    CHECK(host.ownerOf(FakeWorld::H(2)) == 1);
    Run({{&host, &hw}}, 0.3);
    CHECK(hw.controllable.size() == 3);
}

static void TestDivergenceIsCorrected() {
    std::printf("session: a diverged client is pulled back to host state\n");
    FakeWorld hw, cw;
    SetupHost(hw);
    AtMenu(cw);
    SessionConfig hc; hc.characterPerPlayer = false; hc.port = ++g_port; SessionConfig cc; cc.port = hc.port; cc.name = "C";
    Session host(hw, hc, Now, Quiet("host"));
    Session cli(cw, cc, Now, Quiet("cli"));
    std::string err;
    host.Host(&err);
    CHECK(JoinAndWait(host, hw, cli, cw, hc.port, 3));
    cw.chars[1].pos = {9999, 9999, 9999};
    Run({{&host, &hw}, {&cli, &cw}}, 3.0, [&] { return Dist(cw.chars[1].pos, hw.chars[1].pos) < 1e-3f; });
    CHECK(Dist(cw.chars[1].pos, hw.chars[1].pos) < 1e-3f);
}

static void TestRejections() {
    std::printf("session: mismatched game / mods / world are refused\n");
    struct Case { const char* what; std::function<void(FakeWorld&)> mutate; std::string expect; };
    std::vector<Case> cases = {
        {"build", [](FakeWorld& w) { w.build = 1; }, ToString(RejectReason::GameMismatch)},
        {"mods", [](FakeWorld& w) { w.mods = 1; }, ToString(RejectReason::ModsMismatch)},
        {"world", [](FakeWorld& w) { w.corruptImport = true; }, "differs from the host"},
    };
    for (auto& tc : cases) {
        FakeWorld hw, cw;
        SetupHost(hw);
        AtMenu(cw);
        tc.mutate(cw);
        SessionConfig hc; hc.characterPerPlayer = false; hc.port = ++g_port; SessionConfig cc; cc.port = hc.port; cc.name = "C"; cc.loadTimeout = 4.0;
        Session host(hw, hc, Now, Quiet("host"));
        Session cli(cw, cc, Now, Quiet("cli"));
        std::string err;
        host.Host(&err);
        cli.Join("127.0.0.1", hc.port, &err);
        Run({{&host, &hw}, {&cli, &cw}}, 8.0, [&] { return cli.state() == SessionState::Failed; });
        CHECK(cli.state() == SessionState::Failed);
        CHECK(cli.lastError().find(tc.expect) != std::string::npos);
        CHECK(!cw.active);
        Run({{&host, &hw}}, 3.0, [&] { return host.players().empty() && !hw.holding; });
        CHECK(host.players().empty());
        CHECK(!hw.holding);   // a failed join never leaves the host frozen
    }
    {
        FakeWorld hw, cw; SetupHost(hw); AtMenu(cw);
        SessionConfig hc; hc.characterPerPlayer = false; hc.port = ++g_port; SessionConfig cc; cc.port = hc.port; cc.name = " bad";
        Session host(hw, hc, Now, Quiet("host")); Session cli(cw, cc, Now, Quiet("cli"));
        std::string err; host.Host(&err); cli.Join("127.0.0.1", hc.port, &err);
        Run({{&host, &hw}, {&cli, &cw}}, 5.0, [&] { return cli.state() == SessionState::Failed; });
        CHECK(cli.lastError().find(ToString(RejectReason::BadName)) != std::string::npos);
    }
    {
        // the host removes a player: the player is told why, the host forgets them
        FakeWorld hw, cw; SetupHost(hw); AtMenu(cw);
        SessionConfig hc; hc.characterPerPlayer = false; hc.port = ++g_port; SessionConfig cc; cc.port = hc.port; cc.name = "C";
        Session host(hw, hc, Now, Quiet("host")); Session cli(cw, cc, Now, Quiet("cli"));
        std::string err; host.Host(&err);
        CHECK(JoinAndWait(host, hw, cli, cw, hc.port, 3));
        CHECK(!host.KickPlayer(99));
        CHECK(host.KickPlayer(host.players().begin()->first));
        Run({{&host, &hw}, {&cli, &cw}}, 5.0, [&] { return cli.state() == SessionState::Failed && host.players().empty(); });
        CHECK(cli.state() == SessionState::Failed && cli.lastError().find(ToString(RejectReason::Kicked)) != std::string::npos);
        CHECK(host.players().empty());
        CHECK(!cli.KickPlayer(1));
    }
    {
        // names identify each player's own character: a player with the host's name is renamed
        FakeWorld hw, cw; SetupHost(hw); AtMenu(cw);
        SessionConfig hc; hc.name = "Same"; hc.port = ++g_port; SessionConfig cc; cc.port = hc.port; cc.name = "Same";
        Session host(hw, hc, Now, Quiet("host")); Session cli(cw, cc, Now, Quiet("cli"));
        std::string err; host.Host(&err); cli.Join("127.0.0.1", hc.port, &err);
        Run({{&host, &hw}, {&cli, &cw}}, 10.0, [&] { return cli.state() == SessionState::Connected && cw.controllable.size() == 1; });
        CHECK(cli.state() == SessionState::Connected);
        CHECK(hw.named.size() == 1 && hw.named.count("Same 2") == 1);
        CHECK(host.players().size() == 1 && host.players().begin()->second.name == "Same 2");
    }
    {
        // the host's save fails: the joiner is told, the host is released
        FakeWorld hw, cw; SetupHost(hw); AtMenu(cw);
        hw.exportFrames = -1000;  // PollWorldExport -> Failed
        SessionConfig hc; hc.characterPerPlayer = false; hc.port = ++g_port; SessionConfig cc; cc.port = hc.port; cc.name = "C";
        Session host(hw, hc, Now, Quiet("host")); Session cli(cw, cc, Now, Quiet("cli"));
        std::string err; host.Host(&err); cli.Join("127.0.0.1", hc.port, &err);
        Run({{&host, &hw}, {&cli, &cw}}, 6.0, [&] { return cli.state() == SessionState::Failed && !hw.holding; });
        CHECK(cli.state() == SessionState::Failed);
        CHECK(!hw.holding);
    }
    {
        FakeWorld w; w.ready = false; SessionConfig c; c.port = ++g_port;
        Session s(w, c, Now, Quiet("x")); std::string err;
        CHECK(!s.Host(&err));
    }
    {
        FakeWorld w; SessionConfig c; c.port = ++g_port; Session s(w, c, Now, Quiet("x")); std::string err;
        CHECK(s.Join("127.0.0.1", c.port, &err));
        Run({{&s, &w}}, 12.0, [&] { return s.state() == SessionState::Failed; });
        CHECK(s.state() == SessionState::Failed);
    }
}

static void TestWorldAuthority() {
    std::printf("session: NPC interest, health authority, clock and pause\n");
    FakeWorld hw, cw;
    SetupHost(hw);
    AtMenu(cw);
    FakeChar near; near.squad = false; near.pos = {150, 0, 30}; near.dest = near.pos; near.vit.blood = 100;
    FakeChar far; far.squad = false; far.pos = {90000, 0, 0}; far.dest = far.pos;
    hw.chars[10] = near;
    hw.chars[11] = far;
    SessionConfig hc; hc.characterPerPlayer = false; hc.port = ++g_port; hc.interestRadius = 1000; SessionConfig cc; cc.port = hc.port; cc.name = "C";
    Session host(hw, hc, Now, Quiet("host"));
    Session cli(cw, cc, Now, Quiet("cli"));
    std::string err;
    host.Host(&err);
    CHECK(JoinAndWait(host, hw, cli, cw, hc.port, 4));   // 3 squad + the near NPC, not the far one
    CHECK(cli.npcCount() == 1);

    hw.chars[10].dest = {300, 0, 60};
    cw.chars[2].vit.blood = 3; cw.chars[2].vit.flags = kVitDead;   // stray local "death" on the client
    hw.chars[1].vit.blood = 40; hw.chars[1].vit.flags = kVitUnconscious; hw.chars[1].vit.koTimer = 30;
    hw.time = TimeState{1.0f, true, 1234.5};
    Run({{&host, &hw}, {&cli, &cw}}, 6.0, [&] {
        return Dist(cw.chars[10].pos, hw.chars[10].pos) < 1e-3f && Dist(hw.chars[10].pos, {300, 0, 60}) < 1e-3f &&
               cw.chars[2].vit.blood == 100 && cw.chars[1].vit.blood == 40 && cw.time.paused;
    });
    CHECK(Dist(cw.chars[10].pos, hw.chars[10].pos) < 1e-3f);
    CHECK(cw.chars[2].vit.blood == 100 && cw.chars[2].vit.flags == 0);   // local death undone
    CHECK(cw.chars[1].vit.blood == 40 && (cw.chars[1].vit.flags & kVitUnconscious));
    CHECK(cw.time.paused && cw.time.gameHours == 1234.5);
    // weather of every region follows the host
    RegionWeather rw; rw.regionSid = "biome-1"; rw.seasonSid = "season-dry"; rw.weatherSid = "acid-rain"; rw.strength = 0.8f; rw.endMinutes = 900;
    hw.weather = {rw};
    Run({{&host, &hw}, {&cli, &cw}}, 4.0, [&] { return !cw.weather.empty(); });
    CHECK(cw.weather.size() == 1 && cw.weather[0].weatherSid == "acid-rain" && cw.weather[0].strength == 0.8f);
    hw.weather[0].weatherSid = "clear"; hw.weather[0].endMinutes = 1200;
    Run({{&host, &hw}, {&cli, &cw}}, 4.0, [&] { return !cw.weather.empty() && cw.weather[0].weatherSid == "clear"; });
    CHECK(!cw.weather.empty() && cw.weather[0].weatherSid == "clear");
    // weather effects: a bolt the host's game drops reaches the client at once, with the host's spot
    // and random rolls; a storm's moves follow; removals too; and the complete set comes regularly
    WeatherEffect bolt; bolt.id = 7; bolt.kind = EffectKind::Point; bolt.regionSid = "biome-1"; bolt.effectSid = "lightning";
    bolt.pos = {12, 3, 45}; bolt.life = 0.8f; bolt.strikeIn = 0.1f;
    hw.fxLive = {bolt};
    hw.fxPending.spawned = {bolt};
    cw.fxApplied.clear();
    auto arrived = [&](auto pred) { for (const auto& m : cw.fxApplied) if (pred(m)) return true; return false; };
    Run({{&host, &hw}, {&cli, &cw}}, 2.0, [&] { return arrived([](const EffectsMsg& m) { return !m.spawned.empty(); }); });
    CHECK(arrived([](const EffectsMsg& m) {
        return !m.spawned.empty() && m.spawned[0].id == 7 && m.spawned[0].pos.z == 45 && m.spawned[0].strikeIn == 0.1f && m.spawned[0].effectSid == "lightning";
    }));
    hw.fxPending.moved.push_back({9, {5, 0, 5}, {1, 0, 0}, {0, 0, 1}});
    hw.fxPending.ended = {7};
    hw.fxLive.clear();
    Run({{&host, &hw}, {&cli, &cw}}, 2.0, [&] { return arrived([](const EffectsMsg& m) { return !m.ended.empty(); }); });
    CHECK(arrived([](const EffectsMsg& m) { return m.ended == std::vector<uint32_t>{7} && m.moved.size() == 1 && m.moved[0].id == 9; }));
    cw.fxApplied.clear();
    Run({{&host, &hw}, {&cli, &cw}}, 6.0, [&] { return arrived([](const EffectsMsg& m) { return m.full; }); });
    CHECK(arrived([](const EffectsMsg& m) { return m.full && m.spawned.empty(); }));
    CHECK(!arrived([](const EffectsMsg& m) { return !m.full && m.empty(); }));   // nothing empty goes out
    // animations: an attack the host's NPC starts plays on the client's copy; unknown characters are skipped
    AnimEvent swing; swing.kind = AnimKind::Combat; swing.name = "chop"; swing.a = 1.1f;
    hw.animPending = {{FakeWorld::H(10), swing}, {FakeWorld::H(77), swing}};
    cw.animApplied.clear();
    Run({{&host, &hw}, {&cli, &cw}}, 2.0, [&] { return !cw.animApplied.empty(); });
    CHECK(cw.animApplied.size() == 1 && cw.animApplied[0].first == 10 && cw.animApplied[0].second.name == "chop" &&
          cw.animApplied[0].second.a == 1.1f);

    // melee: the NPC engages squad member 1 on the host; the client's copy engages the same target
    hw.chars[10].fights = 1;
    Run({{&host, &hw}, {&cli, &cw}}, 3.0, [&] { return cw.chars[10].fights == 1; });
    CHECK(cw.chars[10].fights == 1);
    hw.chars[10].fights = 0;
    Run({{&host, &hw}, {&cli, &cw}}, 3.0, [&] { return cw.chars[10].fights == 0; });
    CHECK(cw.chars[10].fights == 0);

    hw.chars[10].pos = {50000, 0, 0}; hw.chars[10].dest = hw.chars[10].pos;
    Run({{&host, &hw}, {&cli, &cw}}, 3.0, [&] { return cli.npcCount() == 0; });
    CHECK(cli.npcCount() == 0);
    Command c; c.kind = CommandKind::MoveTo; c.pos = {0, 0, 0};
    cw.localOrders.push_back({FakeWorld::H(11), c});
    Run({{&host, &hw}, {&cli, &cw}}, 0.5);
    CHECK(Dist(hw.chars[11].pos, {90000, 0, 0}) < 1e-3f);
}

static void TestSpawnReplication() {
    std::printf("session: characters the client lacks are recreated, and removed with the host's\n");
    FakeWorld hw, cw;
    SetupHost(hw);
    AtMenu(cw);
    SessionConfig hc; hc.characterPerPlayer = false; hc.port = ++g_port; SessionConfig cc; cc.port = hc.port; cc.name = "C";
    Session host(hw, hc, Now, Quiet("host"));
    Session cli(cw, cc, Now, Quiet("cli"));
    std::string err;
    host.Host(&err);
    CHECK(JoinAndWait(host, hw, cli, cw, hc.port, 3));
    // the host's world spawns a wandering squad after the client joined
    for (uint32_t i = 50; i < 53; ++i) {
        FakeChar c; c.squad = false; c.pos = {120.0f + i, 0, 40}; c.dest = {200.0f + i, 0, 90}; c.vit.blood = 77;
        hw.chars[i] = c;
    }
    Run({{&host, &hw}, {&cli, &cw}}, 8.0, [&] {
        for (uint32_t i = 50; i < 53; ++i)
            if (!cw.chars.count(i) || Dist(cw.chars[i].pos, hw.chars[i].pos) > 1e-3f || Dist(hw.chars[i].pos, hw.chars[i].dest) > 1e-3f) return false;
        return true;
    });
    CHECK(cw.spawns == 3);
    for (uint32_t i = 50; i < 53; ++i) {
        CHECK(cw.chars.count(i) == 1);
        if (cw.chars.count(i)) { CHECK(Dist(cw.chars[i].pos, hw.chars[i].pos) < 1e-3f); CHECK(cw.chars[i].vit.blood == 77); }
    }
    CHECK(cli.spawnedNpcs() == 3 && cli.missingNpcs() == 0);
    // they die / leave the area on the host: their stand-ins go away too
    hw.chars.erase(50);
    hw.chars[51].pos = {1e6f, 0, 0}; hw.chars[51].dest = hw.chars[51].pos;
    SessionConfig dummy; (void)dummy;
    Run({{&host, &hw}, {&cli, &cw}}, 4.0, [&] { return !cw.chars.count(50); });
    CHECK(!cw.chars.count(50));
    CHECK(cw.despawns >= 1);
    CHECK(cw.chars.count(52) == 1);

    std::printf("session: NPCs the client's game makes by itself are adopted or removed\n");
    // the client's game spawns two characters of its own: one of the same kind as a character the
    // host is about to spawn (it must become its stand-in), one that the host never has (removed)
    FakeChar mine; mine.squad = false; mine.pos = {300, 0, 0}; mine.dest = mine.pos;
    cw.chars[900] = mine; cw.templateOf[900] = "tmpl-60";
    cw.chars[901] = mine; cw.templateOf[901] = "tmpl-unknown";
    const int spawnsBefore = cw.spawns;
    FakeChar unique; unique.squad = false; unique.pos = {140, 0, 60}; unique.dest = unique.pos;
    hw.chars[60] = unique;
    Run({{&host, &hw}, {&cli, &cw}}, 9.0, [&] {
        return cw.chars.count(60) && !cw.chars.count(900) && !cw.chars.count(901) && Dist(cw.chars[60].pos, hw.chars[60].pos) < 1e-3f;
    });
    CHECK(cw.adoptions == 1);
    CHECK(cw.spawns == spawnsBefore);   // adopted, not created
    CHECK(cw.chars.count(60) == 1 && !cw.chars.count(900));
    if (cw.chars.count(60)) CHECK(Dist(cw.chars[60].pos, hw.chars[60].pos) < 1e-3f);
    CHECK(!cw.chars.count(901) && cw.culled == 1);
    CHECK(cli.missingNpcs() == 0);
}

static void TestInventories() {
    std::printf("session: inventories mirror the host; client loot is replayed by the host\n");
    FakeWorld hw, cw;
    SetupHost(hw);
    auto item = [](const char* sid, int q, const char* sec, int x, int y) {
        ItemState i; i.templateSid = sid; i.quantity = q; i.section = sec; i.x = int16_t(x); i.y = int16_t(y); return i;
    };
    hw.chars[1].items = {item("bread", 3, "main", 0, 0)};
    FakeChar corpse; corpse.squad = false; corpse.pos = {130, 0, 0}; corpse.dest = corpse.pos;
    corpse.items = {item("katana", 1, "weapon", 0, 0), item("coins", 50, "main", 2, 0)};
    corpse.vit.flags = kVitDead;
    FakeChar guard; guard.squad = false; guard.pos = {140, 0, 0}; guard.dest = guard.pos;   // conscious NPC
    guard.items = {item("spear", 1, "weapon", 0, 0)};
    hw.chars[21] = guard;
    hw.chars[20] = corpse;
    AtMenu(cw);
    SessionConfig hc; hc.characterPerPlayer = false; hc.port = ++g_port; SessionConfig cc; cc.port = hc.port; cc.name = "C";
    Session host(hw, hc, Now, Quiet("host"));
    Session cli(cw, cc, Now, Quiet("cli"));
    std::string err;
    host.Host(&err);
    CHECK(JoinAndWait(host, hw, cli, cw, hc.port, 5));
    Run({{&host, &hw}, {&cli, &cw}}, 2.0, [&] { return cw.chars[20].items.size() == 2; });
    CHECK(cw.chars[1].items == hw.chars[1].items);
    CHECK(cw.chars[20].items == hw.chars[20].items);
    // the host's character eats a bread: the client sees it
    hw.chars[1].items[0].quantity = 2;
    Run({{&host, &hw}, {&cli, &cw}}, 3.0, [&] { return cw.chars[1].items == hw.chars[1].items; });
    CHECK(cw.chars[1].items == hw.chars[1].items);
    // the client player owns squad member 2 and loots the katana off the corpse (UI drag)
    host.Assign(FakeWorld::H(2), 2);
    Run({{&host, &hw}, {&cli, &cw}}, 1.5);
    auto& c20 = cw.chars[20].items;
    ItemState katana = c20[0].templateSid == "katana" ? c20[0] : c20[1];
    c20.erase(std::remove_if(c20.begin(), c20.end(), [](const ItemState& i) { return i.templateSid == "katana"; }), c20.end());
    katana.section = "main"; katana.x = 4; katana.y = 1;
    cw.chars[2].items.push_back(katana);
    Run({{&host, &hw}, {&cli, &cw}}, 4.0, [&] {
        return hw.chars[2].items.size() == 1 && hw.chars[20].items.size() == 1 && cw.chars[2].items == hw.chars[2].items;
    });
    CHECK(hw.chars[2].items.size() == 1 && hw.chars[2].items[0].templateSid == "katana" && hw.chars[2].items[0].x == 4);
    CHECK(hw.chars[20].items.size() == 1 && hw.chars[20].items[0].templateSid == "coins");
    CHECK(cw.chars[2].items == hw.chars[2].items && cw.chars[20].items == hw.chars[20].items);
    // stealing from a character owned by the host is refused and undone on the client
    auto bread = cw.chars[1].items[0];
    cw.chars[1].items.clear();
    cw.chars[2].items.push_back(bread);
    Run({{&host, &hw}, {&cli, &cw}}, 5.0, [&] { return cw.chars[1].items == hw.chars[1].items && cw.chars[2].items == hw.chars[2].items; });
    CHECK(hw.chars[1].items.size() == 1 && hw.chars[1].items[0].templateSid == "bread");
    CHECK(cw.chars[1].items == hw.chars[1].items && cw.chars[2].items == hw.chars[2].items);
    // so is disarming a conscious NPC: only knocked-out or dead ones can be looted
    Run({{&host, &hw}, {&cli, &cw}}, 1.0, [&] { return cw.chars[21].items == hw.chars[21].items; });
    auto spear = cw.chars[21].items.at(0);
    cw.chars[21].items.clear();
    spear.x = 6;
    cw.chars[2].items.push_back(spear);
    Run({{&host, &hw}, {&cli, &cw}}, 5.0, [&] { return cw.chars[21].items == hw.chars[21].items && cw.chars[2].items == hw.chars[2].items; });
    CHECK(hw.chars[21].items.size() == 1 && hw.chars[21].items[0].templateSid == "spear");
    CHECK(cw.chars[21].items == hw.chars[21].items && cw.chars[2].items == hw.chars[2].items);
    // once knocked out, it can
    hw.chars[21].vit.flags = kVitUnconscious;
    Run({{&host, &hw}, {&cli, &cw}}, 1.0);
    cw.chars[21].items.clear();
    cw.chars[2].items.push_back(spear);
    Run({{&host, &hw}, {&cli, &cw}}, 5.0, [&] { return hw.chars[21].items.empty() && cw.chars[2].items == hw.chars[2].items; });
    CHECK(hw.chars[21].items.empty());
    CHECK(hw.chars[2].items.size() == 2 && cw.chars[2].items == hw.chars[2].items);
}

static void TestManyPlayers() {
    std::printf("session: 1 host + 4 clients joining at once, 120 characters\n");
    FakeWorld hw;
    for (uint32_t i = 1; i <= 120; ++i) { FakeChar c; c.pos = {float(i), 0, 0}; c.dest = c.pos; hw.chars[i] = c; }
    SessionConfig hc; hc.characterPerPlayer = false; hc.port = ++g_port;
    Session host(hw, hc, Now, Quiet("host"));
    std::string err;
    CHECK(host.Host(&err));
    std::vector<std::unique_ptr<FakeWorld>> cws;
    std::vector<std::unique_ptr<Session>> clis;
    for (int k = 0; k < 4; ++k) {
        auto w = std::make_unique<FakeWorld>();
        AtMenu(*w);
        SessionConfig cc; cc.port = hc.port; cc.name = "P" + std::to_string(k);
        clis.push_back(std::make_unique<Session>(*w, cc, Now, Quiet("cli")));
        CHECK(clis.back()->Join("127.0.0.1", hc.port, &err));
        cws.push_back(std::move(w));
    }
    std::vector<std::pair<Session*, FakeWorld*>> all{{&host, &hw}};
    for (size_t k = 0; k < clis.size(); ++k) all.push_back({clis[k].get(), cws[k].get()});
    Run(all, 15.0, [&] {
        for (auto& c : clis) if (c->state() != SessionState::Connected || c->entityCount() != 120) return false;
        return true;
    });
    for (auto& c : clis) CHECK(c->state() == SessionState::Connected && c->entityCount() == 120);
    for (int k = 0; k < 4; ++k)
        for (uint32_t i = 1; i <= 10; ++i) host.Assign(FakeWorld::H(uint32_t(k * 10 + i)), uint8_t(clis[k]->localId()));
    Run(all, 2.0);
    CHECK(!hw.holding);
    for (int k = 0; k < 4; ++k) {
        CHECK(cws[k]->controllable.size() == 10);
        for (auto& h : cws[k]->controllable) {
            Command c; c.kind = CommandKind::MoveTo; c.pos = {float(h.serial), 0, 50.0f + k};
            cws[k]->localOrders.push_back({h, c});
        }
    }
    for (uint32_t i = 41; i <= 120; ++i) hw.chars[i].dest = {float(i), 0, -40};
    Run(all, 10.0, [&] {
        for (auto& w : cws) for (auto& [s, c] : hw.chars) if (Dist(w->chars[s].pos, c.pos) > 1e-3f || Dist(c.pos, c.dest) > 1e-3f) return false;
        return true;
    });
    float worst = 0;
    for (auto& w : cws) for (auto& [s, c] : hw.chars) worst = std::max(worst, Dist(w->chars[s].pos, c.pos));
    std::printf("    worst divergence after settle: %g\n", worst);
    CHECK(worst < 1e-3f);
    for (int k = 0; k < 4; ++k)
        for (uint32_t i = 1; i <= 10; ++i) CHECK(std::fabs(hw.chars[k * 10 + i].pos.z - (50.0f + k)) < 1e-3f);
}

int main() {
    TestWire();
    TestFuzz();
    TestJoinFromMenu();
    TestSessionReplication();
    TestDivergenceIsCorrected();
    TestRejections();
    TestWorldAuthority();
    TestOwnCharacter();
    TestSpawnReplication();
    TestInventories();
    TestManyPlayers();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
