#include "debug.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "hooks.h"
#include "steam_link.h"
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
    if (kenshi::Character* ch = w.Find(h)) o << " class=" << std::hex << kenshi::ClassRvas(ch) << std::dec;
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
        const bool ok = cmd == "ko" ? kenshi::CallKnockout(c) : kenshi::CallDeclareDead(c);
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
    if (cmd == "groundnear") {   // groundnear <radius>: items lying around squad[0]: key template x,y,z;...
        float radius = 500;
        in >> radius;
        auto squad = SortedSquad(w);
        kc::Vec3 base;
        if (squad.empty() || !kenshi::GetPosition(w.FindSquad(squad[0]), base)) return "err no squad";
        std::vector<void*> items;
        kenshi::GroundItemsNear(base, radius, items);
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
        char b[120];
        snprintf(b, sizeof(b), "ok %.3f,%.3f,%.3f %s", p.x, p.y, p.z, Key(h).c_str());
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
    if (cmd == "carryreq") {   // carryreq <selectIndex> <targetIndex>: that member alone is ordered to carry the other (as a right click does)
        size_t sel = 0, tgt = 0;
        in >> sel >> tgt;
        auto squad = SortedSquad(w);
        if (sel >= squad.size() || tgt >= squad.size()) return "err";
        bool ok = false;
        kenshi::WithSelection(w.FindSquad(squad[sel]), [&] { ok = kenshi::CallAddTaskNearest(225, w.FindSquad(squad[tgt])); });   // LIFT_PERSON_PLAYER_ORDER
        return ok ? "ok" : "err";
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
    if (cmd == "tpplayer") {   // tpplayer <playerId>: (host) that player's characters next to squad member 0, as the admin teleport does
        int id = -1;
        in >> id;
        auto squad = SortedSquad(w);
        std::vector<kc::Handle> who;
        for (const auto& h : squad) if (s.ownerOf(h) == id) who.push_back(h);
        kc::Vec3 p;
        if (who.empty() || squad.empty() || !kenshi::GetPosition(w.FindSquad(squad[0]), p)) return "err";
        return "ok " + std::to_string(w.TeleportCharacters(who, p));
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
        in >> part;
        std::replace(part.begin(), part.end(), '_', ' ');
        auto squad = SortedSquad(w);
        kc::Vec3 p, op;
        if (squad.empty() || !kenshi::GetPosition(w.FindSquad(squad[0]), p)) return "err";
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
                !kenshi::ObjectPosition(o, op) || !kenshi::ReadInventory(o, items) || items.empty())
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
        return kenshi::CallKnockout(w.FindSquad(squad[idx])) ? "ok" : "err";
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
    if (cmd == "closewindows") return kenshi::CloseInventoryWindows() ? "ok" : "err";   // every inventory / trade window here
    if (cmd == "tradegui") {   // the trade window request the game has not consumed yet (type 0 = none)
        int type = -1;
        std::memcpy(&type, reinterpret_cast<const void*>(kenshi::Addr(kenshi::rva::TradeGui) + 0x58), 4);
        return "ok " + std::to_string(type);
    }
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
        kenshi::Character* a = w.Find(pick(fromS));
        kenshi::Character* b = w.Find(pick(toS));
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
        op.toX = int16_t(toX);   // -1: anywhere it fits
        op.toY = int16_t(toY);
        std::string e;
        HostCallScope scope;
        return kenshi::MoveInventoryItem(a, b, op, &e) ? "ok " + op.item.templateSid : "err " + e;
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
