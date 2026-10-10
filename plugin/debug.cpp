#include "debug.h"
#include "admin.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "hooks.h"
#include "map.h"
#include "ranged.h"
#include "steam_link.h"
#include "kenshi.h"
#include "host_console.h"

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
    if (kenshi::Character* ch = w.Find(h)) o << " class=" << std::hex << kenshi::ClassRvas(ch) << std::dec;
    if (std::string nm; kenshi::Character* ch = w.Find(h)) {   // the character's name ('_' for spaces): who is who with 3+ players
        if (kenshi::CharacterName(ch, nm) && !nm.empty()) {
            for (char& ch2 : nm) if (ch2 == 32 || ch2 == 61 || (unsigned char)ch2 < 32) ch2 = 95;   // spaces, =, control chars -> _
            o << " name=" << nm;
        }
    }
    if (ok) {
        o << " pos=" << st.pos.x << ',' << st.pos.y << ',' << st.pos.z << " dest=" << st.dest.x << ',' << st.dest.y << ',' << st.dest.z
          << " flags=" << unsigned(st.flags) << " gait=" << unsigned(st.gait) << " pace=" << st.pace;
        kc::Vec3 d;
        bool mv = false;
        float sp = 0;
        if (kenshi::Character* ch = w.Find(h); ch && kenshi::GetMovement(ch, d, mv, sp)) o << " speed=" << sp;
        kenshi::AnimModes am;
        if (kenshi::Character* ch = w.Find(h); ch && kenshi::ReadAnimModes(ch, am))
            o << " action=" << (am.action.empty() ? "-" : am.action) << " cmode=" << am.combat << " guard=" << am.guardLegs << am.guardUpper << " tech=" << kenshi::CurrentTechniqueName(ch) << " drawn=" << kenshi::DrawnFrom(ch);
        kc::Vec3 fd;
        kc::Quat q;
        if (kenshi::Character* ch = w.Find(h); ch && kenshi::GetFacing(ch, fd) && kenshi::GetRotation(ch, q))
            o << " face=" << fd.x << ',' << fd.y << ',' << fd.z << " rot=" << q.w << ',' << q.x << ',' << q.y << ',' << q.z;
    }
    kc::Handle ct;
    if (kenshi::Character* ch = w.Find(h); ch && kenshi::ReadCombat(ch, ct)) o << " combat=" << Key(w.HostHandleOf(ct));
    kc::EntityState latest, rendered;
    if (g_dumpSession && !g_dumpSession->isHost() && g_dumpSession->TargetOf(hostHandle, latest, rendered))
        o << " latest=" << latest.pos.x << ',' << latest.pos.y << ',' << latest.pos.z << " lflags=" << unsigned(latest.flags)
          << " rendered=" << rendered.pos.x << ',' << rendered.pos.y << ',' << rendered.pos.z;
    if (kenshi::Character* ch = w.Find(h)) {
        std::vector<kc::ItemState> items;
        if (kenshi::ReadInventory(ch, items)) {
            o << " items=" << items.size() << " inv=";
            for (size_t i = 0; i < items.size(); ++i)
                o << (i ? ";" : "") << items[i].templateSid << 'x' << items[i].quantity << '@' << items[i].section << ':' << items[i].x << ','
                  << items[i].y << (items[i].equipped ? "E" : "");
        }
    }
    if (vok) {
        o << " blood=" << v.blood << " ko=" << v.koTimer << " vflags=" << unsigned(v.flags) << " parts=";
        for (size_t i = 0; i < v.parts.size(); ++i) o << (i ? ";" : "") << v.parts[i].flesh << '/' << v.parts[i].stun;
    }
    o << '\n';
}

