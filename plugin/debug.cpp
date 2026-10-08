#include "debug.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "hooks.h"
#include "kenshi.h"

namespace kcp {

namespace {

double g_nextPoll = 0;
kc::Handle lastSpawned_;

std::string Key(const kc::Handle& h) {
    char b[96];
    snprintf(b, sizeof(b), "%u:%u:%u:%u:%u", h.type, h.container, h.containerSerial, h.index, h.serial);
    return b;
}

std::vector<kc::Handle> SortedSquad(KenshiWorld& w) {
    std::vector<kc::Handle> hs;
    w.PlayerCharacters(hs);
    std::sort(hs.begin(), hs.end(), [](const kc::Handle& a, const kc::Handle& b) {
        return std::tie(a.index, a.serial) < std::tie(b.index, b.serial);
    });
    return hs;
}

const char* StateStr(kc::SessionState s) {
    switch (s) {
    case kc::SessionState::Idle: return "idle";
    case kc::SessionState::Hosting: return "hosting";
    case kc::SessionState::Connecting: return "connecting";
    case kc::SessionState::Handshake: return "handshake";
    case kc::SessionState::Downloading: return "downloading";
    case kc::SessionState::Loading: return "loading";
    case kc::SessionState::Connected: return "connected";
    case kc::SessionState::Failed: return "failed";
    }
    return "?";
}

const kc::Session* g_dumpSession = nullptr;

void DumpCharacter(std::ostream& o, const char* tag, KenshiWorld& w, const kc::Handle& h) {
    kc::EntityState st;
    kc::EntityVitals v;
    const bool ok = w.Read(h, st);
    const bool vok = w.ReadVitals(h, v);
    const kc::Handle hostHandle = w.HostHandleOf(h);   // stand-ins are reported under the host's handle
    o << tag << ' ' << Key(hostHandle) << " read=" << ok << " standin=" << (hostHandle != h);
    if (ok) {
        o << " pos=" << st.pos.x << ',' << st.pos.y << ',' << st.pos.z << " dest=" << st.dest.x << ',' << st.dest.y << ',' << st.dest.z
          << " flags=" << unsigned(st.flags);
    }
    kc::Handle ct;
    if (kenshi::Character* ch = w.Find(h); ch && kenshi::ReadCombat(ch, ct)) o << " combat=" << Key(w.HostHandleOf(ct));
    kc::EntityState latest, rendered;
    if (g_dumpSession && !g_dumpSession->isHost() && g_dumpSession->TargetOf(hostHandle, latest, rendered))
        o << " latest=" << latest.pos.x << ',' << latest.pos.y << ',' << latest.pos.z << " lflags=" << unsigned(latest.flags)
          << " rendered=" << rendered.pos.x << ',' << rendered.pos.y << ',' << rendered.pos.z;
    if (vok) {
        o << " blood=" << v.blood << " ko=" << v.koTimer << " vflags=" << unsigned(v.flags) << " parts=";
        for (size_t i = 0; i < v.parts.size(); ++i) o << (i ? ";" : "") << v.parts[i].flesh << '/' << v.parts[i].stun;
    }
    o << '\n';
}

std::string Execute(kc::Session& s, KenshiWorld& w, bool live, std::istringstream& in, const std::string& cmd) {
    std::string err;
    if (cmd == "echo") return "ok";
    if (cmd == "status") {
        std::ostringstream o;
        o << "ok state=" << StateStr(s.state()) << " live=" << live << " ready=" << w.Ready() << " id=" << int(s.localId())
          << " entities=" << s.entityCount() << " npcs=" << s.npcCount() << " missingNpcs=" << s.missingNpcs()
          << " joining=" << s.joiningPlayers() << " busy=" << kenshi::SaveManagerBusy() << " error=\"" << s.lastError() << "\"";
        return o.str();
    }
    if (cmd == "load") {
        std::string slot;
        in >> slot;
        return kenshi::RequestLoad(slot) ? "ok" : "err the game refused to load " + slot;
    }
    if (cmd == "host") return s.Host(&err) ? "ok" : "err " + err;
    if (cmd == "join") {
        std::string addr = "127.0.0.1";
        int port = kc::kDefaultPort;
        in >> addr >> port;
        return s.Join(addr, uint16_t(port), &err) ? "ok" : "err " + err;
    }
    if (cmd == "leave") { s.Leave(); return "ok"; }

    // everything below touches the world
    if (!live || !w.Ready()) return "err no live world";
    if (cmd == "give") {   // give <playerId> <index|all>
        int pid = 0;
        std::string which;
        in >> pid >> which;
        auto squad = SortedSquad(w);
        int n = 0;
        for (size_t i = 0; i < squad.size(); ++i)
            if (which == "all" || which == std::to_string(i)) { s.Assign(squad[i], uint8_t(pid)); ++n; }
        return n ? "ok " + std::to_string(n) : "err no such squad member";
    }
    if (cmd == "move" || cmd == "moverel") {   // move <index> <x> <z> | moverel <index> <dx> <dz>
        size_t idx = 0;
        float x = 0, z = 0;
        in >> idx >> x >> z;
        auto squad = SortedSquad(w);
        if (idx >= squad.size()) return "err no such squad member";
        kenshi::Character* c = w.FindSquad(squad[idx]);
        kc::Vec3 p;
        if (!c || !kenshi::GetPosition(c, p)) return "err cannot read position";
        kc::Command m;
        m.kind = kc::CommandKind::MoveTo;
        m.pos = cmd == "move" ? kc::Vec3{x, p.y, z} : kc::Vec3{p.x + x, p.y, p.z + z};
        if (s.isHost()) { HostCallScope scope; return CallPlayerMoveOrder(c, m.pos) ? "ok" : "err order failed"; }
        w.QueueLocalOrder(squad[idx], m);   // exactly what a right click does on a client
        return "ok queued";
    }
    if (cmd == "teleport") {   // teleport <index> <x> <y> <z>   (host only: moves a character instantly)
        size_t idx = 0;
        float x = 0, y = 0, z = 0;
        in >> idx >> x >> y >> z;
        auto squad = SortedSquad(w);
        if (!s.isHost() && s.state() != kc::SessionState::Idle) return "err host only";
        if (idx >= squad.size()) return "err no such squad member";
        kenshi::Character* c = w.FindSquad(squad[idx]);
        kc::Quat q;
        kenshi::GetRotation(c, q);
        HostCallScope scope;
        return kenshi::Teleport(c, {x, y, z}, q) ? "ok" : "err teleport failed";
    }
    if (cmd == "probe") {   // probe <index> <dx> <dz> <simple|teleport>: try a position method, report what sticks
        size_t idx = 0;
        float dx = 0, dz = 0;
        std::string mode;
        in >> idx >> dx >> dz >> mode;
        auto squad = SortedSquad(w);
        if (idx >= squad.size()) return "err no such squad member";
        kenshi::Character* c = w.FindSquad(squad[idx]);
        kc::Vec3 p;
        kc::Quat q;
        if (!c || !kenshi::GetPosition(c, p) || !kenshi::GetRotation(c, q)) return "err read";
        const kc::Vec3 to{p.x + dx, p.y, p.z + dz};
        HostCallScope scope;
        const bool ok = mode == "teleport" ? kenshi::Teleport(c, to, q) : kenshi::SetPositionSimple(c, to);
        kc::Vec3 after;
        kenshi::GetPosition(c, after);
        std::ostringstream o;
        o << "ok call=" << ok << " before=" << p.x << ',' << p.z << " target=" << to.x << ',' << to.z << " after=" << after.x << ',' << after.z;
        return o.str();
    }
    if (cmd == "pos") {   // pos <index>
        size_t idx = 0;
        in >> idx;
        auto squad = SortedSquad(w);
        if (idx >= squad.size()) return "err no such squad member";
        kc::Vec3 p;
        if (!kenshi::GetPosition(w.FindSquad(squad[idx]), p)) return "err read";
        std::ostringstream o;
        o << "ok " << p.x << ',' << p.y << ',' << p.z;
        return o.str();
    }
    if (cmd == "spawnnpc") {   // spawnnpc <dx> <dz>: host creates an NPC (copy of a nearby one) next to squad[0]
        float dx = 0, dz = 0;
        in >> dx >> dz;
        auto squad = SortedSquad(w);
        kc::Vec3 base;
        if (squad.empty() || !kenshi::GetPosition(w.FindSquad(squad[0]), base)) return "err no squad";
        std::vector<kenshi::Character*> all;
        kenshi::ActiveCharacters(all);
        kc::SpawnInfo info;
        bool found = false;
        for (kenshi::Character* c : all) {
            kc::Handle h;
            if (!kenshi::GetHandle(c, h) || w.FindSquad(h) || kenshi::IsDead(c)) continue;
            if (kenshi::ReadSpawnSource(c, info)) { found = true; break; }
        }
        if (!found) return "err no NPC to copy";
        std::string e;
        HostCallScope scope;
        kenshi::Character* c = kenshi::CreateCharacter(info, {base.x + dx, base.y, base.z + dz}, &e);
        kc::Handle h;
        if (!c || !kenshi::GetHandle(c, h)) return "err " + e;
        lastSpawned_ = h;
        return "ok " + Key(h) + " " + info.templateSid;
    }
    if (cmd == "fight") {   // fight <index>: squad member engages the NPC created by the last spawnnpc
        size_t idx = 0;
        in >> idx;
        auto squad = SortedSquad(w);
        kenshi::Character* t = w.Find(lastSpawned_);
        if (idx >= squad.size() || !t) return "err need a squad member and a spawned NPC";
        HostCallScope scope;
        return kenshi::StartCombat(w.FindSquad(squad[idx]), lastSpawned_) ? "ok" : "err initCombatMode failed";
    }
    if (cmd == "wake") {   // wake: the NPC created by the last spawnnpc gets up immediately
        kenshi::Character* c = w.Find(lastSpawned_);
        if (!c) return "err no spawned NPC";
        HostCallScope scope;
        return kenshi::StandUp(c) ? "ok" : "err";
    }
    if (cmd == "npcstate") {   // npcstate: posture/vitals of the last spawned NPC, as this machine sees it
        kenshi::Character* c = w.Find(lastSpawned_);
        if (!c) return "err no spawned NPC";
        std::ostringstream o;
        o << "ok down=" << kenshi::IsDown(c) << " unconscious=" << kenshi::IsUnconscious(c) << " dead=" << kenshi::IsDead(c);
        return o.str();
    }
    if (cmd == "ko" || cmd == "kill") {   // ko|kill: knock out / kill the NPC created by the last spawnnpc
        kenshi::Character* c = w.Find(lastSpawned_);
        if (!c) return "err no spawned NPC";
        HostCallScope scope;
        const bool ok = cmd == "ko" ? kenshi::CallKnockout(c) : kenshi::CallDeclareDead(c);
        return ok ? "ok " + Key(lastSpawned_) : "err call failed";
    }
    if (cmd == "pause") { int on = 1; in >> on; return kenshi::CallUserPause(on != 0) ? "ok" : "err"; }
    if (cmd == "speed") { float v = 1; in >> v; return kenshi::CallSetFrameSpeed(v) ? "ok" : "err"; }
    if (cmd == "state") {   // state <file> [radius]
        std::string file;
        float radius = 3000;
        in >> file >> radius;
        g_dumpSession = &s;
        std::ofstream o(file, std::ios::trunc);
        if (!o) return "err cannot write " + file;
        o.setf(std::ios::fixed);
        o.precision(3);
        const kc::TimeState t = w.GetTime();
        o << "session " << StateStr(s.state()) << " id=" << int(s.localId()) << '\n';
        o << "time speed=" << t.speed << " paused=" << t.paused << " hours=" << t.gameHours << '\n';
        std::vector<kc::Vec3> centers;
        for (const auto& h : SortedSquad(w)) {
            DumpCharacter(o, "squad", w, h);
            kc::EntityState st;
            if (w.Read(h, st)) centers.push_back(st.pos);
        }
        s.ForEachEntity([&](uint32_t netId, const kc::Handle& h, uint8_t owner, bool squad, bool present) {
            o << "entity " << netId << ' ' << Key(h) << " owner=" << int(owner) << " squad=" << squad << " present=" << present << '\n';
        });
        std::vector<kenshi::Character*> all;
        kenshi::ActiveCharacters(all);
        for (kenshi::Character* c : all) {
            kc::Handle h;
            kc::Vec3 p;
            if (!kenshi::GetHandle(c, h) || !kenshi::GetPosition(c, p)) continue;
            bool inRange = false;
            for (const auto& ctr : centers) {
                const float dx = ctr.x - p.x, dy = ctr.y - p.y, dz = ctr.z - p.z;
                if (dx * dx + dy * dy + dz * dz <= radius * radius) { inRange = true; break; }
            }
            if (inRange) DumpCharacter(o, "char", w, h);
        }
        o << "end\n";
        return "ok";
    }
    return "err unknown command " + cmd;
}

} // namespace

void DebugPoll(kc::Session& s, KenshiWorld& w, bool live) {
    const double now = NowSeconds();
    if (now < g_nextPoll) return;
    g_nextPoll = now + 0.1;
    const std::wstring dir = GameDir();
    const std::wstring pid = std::to_wstring(GetCurrentProcessId());
    const std::wstring cmdPath = dir + L"kcp_cmd_" + pid + L".txt";
    std::ifstream f(cmdPath);
    if (!f) return;
    std::vector<std::string> lines;
    for (std::string line; std::getline(f, line);) if (!line.empty()) lines.push_back(line);
    f.close();
    DeleteFileW(cmdPath.c_str());
    std::ofstream out(dir + L"kcp_out_" + pid + L".txt", std::ios::app);
    for (const std::string& line : lines) {
        std::istringstream in(line);
        std::string id, cmd;
        in >> id >> cmd;
        std::string result;
        try {
            result = Execute(s, w, live, in, cmd);
        } catch (const std::exception& e) {
            result = std::string("err exception ") + e.what();
        }
        Log("debug: %s -> %s", line.c_str(), result.c_str());
        out << id << ' ' << result << '\n';
    }
}

} // namespace kcp
