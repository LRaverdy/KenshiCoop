// KenshiCoop tests: protocol robustness + full host/client sessions over real loopback ENet,
// using a fake world. Exit code 0 = all passed.
#include <algorithm>
#include <chrono>
#include <memory>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
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

// ---------------------------------------------------------------- fake world
struct FakeChar {
    Vec3 pos, dest;
    Quat rot;
    bool squad = true;
    EntityVitals vit;   // netId unused
};
struct FakeWorld : IWorld {
    bool ready = true;
    uint64_t build = 111, mods = 222, fp = 333;
    std::map<uint32_t, FakeChar> chars;  // key = handle.serial
    std::vector<std::pair<Handle, Command>> localOrders;
    TimeState time;
    bool client = false, active = false;
    std::vector<Handle> controllable;
    float speed = 50.0f;  // units per second

    static Handle H(uint32_t serial) { Handle h; h.type = 3; h.index = serial; h.serial = serial; return h; }

    bool Ready() override { return ready; }
    uint64_t Fingerprint() override { return fp; }
    uint64_t GameBuild() override { return build; }
    uint64_t ModsHash() override { return mods; }
    void PlayerCharacters(std::vector<Handle>& out) override { for (auto& [s, c] : chars) if (c.squad) out.push_back(H(s)); }
    void NearbyCharacters(const std::vector<Vec3>& centers, float radius, std::vector<Handle>& out) override {
        for (auto& [s, c] : chars) {
            if (c.squad) continue;
            for (auto& ctr : centers) {
                const float dx = c.pos.x - ctr.x, dy = c.pos.y - ctr.y, dz = c.pos.z - ctr.z;
                if (dx * dx + dy * dy + dz * dz <= radius * radius) { out.push_back(H(s)); break; }
            }
        }
    }
    bool ReadVitals(const Handle& h, EntityVitals& out) override {
        auto it = chars.find(h.serial);
        if (it == chars.end()) return false;
        out = it->second.vit;
        return true;
    }
    void ApplyVitals(const Handle& h, const EntityVitals& v) override {
        auto it = chars.find(h.serial);
        if (it != chars.end()) { it->second.vit = v; it->second.vit.netId = 0; }
    }
    bool Exists(const Handle& h) override { return chars.count(h.serial) != 0; }
    bool Read(const Handle& h, EntityState& out) override {
        auto it = chars.find(h.serial);
        if (it == chars.end()) return false;
        out.pos = it->second.pos; out.rot = it->second.rot; out.dest = it->second.dest; out.flags = 0;
        return true;
    }
    void Apply(const Handle& h, const EntityState& target, const EntityState& latest) override {
        auto it = chars.find(h.serial);
        if (it == chars.end()) return;
        it->second.pos = target.pos; it->second.rot = target.rot; it->second.dest = latest.dest;
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
    void SetRole(bool c, bool a) override { client = c; active = a; }
    void SetControllable(const std::vector<Handle>& h) override { controllable = h; }

    // host-side simulation: walk toward destination
    void Simulate(float dt) {
        if (client) return;  // clients never simulate replicated characters
        for (auto& [s, c] : chars) {
            const float dx = c.dest.x - c.pos.x, dy = c.dest.y - c.pos.y, dz = c.dest.z - c.pos.z;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float step = speed * dt;
            if (d <= step || d < 1e-4f) c.pos = c.dest;
            else { c.pos.x += dx / d * step; c.pos.y += dy / d * step; c.pos.z += dz / d * step; }
        }
    }
};

static float Dist(const Vec3& a, const Vec3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

// Runs host+clients for `seconds` of real time at ~100 fps.
static void Run(std::vector<std::pair<Session*, FakeWorld*>> s, double seconds, std::function<bool()> until = nullptr) {
    const double end = Now() + seconds;
    double last = Now();
    while (Now() < end) {
        const double n = Now();
        for (auto& [sess, w] : s) { w->Simulate(float(n - last)); sess->Tick(); }
        last = n;
        if (until && until()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

static uint16_t g_port = 31337;

static void SetupWorlds(FakeWorld& host, FakeWorld& cli) {
    for (uint32_t i = 1; i <= 3; ++i) {
        FakeChar c;
        c.pos = {float(i) * 100, 0, 0};
        c.dest = c.pos;
        host.chars[i] = c;
        cli.chars[i] = c;
    }
}

// ---------------------------------------------------------------- tests
static void TestWire() {
    std::printf("wire/protocol roundtrips\n");
    Writer w;
    w.varint(0); w.varint(127); w.varint(128); w.varint(0xFFFFFFFFFFFFFFFFull); w.str("héllo"); w.f32(1.5f);
    Reader r(w.data(), w.size());
    CHECK(r.varint() == 0); CHECK(r.varint() == 127); CHECK(r.varint() == 128);
    CHECK(r.varint() == 0xFFFFFFFFFFFFFFFFull); CHECK(r.str(100) == "héllo"); CHECK(r.f32() == 1.5f);
    CHECK(r.ok() && r.atEnd());

    // reads past the end fail and latch
    Reader r2(w.data(), 2);
    r2.u64();
    CHECK(!r2.ok());
    // string length beyond max fails without allocating
    Writer w3; w3.varint(1000000); Reader r3(w3.data(), w3.size()); CHECK(r3.str(10).empty()); CHECK(!r3.ok());
    // NaN floats are rejected
    Writer w4; w4.f32(std::nanf("")); Reader r4(w4.data(), w4.size()); r4.f32(); CHECK(!r4.ok());

    Hello h; h.gameBuild = 5; h.modsHash = 6; h.worldHash = 7; h.name = "Beep";
    Writer hw; Encode(hw, h);
    Reader hr(hw.data(), hw.size());
    CHECK(PeekType(hr) == Msg::Hello);
    Hello h2; CHECK(Decode(hr, h2)); CHECK(h2.name == "Beep" && h2.worldHash == 7 && h2.modsHash == 6);

    // snapshot splitting: every packet within budget, all entities survive
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
        for (auto& e : d.entities) {
            const uint32_t i = e.netId / 1000;
            CHECK(e.pos.x == float(i) && e.flags == uint8_t(i));
        }
        total += d.entities.size();
    }
    CHECK(total == 300);

    // quaternion packing accuracy
    std::mt19937 rng(1);
    std::uniform_real_distribution<float> u(-1, 1);
    float worst = 0;
    for (int i = 0; i < 2000; ++i) {
        Quat q{u(rng), u(rng), u(rng), u(rng)};
        const float l = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
        if (l < 1e-3f) continue;
        q.w /= l; q.x /= l; q.y /= l; q.z /= l;
        Quat p = UnpackQuat(PackQuat(q));
        const float dot = std::fabs(q.w * p.w + q.x * p.x + q.y * p.y + q.z * p.z);
        worst = std::max(worst, 1.0f - dot);
    }
    CHECK(worst < 1e-4f);
}

static void TestFuzz() {
    std::printf("decoder fuzzing (random + mutated packets)\n");
    std::mt19937 rng(42);
    // seeds: one valid instance of every message
    std::vector<std::vector<uint8_t>> seeds;
    auto add = [&](auto&& fn) { Writer w; fn(w); seeds.push_back(w.vec()); };
    add([](Writer& w) { Hello h; h.name = "abc"; Encode(w, h); });
    add([](Writer& w) { Welcome m; m.players = {{1, "a"}, {2, "b"}}; Encode(w, m); });
    add([](Writer& w) { Encode(w, Reject{RejectReason::Full}); });
    add([](Writer& w) { Encode(w, PlayerInfo{3, "x"}); });
    add([](Writer& w) { Encode(w, Chat{1, "hi"}); });
    add([](Writer& w) { Bind b; b.netId = 5; b.handle.serial = 9; Encode(w, b); });
    add([](Writer& w) { Encode(w, Unbind{5}); });
    add([](Writer& w) { Command c; c.netId = 3; Encode(w, c); });
    add([](Writer& w) { Encode(w, TimeState{2.0f, true, 10.0}); });
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
        }
    };
    for (int i = 0; i < 200000; ++i) {
        std::vector<uint8_t> p;
        if (i % 2) {
            p.resize(rng() % 64);
            for (auto& b : p) b = uint8_t(rng());
            if (!p.empty()) p[0] = uint8_t(1 + rng() % 14);
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

static Session::LogFn Quiet(const char* tag, bool verbose = false) {
    return [tag, verbose](const std::string& s) { if (verbose) std::printf("    [%s] %s\n", tag, s.c_str()); };
}

static void TestSessionReplication() {
    std::printf("session: handshake, ownership, commands, convergence\n");
    FakeWorld hw, cw;
    SetupWorlds(hw, cw);
    SessionConfig hc; hc.name = "Host"; hc.port = ++g_port;
    SessionConfig cc; cc.name = "Client"; cc.port = hc.port;
    Session host(hw, hc, Now, Quiet("host"));
    Session cli(cw, cc, Now, Quiet("cli"));
    std::string err;
    CHECK(host.Host(&err));
    CHECK(cli.Join("127.0.0.1", hc.port, &err));
    Run({{&host, &hw}, {&cli, &cw}}, 5.0, [&] { return cli.state() == SessionState::Connected && cli.entityCount() == 3; });
    CHECK(cli.state() == SessionState::Connected);
    CHECK(cli.localId() == 2);
    CHECK(cli.entityCount() == 3);
    CHECK(cli.missingSquad() == 0);
    CHECK(cw.client && cw.active);
    CHECK(host.players().size() == 1);
    CHECK(cw.controllable.empty());     // nothing assigned yet
    CHECK(hw.controllable.size() == 3); // host controls everything by default

    // hand character #2 to the client
    host.Assign(FakeWorld::H(2), 2);
    Run({{&host, &hw}, {&cli, &cw}}, 2.0, [&] { return cw.controllable.size() == 1; });
    CHECK(cw.controllable.size() == 1 && cw.controllable[0].serial == 2);
    CHECK(hw.controllable.size() == 2);

    // client orders its own character: the host executes it, the client converges
    Command c; c.kind = CommandKind::MoveTo; c.pos = {200, 0, 80};
    cw.localOrders.push_back({FakeWorld::H(2), c});
    // client orders a character it does NOT own: must be ignored
    Command bad; bad.kind = CommandKind::MoveTo; bad.pos = {-500, 0, 0};
    cw.localOrders.push_back({FakeWorld::H(1), bad});
    // the host moves its own character
    hw.chars[3].dest = {300, 0, -60};

    Run({{&host, &hw}, {&cli, &cw}}, 6.0, [&] {
        return Dist(hw.chars[2].pos, c.pos) < 1e-3f && Dist(hw.chars[3].pos, hw.chars[3].dest) < 1e-3f &&
               Dist(cw.chars[2].pos, hw.chars[2].pos) < 1e-3f && Dist(cw.chars[3].pos, hw.chars[3].pos) < 1e-3f;
    });
    CHECK(Dist(hw.chars[2].pos, c.pos) < 1e-3f);          // host executed the client's order
    CHECK(Dist(hw.chars[1].pos, {100, 0, 0}) < 1e-3f);    // unowned order was rejected
    for (uint32_t i = 1; i <= 3; ++i)                      // zero divergence once settled
        CHECK(Dist(cw.chars[i].pos, hw.chars[i].pos) < 1e-3f);

    // time state follows the host
    hw.time = TimeState{3.0f, false};
    Run({{&host, &hw}, {&cli, &cw}}, 2.0, [&] { return cw.time.speed == 3.0f; });
    CHECK(cw.time.speed == 3.0f);

    // chat
    cli.SendChat("hello\x01 there");
    Run({{&host, &hw}, {&cli, &cw}}, 2.0, [&] { return !host.chatLog().empty() && host.chatLog().back() == "Client: hello there"; });
    CHECK(!host.chatLog().empty() && host.chatLog().back() == "Client: hello there");

    // disconnect: the client's character returns to the host
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
    SetupWorlds(hw, cw);
    SessionConfig hc; hc.port = ++g_port; SessionConfig cc; cc.port = hc.port; cc.name = "C";
    Session host(hw, hc, Now, Quiet("host"));
    Session cli(cw, cc, Now, Quiet("cli"));
    std::string err;
    host.Host(&err);
    cli.Join("127.0.0.1", hc.port, &err);
    Run({{&host, &hw}, {&cli, &cw}}, 5.0, [&] { return cli.entityCount() == 3; });
    // something locally corrupts the client's copy (lag spike, local physics shove...)
    cw.chars[1].pos = {9999, 9999, 9999};
    Run({{&host, &hw}, {&cli, &cw}}, 3.0, [&] { return Dist(cw.chars[1].pos, hw.chars[1].pos) < 1e-3f; });
    CHECK(Dist(cw.chars[1].pos, hw.chars[1].pos) < 1e-3f);
}

static void TestRejections() {
    std::printf("session: mismatched game / mods / save are refused\n");
    struct Case { const char* what; std::function<void(FakeWorld&)> mutate; RejectReason expect; };
    std::vector<Case> cases = {
        {"build", [](FakeWorld& w) { w.build = 1; }, RejectReason::GameMismatch},
        {"mods", [](FakeWorld& w) { w.mods = 1; }, RejectReason::ModsMismatch},
        {"world", [](FakeWorld& w) { w.fp = 1; }, RejectReason::WorldMismatch},
    };
    for (auto& tc : cases) {
        FakeWorld hw, cw;
        SetupWorlds(hw, cw);
        tc.mutate(cw);
        SessionConfig hc; hc.port = ++g_port; SessionConfig cc; cc.port = hc.port; cc.name = "C";
        Session host(hw, hc, Now, Quiet("host"));
        Session cli(cw, cc, Now, Quiet("cli"));
        std::string err;
        host.Host(&err);
        cli.Join("127.0.0.1", hc.port, &err);
        Run({{&host, &hw}, {&cli, &cw}}, 5.0, [&] { return cli.state() == SessionState::Failed; });
        CHECK(cli.state() == SessionState::Failed);
        CHECK(cli.lastError().find(ToString(tc.expect)) != std::string::npos);
        CHECK(!cw.active);
        CHECK(host.players().empty());
    }
    // invalid name
    {
        FakeWorld hw, cw; SetupWorlds(hw, cw);
        SessionConfig hc; hc.port = ++g_port; SessionConfig cc; cc.port = hc.port; cc.name = " bad";
        Session host(hw, hc, Now, Quiet("host")); Session cli(cw, cc, Now, Quiet("cli"));
        std::string err; host.Host(&err); cli.Join("127.0.0.1", hc.port, &err);
        Run({{&host, &hw}, {&cli, &cw}}, 5.0, [&] { return cli.state() == SessionState::Failed; });
        CHECK(cli.lastError().find(ToString(RejectReason::BadName)) != std::string::npos);
    }
    // cannot host or join without a loaded world
    {
        FakeWorld w; w.ready = false; SessionConfig c; c.port = ++g_port;
        Session s(w, c, Now, Quiet("x")); std::string err;
        CHECK(!s.Host(&err)); CHECK(!s.Join("127.0.0.1", c.port, &err));
    }
    // nobody listening: join times out cleanly
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
    SetupWorlds(hw, cw);
    // an NPC near the squad, one far away; both exist in both worlds (same save)
    FakeChar near; near.squad = false; near.pos = {150, 0, 30}; near.dest = near.pos; near.vit.blood = 100;
    FakeChar far; far.squad = false; far.pos = {90000, 0, 0}; far.dest = far.pos;
    hw.chars[10] = near; cw.chars[10] = near;
    hw.chars[11] = far;  cw.chars[11] = far;
    for (uint32_t i = 1; i <= 3; ++i) { hw.chars[i].vit.blood = 100; cw.chars[i].vit.blood = 100; hw.chars[i].vit.parts = {{50, 0, 0}, {40, 0, 0}}; cw.chars[i].vit.parts = hw.chars[i].vit.parts; }
    SessionConfig hc; hc.port = ++g_port; hc.interestRadius = 1000; SessionConfig cc; cc.port = hc.port; cc.name = "C";
    Session host(hw, hc, Now, Quiet("host"));
    Session cli(cw, cc, Now, Quiet("cli"));
    std::string err;
    host.Host(&err);
    cli.Join("127.0.0.1", hc.port, &err);
    Run({{&host, &hw}, {&cli, &cw}}, 5.0, [&] { return cli.entityCount() == 4; });
    CHECK(cli.entityCount() == 4);          // 3 squad + the near NPC, not the far one
    CHECK(cli.npcCount() == 1);

    // the host's NPC walks; the client copy follows exactly
    hw.chars[10].dest = {300, 0, 60};
    // the client locally "hurts" a squad member (stray local simulation); the host's value must win
    cw.chars[2].vit.blood = 3; cw.chars[2].vit.flags = kVitDead;
    // the host's character really takes damage
    hw.chars[1].vit.parts[0].flesh = 12; hw.chars[1].vit.flags = kVitUnconscious; hw.chars[1].vit.koTimer = 30;
    hw.time = TimeState{1.0f, true, 1234.5};
    Run({{&host, &hw}, {&cli, &cw}}, 6.0, [&] {
        return Dist(cw.chars[10].pos, hw.chars[10].pos) < 1e-3f && Dist(hw.chars[10].pos, {300, 0, 60}) < 1e-3f &&
               cw.chars[2].vit.blood == 100 && cw.chars[1].vit.parts[0].flesh == 12 && cw.time.paused;
    });
    CHECK(Dist(cw.chars[10].pos, hw.chars[10].pos) < 1e-3f);
    CHECK(cw.chars[2].vit.blood == 100 && cw.chars[2].vit.flags == 0);   // local death undone
    CHECK(cw.chars[1].vit.parts[0].flesh == 12 && (cw.chars[1].vit.flags & kVitUnconscious));
    CHECK(cw.time.paused && cw.time.gameHours == 1234.5);

    // the NPC wanders out of range: it is unbound on the client
    hw.chars[10].pos = {50000, 0, 0}; hw.chars[10].dest = hw.chars[10].pos;
    Run({{&host, &hw}, {&cli, &cw}}, 3.0, [&] { return cli.npcCount() == 0; });
    CHECK(cli.npcCount() == 0);
    // clients cannot command NPCs
    Command c; c.kind = CommandKind::MoveTo; c.pos = {0, 0, 0};
    cw.localOrders.push_back({FakeWorld::H(11), c});
    Run({{&host, &hw}, {&cli, &cw}}, 0.5);
    CHECK(Dist(hw.chars[11].pos, {90000, 0, 0}) < 1e-3f);
}

static void TestManyPlayers() {
    std::printf("session: 1 host + 4 clients, 120 characters\n");
    FakeWorld hw;
    std::vector<std::unique_ptr<FakeWorld>> cws;
    for (uint32_t i = 1; i <= 120; ++i) { FakeChar c; c.pos = {float(i), 0, 0}; c.dest = c.pos; hw.chars[i] = c; }
    SessionConfig hc; hc.port = ++g_port;
    Session host(hw, hc, Now, Quiet("host"));
    std::string err;
    CHECK(host.Host(&err));
    std::vector<std::unique_ptr<Session>> clis;
    for (int k = 0; k < 4; ++k) {
        auto w = std::make_unique<FakeWorld>();
        w->chars = hw.chars;
        SessionConfig cc; cc.port = hc.port; cc.name = "P" + std::to_string(k);
        clis.push_back(std::make_unique<Session>(*w, cc, Now, Quiet("cli")));
        CHECK(clis.back()->Join("127.0.0.1", hc.port, &err));
        cws.push_back(std::move(w));
    }
    std::vector<std::pair<Session*, FakeWorld*>> all{{&host, &hw}};
    for (size_t k = 0; k < clis.size(); ++k) all.push_back({clis[k].get(), cws[k].get()});
    Run(all, 8.0, [&] {
        for (auto& c : clis) if (c->entityCount() != 120) return false;
        return true;
    });
    for (auto& c : clis) CHECK(c->entityCount() == 120);
    // give each client 10 characters and have everyone move all of theirs at once
    for (int k = 0; k < 4; ++k)
        for (uint32_t i = 1; i <= 10; ++i) host.Assign(FakeWorld::H(uint32_t(k * 10 + i)), uint8_t(clis[k]->localId()));
    Run(all, 2.0);
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
    TestSessionReplication();
    TestDivergenceIsCorrected();
    TestRejections();
    TestWorldAuthority();
    TestManyPlayers();
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