std::string Execute(kc::Session& s, KenshiWorld& w, bool live, std::istringstream& in, const std::string& cmd) {
    std::string err;
    if (cmd == "echo") return "ok";
    // ---- map markers, minimap, heads, squad bar, pings (exp_map)
    if (cmd == "mapscene") {   // mapscene <carte|minicarte|tetes|barre|pings>: what this machine draws
        std::string what;
        in >> what;
        return DescribeMapScene(what);
    }
    if (cmd == "mapfeed") {   // mapfeed: the map feed as this machine has it (host: built, client: received)
        const kc::MapMarkersMsg& m = s.mapMarkers();
        std::ostringstream o;
        o.setf(std::ios::fixed);
        o.precision(1);
        o << "ok age=" << std::min(999.0, s.mapMarkersAge()) << " players=" << m.players.size() << " chars=" << m.chars.size() << " threats=" << m.threats.size() << " ;";
        for (const auto& c : m.chars) {
            std::string n = c.name;
            for (char& ch : n) if (ch == ' ' || ch == '=' || ch == ';' || (unsigned char)ch < 32) ch = '_';
            o << ' ' << (n.empty() ? "-" : n) << ":owner=" << int(c.owner) << ":av=" << ((c.flags & kc::kMapAvatar) ? 1 : 0) << ":x=" << c.pos.x << ":z=" << c.pos.z;
        }
        o << " ;";
        for (const auto& t : m.threats) o << " threat:kind=" << int(t.kind) << ":n=" << int(t.count) << ":x=" << t.pos.x << ":z=" << t.pos.z;
        return o.str();
    }
    if (cmd == "mapproj") {   // mapproj <x> <z>: the game's MapScreen::worldToMapCoords against ours
        float x = 0, z = 0;
        if (!(in >> x >> z)) return "err usage: mapproj <x> <z>";
        return CheckMapProjection(x, z);
    }
    if (cmd == "ping") {   // ping <x> <z> [type 0-3]: this player pings that spot (as a middle click would)
        float x = 0, z = 0;
        int type = 0;
        if (!(in >> x >> z)) return "err usage: ping <x> <z> [type]";
        in >> type;
        if (type < 0 || type >= int(kc::kPingKinds)) return "err type 0-3";
        return s.PlaceMapPing({x, 0, z}, kc::PingKind(type)) ? "ok" : "err refused (too soon, or no session)";
    }
    if (cmd == "pings") {   // pings: the live pings here
        std::ostringstream o;
        o.setf(std::ios::fixed);
        o.precision(1);
        o << "ok n=" << s.pings().size();
        for (const auto& p : s.pings())
            o << " id=" << p.ping.id << ":owner=" << int(p.ping.owner) << ":kind=" << int(p.ping.kind) << ":x=" << p.ping.pos.x << ":z=" << p.ping.pos.z
              << ":age=" << s.pingAge(p);
        return o.str();
    }
    if (cmd == "status") {
        std::ostringstream o;
        o << "ok state=" << StateStr(s.state()) << " live=" << live << " ready=" << w.Ready() << " id=" << int(s.localId())
          << " entities=" << s.entityCount() << " npcs=" << s.npcCount() << " missingNpcs=" << s.missingNpcs()
          << " joining=" << s.joiningPlayers();
        // join queue (names without spaces): client "queue=2/3 queueWait=<name> queuePhase=saving|loading|editor",
        // host "queue=<name>:<phase>,<name>:wait,..."
        auto word = [](std::string t) { for (char& c : t) if (c == ' ' || c == '"') c = '_'; return t.empty() ? std::string("-") : t; };
        auto phase = [](kc::JoinPhase p) { return p == kc::JoinPhase::Saving ? "saving" : p == kc::JoinPhase::Loading ? "loading" : "editor"; };
        if (const kc::JoinQueueMsg* q = s.queueStatus())
            o << " queue=" << int(q->position) << '/' << int(q->total) << " queueWait=" << word(q->current) << " queuePhase=" << phase(q->phase);
        else if (s.isHost()) {
            std::string list;
            for (const auto& e : s.joinQueue()) list += (list.empty() ? "" : ",") + word(e.name) + ':' + (e.waiting ? "wait" : phase(e.phase));
            o << " queue=" << (list.empty() ? "-" : list);
        } else o << " queue=-";
        o << " busy=" << kenshi::SaveManagerBusy() << " error=\"" << s.lastError() << "\"";
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
        return steam::JoinAddress(s, addr, uint16_t(port), &err) ? "ok" : "err " + err;
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
    if (cmd == "spawnnpc") {   // spawnnpc <dx> <dz> [squadIndex]: host creates an NPC (copy of a nearby one) next to squad[0] (or that one)
        float dx = 0, dz = 0;
        size_t near0 = 0;
        in >> dx >> dz;
        if (!(in >> near0)) near0 = 0;
        auto squad = SortedSquad(w);
        kc::Vec3 base;
        if (near0 >= squad.size() || !kenshi::GetPosition(w.FindSquad(squad[near0]), base)) return "err no squad";
        std::vector<kenshi::Character*> all;
        kenshi::ActiveCharacters(all);
        kc::SpawnInfo info;
        std::string e = "no NPC to copy";
        HostCallScope scope;
        for (kenshi::Character* src : all) {
            kc::Handle h;
            if (!kenshi::GetHandle(src, h) || w.FindSquad(h) || kenshi::IsDead(src) || !kenshi::ReadSpawnSource(src, info)) continue;
            kenshi::Character* c = kenshi::CreateCharacter(info, {base.x + dx, base.y, base.z + dz}, &e);
            if (!c || !kenshi::GetHandle(c, h)) { Log("spawnnpc: %s: %s", info.templateSid.c_str(), e.c_str()); continue; }
            lastSpawned_ = h;
            return "ok " + Key(h) + " " + info.templateSid;
        }
        return "err " + e;
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
        const bool ok = cmd == "ko" ? kenshi::ForceKnockout(c, 60.0f) : kenshi::CallDeclareDead(c);
        return ok ? "ok " + Key(lastSpawned_) : "err call failed";
    }
    if (cmd == "rollweather") return "ok " + std::to_string(w.ExpireAllWeather());
    if (cmd == "anims") {   // anims <file>: every squad member's playing animations
        std::string file;
        in >> file;
        std::ofstream o(file, std::ios::trunc);
        if (!o) return "err cannot write " + file;
        o << "hook " << AnimHookStats() << " targets=" << w.AnimTargetCount() << '\n';
        for (const auto& h : SortedSquad(w)) {
            std::vector<kenshi::PlayingAnim> v;
            kenshi::ReadPlayingAnims(w.FindSquad(h), v);
            float mt = 0, ms = 0;
            kenshi::ReadAnimMaster(w.FindSquad(h), mt, ms);
            o << "char " << Key(h) << " n=" << v.size() << " master=" << mt << " mspeed=" << ms << '\n';
            for (const auto& a : v)
                o << "  L" << int(a.layer) << (a.fadingOut ? " out " : " in  ") << a.anim << " | " << a.data << " t=" << a.time << " t01=" << a.time01
                  << " w=" << a.weight << " dw=" << a.desired << " sp=" << a.speed << " loop=" << a.looped << " synch=" << a.synched << '\n';
        }
        return "ok";
    }
    if (cmd == "drop") {   // drop <squadIndex>: that squad member drops an unequipped item; answers its handle
        size_t idx = 0;
        in >> idx;
        auto squad = SortedSquad(w);
        if (idx >= squad.size()) return "err no such squad member";
        kenshi::Character* c = w.FindSquad(squad[idx]);
        void* item = kenshi::FirstLooseItem(c);
        if (!item) return "err nothing to drop";
        if (!kenshi::CallDropItem(c, item)) return "err drop failed";
        kc::Handle ih;
        kc::ItemState st;
        kc::Vec3 p;
        if (!kenshi::DescribeGroundItem(item, ih, st, p)) return "err dropped but unreadable";
        char b[200];
        snprintf(b, sizeof(b), "ok %s %s %.1f,%.1f,%.1f", Key(ih).c_str(), st.templateSid.c_str(), p.x, p.y, p.z);
        return b;
    }
    if (cmd == "pickup") {   // pickup <squadIndex> <itemKey>: that squad member takes the item from the ground
        size_t idx = 0;
        std::string k;
        in >> idx >> k;
        kc::Handle ih;
        sscanf(k.c_str(), "%u:%u:%u:%u:%u", &ih.type, &ih.container, &ih.containerSerial, &ih.index, &ih.serial);
        auto squad = SortedSquad(w);
        void* item = kenshi::ResolveItem(ih);
        if (idx >= squad.size() || !item) return "err no such squad member or item";
        return kenshi::CallGiveItem(w.FindSquad(squad[idx]), item) ? "ok" : "err giveItem refused";
    }
    if (cmd == "pickupreq") {   // pickupreq <squadIndex> <itemSid> <x,y,z>: what a client's "pick up" click sends
        size_t idx = 0;
        std::string sid, at;
        in >> idx >> sid >> at;
        auto squad = SortedSquad(w);
        kc::Command c;
        c.kind = kc::CommandKind::PickUp;
        c.itemSid = sid;
        if (idx >= squad.size() || sscanf(at.c_str(), "%f,%f,%f", &c.pos.x, &c.pos.y, &c.pos.z) != 3) return "err usage";
        w.QueueLocalOrder(squad[idx], c);
        return "ok";
    }
    if (cmd == "groundnear") {   // groundnear <radius> [loose|ground] [squadIndex]: items lying around that squad member (default 0)
        float radius = 500;
        std::string mode;
        size_t sel = 0;
        in >> radius >> mode >> sel;
        auto squad = SortedSquad(w);
        kc::Vec3 base;
        if (sel >= squad.size() || !kenshi::GetPosition(w.FindSquad(squad[sel]), base)) return "err no squad";
        std::vector<void*> items;
        if (mode == "loose") kenshi::LooseItemsNear(base, radius, items);
        else kenshi::GroundItemsNear(base, radius, items);
        std::string out = "ok " + std::to_string(items.size());
        for (void* it : items) {
            kc::Handle ih;
            kc::ItemState st;
            kc::Vec3 p;
            if (!kenshi::DescribeGroundItem(it, ih, st, p)) continue;
            char b[160];
            snprintf(b, sizeof(b), " %s|%s|%.1f,%.1f,%.1f", Key(ih).c_str(), st.templateSid.c_str(), p.x, p.y, p.z);
            out += b;
            if (out.size() > 3000) break;
        }
        return out;
    }
    if (cmd == "ground") {   // ground <hostItemKey>: is (our copy of) that item lying on the ground here?
        std::string k;
        in >> k;
        kc::Handle ih;
        sscanf(k.c_str(), "%u:%u:%u:%u:%u", &ih.type, &ih.container, &ih.containerSerial, &ih.index, &ih.serial);
        void* item = kenshi::ResolveItem(w.GroundCopyOf(ih));
        if (!item) return "ok absent";
        kc::Handle h2;
        kc::ItemState st;
        kc::Vec3 p;
        if (!kenshi::ItemOnGround(item) || !kenshi::DescribeGroundItem(item, h2, st, p)) return "ok not-on-ground";
        char b[200];
        snprintf(b, sizeof(b), "ok on-ground %s %.1f,%.1f,%.1f", st.templateSid.c_str(), p.x, p.y, p.z);
        return b;
    }
    if (cmd == "uidrop") {   // uidrop <squadIndex> <weapon|armour|item|sid|name_part>: drop it as the inventory window does (Inventory::dropItem)
        size_t idx = 0;
        std::string kind;
        in >> idx >> kind;
        auto squad = SortedSquad(w);
        if (idx >= squad.size() || kind.empty()) return "err usage: uidrop <squadIndex> <weapon|armour|item|sid>";
        kenshi::Character* c = w.FindSquad(squad[idx]);
        void* item = kenshi::FindItemOfKind(c, kind);
        if (!item) return "err no " + kind + " carried";
        kc::ItemState st;
        kenshi::DescribeInventoryItem(item, st);
        // no HostCallScope: the hooks see it as the player's own drop (a client asks the host)
        if (!kenshi::InventoryDrop(kenshi::InventoryOfHolder(c), item)) return "err drop failed";
        if (!s.isHost()) return "ok asked " + st.templateSid + " x" + std::to_string(st.quantity);
        kc::Handle ih;
        kc::Vec3 p;
        if (!kenshi::ItemInWorld(item) || !kenshi::DescribeGroundItem(item, ih, st, p)) return "err not in the world after the drop";
        char b[220];
        snprintf(b, sizeof(b), "ok %s %s %d %.1f,%.1f,%.1f", Key(ih).c_str(), st.templateSid.c_str(), st.quantity, p.x, p.y, p.z);
        return b;
    }
    if (cmd == "fetchitem") {   // fetchitem <squadIndex> <weapon|armour|sid>: (host) one from another squad member (worn or not) into its bag
        size_t idx = 0;
        std::string kind;
        in >> idx >> kind;
        if (!s.isHost()) return "err host only";
        auto squad = SortedSquad(w);
        if (idx >= squad.size()) return "err no such squad member";
        kenshi::Character* to = w.FindSquad(squad[idx]);
        if (void* mine = kenshi::FindItemOfKind(to, kind)) {
            kc::ItemState st;
            kenshi::DescribeInventoryItem(mine, st);
            if (st.section == "main") return "ok " + st.templateSid + " (already carried)";
        }
        HostCallScope scope;
        for (size_t j = 0; j < squad.size(); ++j) {
            if (j == idx) continue;
            kenshi::Character* from = w.FindSquad(squad[j]);
            void* it = kenshi::FindItemOfKind(from, kind);
            kc::InvOp op;
            if (!it || !kenshi::DescribeInventoryItem(it, op.item)) continue;
            op.toSection = "main";
            op.toX = op.toY = -1;
            std::string e;
            if (kenshi::MoveInventoryItem(from, to, op, &e)) return "ok " + op.item.templateSid + " from squad" + std::to_string(j);
        }
        return "err no " + kind + " to fetch";
    }
    if (cmd == "groundall") {   // groundall <radius> [squadIndex]: items lying in the world around it, "key|sid|qty|x,y,z"
        float radius = 500;
        size_t sel = 0;
        in >> radius >> sel;
        auto squad = SortedSquad(w);
        kc::Vec3 base;
        if (sel >= squad.size() || !kenshi::GetPosition(w.FindSquad(squad[sel]), base)) return "err no squad";
        std::vector<void*> items;
        kenshi::WorldItemsNear(base, radius, items);
        std::string out = "ok " + std::to_string(items.size());
        for (void* it : items) {
            kc::Handle ih;
            kc::ItemState st;
            kc::Vec3 p;
            if (!kenshi::DescribeGroundItem(it, ih, st, p)) continue;
            char b[200];
            snprintf(b, sizeof(b), " %s|%s|%d|%.1f,%.1f,%.1f", Key(ih).c_str(), st.templateSid.c_str(), st.quantity, p.x, p.y, p.z);
            out += b;
            if (out.size() > 6000) break;
        }
        return out;
    }
    if (cmd == "groundcopy") {   // groundcopy <hostItemKey>: (client) our copy of that host item: "key|sid|qty|x,y,z" or "none"
        std::string k;
        in >> k;
        kc::Handle ih;
        sscanf(k.c_str(), "%u:%u:%u:%u:%u", &ih.type, &ih.container, &ih.containerSerial, &ih.index, &ih.serial);
        void* item = kenshi::ResolveItem(w.GroundCopyOf(ih));
        kc::Handle h2;
        kc::ItemState st;
        kc::Vec3 p;
        if (!item || !kenshi::ItemInWorld(item) || !kenshi::DescribeGroundItem(item, h2, st, p)) return "ok none";
        char b[200];
        snprintf(b, sizeof(b), "ok %s|%s|%d|%.1f,%.1f,%.1f", Key(h2).c_str(), st.templateSid.c_str(), st.quantity, p.x, p.y, p.z);
        return b;
    }
    if (cmd == "groundstats") {   // groundstats: drop / pickup bookkeeping counters (plugin/ground.cpp)
        const auto g = w.GroundCounters();
        char b[300];
        snprintf(b, sizeof(b), "ok hookDrops=%llu scanDrops=%llu scanGone=%llu scanChanged=%llu repeatsSkipped=%llu created=%llu matched=%llu repeats=%llu merged=%llu removed=%llu",
                 (unsigned long long)g.hookDrops, (unsigned long long)g.scanDrops, (unsigned long long)g.scanGone, (unsigned long long)g.scanChanged,
                 (unsigned long long)g.repeatsSkipped, (unsigned long long)g.created, (unsigned long long)g.matched, (unsigned long long)g.repeats,
                 (unsigned long long)g.merged, (unsigned long long)g.removed);
        return b;
    }
    if (cmd == "stats") {   // stats <squadIndex>: its skill levels, comma separated
        size_t idx = 0;
        in >> idx;
        auto squad = SortedSquad(w);
        std::vector<float> st;
        if (idx >= squad.size() || !kenshi::ReadStats(w.FindSquad(squad[idx]), st)) return "err no stats";
        std::ostringstream o;
        o << "ok ";
        for (size_t i = 0; i < st.size(); ++i) o << (i ? "," : "") << st[i];
        kc::EntityVitals v;
        if (kenshi::ReadVitals(w.FindSquad(squad[idx]), v)) o << " hunger=" << v.hunger;
        return o.str();
    }
    if (cmd == "xp") {   // xp <squadIndex> <statIndex> <amount>: an experience gain, as the game gives one (refused on clients)
        size_t idx = 0, stat = 0;
        float amount = 0;
        in >> idx >> stat >> amount;
        auto squad = SortedSquad(w);
        std::vector<float> before, after;
        kenshi::Character* c = idx < squad.size() ? w.FindSquad(squad[idx]) : nullptr;
        if (!c || !kenshi::ReadStats(c, before) || stat >= before.size()) return "err no such character or stat";
        if (!kenshi::GainExperience(c, stat, amount)) return "err call failed";
        kenshi::ReadStats(c, after);
        char b[96];
        snprintf(b, sizeof(b), "ok %.4f -> %.4f", before[stat], after[stat]);
        return b;
    }
    // ---- lot B: factions
    if (cmd == "factions" || cmd == "relation" || cmd == "setrelation") {
        // factions: how many factions the player faction has relations with, and a fingerprint of them all
        // relation <name part>: the player faction toward that faction and back ("ours" / "theirs")
        // setrelation <name part> <value>: (host) both ways set to that value, as a faction event would
        std::string part;
        float value = 0;
        in >> part >> value;
        std::replace(part.begin(), part.end(), '_', ' ');
        kc::FactionsMsg m;
        if (!w.ReadFactions(m)) return "err no player faction";
        if (cmd == "factions") {
            kc::Writer wr;
            kc::Encode(wr, m);
            uint64_t h = 1469598103934665603ull;
            for (size_t i = 0; i < wr.size(); ++i) { h ^= wr.data()[i]; h *= 1099511628211ull; }
            char b[96];
            snprintf(b, sizeof(b), "ok n=%zu rank=%d hash=%016llx", m.factions.size(), m.playerRank, (unsigned long long)h);
            return b;
        }
        for (auto& e : m.factions) {
            std::string name;
            kenshi::TemplateDisplayName(e.factionSid, name);
            if (e.factionSid.find(part) == std::string::npos && name.find(part) == std::string::npos) continue;
            if (cmd == "setrelation") {
                e.ours.relation = value;
                e.theirs.relation = value;
                e.hasOurs = e.hasTheirs = true;
                kc::FactionsMsg one = m;
                one.factions = {e};
                const size_t n = w.ApplyFactions(one);
                return "ok " + name + " set (" + std::to_string(n) + " values)";
            }
            char b[200];
            snprintf(b, sizeof(b), "ok %s ours=%.1f%s%s theirs=%.1f%s%s", name.c_str(), e.hasOurs ? double(e.ours.relation) : -999.0,
                     e.ours.alliance ? ",ally" : "", e.ours.war ? ",war" : "", e.hasTheirs ? double(e.theirs.relation) : -999.0,
                     e.theirs.alliance ? ",ally" : "", e.theirs.war ? ",war" : "");
            return b;
        }
        return "err no such faction";
    }
    if (cmd == "bounty" || cmd == "givebounty") {
        // bounty <squadIndex>: that member's bounties ("faction:amount"), crime being committed, prison hours
        // givebounty <squadIndex> <faction name part> <amount>: (host) put that bounty on it
        size_t idx = 0;
        std::string part;
        int amount = 0;
        in >> idx >> part >> amount;
        std::replace(part.begin(), part.end(), '_', ' ');
        auto squad = SortedSquad(w);
        if (idx >= squad.size()) return "err no such squad member";
        kc::CharBounties b;
        if (!w.ReadBounties(squad[idx], b)) return "err cannot read";
        if (cmd == "givebounty") {
            kc::FactionsMsg m;
            w.ReadFactions(m);
            std::string sid;
            for (const auto& e : m.factions) {
                std::string name;
                kenshi::TemplateDisplayName(e.factionSid, name);
                if (e.factionSid.find(part) != std::string::npos || name.find(part) != std::string::npos) { sid = e.factionSid; break; }
            }
            if (sid.empty()) return "err no such faction";
            auto it = std::find_if(b.bounties.begin(), b.bounties.end(), [&](const kc::BountyEntry& e) { return e.factionSid == sid; });
            if (it == b.bounties.end()) { b.bounties.push_back({}); it = b.bounties.end() - 1; it->factionSid = sid; }
            it->amount = amount;
            it->crimes |= 1u << 3;   // THEFT
            return "ok " + std::to_string(w.ApplyBounties(squad[idx], b)) + " values";
        }
        std::string out = "ok";
        int total = 0;
        for (const auto& e : b.bounties) {
            std::string name;
            kenshi::TemplateDisplayName(e.factionSid, name);
            out += " " + (name.empty() ? e.factionSid : name) + ":" + std::to_string(e.amount);
            total += e.amount;
        }
        char tail[120];
        snprintf(tail, sizeof(tail), " total=%d crime=%d prison=%.1f", total, b.crime, double(b.prisonSentence));
        return out + tail;
    }
    if (cmd == "factionsync") {   // factionsync: relation/bounty messages received, values corrected (client), sent (host)
        const auto v = s.factionsView();
        return "ok received=" + std::to_string(v.received) + " bounties=" + std::to_string(v.bountiesReceived) + " corrected=" +
               std::to_string(v.corrected) + " sent=" + std::to_string(v.sent);
    }
    // ---- diplomacy: relations between factions, unique characters, towns
    if (cmd == "diplo" || cmd == "diplopair" || cmd == "setdiplopair" || cmd == "unique" || cmd == "setunique" || cmd == "town" ||
        cmd == "settownowner" || cmd == "diplosync") {
        // diplo: wars/alliances between factions, unique characters, towns: counts and fingerprints
        // diplopair <A> <B>: A toward B and back; setdiplopair <A> <B> <value> [war|ally|peace|none]: both ways
        // unique <name part>: state (0 dead, 1 alive, 2 imprisoned) and by-player flag; setunique <part> <state> [player]
        // town <name part>: owner and override; settownowner <town part> <faction part>
        // diplosync: Diplomacy parts received, values corrected (client), sent (host)
        if (cmd == "diplosync") {
            const auto v = s.diplomacyView();
            return "ok received=" + std::to_string(v.received) + " corrected=" + std::to_string(v.corrected) + " sent=" + std::to_string(v.sent) +
                   " pairs=" + std::to_string(v.pairs) + " uniques=" + std::to_string(v.uniques) + " towns=" + std::to_string(v.towns);
        }
        kc::DiplomacyState d;
        if (!w.ReadDiplomacy(d)) return "err no world";
        auto fnv = [](uint64_t h, const std::string& t) {
            for (unsigned char c : t) { h ^= c; h *= 1099511628211ull; }
            h ^= 0xFF;
            return h * 1099511628211ull;
        };
        auto nameOf = [](const std::string& sid) {
            std::string n;
            kenshi::TemplateDisplayName(sid, n);
            return n.empty() ? sid : n;
        };
        auto matches = [&](const std::string& sid, const std::string& part) {
            return sid.find(part) != std::string::npos || nameOf(sid).find(part) != std::string::npos;
        };
        if (cmd == "diplo") {
            uint64_t ph = 1469598103934665603ull, uh = ph, th = ph;
            size_t flagged = 0, wars = 0, dead = 0, jailed = 0, overridden = 0;
            for (const auto& p : d.pairs) {
                if (!p.rel.war && !p.rel.alliance && !p.rel.peace) continue;
                ++flagged;
                wars += p.rel.war;
                ph = fnv(ph, p.from + ">" + p.to + (p.rel.war ? "w" : "") + (p.rel.alliance ? "a" : "") + (p.rel.peace ? "p" : ""));
            }
            for (const auto& u : d.uniques) {
                if (u.state == kc::kUniqueAlive && !u.byPlayer) continue;   // what a missing entry means too
                dead += u.state == kc::kUniqueDead;
                jailed += u.state == kc::kUniqueImprisoned;
                uh = fnv(uh, u.sid + char('0' + u.state) + (u.byPlayer ? "p" : ""));
            }
            for (const auto& t : d.towns) {
                overridden += !t.overrideSid.empty();
                th = fnv(th, t.sid + "|" + t.ownerSid + "|" + t.overrideSid);
            }
            char b[300];
            snprintf(b, sizeof(b), "ok pairs=%zu flagged=%zu wars=%zu ph=%016llx uniques=%zu dead=%zu jailed=%zu uh=%016llx towns=%zu overridden=%zu th=%016llx",
                     d.pairs.size(), flagged, wars, (unsigned long long)ph, d.uniques.size(), dead, jailed, (unsigned long long)uh, d.towns.size(),
                     overridden, (unsigned long long)th);
            return b;
        }
        std::string a, b2, extra;
        in >> a;
        std::replace(a.begin(), a.end(), '_', ' ');
        if (cmd == "diplopair" || cmd == "setdiplopair") {
            float value = 0;
            in >> b2 >> value >> extra;
            std::replace(b2.begin(), b2.end(), '_', ' ');
            // factions by name part, from every faction the game knows (the pairs list them all)
            std::string sa, sb;
            for (const auto& p : d.pairs) {
                if (sa.empty() && matches(p.from, a)) sa = p.from;
                if (sb.empty() && matches(p.from, b2)) sb = p.from;
            }
            if (sa.empty() || sb.empty() || sa == sb) return "err no such factions";
            if (cmd == "setdiplopair") {
                kc::RelationState r;
                for (const auto& p : d.pairs) if (p.from == sa && p.to == sb) r = p.rel;
                r.relation = value;
                r.war = extra == "war";
                r.alliance = extra == "ally";
                r.peace = extra == "peace";
                return w.SetFactionPair(sa, sb, r) ? "ok " + nameOf(sa) + " / " + nameOf(sb) + " set" : "err not set";
            }
            std::string out = "ok " + nameOf(sa) + " / " + nameOf(sb);
            for (const auto& p : d.pairs) {
                if (!((p.from == sa && p.to == sb) || (p.from == sb && p.to == sa))) continue;
                char t[96];
                snprintf(t, sizeof(t), " %s=%.0f%s%s%s", p.from == sa ? "ab" : "ba", double(p.rel.relation), p.rel.war ? ",war" : "",
                         p.rel.alliance ? ",ally" : "", p.rel.peace ? ",peace" : "");
                out += t;
            }
            return out;
        }
        if (cmd == "unique" || cmd == "setunique") {
            int state = -1;
            in >> state >> extra;
            for (const auto& u : d.uniques) {
                if (!matches(u.sid, a)) continue;
                if (cmd == "setunique") {
                    if (state < 0 || state > 2) return "err state 0..2";
                    return w.SetUniqueState(u.sid, uint8_t(state), extra == "player") ? "ok " + nameOf(u.sid) + " set" : "err not set";
                }
                return "ok " + nameOf(u.sid) + " sid=" + u.sid + " state=" + std::to_string(u.state) + " player=" + (u.byPlayer ? "1" : "0");
            }
            return "err no such unique character";
        }
        // towns
        in >> b2;
        std::replace(b2.begin(), b2.end(), '_', ' ');
        for (const auto& t : d.towns) {
            if (!matches(t.sid, a)) continue;
            if (cmd == "settownowner") {
                std::string fs;
                for (const auto& p : d.pairs) if (fs.empty() && matches(p.from, b2)) fs = p.from;
                if (fs.empty()) return "err no such faction";
                return w.SetTownOwner(t.sid, fs) ? "ok " + nameOf(t.sid) + " now " + nameOf(fs) : "err not set";
            }
            return "ok " + nameOf(t.sid) + " sid=" + t.sid + " owner=" + (t.ownerSid.empty() ? "-" : nameOf(t.ownerSid)) +
                   " override=" + (t.overrideSid.empty() ? "-" : t.overrideSid);
        }
        return "err no such town";
    }
    if (cmd == "money") {   // money [set]: the player faction's cats (host: set them)
        int32_t m = 0;
        std::string set;
        if (in >> set) { if (!kenshi::WritePlayerMoney(std::stoi(set))) return "err"; }
        return kenshi::ReadPlayerMoney(m) ? "ok " + std::to_string(m) : "err no money";
    }
    if (cmd == "say") {   // say <squadIndex> <text...>: that character says it aloud (through the game's Dialogue::say)
        size_t idx = 0;
        in >> idx;
        std::string text;
        std::getline(in, text);
        while (!text.empty() && text.front() == ' ') text.erase(text.begin());
        auto squad = SortedSquad(w);
        if (idx >= squad.size() || text.empty()) return "err need a character and a text";
        return kenshi::CallSay(w.FindSquad(squad[idx]), text) ? "ok" : "err say failed";
    }
    if (cmd == "says") return "ok " + std::to_string(w.saysApplied) + " " + w.lastSay;   // client: bubbles replayed, last one
    if (cmd == "taskreq") {   // taskreq <selectIndex> <task> <subjectIndex>: select that squad member alone, give the order (as the UI does)
        size_t sel = 0, subj = 0;
        int task = 0;
        in >> sel >> task >> subj;
        auto squad = SortedSquad(w);
        if (sel >= squad.size() || subj >= squad.size()) return "err no such squad member";
        bool ok = false;
        kenshi::WithSelection(w.FindSquad(squad[sel]), [&] { ok = kenshi::CallAddTaskNearest(task, w.FindSquad(squad[subj])); });
        return ok ? "ok" : "err call failed";
    }
    if (cmd == "talkreq") {   // talkreq <selectIndex>: select that squad member alone, order it to talk to the nearest NPC
        size_t sel = 0;
        in >> sel;
        auto squad = SortedSquad(w);
        if (sel >= squad.size()) return "err no such squad member";
        kenshi::Character* me = w.FindSquad(squad[sel]);
        kc::Vec3 mp, p;
        if (!kenshi::GetPosition(me, mp)) return "err";
        std::vector<kenshi::Character*> all;
        kenshi::ActiveCharacters(all);
        kenshi::Character* best = nullptr;
        float bestD = 1e30f;
        for (kenshi::Character* c : all) {
            kc::Handle h;
            if (!kenshi::GetHandle(c, h) || w.FindSquad(h) || kenshi::IsDead(c) || kenshi::IsDown(c) || !kenshi::GetPosition(c, p)) continue;
            const float d = (p.x - mp.x) * (p.x - mp.x) + (p.z - mp.z) * (p.z - mp.z);
            if (d < bestD) { bestD = d; best = c; }
        }
        if (!best) return "err no NPC around";
        std::string name;
        kenshi::CharacterName(best, name);
        bool ok = false;
        kenshi::WithSelection(me, [&] { ok = kenshi::CallAddTaskNearest(12, best); });   // PLAYER_TALK_TO
        return ok ? "ok " + name + " at " + std::to_string(int(std::sqrt(bestD))) : "err call failed";
    }
    if (cmd == "where") {   // where <key|npc|squadIndex>: position of a character (the host's handle) as this machine has it
        std::string k;
        in >> k;
        kc::Handle h;
        if (k == "npc") h = lastSpawned_;
        else if (k.find(':') == std::string::npos) { auto squad = SortedSquad(w); size_t i = std::stoul(k); if (i >= squad.size()) return "err"; h = squad[i]; }
        else sscanf(k.c_str(), "%u:%u:%u:%u:%u", &h.type, &h.container, &h.containerSerial, &h.index, &h.serial);
        kenshi::Character* c = w.Find(h);
        kc::Vec3 p;
        if (!c || !kenshi::GetPosition(c, p)) return "err not here";
        // carried=1: its animation is the "being carried" one (on a shoulder). The game's position of a
        // carried body is its carrier's (the body is drawn on the shoulder bone), so x,y,z alone cannot tell.
        kenshi::AnimModes am;
        const int carried = kenshi::ReadAnimModes(c, am) ? int(am.carried) : -1;
        char b[140];
        snprintf(b, sizeof(b), "ok %.3f,%.3f,%.3f %s carried=%d", p.x, p.y, p.z, Key(h).c_str(), carried);
        return b;
    }
    if (cmd == "look") {   // look <name>: appearance summary of that squad member ('_' for spaces)
        std::string who;
        in >> who;
        std::replace(who.begin(), who.end(), '_', ' ');
        std::vector<kenshi::Character*> all;
        kenshi::PlayerCharacters(all);
        for (kenshi::Character* c : all) {
            std::string n;
            if (!kenshi::CharacterName(c, n) || n != who) continue;
            kc::AppearanceMsg m;
            if (!kenshi::ReadAppearance(c, m)) return "err no appearance";
            std::ostringstream o;
            o << "ok fields=" << m.fields.size();
            for (const auto& f : m.fields) {
                if (f.key == "Age" || f.key == "sex female" || f.key == "head" || f.key == "race" || f.key == "hair style" || f.key == "Hair Colour") {
                    o << " " << f.key << "=";
                    if (f.type == kc::AppearanceType::Float) o << f.f[0];
                    else if (f.type == kc::AppearanceType::Bool) o << f.b;
                    else if (f.type == kc::AppearanceType::String) o << f.s;
                    else for (const auto& r : f.refs) o << r;
                }
            }
            return o.str();
        }
        return "err no " + who;
    }
    if (cmd == "lookset") {   // lookset <name> <floatKey> <value>: change one slider of that squad member's looks (tests)
        std::string who, key;
        float v = 0;
        in >> who >> key >> v;
        for (auto* str : {&who, &key}) std::replace(str->begin(), str->end(), '_', ' ');
        std::vector<kenshi::Character*> all;
        kenshi::PlayerCharacters(all);
        for (kenshi::Character* c : all) {
            std::string n;
            if (!kenshi::CharacterName(c, n) || n != who) continue;
            kc::AppearanceMsg m;
            m.name = n;
            kc::AppearanceField f;
            f.type = kc::AppearanceType::Float;
            f.key = key;
            f.f[0] = v;
            m.fields.push_back(f);
            HostCallScope scope;
            return kenshi::WriteAppearance(c, m) ? "ok" : "err";
        }
        return "err no " + who;
    }
    if (cmd == "editchar") return s.EditOwnCharacter() ? "ok" : "err";   // client: the character editor on our own character
    if (cmd == "editdone") {   // editdone: what the editor's confirm button does at the end
        reinterpret_cast<void (*)(void*)>(kenshi::FnAddr(kenshi::FnCloseCharacterEditor))(reinterpret_cast<void*>(kenshi::Addr(0x21337B0)));
        return "ok";
    }
    if (cmd == "objnear") {   // objnear <squadIndex> <radius>: objects around that squad member: template x count
        size_t idx = 0;
        float r = 100;
        in >> idx >> r;
        auto squad = SortedSquad(w);
        kc::Vec3 p;
        if (idx >= squad.size() || !kenshi::GetPosition(w.FindSquad(squad[idx]), p)) return "err";
        std::vector<void*> objs;
        kenshi::ObjectsNear(p, r, objs);
        std::map<std::string, int> kinds;
        for (void* o : objs) {
            std::string sid, name;
            kenshi::ObjectTemplate(o, sid);
            if (!sid.empty()) kenshi::TemplateDisplayName(sid, name);
            ++kinds[(sid.empty() ? "?" : sid) + "(" + name + ")"];
        }
        std::ostringstream o;
        o << "ok " << objs.size();
        for (auto& [k, n] : kinds) o << " " << k << "x" << n;
        return o.str();
    }
    if (cmd == "squads") {   // squads: the player's squads as this machine has them: "name: member,member | ..."
        std::vector<kc::IWorld::WorldSquad> ws;
        w.ReadSquads(ws);
        std::ostringstream o;
        o << "ok";
        for (const auto& sq : ws) {
            o << " | " << sq.name << ":";
            for (const auto& h : sq.members) {
                std::string n;
                kenshi::CharacterName(w.FindSquad(h), n);
                o << " " << n;
            }
        }
        return o.str();
    }
    if (cmd == "squadmove") {   // squadmove <name> <withName|new>: drop that portrait on the squad of another member (or a new squad), as the UI does ('_' for spaces)
        std::string who, with;
        in >> who >> with;
        for (auto* str : {&who, &with}) std::replace(str->begin(), str->end(), '_', ' ');
        std::vector<kenshi::Character*> all;
        kenshi::PlayerCharacters(all);
        auto byName = [&](const std::string& n) -> kenshi::Character* {
            for (kenshi::Character* c : all) { std::string cn; if (kenshi::CharacterName(c, cn) && cn == n) return c; }
            return nullptr;
        };
        kenshi::Character* c = byName(who);
        if (!c) return "err no " + who;
        void* target = nullptr;
        if (with != "new") {
            kenshi::Character* o = byName(with);
            if (!o) return "err no " + with;
            target = kenshi::SquadOf(o);
        } else {
            HostCallScope scope;   // the "new squad" button itself is not synchronized: only the move is
            target = kenshi::NewSquad();
        }
        std::vector<kenshi::Character*> members;
        kenshi::SquadMembers(target, members);
        using FnAddAt = void (*)(void*, void*, int);
        reinterpret_cast<FnAddAt>(kenshi::FnAddr(kenshi::FnSquadAddCharacterAt))(target, c, int(members.size()));
        return "ok";
    }
    if (cmd == "orderreq") {   // orderreq <selectIndex> <standingOrder>: the squad bar's toggle for that member alone, as the UI does
        size_t sel = 0;
        int order = 0;
        in >> sel >> order;
        auto squad = SortedSquad(w);
        if (sel >= squad.size()) return "err no such squad member";
        kenshi::WithSelection(w.FindSquad(squad[sel]), [&] {
            reinterpret_cast<void (*)(void*, int)>(kenshi::FnAddr(kenshi::FnSetOrderSelected))(kenshi::Player(), order);
        });
        return "ok";
    }
    if (cmd == "modes") {   // modes <squadIndex>: standing orders as bits (1 stealth, 2 defensive, 4 ranged, 8 taunt, 16 hold, 32 passive, 64 chase) and fight style
        size_t idx = 0;
        in >> idx;
        auto squad = SortedSquad(w);
        if (idx >= squad.size()) return "err";
        uint8_t style = 0;
        const uint16_t m = kenshi::ReadModes(w.FindSquad(squad[idx]), style);
        return "ok " + std::to_string(m) + " " + std::to_string(style);
    }
    // ---- fix G5
    if (cmd == "attackreq") {   // attackreq <selectIndex>: that member alone is ordered to attack the last spawned NPC (a right click's addOrderSelectedCharacters)
        size_t sel = 0;
        in >> sel;
        auto squad = SortedSquad(w);
        kenshi::Character* t = w.Find(lastSpawned_);
        kc::Vec3 tp;
        if (sel >= squad.size() || !t || !kenshi::GetPosition(t, tp)) return "err need a squad member and a spawned NPC";
        const float loc[3] = {tp.x, tp.y, tp.z};
        using FnAddOrder = void (*)(void*, void*, int, void*, bool, bool, const float*);
        kenshi::WithSelection(w.FindSquad(squad[sel]), [&] {
            reinterpret_cast<FnAddOrder>(kenshi::FnAddr(kenshi::FnAddOrderSelected))(kenshi::Player(), nullptr, 5, t, false, false, loc);   // ATTACK
        });
        return "ok";
    }
    if (cmd == "combat") {   // combat <squadIndex>: 1 and its target when that member is in combat mode
        size_t idx = 0;
        in >> idx;
        auto squad = SortedSquad(w);
        if (idx >= squad.size()) return "err";
        kc::Handle t;
        const bool on = kenshi::ReadCombat(w.FindSquad(squad[idx]), t);
        return std::string("ok ") + (on ? "1 " + Key(t) : "0");
    }
    if (cmd == "selectset") {   // selectset <i> [<j>...]: the player's selection becomes exactly those squad members (as clicks would)
        auto squad = SortedSquad(w);
        if (!kenshi::Player()) return "err";
        std::vector<kenshi::Character*> want;
        size_t i = 0;
        while (in >> i)
            if (i < squad.size())
                if (kenshi::Character* c = w.FindSquad(squad[i])) want.push_back(c);
        std::string why;
        // unselectAll keeps the main selected character: SelectExactly takes it out when it is not wanted
        if (!kenshi::SelectExactly(want, &why)) return "err " + why;
        return "ok " + std::to_string(want.size());
    }
    // ---- actor safety
    if (cmd == "selected") {   // selected: squad indices (this machine's order) of the player's selection, sorted
        auto squad = SortedSquad(w);
        std::vector<kc::Handle> sel;
        kenshi::SelectedHandles(sel);
        std::vector<size_t> idx;
        size_t other = 0;
        for (const auto& h : sel) {
            auto it = std::find(squad.begin(), squad.end(), h);
            if (it != squad.end()) idx.push_back(size_t(it - squad.begin()));
            else ++other;
        }
        std::sort(idx.begin(), idx.end());
        std::string out = "ok";
        for (size_t k : idx) out += " " + std::to_string(k);
        if (other) out += " +" + std::to_string(other);
        return out;
    }
    if (cmd == "charstate") {   // charstate <squadIndex>: tasks=<task system> jobs=<Tâches> in=<0 nothing,1 bed,2 cage> dialog=<0/1> pos=x,z
        size_t idx = 0;
        in >> idx;
        auto squad = SortedSquad(w);
        if (idx >= squad.size()) return "err no such squad member";
        kenshi::Character* c = w.FindSquad(squad[idx]);
        kc::Vec3 p;
        if (!c || !kenshi::GetPosition(c, p)) return "err";
        int inside = -1;
        kenshi::ReadInSomething(c, inside);
        void* d = kenshi::CharacterDialogue(c);
        const bool talking = d && kenshi::DialogueTarget(d) != nullptr;
        char b[200];
        snprintf(b, sizeof(b), "ok tasks=%zu jobs=%d in=%d dialog=%d pos=%.1f,%.1f", kenshi::LocalTaskCount(c), kenshi::PermajobCount(c), inside,
                 talking ? 1 : 0, p.x, p.z);
        return b;
    }
    if (cmd == "actorstats") {   // actorstats: (host) refusals of client requests, refusals for their target, leaks, selection restore failures
        char b[200];
        snprintf(b, sizeof(b), "ok refused=%u target=%d leaks=%d restorefail=%d", unsigned(s.actorRefusals()), w.targetRefusals(), ActorLeaks(),
                 kenshi::SelectionRestoreFailures());
        return b;
    }
    if (cmd == "results") {   // results: (client) answers of the host: rejected count, then the last ones as seq:state:reason
        std::string out = "ok " + std::to_string(s.rejectedCount());
        const auto& r = s.results();
        for (size_t k = r.size() > 12 ? r.size() - 12 : 0; k < r.size(); ++k)
            out += " " + std::to_string(r[k].seq) + ":" + std::to_string(int(r[k].state)) + ":" + std::to_string(int(r[k].reason));
        return out;
    }
    if (cmd == "forgeorder") {
        // forgeorder <actorIndex> <via> <task> <subject>: (client) an order sent to the host as is, without
        // our own checks: a forged request. actorIndex: squad index (any owner). subject: none, self,
        // squad<i>, npc (nearest living NPC), item (nearest loose item), building (nearest building)
        size_t idx = 0;
        int via = 0, task = 0;
        std::string subj = "none";
        in >> idx >> via >> task >> subj;
        auto squad = SortedSquad(w);
        if (idx >= squad.size()) return "err no such squad member";
        kenshi::Character* me = w.FindSquad(squad[idx]);
        kc::Vec3 mp, p;
        if (!me || !kenshi::GetPosition(me, mp)) return "err";
        const kc::Handle hostHandle = w.HostHandleOf(squad[idx]);
        uint32_t netId = 0;
        s.ForEachEntity([&](uint32_t id, const kc::Handle& h, uint8_t, bool, bool) { if (h == hostHandle) netId = id; });
        if (!netId) return "err that character is not followed";
        kc::Command c;
        c.netId = netId;
        c.kind = via == 0 ? kc::CommandKind::MoveTo : kc::CommandKind::Task;
        c.via = kc::TaskVia(via ? via : 1);
        c.task = task;
        c.pos = mp;
        void* o = nullptr;
        if (subj == "self") o = me;
        else if (subj.rfind("squad", 0) == 0) {
            const size_t k = size_t(std::atoi(subj.c_str() + 5));
            if (k < squad.size()) o = w.FindSquad(squad[k]);
        } else if (subj == "npc") {
            std::vector<kenshi::Character*> all;
            kenshi::ActiveCharacters(all);
            float best = 1e30f;
            for (kenshi::Character* ch : all) {
                kc::Handle h;
                if (!kenshi::GetHandle(ch, h) || w.FindSquad(h) || kenshi::IsDead(ch) || kenshi::IsDown(ch) || !kenshi::GetPosition(ch, p)) continue;
                const float d = (p.x - mp.x) * (p.x - mp.x) + (p.z - mp.z) * (p.z - mp.z);
                if (d < best) { best = d; o = ch; }
            }
        } else if (subj == "item" || subj == "building") {
            std::vector<void*> objs;
            if (subj == "item") kenshi::LooseItemsNear(mp, 400.0f, objs);
            else kenshi::ObjectsNear(mp, 600.0f, objs);
            float best = 1e30f;
            for (void* ob : objs) {
                if (!kenshi::ObjectPosition(ob, p)) continue;
                const float d = (p.x - mp.x) * (p.x - mp.x) + (p.z - mp.z) * (p.z - mp.z);
                if (d < best) { best = d; o = ob; }
            }
        } else if (subj != "none") {
            return "err unknown subject kind";
        }
        if (subj != "none" && !o) return "err no such subject around";
        if (o) {
            kc::Handle sh;
            if (kenshi::ObjectHandle(o, sh)) c.subject = kenshi::IsCharacter(o) ? w.HostHandleOf(sh) : sh;
            if (!kenshi::IsCharacter(o)) { kenshi::ObjectTemplate(o, c.itemSid); kenshi::ObjectPosition(o, c.subjectPos); }
        }
        return s.SendRawCommandForTest(c) ? "ok netId=" + std::to_string(netId) : "err not connected";
    }
    if (cmd == "selorder") {   // selorder <standingOrder>: the squad bar's toggle on the current selection, as a click does
        int order = 0;
        in >> order;
        reinterpret_cast<void (*)(void*, int)>(kenshi::FnAddr(kenshi::FnSetOrderSelected))(kenshi::Player(), order);
        std::vector<kc::Handle> sel;
        kenshi::SelectedHandles(sel);
        return "ok " + std::to_string(sel.size()) + " selected";
    }
    if (cmd == "npcreq") {   // npcreq <selectIndex> <task> <namePart>: that member alone gets the order on the nearest NPC whose name has that part ('_' for spaces), as a right click does
        size_t sel = 0;
        int task = 0;
        std::string part;
        in >> sel >> task >> part;
        std::replace(part.begin(), part.end(), '_', ' ');
        auto squad = SortedSquad(w);
        if (sel >= squad.size()) return "err no such squad member";
        kenshi::Character* me = w.FindSquad(squad[sel]);
        kc::Vec3 mp, p;
        if (!kenshi::GetPosition(me, mp)) return "err";
        std::vector<kenshi::Character*> all;
        kenshi::ActiveCharacters(all);
        kenshi::Character* best = nullptr;
        float bestD = 1e30f;
        for (kenshi::Character* c : all) {
            kc::Handle h;
            std::string name;
            if (!kenshi::GetHandle(c, h) || w.FindSquad(h) || kenshi::IsDead(c) || kenshi::IsDown(c) || !kenshi::GetPosition(c, p)) continue;
            if (!part.empty() && (!kenshi::CharacterName(c, name) || name.find(part) == std::string::npos)) continue;
            const float d = (p.x - mp.x) * (p.x - mp.x) + (p.z - mp.z) * (p.z - mp.z);
            if (d < bestD) { bestD = d; best = c; }
        }
        if (!best) return "err no such NPC around";
        std::string name;
        kenshi::CharacterName(best, name);
        bool ok = false;
        kenshi::WithSelection(me, [&] { ok = kenshi::CallAddTaskNearest(task, best); });
        return ok ? "ok " + name + " at " + std::to_string(int(std::sqrt(bestD))) : "err call failed";
    }
    if (cmd == "jobs") {   // jobs <squadIndex>: its job list (Tâches panel), by kind
        size_t idx = 0;
        in >> idx;
        auto squad = SortedSquad(w);
        if (idx >= squad.size()) return "err";
        kenshi::Character* c = w.FindSquad(squad[idx]);
        const int n = kenshi::PermajobCount(c);
        std::string o = "ok " + std::to_string(n);
        for (int i = 0; i < n; ++i) o += " " + std::to_string(kenshi::PermajobType(c, i));
        return o;
    }
    if (cmd == "jobreq") {   // jobreq <selectIndex> <task> <subjectIndex>: that member alone gets a permanent job on the other (addJobSelectedCharacters, as the UI does)
        size_t sel = 0, subj = 0;
        int task = 0;
        in >> sel >> task >> subj;
        auto squad = SortedSquad(w);
        if (sel >= squad.size() || subj >= squad.size()) return "err no such squad member";
        kenshi::Character* s2 = w.FindSquad(squad[subj]);
        kc::Vec3 p;
        if (!kenshi::GetPosition(s2, p)) return "err";
        const float loc[3] = {p.x, p.y, p.z};
        using FnAddJob = void (*)(void*, int, void*, bool, bool, const float*);
        kenshi::WithSelection(w.FindSquad(squad[sel]), [&] {
            // the first bool (KenshiLib's "shift") makes it a permanent job, in the Tâches panel
            // (OrdersReceiver +0x88 list); false only gives a passing order (+0x70 list). Only a task
            // whose TaskData is a permajob lands there: 31 STAY_CLOSE_TO_TARGET is the panel's "follow"
            // job, 44 FOLLOW_PLAYER_ORDER is the passing follow order and never becomes a job.
            reinterpret_cast<FnAddJob>(kenshi::FnAddr(kenshi::FnAddJobSelected))(kenshi::Player(), task, s2, true, true, loc);
        });
        return "ok";
    }
    if (cmd == "jobremove") {   // jobremove <squadIndex> <slot>: the Tâches panel's cross on that job (Character::removePermajob)
        size_t idx = 0;
        int slot = 0;
        in >> idx >> slot;
        auto squad = SortedSquad(w);
        if (idx >= squad.size()) return "err";
        kenshi::Character* c = w.FindSquad(squad[idx]);
        if (slot < 0 || slot >= kenshi::PermajobCount(c)) return "err no such job";
        reinterpret_cast<void (*)(void*, int)>(kenshi::FnAddr(kenshi::FnCharRemovePermajob))(c, slot);
        return "ok";
    }
    if (cmd == "carryreq") {   // carryreq <selectIndex> <targetIndex>: that member alone is ordered to carry the other (as a right click does)
        size_t sel = 0, tgt = 0;
        in >> sel >> tgt;
        auto squad = SortedSquad(w);
        if (sel >= squad.size() || tgt >= squad.size()) return "err";
        bool ok = false;
        kenshi::WithSelection(w.FindSquad(squad[sel]), [&] { ok = kenshi::CallAddTaskNearest(225, w.FindSquad(squad[tgt])); });   // LIFT_PERSON_PLAYER_ORDER
        return ok ? "ok" : "err";
    }
    if (cmd == "carrynpc" || cmd == "carrydrop") {   // carrynpc|carrydrop <squadIndex>: (host) that member picks up the last spawned NPC / puts its body down
        size_t idx = 0;
        in >> idx;
        auto squad = SortedSquad(w);
        if (idx >= squad.size()) return "err";
        kenshi::Character* me = w.FindSquad(squad[idx]);
        HostCallScope scope;
        if (cmd == "carrydrop") return kenshi::DropCarried(me) ? "ok" : "err";
        kenshi::Character* body = w.Find(lastSpawned_);
        if (!body) return "err no spawned NPC";
        return kenshi::CarryCharacter(me, body) ? "ok " + Key(lastSpawned_) : "err refused";
    }
    if (cmd == "carrying") {   // carrying <squadIndex>: who that member carries (host handle key) or none
        size_t idx = 0;
        in >> idx;
        auto squad = SortedSquad(w);
        kc::Handle c;
        if (idx >= squad.size()) return "err";
        if (!kenshi::ReadCarried(w.FindSquad(squad[idx]), c)) return "ok none";
        return "ok " + Key(w.HostHandleOf(c));
    }
    if (cmd == "paused") return std::string("ok ") + (kenshi::GetPaused() ? "1" : "0") + " " + std::to_string(kenshi::GetFrameSpeed());
    if (cmd == "animstats") {   // animstats: animation clock corrections since the last call (client)
        char b[80];
        snprintf(b, sizeof(b), "ok %llu %llu", static_cast<unsigned long long>(w.masterCorrections), static_cast<unsigned long long>(w.masterChecks));
        w.masterCorrections = w.masterChecks = 0;
        return b;
    }
    if (cmd == "ownidx") {   // ownidx [playerId]: squad indices (this machine's order) of that player's characters (default: mine)
        int id = -1;
        if (!(in >> id)) id = s.localId();
        auto squad = SortedSquad(w);
        std::string out = "ok";
        for (size_t i = 0; i < squad.size(); ++i)
            if (s.ownerOf(w.HostHandleOf(squad[i])) == id) out += " " + std::to_string(i);
        return out;
    }
    if (cmd == "admin") {   // admin <verb> ...: (host) the Administration section's actions (plugin/admin.cpp); refused elsewhere
        std::string line;
        std::getline(in, line);
        bool ok = false;
        std::string r = AdminRun(line, s, w, &ok);
        for (auto& ch : r) if (ch == '\n') ch = '|';
        return (ok ? "ok " : "err ") + r;
    }
    if (cmd == "tpplayer") {   // tpplayer <playerId>: (host) that player's characters next to squad member 0, as the admin teleport does
        int id = -1;
        in >> id;
        auto squad = SortedSquad(w);
        std::vector<kc::Handle> who;
        for (const auto& h : squad) if (s.ownerOf(h) == id) who.push_back(h);
        kc::Vec3 p;
        if (who.empty() || squad.empty() || !kenshi::GetPosition(w.FindSquad(squad[0]), p)) return "err";
        s.ExpectStall(uint8_t(id), 120.0);   // fix G6: as the admin "tp"
        return "ok " + std::to_string(w.TeleportCharacters(who, p));
    }
    // ---- fix G6
    if (cmd == "squadbar") {   // squadbar: how many portraits the squad bar shows, and the squad count here
        std::vector<kenshi::Character*> shown;
        kenshi::SquadMembers(kenshi::ShownSquad(), shown);
        return "ok " + std::to_string(shown.size()) + " " + std::to_string(w.CharacterCount()) + " gen=" + std::to_string(w.WorldGeneration());
    }
    if (cmd == "floor") {   // floor <squadIndex> [group]: its floor group (9: ground floor); with a group: put it there (host)
        int i = -1, g = -1;
        in >> i >> g;
        auto squad = SortedSquad(w);
        if (i < 0 || i >= int(squad.size())) return "err";
        kenshi::Character* c = w.FindSquad(squad[i]);
        int32_t cur = 0;
        if (!c || !kenshi::ReadFloorGroup(c, cur)) return "err";
        if (g >= 0) {
            HostCallScope scope;
            if (g < 9 || !kenshi::PlaceOnFloor(c, g)) kenshi::WriteFloorGroup(c, g);   // the game's own path when it can
            kenshi::ReadFloorGroup(c, cur);
        }
        return "ok " + std::to_string(cur);
    }
    if (cmd == "consolewin") {   // consolewin: is the host console window there, and how much log does it show
        HWND wnd = FindWindowA("KenshiCoopHostConsole", nullptr);
        if (!wnd) return "ok none";
        HWND edit = nullptr;
        int longest = 0;
        for (HWND c = GetWindow(wnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
            const int n = GetWindowTextLengthA(c);
            if (n > longest) { longest = n; edit = c; }
        }
        return "ok window visible=" + std::to_string(IsWindowVisible(wnd) ? 1 : 0) + " log=" + std::to_string(longest);
    }
    if (cmd == "resync") {   // resync [playerId]: (host) that player (0: everyone) reloads the host's world
        int id = 0;
        in >> id;
        return "ok " + std::to_string(s.RequestResync(uint8_t(std::clamp(id, 0, 255))));
    }
    if (cmd == "objreq") {   // objreq <selectIndex> <task> <name part>: that member alone is ordered to use the nearest object whose name has that text ('_' for spaces)
        size_t sel = 0;
        int task = 0;
        std::string part;
        in >> sel >> task >> part;
        std::replace(part.begin(), part.end(), '_', ' ');
        auto squad = SortedSquad(w);
        if (sel >= squad.size()) return "err";
        kenshi::Character* me = w.FindSquad(squad[sel]);
        kc::Vec3 p, op;
        if (!kenshi::GetPosition(me, p)) return "err";
        std::vector<void*> objs;
        kenshi::ObjectsNear(p, 1500, objs);
        void* best = nullptr;
        float bestD = 1e30f;
        std::string bestName;
        for (void* o : objs) {
            std::string sid, name;
            if (!kenshi::ObjectTemplate(o, sid) || !kenshi::TemplateDisplayName(sid, name) || name.find(part) == std::string::npos || !kenshi::ObjectPosition(o, op)) continue;
            const float d = (op.x - p.x) * (op.x - p.x) + (op.z - p.z) * (op.z - p.z);
            if (d < bestD) { bestD = d; best = o; bestName = name; }
        }
        if (!best) return "err nothing called " + part;
        kenshi::ObjectPosition(best, op);
        bool ok = false;
        // the UI's path for furniture: a new task on that object's handle, with the building it is in
        // (the click gives it; here: the nearest house-like building)
        void* dest = nullptr;
        float destD = 1e30f;
        for (void* o : objs) {
            std::string sid, name;
            kc::Vec3 hp;
            if (!kenshi::ObjectTemplate(o, sid) || !kenshi::TemplateDisplayName(sid, name) || !kenshi::ObjectPosition(o, hp)) continue;
            if (name.find("Maison") == std::string::npos && name.find("Cabane") == std::string::npos && name.find("Abri") == std::string::npos &&
                name.find("Tente") == std::string::npos && name.find("Tour") == std::string::npos)
                continue;
            const float d = (hp.x - op.x) * (hp.x - op.x) + (hp.z - op.z) * (hp.z - op.z);
            if (d < destD) { destD = d; dest = o; }
        }
        kenshi::WithSelection(me, [&] { ok = kenshi::CallNewPlayerTaskOn(task, best, op, dest); });
        return (ok ? "ok " : "err ") + bestName + " at " + std::to_string(int(std::sqrt(bestD)));
    }
    if (cmd == "furnparent") {   // furnparent <name part>: what the nearest object with that name is furniture of (tests)
        std::string part;
        size_t around = 0;
        in >> part >> around;   // optional: around that squad member instead of member 0
        std::replace(part.begin(), part.end(), '_', ' ');
        auto squad = SortedSquad(w);
        kc::Vec3 p, op;
        if (around >= squad.size() || !kenshi::GetPosition(w.FindSquad(squad[around]), p)) return "err";
        std::vector<void*> objs;
        kenshi::ObjectsNear(p, 1500, objs);
        std::ostringstream o;
        o << "ok";
        int n = 0;
        for (void* obj : objs) {
            std::string sid, name;
            if (!kenshi::ObjectTemplate(obj, sid) || !kenshi::TemplateDisplayName(sid, name) || name.find(part) == std::string::npos) continue;
            void* parent = kenshi::FurnitureParent(obj);
            std::string psid, pname;
            if (parent && kenshi::ObjectTemplate(parent, psid)) kenshi::TemplateDisplayName(psid, pname);
            uint8_t raw[0x28] = {};
            for (size_t k = 0; k < sizeof(raw); ++k) raw[k] = reinterpret_cast<const uint8_t*>(obj)[0x238 + k];
            char hex[100];
            snprintf(hex, sizeof(hex), "%02x%02x%02x%02x%02x%02x%02x%02x %02x%02x%02x%02x", raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7], raw[8], raw[9], raw[10], raw[11]);
            o << " | " << name << " -> " << (parent ? pname : "none") << " [" << hex << "]";
            if (++n >= 6) break;
        }
        return o.str();
    }
    if (cmd == "containerreq" || cmd == "contake") {
        // containerreq <selectIndex> <name part>: right click on the nearest container with that name (loot order)
        // contake <selectIndex> <name part>: in the open window, take its first item into that member's bag
        size_t sel = 0;
        std::string part;
        in >> sel >> part;
        std::replace(part.begin(), part.end(), '_', ' ');
        auto squad = SortedSquad(w);
        if (sel >= squad.size()) return "err";
        kenshi::Character* me = w.FindSquad(squad[sel]);
        kc::Vec3 p, op;
        if (!kenshi::GetPosition(me, p)) return "err";
        std::vector<void*> objs;
        kenshi::ObjectsNear(p, 1500, objs);
        void* best = nullptr;
        float bestD = 1e30f;
        std::string bestName;
        for (void* o : objs) {
            std::string sid, name;
            std::vector<kc::ItemState> items;
            if (!kenshi::ObjectTemplate(o, sid) || !kenshi::TemplateDisplayName(sid, name) || (part != "any" && name.find(part) == std::string::npos) ||
                !kenshi::ObjectPosition(o, op) || !kenshi::ReadInventory(o, items) || (cmd == "contake" && items.empty()))
                continue;
            const float d = (op.x - p.x) * (op.x - p.x) + (op.z - p.z) * (op.z - p.z);
            if (d < bestD) { bestD = d; best = o; bestName = name; }
        }
        if (!best) return "err no container called " + part;
        kenshi::ObjectPosition(best, op);
        if (cmd == "containerreq") {
            bool ok = false;
            kenshi::WithSelection(me, [&] { ok = kenshi::CallAddTaskNearestObject(26, best, op); });
            return (ok ? "ok " : "err ") + bestName + " at " + std::to_string(int(std::sqrt(bestD)));
        }
        std::vector<kc::ItemState> items;
        kenshi::ReadInventory(best, items);
        if (items.empty()) return "err the container is empty";
        kc::InvOp mv;
        mv.kind = kc::InvOpKind::Move;
        mv.item = items[0];
        mv.toSection = "main";
        mv.toX = -1;
        mv.toY = -1;
        std::string why;
        HostCallScope scope;   // the window's own move (the client's diff then asks the host)
        const bool ok = kenshi::MoveInventoryItem(best, me, mv, &why);
        return (ok ? "ok took " : "err ") + items[0].templateSid + (ok ? "" : " " + why) + " from " + bestName + " (" + std::to_string(items.size()) + " stacks)";
    }
    if (cmd == "contcount") {   // contcount <name part>: stacks in the nearest container with that name (around squad member 0)
        std::string part;
        in >> part;
        std::replace(part.begin(), part.end(), '_', ' ');
        auto squad = SortedSquad(w);
        kc::Vec3 p, op;
        if (squad.empty() || !kenshi::GetPosition(w.FindSquad(squad[0]), p)) return "err";
        std::vector<void*> objs;
        kenshi::ObjectsNear(p, 1500, objs);
        int best = -1;
        float bestD = 1e30f;
        for (void* o : objs) {
            std::string sid, name;
            std::vector<kc::ItemState> items;
            if (!kenshi::ObjectTemplate(o, sid) || !kenshi::TemplateDisplayName(sid, name) || (part != "any" && name.find(part) == std::string::npos) ||
                !kenshi::ObjectPosition(o, op) || !kenshi::ReadInventory(o, items) || items.empty())
                continue;
            const float d = (op.x - p.x) * (op.x - p.x) + (op.z - p.z) * (op.z - p.z);
            if (d < bestD) { bestD = d; best = int(items.size()); }
        }
        return "ok " + std::to_string(best) + " windows=" + std::to_string(kenshi::OpenInventoryWindows());
    }
    if (cmd == "robuststats") {   // robuststats: (client) characters freed from a wall and far walkers put back, since the last call
        const std::string out = "ok stuck=" + std::to_string(w.stuckFixes) + " far=" + std::to_string(w.farSnaps);
        w.stuckFixes = w.farSnaps = 0;
        return out;
    }
    if (cmd == "strand") {
        // strand <dx> <dy> <dz>: (client) our copy of the NPC nearest to squad member 0 is moved by that
        // much here only (into a wall, under a floor): the client must bring it back by itself
        float dx = 0, dy = 0, dz = 0;
        in >> dx >> dy >> dz;
        auto squad = SortedSquad(w);
        kc::Vec3 mp, p;
        if (squad.empty() || !kenshi::GetPosition(w.FindSquad(squad[0]), mp)) return "err";
        std::vector<kenshi::Character*> all;
        kenshi::ActiveCharacters(all);
        kenshi::Character* best = nullptr;
        float bestD = 1e30f;
        kc::Handle bestH;
        for (kenshi::Character* c : all) {
            kc::Handle h;
            if (!kenshi::GetHandle(c, h) || w.FindSquad(h) || kenshi::IsDead(c) || kenshi::IsRagdoll(c) || !kenshi::GetPosition(c, p)) continue;
            const float d = (p.x - mp.x) * (p.x - mp.x) + (p.z - mp.z) * (p.z - mp.z);
            if (d < bestD) { bestD = d; best = c; bestH = h; }
        }
        if (!best || !kenshi::GetPosition(best, p)) return "err no NPC around";
        kc::Quat q;
        kenshi::GetRotation(best, q);
        HostCallScope scope;
        kenshi::Teleport(best, {p.x + dx, p.y + dy, p.z + dz}, q);
        return "ok " + Key(w.HostHandleOf(bestH));
    }
    if (cmd == "bedreq" || cmd == "minereq") {
        // bedreq <selectIndex>: that member alone is ordered to sleep in the nearest free bed (as a right click does)
        // minereq <selectIndex>: ... to work the nearest mine or machine that mines
        size_t sel = 0;
        in >> sel;
        auto squad = SortedSquad(w);
        if (sel >= squad.size()) return "err no such squad member";
        kenshi::Character* me = w.FindSquad(squad[sel]);
        kc::Vec3 p, op;
        if (!kenshi::GetPosition(me, p)) return "err";
        std::vector<void*> objs;
        kenshi::ObjectsNear(p, 2000, objs);
        const bool bed = cmd == "bedreq";
        void* best = nullptr;
        float bestD = 1e30f;
        std::string bestName;
        for (void* o : objs) {
            const int f = kenshi::BuildingFunctionOf(o);
            if (bed ? f != 6 : (f != 1 && f != 27)) continue;   // BF_BED; BF_MINE, BF_MINE_NATURAL
            uint64_t operators = 0;
            if (bed && kenshi::ReadOperatorCount(o, operators) && operators) continue;   // taken
            std::string sid, name;
            if (!kenshi::ObjectTemplate(o, sid) || !kenshi::ObjectPosition(o, op)) continue;
            kenshi::TemplateDisplayName(sid, name);
            const float d = (op.x - p.x) * (op.x - p.x) + (op.z - p.z) * (op.z - p.z);
            if (d < bestD) { bestD = d; best = o; bestName = name; }
        }
        if (!best) return bed ? "err no free bed around" : "err no mine around";
        kenshi::ObjectPosition(best, op);
        bool ok = false;
        void* dest = kenshi::FurnitureParent(best);
        kenshi::WithSelection(me, [&] { ok = kenshi::CallNewPlayerTaskOn(bed ? 258 : 87, best, op, dest); });   // USE_BED_ORDER / OPERATE_MACHINERY
        return (ok ? "ok " : "err ") + bestName + " at " + std::to_string(int(std::sqrt(bestD)));
    }
    if (cmd == "insomething") {   // insomething <squadIndex>: 0 nothing, 1 in bed, 2 in a cage
        size_t idx = 0;
        in >> idx;
        auto squad = SortedSquad(w);
        int v = -1;
        if (idx < squad.size()) kenshi::ReadInSomething(w.FindSquad(squad[idx]), v);
        return "ok " + std::to_string(v);
    }
    if (cmd == "camto") {   // camto <squadIndex>: the camera goes to that squad member
        size_t idx = 0;
        in >> idx;
        auto squad = SortedSquad(w);
        return idx < squad.size() && kenshi::FocusCamera(w.FindSquad(squad[idx])) ? "ok" : "err";
    }
    if (cmd == "kosquad") {   // kosquad <squadIndex>: (host) that squad member is knocked out
        size_t idx = 0;
        in >> idx;
        auto squad = SortedSquad(w);
        if (idx >= squad.size()) return "err no such squad member";
        HostCallScope scope;
        return kenshi::ForceKnockout(w.FindSquad(squad[idx]), 60.0f) ? "ok" : "err";
    }
    if (cmd == "convo") {   // convo <squadIndex> <k>: (host) the k-th nearest NPC starts a conversation with that squad member
        size_t sel = 0, k = 0;
        in >> sel >> k;
        auto squad = SortedSquad(w);
        if (sel >= squad.size()) return "err no such squad member";
        kenshi::Character* me = w.FindSquad(squad[sel]);
        kc::Vec3 mp, p;
        if (!kenshi::GetPosition(me, mp)) return "err";
        std::vector<kenshi::Character*> all;
        kenshi::ActiveCharacters(all);
        std::vector<std::pair<float, kenshi::Character*>> nearby;
        for (kenshi::Character* c : all) {
            kc::Handle h;
            if (!kenshi::GetHandle(c, h) || w.FindSquad(h) || kenshi::IsDead(c) || kenshi::IsDown(c) || !kenshi::GetPosition(c, p)) continue;
            nearby.emplace_back((p.x - mp.x) * (p.x - mp.x) + (p.z - mp.z) * (p.z - mp.z), c);
        }
        std::sort(nearby.begin(), nearby.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        if (k >= nearby.size()) return "err not that many NPCs";
        std::string name;
        kenshi::CharacterName(nearby[k].second, name);
        HostCallScope scope;
        const bool ok = kenshi::CallStartPlayerConversation(nearby[k].second, me);
        return (ok ? "ok " : "err ") + name + " at " + std::to_string(int(std::sqrt(nearby[k].first)));
    }
    if (cmd == "dialog") {   // dialog: the conversation window this client shows (client)
        const auto& d = s.dialog();
        std::ostringstream o;
        o << "ok open=" << d.open << " id=" << d.id << " waiting=" << d.waiting << " name=" << d.name << " | " << d.text << " |";
        for (const auto& r : d.replies) o << " [" << r << "]";
        return o.str();
    }
    if (cmd == "answer") {   // answer <index>: pick that answer in the conversation window (client)
        int i = 0;
        in >> i;
        s.AnswerDialog(i);
        return "ok";
    }
    if (cmd == "fxhurry") return "ok " + std::to_string(w.HurryEffects());   // fxhurry: every effect group places one now
    if (cmd == "setweather") {   // setweather <regionSid> <seasonSid> <weatherSid>
        std::string region, season, weather;
        in >> region >> season >> weather;
        if (weather.empty()) return "err usage: setweather <regionSid> <seasonSid> <weatherSid>";
        w.ForceWeather(region, season, weather);
        return "ok";
    }
    if (cmd == "weathers") {   // weathers <file>: regions, their effect groups and the weathers they can have
        std::string file;
        in >> file;
        std::ofstream o(file, std::ios::trunc);
        if (!o) return "err cannot write " + file;
        o << w.WeatherGroups();
        return "ok";
    }
    if (cmd == "loot") {   // loot <target> [squadIndex]: what a loot order on a body does on a client
        std::string k;
        size_t idx = 0;
        in >> k >> idx;
        kc::Handle h;
        sscanf(k.c_str(), "%u:%u:%u:%u:%u", &h.type, &h.container, &h.containerSerial, &h.index, &h.serial);
        auto squad = SortedSquad(w);
        kenshi::Character* t = w.Find(h);
        if (!t || idx >= squad.size()) return "err bad target or looter";
        w.RequestLoot(squad[idx], t);
        return "ok";
    }
    if (cmd == "hours") {   // hours: the game clock
        double now = -1;
        kenshi::GetGameHours(now);
        char buf[64];
        snprintf(buf, sizeof buf, "ok %.4f", now);
        return buf;
    }
    if (cmd == "lootorder") {   // lootorder <target>: the right-click "loot" path, with the current selection
        std::string k;
        in >> k;
        kc::Handle h;
        sscanf(k.c_str(), "%u:%u:%u:%u:%u", &h.type, &h.container, &h.containerSerial, &h.index, &h.serial);
        kenshi::Character* t = w.Find(h);
        if (!t) return "err no such character";
        return kenshi::CallAddTaskNearest(26, t) ? "ok" : "err call failed";
    }
    if (cmd == "merchants" || cmd == "tradeopen") {
        // merchants: NPCs around squad member 0 who sell from shop counters ("name(counters)" each)
        // tradeopen <squadIndex> <name part|any> [near]: the game asks for a trade window between that
        //   squad member and the nearest such merchant (what "let's trade" in a conversation does); on
        //   the host, for another player's character, it opens on that player's screen. near: the
        //   member is first put next to the merchant (host)
        size_t sel = 0;
        std::string part = "any", nearArg;
        if (cmd == "tradeopen") in >> sel >> part >> nearArg;
        std::replace(part.begin(), part.end(), '_', ' ');
        auto squad = SortedSquad(w);
        if (sel >= squad.size()) return "err no such squad member";
        kenshi::Character* me = w.FindSquad(squad[sel]);
        kc::Vec3 mp, p;
        if (!kenshi::GetPosition(me, mp)) return "err";
        std::vector<kenshi::Character*> all;
        kenshi::ActiveCharacters(all);
        std::string list;
        kenshi::Character* best = nullptr;
        float bestD = 1e30f;
        size_t bestCounters = 0;
        for (kenshi::Character* c : all) {
            kc::Handle h;
            std::string name;
            std::vector<void*> counters;
            if (!kenshi::GetHandle(c, h) || w.FindSquad(h) || kenshi::IsDead(c) || !kenshi::GetPosition(c, p)) continue;
            const float d = (p.x - mp.x) * (p.x - mp.x) + (p.z - mp.z) * (p.z - mp.z);
            if (d > 1500.0f * 1500.0f || !kenshi::CharacterName(c, name) || !kenshi::ShopCounters(c, counters)) continue;
            if (part != "any" && name.find(part) == std::string::npos) continue;
            list += " " + name + "(" + std::to_string(counters.size()) + ")";
            if (d < bestD) { bestD = d; best = c; bestCounters = counters.size(); }
        }
        if (cmd == "merchants") return "ok" + list;
        if (!best) return "err no merchant around";
        std::string name;
        kenshi::CharacterName(best, name);
        if (nearArg == "near" && kenshi::GetPosition(best, p)) {
            kc::Quat q;
            kenshi::GetRotation(me, q);
            HostCallScope scope;
            kenshi::Teleport(me, {p.x + 8.0f, p.y + 1.0f, p.z + 8.0f}, q);
            bestD = 128.0f;
        }
        return kenshi::OpenTradeWindow(me, best) ? "ok " + name + " " + std::to_string(bestCounters) + " at " + std::to_string(int(std::sqrt(bestD)))
                                                 : "err call failed";
    }
    if (cmd == "tradestate") {   // tradestate: the trade window here (client: the one the host opened for us)
        const auto t = s.tradeView();
        void* npc = kenshi::NpcTrader();
        std::string name = "-";
        if (npc) kenshi::CharacterName(static_cast<kenshi::Character*>(npc), name);
        int32_t mine = 0, theirs = 0;
        kenshi::ReadPlayerMoney(mine);
        if (npc) kenshi::MoneyOf(static_cast<kenshi::Character*>(npc), theirs);
        return "ok pending=" + std::to_string(t.pending) + " open=" + std::to_string(t.open) + " counters=" + std::to_string(t.counters) +
               " unsent=" + std::to_string(t.unsentSpend) + " hosttrades=" + std::to_string(s.hostTrades()) + " windows=" +
               std::to_string(kenshi::OpenInventoryWindows()) + " cats=" + std::to_string(mine) + " merchant=" + name + " merchantcats=" +
               std::to_string(theirs);
    }
    if (cmd == "tradelist") {   // tradelist [merchant|player]: the items of that side of the open trade window
        std::string side = "merchant";
        in >> side;
        std::vector<kenshi::WindowItem> items;
        if (!kenshi::TradeWindowItems(side != "player", items)) return "err no trade window";
        std::string out = "ok " + std::to_string(items.size());
        for (const auto& it : items)
            out += " " + it.state.templateSid + ":" + std::to_string(it.state.quantity) + "@" + it.section + ":" + std::to_string(it.x) + "," + std::to_string(it.y);
        return out;
    }
    if (cmd == "tradebuy" || cmd == "tradesell") {
        // tradebuy [index]: right click on the merchant's item #index (one unit is bought, the game's way)
        // tradesell [index]: right click on our item #index (one unit is sold); worn gear only when
        // nothing else is carried (bought gear is often put on at once)
        size_t idx = 0;
        in >> idx;
        const bool buy = cmd == "tradebuy";
        std::vector<kenshi::WindowItem> items;
        if (!kenshi::TradeWindowItems(buy, items)) return "err no trade window";
        std::vector<kenshi::WindowItem> pick;
        for (const auto& it : items)
            if (buy || !it.state.equipped) pick.push_back(it);
        if (!buy && pick.empty()) pick = items;
        if (idx >= pick.size()) return "err only " + std::to_string(pick.size()) + " items";
        int32_t before = 0, after = 0;
        kenshi::ReadPlayerMoney(before);
        const int r = kenshi::TradeRightClick(buy, pick[idx]);
        kenshi::ReadPlayerMoney(after);
        return (r == 0 ? "ok " : "err result=" + std::to_string(r) + " ") + pick[idx].state.templateSid + " cats " + std::to_string(before) + "->" +
               std::to_string(after);
    }
    if (cmd == "squadanimals") {   // squadanimals: the squad indexes (SortedSquad order) that are animals, with their stacks
        auto squad = SortedSquad(w);
        std::string out;
        int n = 0;
        for (size_t i = 0; i < squad.size(); ++i) {
            kenshi::Character* c = w.FindSquad(squad[i]);
            if (!kenshi::IsAnimal(c)) continue;
            std::vector<kc::ItemState> items;
            kenshi::ReadInventory(c, items);
            out += " " + std::to_string(i) + ":" + Key(squad[i]) + ":" + std::to_string(items.size());
            ++n;
        }
        return "ok " + std::to_string(n) + out;
    }
    if (cmd == "caravan" || cmd == "caravanbeast" || cmd == "caravanopen") {
        // caravan: the nearest living stranger without a home building whose squad has pack animals
        // (a travelling trader): "trader=<name> key=<key> home=0 members=N animals=M stock=<stacks of
        // each member> back=<stacks in the worn backpacks its window sells from> packs=<members wearing
        // one>". caravanbeast: its first pack animal becomes
        // the target of kill/npcstate. caravanopen <squadIndex>: that squad member goes next to the
        // trader and opens the trade window (the game's own call).
        size_t idx = 0;
        in >> idx;
        auto squad = SortedSquad(w);
        kc::Vec3 mp, p;
        kenshi::Character* me = idx < squad.size() ? w.FindSquad(squad[idx]) : nullptr;
        if (!me || !kenshi::GetPosition(me, mp)) return "err no squad";
        std::vector<kenshi::Character*> all;
        kenshi::ActiveCharacters(all);
        kenshi::Character* best = nullptr;
        float bestD = 1e30f;
        for (kenshi::Character* c : all) {
            kc::Handle h;
            if (kenshi::IsAnimal(c) || !kenshi::GetHandle(c, h) || w.FindSquad(h) || kenshi::IsDead(c) || kenshi::HasHomeBuilding(c) ||
                !kenshi::GetPosition(c, p))
                continue;
            void* sq = kenshi::SquadOf(c);
            bool beast = false;
            for (kenshi::Character* o : all)
                if (o != c && kenshi::IsAnimal(o) && kenshi::SquadOf(o) == sq && !kenshi::IsDead(o)) beast = true;
            const float d = (p.x - mp.x) * (p.x - mp.x) + (p.z - mp.z) * (p.z - mp.z);
            if (sq && beast && d < bestD) { bestD = d; best = c; }
        }
        if (!best) return "err no caravan around";
        void* sq = kenshi::SquadOf(best);
        int members = 0, animals = 0, back = 0, packs = 0;
        std::string stock;
        kenshi::Character* firstBeast = nullptr;
        for (kenshi::Character* o : all) {
            if (kenshi::SquadOf(o) != sq) continue;
            ++members;
            if (kenshi::IsAnimal(o)) { ++animals; if (!firstBeast && !kenshi::IsDead(o)) firstBeast = o; }
            std::vector<kc::ItemState> items;
            kenshi::ReadInventory(o, items);
            stock += (stock.empty() ? "" : ",") + std::to_string(items.size());
            if (void* pack = kenshi::WornBackpack(o)) {
                std::vector<kc::ItemState> inside;
                kenshi::ReadInventory(pack, inside);
                back += int(inside.size());
                ++packs;
            }
        }
        std::string name;
        kenshi::CharacterName(best, name);
        kc::Handle th;
        kenshi::GetHandle(best, th);
        if (cmd == "caravanbeast") {
            kc::Handle bh;
            if (!firstBeast || !kenshi::GetHandle(firstBeast, bh)) return "err no living pack animal";
            lastSpawned_ = bh;
            std::vector<kc::ItemState> items;
            kenshi::ReadInventory(firstBeast, items);
            return "ok " + Key(bh) + " stacks=" + std::to_string(items.size());
        }
        if (cmd == "caravanopen") {
            if (kenshi::GetPosition(best, p)) {
                kc::Quat q;
                kenshi::GetRotation(me, q);
                HostCallScope scope;
                kenshi::Teleport(me, {p.x + 6.0f, p.y + 1.0f, p.z + 6.0f}, q);
            }
            return kenshi::OpenTradeWindow(me, best) ? "ok " + name : "err call failed";
        }
        std::replace(name.begin(), name.end(), ' ', '_');
        return "ok trader=" + name + " key=" + Key(th) + " home=0 members=" + std::to_string(members) + " animals=" + std::to_string(animals) +
               " stock=" + stock + " back=" + std::to_string(back) + " packs=" + std::to_string(packs) + " at=" + std::to_string(int(std::sqrt(bestD)));
    }
    if (cmd == "shopcounters") {   // shopcounters <name part>: what that merchant sells from: "name/function/stacks" each
        std::string part;
        in >> part;
        std::replace(part.begin(), part.end(), '_', ' ');
        std::vector<kenshi::Character*> all;
        kenshi::ActiveCharacters(all);
        for (kenshi::Character* c : all) {
            std::string name;
            std::vector<void*> counters;
            if (!kenshi::CharacterName(c, name) || name.find(part) == std::string::npos || !kenshi::ShopCounters(c, counters)) continue;
            std::string out = "ok " + name + ":";
            for (void* f : counters) {
                std::string sid, shown;
                std::vector<kc::ItemState> items;
                kenshi::ObjectTemplate(f, sid);
                kenshi::TemplateDisplayName(sid, shown);
                kenshi::ReadInventory(f, items);
                out += " " + shown + "/" + std::to_string(kenshi::BuildingFunctionOf(f)) + "/" + std::to_string(items.size());
            }
            return out;
        }
        return "err no such merchant";
    }
    // ---- lot A: doors and locks. <who> is a squad index (around whom to look); <what> picks the
    // nearest object: "door" (any door), "lock" (any locked-able furniture: chest, cage...), or a part
    // of its name ('_' for spaces).
    if (cmd == "doors" || cmd == "doorstate" || cmd == "doorset" || cmd == "doorlocal" || cmd == "doorbutton" || cmd == "doororder" ||
        cmd == "doorsknown") {
        if (cmd == "doorsknown") return "ok known=" + std::to_string(s.doorsKnown()) + " applied=" + std::to_string(s.doorsApplied());
        size_t who = 0;
        std::string what = "door", action;
        int task = 0;
        in >> who;
        if (cmd == "doororder") in >> task;
        in >> what >> action;
        std::replace(what.begin(), what.end(), '_', ' ');
        auto squad = SortedSquad(w);
        if (who >= squad.size()) return "err no such squad member";
        kenshi::Character* me = w.FindSquad(squad[who]);
        kc::Vec3 mp, p;
        if (!kenshi::GetPosition(me, mp)) return "err";
        std::vector<void*> objs;
        kenshi::ObjectsNear(mp, cmd == "doors" ? 600.0f : 1500.0f, objs);
        void* best = nullptr;
        float bestD = 1e30f;
        kc::DoorState bestState;
        std::string bestName, list;
        int count = 0;
        for (void* o : objs) {
            kc::DoorState d;
            std::string sid, name;
            if (!kenshi::ReadDoor(o, d) || !kenshi::ObjectTemplate(o, sid) || !kenshi::ObjectPosition(o, p)) continue;
            kenshi::TemplateDisplayName(sid, name);
            const bool isDoor = d.kind == kc::DoorKind::Door;
            if (what == "door" ? !isDoor : what == "lock" ? isDoor : name.find(what) == std::string::npos) continue;
            const float dd = (p.x - mp.x) * (p.x - mp.x) + (p.y - mp.y) * (p.y - mp.y) + (p.z - mp.z) * (p.z - mp.z);
            ++count;
            if (cmd == "doors" && count <= 25)
                list += " " + name + "/" + (isDoor ? "d" : "l") + std::to_string(d.state) + "/f" + std::to_string(d.flags) + "/L" +
                        std::to_string(d.lockLevel) + "@" + std::to_string(int(std::sqrt(dd)));
            if (dd < bestD) { bestD = dd; best = o; bestState = d; bestName = name; }
        }
        if (cmd == "doors") return "ok " + std::to_string(count) + list;
        if (!best) return "err no such door around";
        auto describe = [&](void* o) {
            kc::DoorState d;
            kc::Vec3 at{};
            kenshi::ReadDoor(o, d);
            kenshi::ObjectPosition(o, at);
            char b[200];
            snprintf(b, sizeof(b), "ok %s kind=%d state=%d flags=%d level=%d amount=%.2f at=%.1f,%.1f,%.1f", bestName.c_str(), int(d.kind), int(d.state),
                     int(d.flags), d.lockLevel, double(d.openAmount), at.x, at.y, at.z);
            return std::string(b);
        };
        if (cmd == "doorstate") return describe(best);
        if (cmd == "doorbutton") {   // the door panel's button, exactly as a click (clients: goes to the host)
            if (bestState.kind != kc::DoorKind::Door) return "err not a door";
            return kenshi::PressDoorButton(best, action == "lock" ? kc::DoorAction::LockButton : kc::DoorAction::OpenButton) ? describe(best)
                                                                                                                       : "err call failed";
        }
        if (cmd == "doororder") {   // the order a right click gives (72 open, 73 close, 76 pick lock, 77 lock, 78 unlock, 81 bash)
            kenshi::GetPosition(me, mp);
            kenshi::ObjectPosition(best, p);
            bool ok = false;
            kenshi::WithSelection(me, [&] { ok = kenshi::CallAddTaskNearestObject(task, best, p); });
            return (ok ? "ok " : "err ") + bestName + " at " + std::to_string(int(std::sqrt(bestD)));
        }
        // doorset (as the host's game would: through HostCallScope) / doorlocal (no scope: what the
        // client's own game would try, which must be refused there)
        kc::DoorState d = bestState;
        if (action == "open") d.state = 1;
        else if (action == "close") d.state = 0;
        else if (action == "lock") d.flags |= kc::kDoorLocked;
        else if (action == "unlock") d.flags &= uint8_t(~kc::kDoorLocked);
        else if (action == "break") d.flags |= kc::kDoorBroken;
        else if (action == "fix") d.flags &= uint8_t(~kc::kDoorBroken);
        else return "err action: open close lock unlock break fix";
        if ((action == "lock" || action == "unlock") && !(d.flags & kc::kDoorHasLock)) return "err it has no lock";
        if (cmd == "doorset") {
            HostCallScope scope;
            if (!kenshi::ApplyDoorState(best, d)) return "err apply failed";
        } else if (!kenshi::ApplyDoorState(best, d)) {
            return "err apply failed";
        }
        return describe(best);
    }
    if (cmd == "console") {   // console <line>: a host console command, as typed in the window (output in the log)
        std::string line;
        std::getline(in, line);
        while (!line.empty() && line.front() == ' ') line.erase(line.begin());
        HostConsoleInject(line);
        return "ok";
    }
    if (cmd == "vitals") {   // vitals <squadIndex>: blood and the lowest body part health
        size_t idx = 0;
        in >> idx;
        auto squad = SortedSquad(w);
        kc::EntityVitals v;
        if (idx >= squad.size() || !w.ReadVitals(squad[idx], v)) return "err";
        float low = 1e9f;
        for (const auto& p : v.parts) low = std::min(low, p.flesh);
        char b[96];
        snprintf(b, sizeof(b), "ok blood=%.1f lowest=%.1f flags=%d", v.blood, low, int(v.flags));
        return b;
    }
    if (cmd == "closewindows") return kenshi::CloseInventoryWindows() ? "ok" : "err";   // every inventory / trade window here
    // ---- lot E: buildings
    if (cmd == "buildtypes") {   // buildtypes <name part>: building templates ("sid=name" each, '_' for spaces)
        std::string part;
        in >> part;
        std::replace(part.begin(), part.end(), '_', ' ');
        std::vector<std::pair<std::string, std::string>> found;
        kenshi::BuildingTemplates(part, found, 20);
        std::string out = "ok " + std::to_string(found.size());
        for (auto& [sid, name] : found) {
            std::replace(name.begin(), name.end(), ' ', '_');
            out += " " + sid + "=" + name;
        }
        return out;
    }
    if (cmd == "buildplace" || cmd == "furnplace" || cmd == "buildplaceat" || cmd == "buildcheck" || cmd == "buildcheckat") {
        // buildplace <sid> <dx> <dz> [yawDeg] [squadIndex] [force]: a placement as build mode makes it, next to squad member 0 (or that one)
        //   (client: asked of the host; host: built and announced). Checked first as build mode
        //   checks a spot (CheckPlacement): "err invalid spot: <why>" and nothing is placed. "force"
        //   (client only) skips that local check, so that the host's own check is what refuses it.
        // buildplaceat <sid> <x> <z> [yawDeg] [force]: the same at a world position
        // buildcheck <sid> <dx> <dz> [yawDeg] [squadIndex] / buildcheckat <sid> <x> <z> [yawDeg]:
        //   only the check ("ok valid" or "err invalid spot: <why>")
        // furnplace <sid> <building name part> <dx> <dz>: a piece of furniture inside the nearest such
        //   building of ours (position relative to that building)
        const bool at = cmd == "buildplaceat" || cmd == "buildcheckat";
        const bool onlyCheck = cmd == "buildcheck" || cmd == "buildcheckat";
        std::string sid, part, tok;
        float dx = 0, dz = 0, yaw = 0;
        size_t near0 = 0;
        bool force = false;
        if (cmd == "furnplace") in >> sid >> part >> dx >> dz;
        else {
            in >> sid >> dx >> dz >> yaw;
            while (in >> tok) {
                if (tok == "force") force = true;
                else if (!at) near0 = std::strtoul(tok.c_str(), nullptr, 10);
            }
        }
        std::replace(part.begin(), part.end(), '_', ' ');
        kc::Vec3 me;
        if (!at) {
            auto squad = SortedSquad(w);
            if (near0 >= squad.size() || !kenshi::GetPosition(w.FindSquad(squad[near0]), me)) return "err no squad";
        }
        if (!kenshi::GameDataBySid(sid)) return "err unknown template " + sid;
        kc::BuildPlace p;
        p.sid = sid;
        const float a = yaw * 3.14159265f / 180.0f;
        p.rot = {std::cos(a / 2), 0, std::sin(a / 2), 0};
        if (cmd == "furnplace") {
            void* parent = w.NearestBuilding(me, part, 2000.0f, 2);
            std::string psid;
            kc::Vec3 ppos;
            if (!parent || !kenshi::ObjectTemplate(parent, psid) || !kenshi::ObjectPosition(parent, ppos)) return "err no building of ours named " + part;
            p.pos = {dx, 0.0f, dz};
            p.parentSid = psid;
            p.parentPos = ppos;
            p.indoorsSid = psid;
            p.indoorsPos = ppos;
        } else {
            // height: above the terrain (with water), as build mode gives it to the factory
            p.pos = at ? kc::Vec3{dx, 0.0f, dz} : kc::Vec3{me.x + dx, 0.0f, me.z + dz};
            std::string why, whyFr;
            const bool valid = w.CheckPlacement(p, why, whyFr);
            char where[64];
            snprintf(where, sizeof(where), " at %.1f,%.1f", double(p.pos.x), double(p.pos.z));
            if (onlyCheck) return valid ? std::string("ok valid") + where : "err invalid spot: " + why + where;
            if (!valid && !(force && !s.isHost())) return "err invalid spot: " + why + where;
            if (!valid) Log("debug placement of %s: invalid here (%s), sent anyway (force)", sid.c_str(), why.c_str());
        }
        return w.DebugPlace(p) ? "ok " + sid + (s.isHost() ? " built" : " asked") : "err placement failed";
    }
    if (cmd == "groundat") {   // groundat <x> <z>: the ground there (-99: unknown here), build mode's reference height, land / water / unknown
        float x = 0, z = 0, g = 0, ww = 0;
        in >> x >> z;
        if (!KenshiWorld::GroundAt(x, z, g, ww)) return "err no game";
        char b[96];
        snprintf(b, sizeof(b), "ok %.1f %.1f %s", double(g), double(ww), g == -99.0f ? "unknown" : (g < 98.0f ? "water" : "land"));
        return b;
    }
    if (cmd == "chatlast") {   // chatlast [n]: the last chat lines here, notices from the host included (" | " between lines)
        size_t n = 5;
        in >> n;
        const auto& lines = s.chatLog();
        std::string out = "ok";
        for (size_t i = lines.size() > n ? lines.size() - n : 0; i < lines.size(); ++i) out += (out.size() > 2 ? " | " : " ") + lines[i];
        return out;
    }
    if (cmd == "buildlist") {   // buildlist [name part] [radius] [squadIndex]: buildings around squad member 0 or that one ("sid@x,y,z:progress/flags" each)
        std::string part = "any";
        float radius = 1500.0f;
        size_t near0 = 0;
        in >> part >> radius;
        if (!(in >> near0)) near0 = 0;
        if (!(radius > 0) || radius > 5000.0f) radius = 1500.0f;
        std::replace(part.begin(), part.end(), '_', ' ');
        auto squad = SortedSquad(w);
        kc::Vec3 me;
        if (near0 >= squad.size() || !kenshi::GetPosition(w.FindSquad(squad[near0]), me)) return "err no squad";
        std::vector<void*> objs;
        kenshi::ObjectsNear(me, radius, objs);
        std::vector<std::pair<float, std::string>> found;   // nearest first: both games list the same ones
        int buildings = 0;
        for (void* o : objs) {
            std::string sid, name;
            kc::Vec3 p;
            float progress = 0;
            uint8_t flags = 0;
            // the object itself, not a handle round trip (furniture and town buildings resolve badly)
            if (kenshi::IsCharacter(o) || !KenshiWorld::ReadBuildStateOf(o, progress, flags)) continue;
            ++buildings;
            if (!kenshi::ObjectTemplate(o, sid) || !kenshi::ObjectPosition(o, p)) continue;
            if (!kenshi::TemplateDisplayName(sid, name)) name = sid;
            if (part != "any" && name.find(part) == std::string::npos && sid.find(part) == std::string::npos) continue;
            char b[200];
            snprintf(b, sizeof(b), " %s@%.1f,%.1f,%.1f:%.1f/%u", sid.c_str(), p.x, p.y, p.z, progress, unsigned(flags));
            const float dx = p.x - me.x, dz = p.z - me.z;
            found.push_back({dx * dx + dz * dz, b});
        }
        std::sort(found.begin(), found.end());
        const int n = int(std::min<size_t>(found.size(), 40));
        std::string out;
        for (int i = 0; i < n; ++i) out += found[size_t(i)].second;
        if (n == 0) return "ok 0 (objects=" + std::to_string(objs.size()) + " buildings=" + std::to_string(buildings) + " radius=" + std::to_string(int(radius)) + ")";
        return "ok " + std::to_string(n) + out;
    }
    if (cmd == "buildcount") return "ok followed=" + std::to_string(s.buildingCount()) + " found=" + std::to_string(s.buildingsResolved());
    if (cmd == "buildprogress" || cmd == "buildbuy" || cmd == "builddismantle" || cmd == "buildforsale") {
        // buildprogress <name part> <amount>: (host) workers' progress on the nearest unfinished building of ours
        // buildbuy <name part>: confirm buying the nearest building for sale (as its window does)
        // builddismantle <name part>: confirm dismantling the nearest building of ours (as its window does)
        // buildforsale [name part]: the nearest building for sale and its price
        std::string part = "any";
        float amount = 0;
        in >> part >> amount;
        std::replace(part.begin(), part.end(), '_', ' ');
        auto squad = SortedSquad(w);
        kc::Vec3 me;
        if (squad.empty() || !kenshi::GetPosition(w.FindSquad(squad[0]), me)) return "err no squad";
        const int want = cmd == "buildprogress" ? 3 : (cmd == "builddismantle" ? 2 : 1);
        void* b = w.NearestBuilding(me, part, 3000.0f, want);
        std::string sid;
        kc::Vec3 p;
        if (!b || !kenshi::ObjectTemplate(b, sid) || !kenshi::ObjectPosition(b, p)) return "err no such building";
        char where[160];
        snprintf(where, sizeof(where), "%s@%.1f,%.1f,%.1f", sid.c_str(), p.x, p.y, p.z);
        using AmountFn = void (*)(void*, float);
        using AnswerFn = void (*)(void*, int);
        using PriceFn = int (*)(void*);
        if (cmd == "buildprogress") {
            if (!s.isHost()) return "err host only";
            HostCallScope scope;
            reinterpret_cast<AmountFn>(kenshi::FnAddr(kenshi::FnAddConstructionProgress))(b, amount);
            return std::string("ok ") + where;
        }
        if (cmd == "buildforsale") return "ok " + std::string(where) + " price=" + std::to_string(reinterpret_cast<PriceFn>(kenshi::FnAddr(kenshi::FnCalculateSaleValue))(b));
        // no HostCallScope: exactly what the building's window does (a client asks the host)
        reinterpret_cast<AnswerFn>(kenshi::FnAddr(cmd == "buildbuy" ? kenshi::FnBuyMeCallback : kenshi::FnConfirmDismantle))(b, 2);
        return std::string("ok ") + where;
    }
    // ---- construction, purchases and supplies (coop_test construct / buyhouse / farlong)
    if (cmd == "givemoney") {   // givemoney <n>: (host) the player faction gets n more cats
        int32_t n = 0, m = 0;
        in >> n;
        if (!s.isHost()) return "err host only";
        if (!kenshi::ReadPlayerMoney(m) || !kenshi::WritePlayerMoney(m + n)) return "err no money";
        kenshi::ReadPlayerMoney(m);
        return "ok " + std::to_string(m);
    }
    if (cmd == "itemtypes") {   // itemtypes <name part>: item templates ("sid=name" each, '_' for spaces)
        std::string part;
        in >> part;
        std::replace(part.begin(), part.end(), '_', ' ');
        std::vector<std::pair<std::string, std::string>> found;
        kenshi::ItemTemplates(part, found, 20);
        std::string out = "ok " + std::to_string(found.size());
        for (auto& [sid, name] : found) {
            std::replace(name.begin(), name.end(), ' ', '_');
            out += " " + sid + "=" + name;
        }
        return out;
    }
    if (cmd == "giveitem") {   // giveitem <templateSid|name part> <n> <squadIndex>: (host) n new items into that member's inventory
        std::string what;
        int32_t n = 1;
        size_t idx = 0;
        in >> what >> n >> idx;
        if (!s.isHost()) return "err host only";
        std::replace(what.begin(), what.end(), '_', ' ');
        auto squad = SortedSquad(w);
        if (idx >= squad.size()) return "err no such squad member";
        // a name can belong to several records (the item, and e.g. a research or a dialogue line called
        // "Building Materials", which the item factory refuses): items first (ItemTemplates), then
        // each candidate until one is an item the factory makes
        std::vector<std::string> cands;
        if (kenshi::GameDataBySid(what)) cands.push_back(what);
        std::vector<std::pair<std::string, std::string>> found;
        kenshi::ItemTemplates(what, found, 64);
        for (const auto& f : found) cands.push_back(f.first);
        if (cands.empty()) return "err unknown item " + what;
        std::string why, all;
        HostCallScope scope;
        for (const auto& sid : cands) {
            if (kenshi::GiveNewItem(w.FindSquad(squad[idx]), sid, n, &why)) return "ok " + sid + " x" + std::to_string(n);
            if (all.size() < 400) all += (all.empty() ? "" : "; ") + why;
        }
        return "err " + all;
    }
    if (cmd == "invcount") {   // invcount <squadIndex|all> <templateSid|name part>: how many of those items that member (or the whole squad) carries
        std::string who, part;
        in >> who >> part;
        std::replace(part.begin(), part.end(), '_', ' ');
        auto squad = SortedSquad(w);
        long total = 0;
        for (size_t i = 0; i < squad.size(); ++i) {
            if (who != "all" && who != std::to_string(i)) continue;
            std::vector<kc::ItemState> items;
            if (!kenshi::ReadInventory(w.FindSquad(squad[i]), items)) continue;
            for (const auto& it : items) {
                std::string name;
                if (!kenshi::TemplateDisplayName(it.templateSid, name)) name = it.templateSid;
                if (it.templateSid == part || name.find(part) != std::string::npos) total += it.quantity;
            }
        }
        return "ok " + std::to_string(total);
    }
    if (cmd == "buildreq") {   // buildreq <selectIndex> <task> [name part]: that member alone is ordered to work on the nearest unfinished building of ours (newPlayerTaskSelectedCharacters, as a right click does)
        size_t sel = 0;
        int task = 0;
        std::string part = "any";
        in >> sel >> task >> part;
        std::replace(part.begin(), part.end(), '_', ' ');
        auto squad = SortedSquad(w);
        if (sel >= squad.size()) return "err no such squad member";
        kenshi::Character* me = w.FindSquad(squad[sel]);
        kc::Vec3 mp, bp;
        if (!kenshi::GetPosition(me, mp)) return "err";
        void* b = w.NearestBuilding(mp, part, 3000.0f, 3);
        std::string sid;
        if (!b || !kenshi::ObjectTemplate(b, sid) || !kenshi::ObjectPosition(b, bp)) return "err no unfinished building of ours";
        bool ok = false;
        kenshi::WithSelection(me, [&] { ok = kenshi::CallNewPlayerTaskOn(task, b, bp, b); });
        char where[160];
        snprintf(where, sizeof(where), "%s@%.1f,%.1f,%.1f", sid.c_str(), bp.x, bp.y, bp.z);
        return (ok ? "ok " : "err call failed ") + std::string(where);
    }
    if (cmd == "buildinfo") {   // buildinfo [name part] [squadIndex]: the nearest building (any owner): sid@pos forsale ours price
        std::string part = "any";
        size_t near0 = 0;
        in >> part;
        if (!(in >> near0)) near0 = 0;
        std::replace(part.begin(), part.end(), '_', ' ');
        auto squad = SortedSquad(w);
        kc::Vec3 me, p;
        if (near0 >= squad.size() || !kenshi::GetPosition(w.FindSquad(squad[near0]), me)) return "err no squad";
        void* b = nullptr;
        if (const size_t at = part.find('@'); at != std::string::npos) {   // sid@x,y,z: that very building
            kc::Vec3 q;
            if (sscanf(part.c_str() + at + 1, "%f,%f,%f", &q.x, &q.y, &q.z) == 3) b = w.BuildingAt(part.substr(0, at), q);
        } else {
            b = w.NearestBuilding(me, part, 3000.0f, 0);
        }
        std::string sid;
        if (!b || !kenshi::ObjectTemplate(b, sid) || !kenshi::ObjectPosition(b, p)) return "err no such building";
        using PriceFn = int (*)(void*);
        char o[240];
        snprintf(o, sizeof(o), "ok %s@%.1f,%.1f,%.1f forsale=%d ours=%d price=%d", sid.c_str(), p.x, p.y, p.z, KenshiWorld::IsBuildingForSale(b) ? 1 : 0,
                 KenshiWorld::IsPlayerBuilding(b) ? 1 : 0, reinterpret_cast<PriceFn>(kenshi::FnAddr(kenshi::FnCalculateSaleValue))(b));
        return o;
    }
    // ---- lot D: prisons
    if (cmd == "cage" || cmd == "chain" || cmd == "enslave" || cmd == "captive") {
        // cage <squadIndex> [off]: (host) into the nearest cage (BF_CAGE) within 300 m, or out of it
        // chain <squadIndex> [off]: (host) the game's own shackling (equips shackles) / unshackling
        // enslave <squadIndex> <0-3>: (host) slave state (0 not, 1 slave, 2 escaping, 3 ex-slave)
        // captive <squadIndex>: what holds it captive here (in=2: in a cage) and where it is
        size_t idx = 0;
        std::string arg;
        in >> idx >> arg;
        auto squad = SortedSquad(w);
        if (idx >= squad.size()) return "err no such squad member";
        kenshi::Character* c = w.FindSquad(squad[idx]);
        if (!c) return "err not here";
        if (cmd == "captive") {
            kenshi::Captivity k;
            if (!kenshi::ReadCaptivity(c, k)) return "err";
            std::string cage = "-", sid;
            if (k.cage && kenshi::ObjectTemplate(k.cage, sid)) cage = sid;
            const std::string fac = kenshi::FactionSidOf(k.slaveOf);
            kc::Vec3 p;
            kenshi::GetPosition(c, p);
            char b[400];
            snprintf(b, sizeof(b), "ok in=%d cage=%s chained=%d slave=%d slaveof=%s escaped=%d kidnapped=%d sentence=%.1f pos=%.2f,%.2f,%.2f captives=%zu",
                     k.inSomething, cage.c_str(), k.chained ? 1 : 0, k.slaveState, fac.empty() ? "-" : fac.c_str(), k.escaped ? 1 : 0,
                     k.kidnapped ? 1 : 0, double(k.sentence), p.x, p.y, p.z, s.captiveCount());
            return b;
        }
        HostCallScope scope;
        if (cmd == "cage") {
            if (arg == "off") return kenshi::SetPrisonMode(c, false, nullptr) ? "ok out" : "err call failed";
            kc::Vec3 p;
            if (!kenshi::GetPosition(c, p)) return "err";
            void* cage = kenshi::NearestCage(p, 3000.0f);
            if (!cage) return "err no cage around";
            std::string sid, name;
            kenshi::ObjectTemplate(cage, sid);
            kenshi::TemplateDisplayName(sid, name);
            const bool ok = kenshi::SetPrisonMode(c, true, cage);
            int now = 0;
            kenshi::ReadInSomething(c, now);
            return (ok ? "ok " : "err ") + name + " in=" + std::to_string(now);
        }
        if (cmd == "chain") return kenshi::CallSetChainedMode(c, arg != "off") ? "ok" : "err call failed";
        int state = 1;
        try { state = std::stoi(arg); } catch (...) { return "err state 0-3"; }
        return kenshi::CallSetSlaveState(c, state) ? "ok" : "err call failed";
    }
    if (cmd == "tradegui") {   // the trade window request the game has not consumed yet (type 0 = none)
        int type = -1;
        std::memcpy(&type, reinterpret_cast<const void*>(kenshi::Addr(kenshi::rva::TradeGui) + 0x58), 4);
        return "ok " + std::to_string(type);
    }
    if (std::string out; RangedDebugCommand(s, w, in, cmd, out)) return out;   // lot C: rangedlist, shoot, shots, turrets, turretaim, rangedaim
    if (cmd == "trace") {   // trace <handle> <frames>: log the client's position corrections of a character
        std::string k;
        in >> k >> w.traceFrames;
        sscanf(k.c_str(), "%u:%u:%u:%u:%u", &w.traceHandle.type, &w.traceHandle.container, &w.traceHandle.containerSerial,
               &w.traceHandle.index, &w.traceHandle.serial);
        return "ok";
    }
    if (cmd == "invmove") {   // invmove <from> <to> <itemIndex|main|worn> [qty]: what an inventory drag & drop does
        std::string fromS, toS, which;
        size_t idx = 0;
        int qty = 0;
        int toX = -1, toY = -1;
        std::string toSec = "main";
        in >> fromS >> toS >> which >> qty >> toX >> toY >> toSec;
        auto squad = SortedSquad(w);
        auto pick = [&](const std::string& s) -> kc::Handle {
            if (s == "spawned") return lastSpawned_;
            if (s.rfind("squad", 0) == 0) { size_t i = std::stoul(s.substr(5)); return i < squad.size() ? squad[i] : kc::Handle{}; }
            kc::Handle h;
            sscanf(s.c_str(), "%u:%u:%u:%u:%u", &h.type, &h.container, &h.containerSerial, &h.index, &h.serial);
            return h;
        };
        // bag:<who> is the backpack that character wears
        auto holder = [&](const std::string& s) -> void* {
            if (s.rfind("bag:", 0) == 0) return kenshi::WornBackpack(w.Find(pick(s.substr(4))));
            return w.Find(pick(s));
        };
        void* a = holder(fromS);
        void* b = holder(toS);
        std::vector<kc::ItemState> items;
        if (!a || !b || !kenshi::ReadInventory(a, items)) return "err bad characters";
        if (which == "main" || which == "worn") {   // first loose item / first equipped item
            idx = items.size();
            for (size_t i = 0; i < items.size() && idx == items.size(); ++i)
                if ((which == "main") == (items[i].section == "main")) idx = i;
        } else {
            idx = std::strtoul(which.c_str(), nullptr, 10);
        }
        if (idx >= items.size()) return "err no such item";
        kc::InvOp op;
        op.item = items[idx];
        if (qty > 0) op.item.quantity = std::min(qty, op.item.quantity);
        op.toSection = toSec;
        if (toS.rfind("bag:", 0) == 0 && toSec == "main") op.toSection = kenshi::FirstSectionName(b);   // a backpack's own section
        op.toX = int16_t(toX);   // -1: anywhere it fits
        op.toY = int16_t(toY);
        std::string e;
        HostCallScope scope;
        return kenshi::MoveInventoryItem(a, b, op, &e) ? "ok " + op.item.templateSid : "err " + e;
    }
    if (cmd == "bag") {   // bag <who>: the backpack it wears: "ok <template> <stacks> sid:qty ..." (spawned|squadN|key)
        std::string who;
        in >> who;
        auto squad = SortedSquad(w);
        kc::Handle h;
        if (who == "spawned") h = lastSpawned_;
        else if (who.rfind("squad", 0) == 0) { size_t i = std::stoul(who.substr(5)); if (i < squad.size()) h = squad[i]; }
        else sscanf(who.c_str(), "%u:%u:%u:%u:%u", &h.type, &h.container, &h.containerSerial, &h.index, &h.serial);
        void* pack = kenshi::WornBackpack(w.Find(h));
        if (!pack) return "err no worn backpack";
        std::string sid;
        kenshi::ObjectTemplate(pack, sid);
        std::vector<kc::ItemState> items;
        kenshi::ReadInventory(pack, items);
        std::string out = "ok " + sid + " " + std::to_string(items.size());
        for (const auto& it : items) out += " " + it.templateSid + ":" + std::to_string(it.quantity);
        return out;
    }
    if (cmd == "invsecs") {   // invsecs <who>: that character's items as "index:section" (spawned|squadN|key)
        std::string who;
        in >> who;
        auto squad = SortedSquad(w);
        kc::Handle h;
        if (who == "spawned") h = lastSpawned_;
        else if (who.rfind("squad", 0) == 0) { size_t i = std::stoul(who.substr(5)); if (i < squad.size()) h = squad[i]; }
        else sscanf(who.c_str(), "%u:%u:%u:%u:%u", &h.type, &h.container, &h.containerSerial, &h.index, &h.serial);
        kenshi::Character* c = w.Find(h);
        std::vector<kc::ItemState> items;
        if (!c || !kenshi::ReadInventory(c, items)) return "err bad character";
        std::string r = "ok " + std::to_string(items.size());
        for (size_t i = 0; i < items.size(); ++i) r += " " + std::to_string(i) + ":" + items[i].section;
        return r;
    }
    if (cmd == "invswap") {   // invswap <from> <to> <section>: what dropping <from>'s item there on <to>'s (occupied) slot does
        std::string fromS, toS, sec;
        in >> fromS >> toS >> sec;
        auto squad = SortedSquad(w);
        auto pick = [&](const std::string& s) -> kc::Handle {
            if (s == "spawned") return lastSpawned_;
            if (s.rfind("squad", 0) == 0) { size_t i = std::stoul(s.substr(5)); return i < squad.size() ? squad[i] : kc::Handle{}; }
            kc::Handle h;
            sscanf(s.c_str(), "%u:%u:%u:%u:%u", &h.type, &h.container, &h.containerSerial, &h.index, &h.serial);
            return h;
        };
        kenshi::Character* a = w.Find(pick(fromS));
        kenshi::Character* b = w.Find(pick(toS));
        std::vector<kc::ItemState> ia, ib;
        if (!a || !b || !kenshi::ReadInventory(a, ia) || !kenshi::ReadInventory(b, ib)) return "err bad characters";
        // "any": the first worn section (not the bag) occupied on both sides
        if (sec == "any") {
            sec.clear();
            for (const auto& x : ia)
                if (x.section != "main" && std::any_of(ib.begin(), ib.end(), [&](const kc::ItemState& y) { return y.section == x.section; })) { sec = x.section; break; }
            if (sec.empty()) {
                std::string sa, sb;
                for (const auto& x : ia) sa += " " + x.section;
                for (const auto& x : ib) sb += " " + x.section;
                return "err no common occupied section; from:" + sa + " | to:" + sb;
            }
        }
        auto inSec = [&](const std::vector<kc::ItemState>& v) { return std::find_if(v.begin(), v.end(), [&](const kc::ItemState& i) { return i.section == sec; }); };
        auto xa = inSec(ia);
        auto xb = inSec(ib);
        if (xa == ia.end() || xb == ib.end()) return "err nothing in " + sec + " on both";
        kc::InvOp opA, opB;
        opA.item = *xa; opA.toSection = xb->section; opA.toX = xb->x; opA.toY = xb->y;
        opB.item = *xb; opB.toSection = xa->section; opB.toX = xa->x; opB.toY = xa->y;
        std::string e;
        HostCallScope scope;
        return kenshi::SwapInventoryItems(a, b, opA, opB, &e) ? "ok " + xa->templateSid + " <-> " + xb->templateSid : "err " + e;
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
        std::vector<kc::RegionWeather> wr;
        w.ReadWeather(wr);
        for (const auto& r : wr)
            o << "weather " << r.regionSid << " season=" << r.seasonSid << " type=" << r.weatherSid << " str=" << r.strength
              << " end=" << r.endMinutes << '\n';
        o << w.EffectsReport();
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
        kenshi::DeadBodies(all);
        for (kenshi::Character* c : all) {
            kc::Handle h;
            kc::Vec3 p;
            if (!kenshi::GetHandle(c, h) || !kenshi::GetPosition(c, p)) continue;
            o << "dead " << Key(w.HostHandleOf(h)) << " isdead=" << kenshi::IsDead(c) << " pos=" << p.x << ',' << p.y << ',' << p.z << '\n';
            bool inRange = false;
            for (const auto& ctr : centers) {
                const float dx = ctr.x - p.x, dy = ctr.y - p.y, dz = ctr.z - p.z;
                if (dx * dx + dy * dy + dz * dz <= radius * radius) { inRange = true; break; }
            }
            if (inRange) DumpCharacter(o, "char", w, h);   // corpses are compared like everyone else
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
