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
    if (cmd == "spawnnpc") {   // spawnnpc <dx> <dz>: host creates an NPC (copy of a nearby one) next to squad[0]
        float dx = 0, dz = 0;
        in >> dx >> dz;
        auto squad = SortedSquad(w);
        kc::Vec3 base;
        if (squad.empty() || !kenshi::GetPosition(w.FindSquad(squad[0]), base)) return "err no squad";
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
