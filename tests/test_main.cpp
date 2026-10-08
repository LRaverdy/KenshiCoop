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
    void PlayerCharacters(std::vector<Handle>& out) override { for (auto& [s, c] : chars) if (c.squad) out.push_back(H(s)); }
    void NearbyCharacters(const std::vector<Vec3>& centers, float radius, std::vector<Handle>& out) override {
        for (auto& [s, c] : chars) {
            if (c.squad) continue;
            if (radius <= 0) { out.push_back(H(s)); continue; }   // 0 = every active character
            for (auto& ctr : centers) if (Dist(c.pos, ctr) <= radius) { out.push_back(H(s)); break; }
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
    bool ReadSpawnInfo(const Handle& h, SpawnInfo& out) override {
        auto it = chars.find(h.serial);
        if (it == chars.end() || it->second.squad) return false;
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
        }
    };
    for (int i = 0; i < 300000; ++i) {
        std::vector<uint8_t> p;
        if (i % 2) {
            p.resize(rng() % 64);
            for (auto& b : p) b = uint8_t(rng());
            if (!p.empty()) p[0] = uint8_t(1 + rng() % 18);
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
    SessionConfig hc; hc.name = "Host"; hc.port = ++g_port;
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

static void TestSessionReplication() {
    std::printf("session: ownership, commands, convergence, chat, leave\n");
    FakeWorld hw, cw;
    SetupHost(hw);
    AtMenu(cw);
    SessionConfig hc; hc.name = "Host"; hc.port = ++g_port;
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
    SessionConfig hc; hc.port = ++g_port; SessionConfig cc; cc.port = hc.port; cc.name = "C";
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
        SessionConfig hc; hc.port = ++g_port; SessionConfig cc; cc.port = hc.port; cc.name = "C"; cc.loadTimeout = 4.0;
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
        SessionConfig hc; hc.port = ++g_port; SessionConfig cc; cc.port = hc.port; cc.name = " bad";
        Session host(hw, hc, Now, Quiet("host")); Session cli(cw, cc, Now, Quiet("cli"));
        std::string err; host.Host(&err); cli.Join("127.0.0.1", hc.port, &err);
        Run({{&host, &hw}, {&cli, &cw}}, 5.0, [&] { return cli.state() == SessionState::Failed; });
        CHECK(cli.lastError().find(ToString(RejectReason::BadName)) != std::string::npos);
    }
    {
        // the host's save fails: the joiner is told, the host is released
        FakeWorld hw, cw; SetupHost(hw); AtMenu(cw);
        hw.exportFrames = -1000;  // PollWorldExport -> Failed
        SessionConfig hc; hc.port = ++g_port; SessionConfig cc; cc.port = hc.port; cc.name = "C";
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
    SessionConfig hc; hc.port = ++g_port; hc.interestRadius = 1000; SessionConfig cc; cc.port = hc.port; cc.name = "C";
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
    SessionConfig hc; hc.port = ++g_port; SessionConfig cc; cc.port = hc.port; cc.name = "C";
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
}

static void TestManyPlayers() {
    std::printf("session: 1 host + 4 clients joining at once, 120 characters\n");
    FakeWorld hw;
    for (uint32_t i = 1; i <= 120; ++i) { FakeChar c; c.pos = {float(i), 0, 0}; c.dest = c.pos; hw.chars[i] = c; }
    SessionConfig hc; hc.port = ++g_port;
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
    TestSpawnReplication();
    TestManyPlayers();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
