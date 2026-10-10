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

#include <process.h>

#include "kc/admin.h"
#include "kc/call_scopes.h"
#include "kc/protocol.h"
#include "kc/session.h"
#include "kc/streaming.h"
#include "kc/world_identity.h"
#include "../plugin/map_view.h"

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
    CaptiveState cap;      // lot D: cage, shackles, slavery (netId unused)
    std::vector<int32_t> jobs;   // fix G5: its job list (Tâches panel), by kind
    std::vector<JobEntry> jobList;   // the same with targets (JobState)
    bool passive = false;        // the squad bar's PASSIVE toggle
    std::string name;            // empty: "C<serial>"
    bool animal = false;
    std::string bagSid;          // the backpack it wears (empty: none); its handle serial is local
    uint32_t bagSerial = 0;
    std::vector<ItemState> bag;
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
    // character editor (client): off unless a test turns it on; it stays open until the test closes it
    bool editorSupported = false, editorOpen = false;
    int editorOpened = 0;
    bool OpenCharacterEditor(const Handle&) override {
        if (!editorSupported) return false;
        editorOpen = true;
        ++editorOpened;
        return true;
    }
    bool CharacterEditorOpen() override { return editorOpen; }
    int exportFrames = 3, exportCountdown = -1;
    int importFrames = 5, importCountdown = 0;
    std::vector<WorldFile> pendingImport;
    bool corruptImport = false;
    int imports = 0;
    int spawns = 0, despawns = 0;
    bool refuseSpawn = false;                 // client: the factory refuses everything (0.3.1 tests)
    std::vector<Handle> remoteInterest;       // host: SetRemoteInterest's last list
    int resyncs = 0;                          // client: ResyncCharacter calls
    void SetRemoteInterest(const std::vector<Handle>& handles) override { remoteInterest = handles; }
    void ResyncCharacter(const Handle&) override { ++resyncs; }

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
        for (auto& [s, c] : chars) {   // the squad too, as the game's active characters (KenshiWorld)
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
        if (refuseSpawn || chars.count(h.serial) || info.templateSid != "tmpl-" + std::to_string(h.serial)) return false;
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
    int halts = 0;                                     // host: characters halted (their player gone)
    void HaltCharacter(const Handle& h) override { (void)h; ++halts; }
    // actor safety: what each order subject is (serial -> kc::TargetFlags), orders run, refusal
    std::map<uint32_t, uint32_t> subjectFlags;
    int tasksRun = 0;
    std::vector<uint32_t> orderedSerials;   // every character an order ran on
    std::string refusal;
    ResultReason refusalReason = ResultReason::None;
    std::string TakeOrderRefusal(ResultReason& r) override {
        r = refusal.empty() ? ResultReason::None : refusalReason;
        return std::exchange(refusal, std::string{});
    }
    bool Order(const Handle& h, const Command& c) override {
        auto it = chars.find(h.serial);
        if (it == chars.end()) return false;
        if (c.kind == CommandKind::Task) {   // as the plugin does: the subject checked against the task
            uint32_t f = 0;
            if (c.subject.valid()) {
                f = kTgtNamed;
                if (auto sf = subjectFlags.find(c.subject.serial); sf != subjectFlags.end()) f = sf->second | kTgtNamed | kTgtFound;
                if (c.subject.serial == h.serial) f |= kTgtSelf;
            }
            if (!TaskTargetAllowed(c.via, c.task, f, nullptr)) {
                refusal = "Action refusée : cet ordre ne s'applique pas à cette cible.";
                refusalReason = ResultReason::WrongTarget;
                return false;
            }
            ++tasksRun;
            if (c.via == TaskVia::SetOrder && c.task == 13) it->second.passive = c.shift ? c.add : !it->second.passive;
        }
        orderedSerials.push_back(h.serial);
        if (c.kind == CommandKind::MoveTo) it->second.dest = c.pos;
        else it->second.dest = it->second.pos;
        return true;
    }
    void TakeLocalOrders(std::vector<std::pair<Handle, Command>>& out) override { out.swap(localOrders); localOrders.clear(); }
    // containers (shop counters): handle type 0, items by serial; where they stand
    struct FakeBox { std::string sid; Vec3 pos; std::vector<ItemState> items; };
    std::map<uint32_t, FakeBox> boxes;
    static Handle B(uint32_t serial) { Handle h; h.type = 0; h.index = serial; h.serial = serial; return h; }
    static constexpr uint32_t kBagType = 0x2E;   // a worn backpack (an item handle)
    static Handle Pk(uint32_t serial) { Handle h; h.type = kBagType; h.index = serial; h.serial = serial; return h; }
    std::vector<ItemState>* ItemsOf(const Handle& h) {
        if (h.type == 0) { auto b = boxes.find(h.serial); return b == boxes.end() ? nullptr : &b->second.items; }
        if (h.type == kBagType) {
            for (auto& [s, c] : chars)
                if (!c.bagSid.empty() && c.bagSerial == h.serial) return &c.bag;
            return nullptr;
        }
        auto it = chars.find(h.serial);
        return it == chars.end() ? nullptr : &it->second.items;
    }
    bool ReadInventory(const Handle& h, std::vector<ItemState>& out) override {
        auto* items = ItemsOf(h);
        if (!items) return false;
        out = *items;
        return true;
    }
    bool ApplyInventory(const Handle& h, const std::vector<ItemState>& items) override {
        auto* mine = ItemsOf(h);
        if (!mine) return false;
        *mine = items;
        return true;
    }
    std::vector<std::pair<Handle, ItemState>> localDrops;   // client: items the player dropped to the ground
    void TakeLocalDrops(std::vector<std::pair<Handle, ItemState>>& out) override { out.swap(localDrops); localDrops.clear(); }
    int drops = 0;
    Handle lastDropper;
    bool ExecuteInvOp(const Handle& from, const Handle& to, const InvOp& op) override {
        auto* a = ItemsOf(from);
        auto* b = ItemsOf(to);
        if (!a || !b) return false;
        if (op.kind == InvOpKind::Drop) { ++drops; lastDropper = to; }
        for (size_t i = 0; i < a->size(); ++i) {
            ItemState& it = (*a)[i];
            if (!it.sameKind(op.item) || it.quantity < op.item.quantity) continue;
            // like the game: a slot holds one thing; a pile of the same kind takes more of it
            ItemState* onto = nullptr;
            if (op.kind == InvOpKind::Move)
                for (auto& o : *b)
                    if (&o != &it && o.section == op.toSection && o.x == op.toX && o.y == op.toY) onto = &o;
            if (onto && !onto->sameKind(op.item)) { ++invRefusals; return false; }
            ItemState moved = it;
            moved.quantity = op.item.quantity;
            moved.section = op.toSection; moved.x = op.toX; moved.y = op.toY;
            const int qty = op.item.quantity;
            it.quantity -= qty;
            if (it.quantity == 0) {
                if (onto && a == b && onto > &it) --onto;   // the erase shifts it
                a->erase(a->begin() + ptrdiff_t(i));
            }
            if (op.kind == InvOpKind::Move) {
                if (onto) onto->quantity += qty;
                else b->push_back(moved);
            }
            return true;
        }
        return false;
    }
    int invRefusals = 0, invSwaps = 0;
    bool ExecuteInvSwap(const Handle& from, const Handle& to, const InvOp& x, const InvOp& y) override {
        auto* a = ItemsOf(from);
        auto* b = ItemsOf(to);
        if (!a || !b) return false;
        auto at = [](std::vector<ItemState>& v, const ItemState& want) -> ItemState* {
            for (auto& i : v)
                if (i == want) return &i;
            return nullptr;
        };
        ItemState* ia = at(*a, x.item);
        ItemState* ib = at(*b, y.item);
        if (!ia || !ib || ia == ib) return false;
        ia->section = x.toSection; ia->x = x.toX; ia->y = x.toY;
        ib->section = y.toSection; ib->x = y.toX; ib->y = y.toY;
        ItemState ma = *ia, mb = *ib;
        a->erase(a->begin() + (ia - a->data()));
        ib = at(*b, mb);
        b->erase(b->begin() + (ib - b->data()));
        b->push_back(ma);
        a->push_back(mb);
        ++invSwaps;
        return true;
    }
    float DistanceTo(const Handle& who, const Vec3& pos) override {
        auto it = chars.find(who.serial);
        return it == chars.end() ? 1e9f : Dist(it->second.pos, pos);
    }
    std::string CharacterNameOf(const Handle& h) override { return "npc-" + std::to_string(h.serial); }
    // conversations: what the host's game reports, answers and ends asked of it, who talks with whom
    std::vector<WorldDialog> dialogEvents;
    void TakeDialogEvents(std::vector<WorldDialog>& out) override { out.swap(dialogEvents); dialogEvents.clear(); }
    std::vector<std::pair<uint32_t, int>> answers;
    void DialogAnswer(uint32_t dialogId, int index) override { answers.emplace_back(dialogId, index); }
    std::vector<uint32_t> ended;
    void EndDialog(uint32_t dialogId) override { ended.push_back(dialogId); }
    std::map<uint32_t, uint32_t> talking;   // npc serial -> serial of who it talks with
    bool TalkingWith(const Handle& npc, Handle& other) override {
        auto it = talking.find(npc.serial);
        if (it == talking.end()) return false;
        other = H(it->second);
        return true;
    }
    int saysShown = 0;
    void ApplySay(const Handle&, const std::string&, bool) override { ++saysShown; }
    std::vector<MapThreat> threats;   // map: what ReadMapThreats answers
    size_t threatCalls = 0;
    void ReadMapThreats(const std::vector<Vec3>& centers, float radius, std::vector<MapThreat>& out) override {
        (void)radius;
        ++threatCalls;
        out = centers.empty() ? std::vector<MapThreat>{} : threats;
    }
    bool ContainerKind(const Handle& h, std::string& sid) override {
        auto b = h.type == 0 ? boxes.find(h.serial) : boxes.end();
        if (b == boxes.end()) return false;
        sid = b->second.sid;
        return true;
    }
    bool FindContainer(const std::string& sid, const Vec3& pos, Handle& out) override {
        for (auto& [s, b] : boxes)
            if (b.sid == sid && Dist(b.pos, pos) < 30) { out = B(s); return true; }
        return false;
    }
    // trade: the player faction's cats, merchants' cats and counters; trade windows
    int32_t money = 0;
    std::map<uint32_t, int32_t> merchantMoney;
    std::map<uint32_t, std::vector<uint32_t>> shops;   // merchant serial -> its counters
    std::vector<TradeRequest> tradeReqs;               // host: what the game asked for
    bool tradeWindow = false;                          // client: the trade window is open
    int tradeWindowOpens = 0;
    bool ReadMoney(int32_t& m) override { m = money; return true; }
    std::map<uint32_t, uint8_t> floors;   // fix G6: floor group per character serial (absent: 9, the ground floor)
    bool ReadFloor(const Handle& h, uint8_t& g) override {
        if (!chars.count(h.serial)) return false;
        auto it = floors.find(h.serial);
        g = it == floors.end() ? 9 : it->second;
        return true;
    }
    void ApplyFloor(const Handle& h, uint8_t g) override { if (chars.count(h.serial)) floors[h.serial] = g; }
    void ApplyMoney(int32_t m) override { money = m; }
    void TakeTradeRequests(std::vector<TradeRequest>& out) override { out.swap(tradeReqs); tradeReqs.clear(); }
    bool ShopCounters(const Handle& trader, std::vector<ShopCounter>& out) override {
        out.clear();
        auto s = shops.find(trader.serial);
        if (s == shops.end()) return false;
        for (uint32_t b : s->second) out.push_back({B(b), boxes[b].sid, boxes[b].pos});
        return !out.empty();
    }
    bool MoneyOf(const Handle& who, int32_t& m) override {
        if (chars.count(who.serial) && chars[who.serial].squad) { m = money; return true; }
        auto it = merchantMoney.find(who.serial);
        if (it == merchantMoney.end()) return false;
        m = it->second;
        return true;
    }
    void SetMoneyOf(const Handle& who, int32_t m) override { if (!chars[who.serial].squad) merchantMoney[who.serial] = m; }
    bool PayTrade(const Handle& buyer, const Handle& trader, int32_t price) override {
        (void)buyer;
        money -= price;
        merchantMoney[trader.serial] += price;
        return true;
    }
    bool OpenTradeWindow(const Handle&, const Handle&) override { tradeWindow = true; ++tradeWindowOpens; return true; }
    // travelling merchants: their squads (merchant serial -> wearers), worn backpacks, animals
    std::map<uint32_t, std::vector<uint32_t>> caravans;
    std::vector<std::pair<uint32_t, uint32_t>> joins;   // JoinSquadOf(who, leader) asked
    int thefts = 0;
    bool WornBackpack(const Handle& wearer, Handle& bag, std::string& sid) override {
        auto it = chars.find(wearer.serial);
        if (wearer.type == kBagType || it == chars.end() || it->second.bagSid.empty()) return false;
        bag = Pk(it->second.bagSerial);
        sid = it->second.bagSid;
        return true;
    }
    bool TravellingCounters(const Handle& trader, std::vector<Handle>& wearers) override {
        wearers.clear();
        auto c = caravans.find(trader.serial);
        if (c == caravans.end()) return false;
        for (uint32_t s : c->second) if (chars.count(s) && !chars[s].bagSid.empty()) wearers.push_back(Hc(s));
        return !wearers.empty();
    }
    bool IsAnimal(const Handle& h) override { auto it = chars.find(h.serial); return h.type != kBagType && it != chars.end() && it->second.animal; }
    bool JoinSquadOf(const Handle& who, const Handle& leader) override { joins.emplace_back(who.serial, leader.serial); return true; }
    int TheftCheck(const Handle& thief, const Handle& from, const ItemState& item) override {
        (void)thief; (void)from; (void)item;
        ++thefts;
        return 1;   // unseen
    }
    bool ContainerWindowOpen() override { return tradeWindow; }
    void CloseContainerWindows() override { tradeWindow = false; }
    // ---- lot E: buildings (handle type 0, serials from bldgSerial: they differ between machines)
    struct FakeBldg { std::string sid; Vec3 pos; float progress = 0; uint8_t flags = 0; bool forSale = false, ours = true; };
    std::map<uint32_t, FakeBldg> bldgs;
    uint32_t bldgSerial = 20000;
    std::vector<LocalPlacement> placements;     // build mode here
    std::vector<BuildAction> bldgActions;       // windows here (client: asked of the host)
    std::vector<Handle> bldgRemoved;            // host: destroyed by the game
    std::set<uint32_t> tracked;
    int placementsBuilt = 0;
    static Handle Bh(uint32_t serial) { Handle h; h.type = 0; h.index = serial; h.serial = serial; return h; }
    void TakeLocalPlacements(std::vector<LocalPlacement>& out) override { out.swap(placements); placements.clear(); }
    bool ExecutePlacement(const BuildPlace& p, Handle& created, Vec3& worldPos) override {
        if (p.sid.empty() || p.sid == "refused") return false;
        FakeBldg b;
        b.sid = p.sid;
        b.pos = {p.pos.x, p.pos.y + 100.0f, p.pos.z};   // world height: terrain + the placement's
        const uint32_t s = bldgSerial++;
        bldgs[s] = b;
        created = Bh(s);
        worldPos = b.pos;
        ++placementsBuilt;
        return true;
    }
    bool CheckPlacement(const BuildPlace& p, std::string& why, std::string& whyFr) override {
        ++placementsChecked;
        why = whyFr = p.sid == "inwater" ? "in water" : "";
        if (p.sid == "inwater") whyFr = "dans l'eau ou l'acide.";
        return p.sid != "inwater";
    }
    int placementsChecked = 0;
    bool FindBuilding(const std::string& sid, const Vec3& pos, Handle& out) override {
        for (auto& [s, b] : bldgs)
            if (b.sid == sid && Dist(b.pos, pos) < 5) { out = Bh(s); return true; }
        return false;
    }
    bool BuildingIdentity(const Handle& h, std::string& sid, Vec3& pos) override {
        auto it = bldgs.find(h.serial);
        if (h.type != 0 || it == bldgs.end()) return false;
        sid = it->second.sid;
        pos = it->second.pos;
        return true;
    }
    bool ReadBuildState(const Handle& h, float& progress, uint8_t& flags) override {
        auto it = bldgs.find(h.serial);
        if (h.type != 0 || it == bldgs.end()) return false;
        progress = it->second.progress;
        flags = it->second.flags;
        return true;
    }
    void ApplyBuildState(const Handle& h, float progress, uint8_t flags) override {
        auto it = bldgs.find(h.serial);
        if (it != bldgs.end()) { it->second.progress = progress; it->second.flags = flags; }
    }
    void ConstructionSitesNear(const std::vector<Vec3>& centers, float radius, std::vector<Handle>& out) override {
        out.clear();
        for (auto& [s, b] : bldgs) {
            if (!b.ours || ((b.flags & kSiteComplete) && !(b.flags & kSiteDismantling))) continue;
            for (auto& c : centers) if (Dist(c, b.pos) <= radius) { out.push_back(Bh(s)); break; }
        }
    }
    void TrackBuilding(const Handle& h) override { tracked.insert(h.serial); }
    void TakeBuildingRemovals(std::vector<Handle>& out) override { out.swap(bldgRemoved); bldgRemoved.clear(); }
    bool RemoveBuilding(const Handle& h) override { return bldgs.erase(h.serial) != 0; }
    void TakeLocalBuildActions(std::vector<BuildAction>& out) override { out.swap(bldgActions); bldgActions.clear(); }
    bool ExecuteBuildAction(const BuildAction& a, std::string& refused) override {
        refused.clear();
        Handle h;
        if (!FindBuilding(a.sid, a.pos, h)) return false;
        FakeBldg& b = bldgs[h.serial];
        if (a.kind == BuildActionKind::Buy) {
            if (!b.forSale) { refused = "plus a vendre"; return b.ours; }
            if (money < 100) { refused = "pas assez"; return false; }
            money -= 100;
            b.forSale = false;
            b.ours = true;
            return true;
        }
        if (!b.ours) return false;
        b.flags |= kSiteDismantling;
        return true;
    }
    // ---- lot D: prisons
    bool ReadCaptive(const Handle& h, CaptiveState& out) override {
        auto it = chars.find(h.serial);
        if (it == chars.end()) return false;
        out = it->second.cap;
        return true;
    }
    int captiveApplies = 0;
    void ApplyCaptive(const Handle& h, const CaptiveState& s) override {
        auto it = chars.find(h.serial);
        if (it == chars.end()) return;
        it->second.cap = s;
        it->second.cap.netId = 0;
        ++captiveApplies;
    }
    // ---- fix G5: job lists
    bool ReadJobs(const Handle& h, std::vector<int32_t>& out) override {
        auto it = chars.find(h.serial);
        if (it == chars.end()) return false;
        out = it->second.jobs;
        return true;
    }
    void ApplyJobs(const Handle& h, const std::vector<int32_t>& host) override {   // as KenshiWorld: removals only
        auto it = chars.find(h.serial);
        if (it == chars.end()) return;
        std::map<int32_t, int> left;
        for (int32_t j : host) ++left[j];
        std::vector<int32_t> kept;
        for (int32_t j : it->second.jobs)
            if (left[j] > 0) { --left[j]; kept.push_back(j); }
        it->second.jobs = kept;
    }
    // ---- squad window: squads by id (host: the game's; client: ours, mapped to the host's)
    struct FSquad { Handle id; std::string name; std::vector<uint32_t> members; };
    std::vector<FSquad> squads;
    std::map<uint32_t, Handle> squadIds;   // client: our squad (id serial) -> the host's
    std::vector<LocalSquadRequest> squadReqs;
    std::vector<Handle> sharedSet;
    uint32_t nextSquad = 700;
    int sharedOrders = 0;
    static Handle SquadId(uint32_t s) { Handle h; h.type = 9; h.index = s; h.serial = s; return h; }
    FSquad* SquadBy(const Handle& id) { for (auto& q : squads) if (q.id == id) return &q; return nullptr; }
    void TakeOut(uint32_t serial) { for (auto& q : squads) q.members.erase(std::remove(q.members.begin(), q.members.end(), serial), q.members.end()); }
    std::string NameOf(uint32_t s) { auto it = chars.find(s); return it == chars.end() ? "" : it->second.name.empty() ? "C" + std::to_string(s) : it->second.name; }
    void ReadSquadViews(std::vector<SquadView>& out) override {
        out.clear();
        for (auto& q : squads) {
            SquadView v; v.id = q.id; v.key = q.id.serial; v.name = q.name;
            for (uint32_t m : q.members) v.members.push_back(Hc(m));
            out.push_back(v);
        }
    }
    bool SquadMove(const Handle& who, const Handle& squad, int index, bool swap, const std::string& name, Handle& created) override {
        FSquad* t = squad.valid() ? SquadBy(squad) : nullptr;
        if (squad.valid() && !t) return false;
        if (!t) { squads.push_back({SquadId(nextSquad++), name, {}}); t = &squads.back(); created = t->id; }
        auto& m = t->members;
        if (swap) {
            auto a = std::find(m.begin(), m.end(), who.serial);
            if (a == m.end() || index >= int(m.size())) return false;
            std::iter_swap(a, m.begin() + index);
            return true;
        }
        TakeOut(who.serial);
        m.insert(m.begin() + std::min<int>(index, int(m.size())), who.serial);
        return true;
    }
    bool SquadCreate(const std::string& name, Handle& created) override { squads.push_back({SquadId(nextSquad++), name, {}}); created = squads.back().id; return true; }
    bool SquadRename(const Handle& id, const std::string& name) override { FSquad* q = SquadBy(id); if (q) q->name = name; return q != nullptr; }
    bool SquadOrder(const Handle& id, int index) override {
        auto it = std::find_if(squads.begin(), squads.end(), [&](const FSquad& q) { return q.id == id; });
        if (it == squads.end()) return false;
        FSquad q = *it; squads.erase(it);
        squads.insert(squads.begin() + std::min<int>(index, int(squads.size())), q);
        return true;
    }
    bool SquadRemove(const Handle& id) override {
        auto it = std::find_if(squads.begin(), squads.end(), [&](const FSquad& q) { return q.id == id && q.members.empty(); });
        if (it == squads.end()) return false;
        squads.erase(it);
        return true;
    }
    bool RenameCharacter(const Handle& h, const std::string& name) override { auto it = chars.find(h.serial); if (it == chars.end()) return false; it->second.name = name; return true; }
    bool ReadCharacterName(const Handle& h, std::string& out) override { out = NameOf(h.serial); return !out.empty(); }
    void ApplyCharacterName(const Handle& h, const std::string& name) override { if (chars.count(h.serial)) chars[h.serial].name = name; }
    void ReadLocalSquads(std::vector<SquadView>& out) override {
        out.clear();
        for (auto& q : squads) {
            SquadView v; v.key = q.id.serial; v.name = q.name;
            if (auto it = squadIds.find(q.id.serial); it != squadIds.end()) v.id = it->second;
            for (uint32_t m : q.members) v.members.push_back(Hc(m));
            out.push_back(v);
        }
    }
    void ApplySquadViews(const std::vector<SquadView>& host) override {   // as KenshiWorld, simplified
        std::vector<FSquad> next;
        std::set<uint32_t> used;
        for (const auto& hv : host) {
            FSquad* mine = nullptr;
            for (auto& q : squads) if (!used.count(q.id.serial) && squadIds.count(q.id.serial) && squadIds[q.id.serial] == hv.id) mine = &q;
            if (!mine) for (auto& q : squads) if (!used.count(q.id.serial) && !squadIds.count(q.id.serial) && q.members.empty()) { mine = &q; break; }
            FSquad q = mine ? *mine : FSquad{SquadId(nextSquad++), "", {}};
            used.insert(q.id.serial);
            squadIds[q.id.serial] = hv.id;
            q.name = hv.name;
            q.members.clear();
            for (const auto& m : hv.members) q.members.push_back(m.serial);
            next.push_back(q);
        }
        for (auto& q : squads)   // ours the host does not have yet (asked of it), kept while empty
            if (!used.count(q.id.serial) && !squadIds.count(q.id.serial) && q.members.empty()) next.push_back(q);
        squads = next;
    }
    void RemoveLocalSquad(uint64_t key) override {
        squads.erase(std::remove_if(squads.begin(), squads.end(), [&](const FSquad& q) { return q.id.serial == key && q.members.empty(); }), squads.end());
    }
    // the legacy split (Squads): the host's squads from its characters; the client counts what it got
    int legacyApplies = 0;
    void ReadSquads(std::vector<WorldSquad>& out) override {
        out.clear();
        WorldSquad w; w.name = "Legacy";
        for (auto& [s, c] : chars) if (c.squad) w.members.push_back(Hc(s));
        if (!w.members.empty()) out.push_back(w);
    }
    void ApplySquads(const std::vector<WorldSquad>& sq) override { if (!sq.empty()) ++legacyApplies; }
    void TakeLocalSquadRequests(std::vector<LocalSquadRequest>& out) override { out.swap(squadReqs); squadReqs.clear(); }
    void SetShared(const std::vector<Handle>& handles) override { sharedSet = handles; }
    bool OrderShared(const Handle& h, const Command& c) override { ++sharedOrders; return Order(h, c); }
    bool ReadJobList(const Handle& h, std::vector<JobEntry>& out) override {
        auto it = chars.find(h.serial);
        if (it == chars.end()) return false;
        out = it->second.jobList;
        return true;
    }
    void ApplyJobList(const Handle& h, const std::vector<JobEntry>& host) override { if (chars.count(h.serial)) chars[h.serial].jobList = host; }
    // ---- lot B: factions: the player faction's relations, bounties per character (serial)
    FactionsMsg factions;
    std::map<uint32_t, CharBounties> bounties;
    bool ReadFactions(FactionsMsg& out) override { out = factions; return true; }
    size_t ApplyFactions(const FactionsMsg& m) override {
        if (m == factions) return 0;
        factions = m;
        return 1;
    }
    bool ReadBounties(const Handle& h, CharBounties& out) override {
        if (!chars.count(h.serial)) return false;
        out = bounties[h.serial];
        out.netId = 0;
        return true;
    }
    size_t ApplyBounties(const Handle& h, const CharBounties& b) override {
        CharBounties want = b;
        want.netId = 0;
        if (bounties[h.serial] == want) return 0;
        bounties[h.serial] = want;
        return 1;
    }
    // ---- diplomacy: relations between factions, unique characters, towns (as a game holds them)
    DiplomacyState diplo;
    bool ReadDiplomacy(DiplomacyState& out) override { out = diplo; return true; }
    size_t ApplyFactionPairs(const std::vector<FactionPairRelation>& pairs) override {
        size_t n = 0;
        for (const auto& p : pairs) {
            auto it = std::find_if(diplo.pairs.begin(), diplo.pairs.end(), [&](const FactionPairRelation& o) { return o.from == p.from && o.to == p.to; });
            if (it == diplo.pairs.end()) { diplo.pairs.push_back(p); ++n; }
            else if (!SamePairRelation(it->rel, p.rel)) { it->rel = p.rel; ++n; }
        }
        return n;
    }
    size_t ApplyUniques(const std::vector<UniqueState>& uniques) override {
        if (diplo.uniques == uniques) return 0;
        diplo.uniques = uniques;
        return 1;
    }
    size_t ApplyTowns(const std::vector<TownState>& towns) override {
        size_t n = 0;
        for (const auto& t : towns)
            for (auto& o : diplo.towns)
                if (o.sid == t.sid && !(o == t)) { o = t; ++n; }
        return n;
    }
    // ---- lot A: doors and locks (key: sid; the fake world has one door per kind)
    std::map<std::string, DoorState> doors;
    std::vector<DoorRequest> doorReqs;      // client: buttons the player clicked
    std::set<uint32_t> lockedBoxes;         // host: containers whose lock holds
    std::vector<ContainerRequest> containerReqs;   // client: right clicks on containers
    int doorApplies = 0;
    void ReadDoors(const std::vector<Vec3>& centers, float radius, std::vector<DoorState>& out) override {
        out.clear();
        for (auto& [sid, d] : doors)
            for (auto& c : centers)
                if (Dist(c, d.pos) <= radius) { out.push_back(d); break; }
    }
    bool ApplyDoor(const DoorState& d) override {
        auto it = doors.find(d.sid);
        if (it == doors.end() || Dist(it->second.pos, d.pos) > 10) return false;
        ++doorApplies;
        it->second = d;
        return true;
    }
    bool ExecuteDoorRequest(const DoorRequest& r) override {
        auto it = doors.find(r.sid);
        if (it == doors.end()) return false;
        DoorState& d = it->second;
        if (r.action == DoorAction::OpenButton) d.state = (d.state == 1 || d.state == 2) ? 0 : 1;
        else d.flags ^= kDoorLocked;
        return true;
    }
    void TakeDoorRequests(std::vector<DoorRequest>& out) override { out.swap(doorReqs); doorReqs.clear(); }
    bool ContainerLocked(const Handle& h) override { return h.type == 0 && lockedBoxes.count(h.serial) != 0; }
    void TakeContainerRequests(std::vector<ContainerRequest>& out) override { out.swap(containerReqs); containerReqs.clear(); }
    // ---- end lot A
    // ---- workshop: research (techs "tech-*"; one artifact pays a tech once), machines by sid
    ResearchState research;
    int artifacts = 0;
    std::set<std::string> paid;
    int researchApplies = 0, researchPays = 0;
    std::vector<LocalResearchAsk> researchAsks;
    std::vector<LocalMachineAsk> machineAsks;
    struct FakeMachine { MachineState s; uint32_t box = 0; std::vector<Handle> ops; };
    std::map<std::string, FakeMachine> machines;
    std::vector<TownPower> townPower;
    int machineApplies = 0;
    std::map<uint32_t, std::vector<float>> skills;   // ReadProgress: stats per character serial
    bool ReadProgress(const Handle& h, std::vector<float>& stats, uint16_t& modes, uint8_t& style) override {
        modes = 0; style = 0;
        auto it = skills.find(h.serial);
        if (it == skills.end()) { stats.clear(); return false; }
        stats = it->second;
        return true;
    }
    bool ReadResearch(ResearchState& out) override {
        out = research;
        std::sort(out.finished.begin(), out.finished.end());
        return true;
    }
    bool ExecuteResearchRequest(const ResearchRequest& r, const Handle& actor, std::string& why) override {
        auto inQueue = [&](const std::string& s) { for (auto& q : research.queue) if (q.sid == s) return true; return false; };
        auto finished = [&](const std::string& s) { return std::find(research.finished.begin(), research.finished.end(), s) != research.finished.end(); };
        if (r.action == ResearchAction::LearnBlueprint) {
            auto* items = ItemsOf(actor);
            if (!items) { why = "pas de personnage"; return false; }
            for (size_t i = 0; i < items->size(); ++i)
                if ((*items)[i].templateSid == r.sid) {
                    items->erase(items->begin() + ptrdiff_t(i));
                    research.finished.push_back("tech-of-" + r.sid);
                    return true;
                }
            why = "pas de plan";
            return false;
        }
        if (r.action == ResearchAction::Cancel) {
            for (size_t i = 0; i < research.queue.size(); ++i)
                if (research.queue[i].sid == r.sid) { research.queue.erase(research.queue.begin() + ptrdiff_t(i)); return true; }
            why = "pas dans la file";
            return false;
        }
        if (finished(r.sid)) { why = "deja recherchee"; return false; }
        if (inQueue(r.sid)) { why = "deja dans la file"; return false; }
        if (!paid.count(r.sid)) {
            if (artifacts <= 0) { why = "artefacts manquants"; return false; }
            --artifacts;
            ++researchPays;
            paid.insert(r.sid);
        }
        research.queue.push_back({r.sid, 0});
        return true;
    }
    size_t ApplyResearch(const ResearchState& s) override {
        if (s == research) return 0;
        research = s;
        ++researchApplies;
        return 1;
    }
    void ReadMachines(const std::vector<Vec3>& centers, float radius, std::vector<WorldMachine>& out, std::vector<TownPower>& towns) override {
        out.clear();
        for (auto& [sid, m] : machines)
            for (auto& c : centers)
                if (Dist(c, m.s.pos) <= radius) {
                    WorldMachine w;
                    w.state = m.s;
                    w.state.operatorCount = uint8_t(m.ops.size());
                    w.handle = m.box ? B(m.box) : Handle{};
                    w.operators = m.ops;
                    out.push_back(w);
                    break;
                }
        towns = townPower;
    }
    bool ExecuteMachineRequest(const MachineRequest& r, const Handle&, std::string& why) override {
        auto it = machines.find(r.sid);
        if (it == machines.end()) { why = "introuvable"; return false; }
        MachineState& s = it->second.s;
        switch (r.action) {
        case MachineAction::AddCraft: s.crafts.push_back({r.baseSid, r.materialSid, "item-" + r.baseSid, 0}); return true;
        case MachineAction::RemoveCraft:
            if (r.index < 0 || size_t(r.index) >= s.crafts.size()) { why = "plus la"; return false; }
            s.crafts.erase(s.crafts.begin() + r.index);
            return true;
        case MachineAction::SetRepeat: s.flags = uint8_t(r.value ? (s.flags | kMachRepeat) : (s.flags & ~kMachRepeat)); return true;
        case MachineAction::SetPower: s.flags = uint8_t(r.value ? (s.flags | kMachPowerOn) : (s.flags & ~kMachPowerOn)); return true;
        case MachineAction::SetBattery: s.flags = uint8_t(r.value ? (s.flags | kMachBatteryOn) : (s.flags & ~kMachBatteryOn)); return true;
        }
        return false;
    }
    bool ApplyMachine(const MachineState& s, const std::vector<Handle>& operators) override {
        auto it = machines.find(s.sid);
        if (it == machines.end()) return false;
        it->second.s = s;
        it->second.ops = operators;
        ++machineApplies;
        return true;
    }
    bool ApplyTownPower(const TownPower& t) override {
        for (auto& p : townPower) if (p.sid == t.sid) { p = t; return true; }
        townPower.push_back(t);
        return true;
    }
    void TakeWorkshopAsks(std::vector<LocalResearchAsk>& r, std::vector<LocalMachineAsk>& m) override {
        r.swap(researchAsks); researchAsks.clear();
        m.swap(machineAsks); machineAsks.clear();
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
    bool SetGameHours(double hours) override { time.gameHours = hours; ++hourSets; return true; }
    int hourSets = 0;
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

    // ---- lot C: ranged combat
    std::vector<WorldShot> shotsOut;                   // host: what its game fired
    std::vector<WorldShot> shotsFired;                 // client: the host's shots fired here
    std::map<uint32_t, WorldAim> aims;                 // host: characters in ranged combat (by serial)
    std::map<uint32_t, std::pair<bool, WorldAim>> aimsApplied;   // client: last imposed (ranged?, aim)
    std::vector<TurretAim> turrets;                    // host: turrets near the players
    std::vector<TurretAim> turretsApplied;             // client
    void TakeShots(std::vector<WorldShot>& out) override { out.swap(shotsOut); shotsOut.clear(); }
    bool ReplayShot(const WorldShot& s) override {
        if (!chars.count(s.shooter.serial)) return false;
        shotsFired.push_back(s);
        return true;
    }
    bool ReadRangedAim(const Handle& h, WorldAim& out) override {
        auto it = aims.find(h.serial);
        if (it == aims.end()) return false;
        out = it->second;
        return true;
    }
    void ApplyRangedAim(const Handle& h, bool ranged, const WorldAim& a) override { aimsApplied[h.serial] = {ranged, a}; }
    void ReadTurrets(const std::vector<Vec3>& centers, float, std::vector<TurretAim>& out) override { out = centers.empty() ? std::vector<TurretAim>{} : turrets; }
    void ApplyTurret(const TurretAim& t) override { turretsApplied.push_back(t); }

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

// Loopback ports for the sessions of this run: their own range per process (several test runs at
// once, e.g. in parallel worktrees, would otherwise connect to each other's hosts).
static uint16_t g_port = uint16_t(20000 + (_getpid() % 1400) * 32);

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
    if (std::getenv("KC_VERBOSE")) verbose = true;
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
    {   // fix G6: the stall notice
        Writer sw; Encode(sw, StallMsg{90});
        Reader sr(sw.data(), sw.size());
        CHECK(PeekType(sr) == Msg::Stall);
        StallMsg s2; CHECK(Decode(sr, s2)); CHECK(s2.seconds == 90);
        Writer qw; Encode(qw, JoinQueueMsg{2, 3, JoinPhase::Editor, "Joueur2"});   // join queue
        Reader qr(qw.data(), qw.size());
        CHECK(PeekType(qr) == Msg::JoinQueue);
        JoinQueueMsg q2; CHECK(Decode(qr, q2)); CHECK(q2.position == 2 && q2.total == 3 && q2.phase == JoinPhase::Editor && q2.current == "Joueur2");
        Writer bad; Encode(bad, JoinQueueMsg{4, 3, JoinPhase::Saving, "x"});   // a place beyond the queue: refused
        Reader br(bad.data(), bad.size()); PeekType(br);
        JoinQueueMsg q3; CHECK(!Decode(br, q3));
        FloorsMsg f; f.entries = {{3, 10}, {70000, 9}};
        Writer fw; Encode(fw, f);
        Reader fr(fw.data(), fw.size());
        CHECK(PeekType(fr) == Msg::Floors);
        FloorsMsg f2; CHECK(Decode(fr, f2)); CHECK(f2.entries.size() == 2 && f2.entries[1].netId == 70000 && f2.entries[0].group == 10);
    }

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
        DialogReply rep; rep.dialogId = 3; rep.actor = 9; rep.turn = 4; rep.index = 1;
        Writer rw; Encode(rw, rep);
        Reader rr(rw.data(), rw.size()); CHECK(PeekType(rr) == Msg::DialogReply);
        DialogReply rep2; CHECK(Decode(rr, rep2) && rep2.dialogId == 3 && rep2.actor == 9 && rep2.turn == 4 && rep2.index == 1);
        // the actor, the line and the "busy" kind travel; an answer naming no actor is not read
        DialogEvent busy; busy.kind = DialogKind::Busy; busy.netId = 5; busy.pcNetId = 9; busy.text = "Garde";
        txt.pcNetId = 9; txt.turn = 7;
        DialogMsg dm3; dm3.events = {txt, busy};
        Writer bw; Encode(bw, dm3);
        Reader br(bw.data(), bw.size()); CHECK(PeekType(br) == Msg::Dialog);
        DialogMsg dm4; CHECK(Decode(br, dm4));
        CHECK(dm4.events.size() == 2 && dm4.events[0].pcNetId == 9 && dm4.events[0].turn == 7 && dm4.events[1].kind == DialogKind::Busy &&
              dm4.events[1].pcNetId == 9);
        DialogReply noActor; noActor.dialogId = 3; noActor.index = 0;
        Writer nw; Encode(nw, noActor);
        Reader nr(nw.data(), nw.size()); CHECK(PeekType(nr) == Msg::DialogReply);
        DialogReply na2; CHECK(!Decode(nr, na2));
        DialogReply leave; leave.dialogId = 3; leave.actor = 9; leave.index = kDialogLeave;
        Writer lw; Encode(lw, leave);
        Reader lr(lw.data(), lw.size()); CHECK(PeekType(lr) == Msg::DialogReply);
        DialogReply lv2; CHECK(Decode(lr, lv2) && lv2.index == kDialogLeave);
        DialogReply bad; bad.dialogId = 3; bad.actor = 9; bad.index = -2;
        Writer xw; Encode(xw, bad);
        Reader xr(xw.data(), xw.size()); CHECK(PeekType(xr) == Msg::DialogReply);
        DialogReply bad2; CHECK(!Decode(xr, bad2));
        Result rb; rb.seq = 3; rb.state = ResultState::Rejected; rb.reason = ResultReason::Busy; rb.text = "Garde est occupé";
        Writer rbw; Encode(rbw, rb);
        Reader rbr(rbw.data(), rbw.size()); CHECK(PeekType(rbr) == Msg::Result);
        Result rb2; CHECK(Decode(rbr, rb2) && rb2.reason == ResultReason::Busy && rb2.text == rb.text);
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
        CHECK(std::string(TaskLabel(258)) == "sleep" && std::string(TaskLabel(9999)) == "?");
    }
    {   // ---- workshop: research, requests, machines
        ResearchState rs; rs.deskLevel = 3; rs.finished = {"tech-a", "tech-b"}; rs.queue = {{"tech-c", 12.5f}, {"tech-d", 0}};
        Writer rsw; Encode(rsw, rs);
        Reader rsr(rsw.data(), rsw.size()); CHECK(PeekType(rsr) == Msg::Research);
        ResearchState rs2; CHECK(Decode(rsr, rs2) && rs2 == rs);
        ResearchRequest rq; rq.seq = 7; rq.actorNetId = 12; rq.action = ResearchAction::LearnBlueprint; rq.sid = "bp-1";
        rq.item.templateSid = "bp-1"; rq.item.section = "main"; rq.item.x = 2; rq.item.y = 3;
        Writer qw; Encode(qw, rq);
        Reader qr(qw.data(), qw.size()); CHECK(PeekType(qr) == Msg::ResearchRequest);
        ResearchRequest rq2; CHECK(Decode(qr, rq2) && rq2.seq == 7 && rq2.actorNetId == 12 && rq2.action == ResearchAction::LearnBlueprint &&
                                   rq2.item.section == "main" && rq2.item.x == 2 && rq2.item.y == 3);
        rq.actorNetId = 0;   // no actor named: refused
        Writer qw2; Encode(qw2, rq);
        Reader qr2(qw2.data(), qw2.size()); PeekType(qr2);
        ResearchRequest rq3; CHECK(!Decode(qr2, rq3));
        MachinesMsg mm; mm.full = true;
        MachineState ms; ms.sid = "bench"; ms.pos = {1, 2, 3}; ms.netId = 44; ms.flags = kMachCrafting | kMachPowerOn; ms.maxOperators = 3; ms.operatorCount = 2;
        ms.operators = {5, 6}; ms.power = 1.5f; ms.stored = 20; ms.progress = 0.25f; ms.production = 3;
        ms.crafts = {{"sword", "iron", "item-sword", 0.5f}};
        mm.machines = {ms};
        TownPower tp; tp.sid = "bench"; tp.pos = {1, 2, 3}; tp.values[0] = 10; tp.values[7] = 4; tp.onBattery = true;
        mm.towns = {tp};
        Writer mw; Encode(mw, mm);
        Reader mr(mw.data(), mw.size()); CHECK(PeekType(mr) == Msg::Machines);
        MachinesMsg mm2; CHECK(Decode(mr, mm2) && mm2.full && mm2.machines.size() == 1 && mm2.machines[0].sameState(ms) && mm2.machines[0].sid == "bench" &&
                               mm2.towns.size() == 1 && mm2.towns[0] == tp);
        MachineRequest mq; mq.seq = 3; mq.actorNetId = 9; mq.action = MachineAction::AddCraft; mq.sid = "bench"; mq.pos = {1, 2, 3}; mq.baseSid = "sword";
        Writer xw; Encode(xw, mq);
        Reader xr(xw.data(), xw.size()); CHECK(PeekType(xr) == Msg::MachineRequest);
        MachineRequest mq2; CHECK(Decode(xr, mq2) && mq2.action == MachineAction::AddCraft && mq2.baseSid == "sword" && mq2.actorNetId == 9);
        mq.baseSid.clear();   // an order for nothing
        Writer xw2; Encode(xw2, mq);
        Reader xr2(xw2.data(), xw2.size()); PeekType(xr2);
        MachineRequest mq3; CHECK(!Decode(xr2, mq3));
        CHECK(std::string(StatNameFr(3)) == "Science" && std::string(StatNameFr(999)) == "?");
    }
    {   // ---- lot A: doors and locks
        DoorsMsg dm; dm.full = true;
        DoorState a; a.sid = "door-1"; a.pos = {1, 2, 3}; a.kind = DoorKind::Door; a.state = 2; a.flags = kDoorHasLock | kDoorWantsLock; a.lockLevel = 45; a.openAmount = 0.5f;
        DoorState b; b.sid = "chest-1"; b.pos = {-4, 0, 9}; b.kind = DoorKind::Lock; b.flags = kDoorHasLock | kDoorLocked; b.lockLevel = 80;
        dm.doors = {a, b};
        Writer dw; Encode(dw, dm);
        Reader dr(dw.data(), dw.size()); CHECK(PeekType(dr) == Msg::Doors);
        DoorsMsg dm2; CHECK(Decode(dr, dm2));
        CHECK(dm2.full && dm2.doors.size() == 2 && dm2.doors[0].sid == "door-1" && dm2.doors[0].state == 2 && dm2.doors[0].lockLevel == 45 &&
              dm2.doors[0].openAmount == 0.5f && dm2.doors[0].sameState(a) && dm2.doors[1].kind == DoorKind::Lock && dm2.doors[1].pos.z == 9 &&
              (dm2.doors[1].flags & kDoorLocked));
        dm.doors[0].state = 7;   // not a door state
        Writer bw; Encode(bw, dm);
        Reader br(bw.data(), bw.size()); PeekType(br);
        DoorsMsg dm3; CHECK(!Decode(br, dm3));
        DoorRequest rq; rq.sid = "door-1"; rq.pos = {1, 2, 3}; rq.action = DoorAction::LockButton;
        Writer qw; Encode(qw, rq);
        Reader qr(qw.data(), qw.size()); CHECK(PeekType(qr) == Msg::DoorRequest);
        DoorRequest rq2; CHECK(Decode(qr, rq2) && rq2.action == DoorAction::LockButton && rq2.pos.y == 2 && rq2.sid == "door-1");
    }
    {   // trading: the window with the shop's counters, then a purchase with its price
        TradeOpen to; to.traderNetId = 40; to.looterNetId = 7; to.traderMoney = -3;
        to.counters = {{41, "box-a", {1, 2, 3}}, {42, "box-b", {4, 5, 6}}};
        Writer tw; Encode(tw, to);
        Reader tr(tw.data(), tw.size()); CHECK(PeekType(tr) == Msg::TradeOpen);
        TradeOpen to2; CHECK(Decode(tr, to2));
        CHECK(to2.traderNetId == 40 && to2.looterNetId == 7 && to2.traderMoney == -3 && to2.counters.size() == 2 &&
              to2.counters[1].netId == 42 && to2.counters[1].sid == "box-b" && to2.counters[1].pos.z == 6 && to2.note.empty());
        {   // a travelling merchant: its counters are worn backpacks, known by their wearer
            TradeOpen tb; tb.traderNetId = 40; tb.looterNetId = 7;
            tb.counters = {{43, "bull-pack", {}, 44}};
            Writer bw2; Encode(bw2, tb);
            Reader br2(bw2.data(), bw2.size()); PeekType(br2);
            TradeOpen tb2; CHECK(Decode(br2, tb2) && tb2.counters.size() == 1 && tb2.counters[0].ownerNetId == 44 && tb2.counters[0].sid == "bull-pack");
            CHECK(to2.counters[0].ownerNetId == 0);
            BagBind bb; bb.netId = 43; bb.ownerNetId = 44; bb.sid = "bull-pack";
            Writer gw; Encode(gw, bb);
            Reader gr(gw.data(), gw.size()); CHECK(PeekType(gr) == Msg::BagBind);
            BagBind bb2; CHECK(Decode(gr, bb2) && bb2.netId == 43 && bb2.ownerNetId == 44 && bb2.sid == "bull-pack");
            BagBind bad; bad.netId = 43; bad.ownerNetId = 44;   // no template: refused
            Writer xw; Encode(xw, bad);
            Reader xr(xw.data(), xw.size()); PeekType(xr);
            BagBind bad2; CHECK(!Decode(xr, bad2));
        }
        TradeOpen none; none.traderNetId = 40; none.note = "pas d'etal";
        Writer nw; Encode(nw, none);
        Reader nr(nw.data(), nw.size()); PeekType(nr);
        TradeOpen none2; CHECK(Decode(nr, none2) && none2.counters.empty() && none2.note == "pas d'etal");
        InvOp buy; buy.fromNetId = 41; buy.toNetId = 7; buy.item.templateSid = "bread"; buy.item.quantity = 2; buy.toSection = "main";
        buy.traderNetId = 40; buy.price = 61;
        Writer bw; Encode(bw, buy);
        Reader br(bw.data(), bw.size()); PeekType(br);
        InvOp buy2; CHECK(Decode(br, buy2) && buy2.traderNetId == 40 && buy2.price == 61 && buy2.item.quantity == 2);
        buy.price = -25;
        Writer sw; Encode(sw, buy);
        Reader sr(sw.data(), sw.size()); PeekType(sr);
        InvOp sell; CHECK(Decode(sr, sell) && sell.price == -25);
    }
    {   // lot B: relations both ways, bounties and crime state
        FactionsMsg fm; fm.playerRank = 3; fm.reputationTrust = 1.5f; fm.reputationBadassery = -2;
        FactionRelationEntry e; e.factionSid = "10-gamedata.base"; e.hasOurs = true; e.ours.relation = -80; e.ours.war = true;
        e.hasTheirs = true; e.theirs.relation = -75.5f; e.theirs.alliance = false; e.theirs.coexists = true; e.theirs.trustNegatives = 4;
        FactionRelationEntry e2; e2.factionSid = "11-gamedata.base"; e2.hasTheirs = true; e2.theirs.relation = 40; e2.theirs.alliance = true;
        fm.factions = {e, e2};
        Writer lw2; Encode(lw2, fm);
        Reader lr2(lw2.data(), lw2.size()); CHECK(PeekType(lr2) == Msg::Factions);
        FactionsMsg fm2; CHECK(Decode(lr2, fm2) && fm2 == fm);
        BountiesMsg bm;
        CharBounties cb; cb.netId = 7; cb.crime = 3; cb.crimeFactionSid = "10-gamedata.base"; cb.crimeExpiry = 2.5f; cb.prisonSentence = 12;
        cb.prisonBegan = 123456789; cb.accessPassSid = "11-gamedata.base"; cb.accessPassUntil = 99;
        cb.bounties = {{"10-gamedata.base", 1500, 8u, true, 777}};
        bm.chars = {cb};
        Writer bw; Encode(bw, bm);
        Reader br(bw.data(), bw.size()); CHECK(PeekType(br) == Msg::Bounties);
        BountiesMsg bm2; CHECK(Decode(br, bm2) && bm2 == bm);
        std::vector<uint8_t> bad(bw.data(), bw.data() + bw.size());   // netId 0 is refused
        bad[2] = 0;
        Reader xr(bad.data(), bad.size()); PeekType(xr);
        BountiesMsg bm3; CHECK(!Decode(xr, bm3));
    }
    {   // diplomacy: the three parts, and what a decoder refuses
        DiplomacyMsg pm; pm.part = DiploPart::Pairs;
        FactionPairRelation fp; fp.from = "5-gamedata.base"; fp.to = "6-gamedata.base"; fp.rel.war = true; fp.rel.relation = -100; fp.rel.strength = 3;
        pm.pairs = {fp, {"6-gamedata.base", "5-gamedata.base", fp.rel}};
        DiplomacyMsg um; um.part = DiploPart::Uniques; um.uniques = {{"tinfist", kUniqueDead, true}, {"phoenix", kUniqueImprisoned, false}, {"ruka", kUniqueAlive, false}};
        DiplomacyMsg tm; tm.part = DiploPart::Towns; tm.towns = {{"town-1", "holy", "town-1-destroyed"}, {"town-2", "", ""}};
        for (const DiplomacyMsg* m : {&pm, &um, &tm}) {
            Writer dw; Encode(dw, *m);
            Reader dr(dw.data(), dw.size()); CHECK(PeekType(dr) == Msg::Diplomacy);
            DiplomacyMsg back; CHECK(Decode(dr, back) && back == *m);
        }
        auto refused = [](DiplomacyMsg m) { Writer w2; Encode(w2, m); Reader r2(w2.data(), w2.size()); PeekType(r2); DiplomacyMsg o; return !Decode(r2, o); };
        DiplomacyMsg self = pm; self.pairs[0].to = self.pairs[0].from; CHECK(refused(self));        // a faction toward itself
        DiplomacyMsg nosid = um; nosid.uniques[0].sid.clear(); CHECK(refused(nosid));
        DiplomacyMsg badstate = um; badstate.uniques[0].state = 3; CHECK(refused(badstate));
        DiplomacyMsg notown = tm; notown.towns[0].sid.clear(); CHECK(refused(notown));
        Writer bp; bp.u8(uint8_t(Msg::Diplomacy)); bp.u8(9); bp.varint(0);
        Reader bpr(bp.data(), bp.size()); PeekType(bpr); DiplomacyMsg o; CHECK(!Decode(bpr, o));   // unknown part
        // the game's own tests for ally / enemy (FactionRelations::isAlly, isEnemy)
        RelationState rs; rs.relation = 49; CHECK(StandingOf(rs) == Standing::Neutral);
        rs.relation = 50; CHECK(StandingOf(rs) == Standing::Ally);
        rs.relation = -30; CHECK(StandingOf(rs) == Standing::Enemy);
        rs.relation = -29; CHECK(StandingOf(rs) == Standing::Neutral);
        rs.alliance = true; CHECK(StandingOf(rs) == Standing::Ally);
        RelationState a, b2; a.relation = 10; b2.relation = 10.6f; b2.trustPositives = 40;
        CHECK(SamePairRelation(a, b2));            // trust and strength are the AI's: noise between NPC factions
        b2.relation = 11.2f; CHECK(!SamePairRelation(a, b2));
        b2.relation = 10; b2.war = true; CHECK(!SamePairRelation(a, b2));
    }
    {   // map: markers and pings
        MapMarkersMsg mm;
        mm.players = {{1, "Hote"}, {2, "Bob"}};
        MapChar a; a.netId = 3; a.owner = 2; a.flags = kMapAvatar | kMapDown; a.name = "Bob"; a.pos = {-1000.5f, 12, 77};
        MapChar b; b.netId = 9; b.owner = 1; b.name = std::string(80, 'x'); b.pos = {1, 2, 3};
        mm.chars = {a, b};
        MapThreat t; t.pos = {5, 6, 7}; t.count = 4; t.kind = ThreatKind::Raid; t.label = "Holy Nation";
        mm.threats = {t};
        Writer mw; Encode(mw, mm);
        Reader mr(mw.data(), mw.size()); CHECK(PeekType(mr) == Msg::MapMarkers);
        MapMarkersMsg mm2; CHECK(Decode(mr, mm2));
        CHECK(mm2.players == mm.players && mm2.chars.size() == 2 && mm2.chars[0] == a && mm2.threats.size() == 1 && mm2.threats[0] == t);
        CHECK(mm2.chars[1].name.size() == kMaxMapLabelLen);   // long names are cut
        mm.threats[0].kind = ThreatKind(9);   // no such kind
        Writer mw2; Encode(mw2, mm);
        Reader mr2(mw2.data(), mw2.size()); PeekType(mr2);
        MapMarkersMsg mm3; CHECK(!Decode(mr2, mm3));
        MapPingMsg pm; pm.id = 77; pm.owner = 3; pm.kind = PingKind::Loot; pm.pos = {1, 2, -3};
        Writer pw; Encode(pw, pm);
        Reader pr(pw.data(), pw.size()); CHECK(PeekType(pr) == Msg::MapPing);
        MapPingMsg pm2; CHECK(Decode(pr, pm2) && pm2 == pm);
        pm.kind = PingKind(7);
        Writer pw2; Encode(pw2, pm);
        Reader pr2(pw2.data(), pw2.size()); PeekType(pr2);
        MapPingMsg pm3; CHECK(!Decode(pr2, pm3));
    }
    {   // lot D: captive characters (a caged one with shackles and a sentence, a freed one)
        CaptivesMsg cm;
        CaptiveState a; a.netId = 7; a.caged = true; a.cageSid = "cage-1"; a.cagePos = {1, 2, 3}; a.chained = true;
        a.slaveOwner.type = 1; a.slaveOwner.index = 4; a.slaveOwner.serial = 9; a.slaveState = 1; a.slaveOf = "slavers"; a.kidnapped = true;
        a.sentenceBegan = 0x4041000000000000ull; a.sentence = 12.5f;
        CaptiveState b; b.netId = 8;
        cm.chars = {a, b};
        Writer cw; Encode(cw, cm);
        Reader cr(cw.data(), cw.size()); CHECK(PeekType(cr) == Msg::Captives);
        CaptivesMsg cm2; CHECK(Decode(cr, cm2));
        CHECK(cm2.chars.size() == 2 && cm2.chars[0] == a && cm2.chars[1] == b && cm2.chars[1].free() && !cm2.chars[0].free());
        cm.chars[1].slaveState = 7;   // no such slave state
        Writer cw2; Encode(cw2, cm);
        Reader cr2(cw2.data(), cw2.size()); PeekType(cr2);
        CaptivesMsg cm3; CHECK(!Decode(cr2, cm3));
    }
    {   // lot C: shots (full-precision path, turret identity) and aims
        ShotsMsg sm;
        ShotEvent a; a.shooterNetId = 4; a.targetNetId = 9; a.stat = 17; a.aimPos = {1, 2, 3}; a.dir = {0.70710678f, 0, 0.70710678f, 0};
        ShotEvent t; t.shooterNetId = 5; t.turretSid = "1234-turret.mod"; t.turretPos = {-500.5f, 10, 77}; t.dir = {1, 0, 0, 0};
        sm.shots = {a, t};
        Writer sw; Encode(sw, sm);
        Reader sr(sw.data(), sw.size()); CHECK(PeekType(sr) == Msg::Shots);
        ShotsMsg sm2; CHECK(Decode(sr, sm2));
        CHECK(sm2.shots.size() == 2 && sm2.shots[0].shooterNetId == 4 && sm2.shots[0].targetNetId == 9 && sm2.shots[0].stat == 17 &&
              sm2.shots[0].dir.w == a.dir.w && sm2.shots[0].dir.y == a.dir.y && sm2.shots[0].aimPos.z == 3 && sm2.shots[0].turretSid.empty());
        CHECK(sm2.shots[1].turretSid == "1234-turret.mod" && sm2.shots[1].turretPos.x == -500.5f && sm2.shots[1].dir.w == 1);
        ShotsMsg bad; ShotEvent z; z.shooterNetId = 1; z.dir = {0, 0, 0, 0}; bad.shots = {z};   // not an orientation
        Writer bw; Encode(bw, bad);
        Reader br(bw.data(), bw.size()); PeekType(br);
        ShotsMsg bad2; CHECK(!Decode(br, bad2));
        RangedMsg rm;
        RangedAim ra; ra.netId = 7; ra.state = 2; ra.aimPos = {5, 6, 7}; ra.targetNetId = 8;
        rm.aims = {ra};
        rm.turrets = {TurretAim{"t-1", {1, 1, 1}, {9, 9, 9}}};
        rm.stopped = {3, 11};
        Writer rw; Encode(rw, rm);
        Reader rr(rw.data(), rw.size()); CHECK(PeekType(rr) == Msg::Ranged);
        RangedMsg rm2; CHECK(Decode(rr, rm2));
        CHECK(rm2.aims.size() == 1 && rm2.aims[0].netId == 7 && rm2.aims[0].state == 2 && rm2.aims[0].aimPos.y == 6 && rm2.aims[0].targetNetId == 8);
        CHECK(rm2.turrets.size() == 1 && rm2.turrets[0].sid == "t-1" && rm2.turrets[0].target.z == 9 && rm2.stopped == std::vector<uint32_t>({3, 11}));
        rm.aims[0].state = 9;   // no such ranged state
        Writer rw2; Encode(rw2, rm);
        Reader rr2(rw2.data(), rw2.size()); PeekType(rr2);
        RangedMsg rm3; CHECK(!Decode(rr2, rm3));
    }
    {   // lot E: buildings
        BuildPlace bp; bp.netId = 0; bp.sid = "123-gamedata.base"; bp.pos = {1, 2, 3}; bp.rot = {0.5f, 0, 0.5f, 0}; bp.floor = 2;
        bp.flags = kBuildOutside | kBuildFloorLayout; bp.parentSid = "house"; bp.parentPos = {4, 5, 6}; bp.indoorsSid = "house";
        bp.snapSid = "wall"; bp.snapPos = {7, 8, 9}; bp.town.index = 12; bp.town.serial = 34; bp.worldPos = {10, 11, 12};
        Writer bw; Encode(bw, bp);
        Reader br(bw.data(), bw.size()); CHECK(PeekType(br) == Msg::BuildPlace);
        BuildPlace bp2; CHECK(Decode(br, bp2));
        CHECK(bp2.sid == bp.sid && bp2.pos.z == 3 && bp2.rot.w == 0.5f && bp2.rot.y == 0.5f && bp2.floor == 2 && bp2.flags == bp.flags &&
              bp2.parentSid == "house" && bp2.parentPos.y == 5 && bp2.indoorsSid == "house" && bp2.snapSid == "wall" && bp2.snapPos.z == 9 &&
              bp2.town.serial == 34 && bp2.worldPos.x == 10);
        bp.floor = 1000;   // nonsense floors are refused
        Writer bw2; Encode(bw2, bp);
        Reader br2(bw2.data(), bw2.size()); PeekType(br2);
        BuildPlace bp3; CHECK(!Decode(br2, bp3));
        BuildStateMsg bs; bs.entries = {{7, "hut", {1, 2, 3}, 42.5f, kSiteComplete | kSitePaused}, {8, "wall", {4, 5, 6}, 0, kSiteDismantling}};
        Writer sw; Encode(sw, bs);
        Reader sr(sw.data(), sw.size()); CHECK(PeekType(sr) == Msg::BuildState);
        BuildStateMsg bs2; CHECK(Decode(sr, bs2));
        CHECK(bs2.entries.size() == 2 && bs2.entries[0].progress == 42.5f && bs2.entries[0].flags == (kSiteComplete | kSitePaused) &&
              bs2.entries[1].sid == "wall" && bs2.entries[1].pos.y == 5);
        Writer rw; Encode(rw, BuildRemove{9, "hut", {1, 2, 3}});
        Reader rr(rw.data(), rw.size()); CHECK(PeekType(rr) == Msg::BuildRemove);
        BuildRemove rm; CHECK(Decode(rr, rm) && rm.netId == 9 && rm.sid == "hut" && rm.pos.x == 1);
        BuildAction ba; ba.kind = BuildActionKind::Dismantle; ba.arg = 2; ba.sid = "hut"; ba.pos = {1, 2, 3};
        Writer baw; Encode(baw, ba);
        Reader bar(baw.data(), baw.size()); CHECK(PeekType(bar) == Msg::BuildAction);
        BuildAction ba2; CHECK(Decode(bar, ba2) && ba2.kind == BuildActionKind::Dismantle && ba2.arg == 2 && ba2.pos.z == 3);
    }
    {   // fix G5: job lists, and the Tâches panel's commands
        JobListMsg jl; jl.entries = {{3, {16, 87, 16}}, {4, {}}};
        Writer jw; Encode(jw, jl);
        Reader jr(jw.data(), jw.size()); CHECK(PeekType(jr) == Msg::JobList);
        JobListMsg jl2; CHECK(Decode(jr, jl2) && jl2.entries.size() == 2 && jl2.entries[0].jobs == jl.entries[0].jobs && jl2.entries[1].jobs.empty());
        Command rc; rc.netId = 3; rc.kind = CommandKind::Task; rc.via = TaskVia::MovePermajob; rc.task = 87; rc.pos = {2, 0, 0};
        Writer cw2; Encode(cw2, rc);
        Reader cr2(cw2.data(), cw2.size()); PeekType(cr2);
        Command rc2; CHECK(Decode(cr2, rc2) && rc2.via == TaskVia::MovePermajob && rc2.task == 87 && rc2.pos.x == 2);
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
    add([](Writer& w) { ShotsMsg m; ShotEvent e; e.shooterNetId = 2; e.turretSid = "t"; m.shots = {e, e}; Encode(w, m); });   // lot C
    add([](Writer& w) { RangedMsg m; m.aims.resize(2); m.aims[0].netId = 1; m.aims[1].netId = 2; m.turrets = {TurretAim{"t", {}, {}}}; m.stopped = {4}; Encode(w, m); });
    add([](Writer& w) {   // map
        MapMarkersMsg m; m.players = {{1, "h"}}; MapChar c; c.netId = 2; c.owner = 1; c.name = "a"; m.chars = {c, c};
        MapThreat t; t.count = 3; t.label = "f"; m.threats = {t}; Encode(w, m);
    });
    add([](Writer& w) { MapPingMsg m; m.id = 4; m.owner = 2; m.kind = PingKind::Help; Encode(w, m); });
    add([](Writer& w) { ContainerOpen m; m.looterNetId = 2; m.sid = "chest"; Encode(w, m); });
    add([](Writer& w) {
        CaptivesMsg m; CaptiveState c; c.netId = 3; c.caged = true; c.cageSid = "cage"; c.chained = true; c.slaveOf = "f"; c.sentence = 2;
        m.chars = {c}; Encode(w, m);
    });
    add([](Writer& w) { ContainerOpened m; m.netId = 9; m.looterNetId = 2; m.sid = "chest"; Encode(w, m); });
    add([](Writer& w) { Encode(w, ContainerClose{9, "trop loin"}); });
    add([](Writer& w) { TradeOpen m; m.traderNetId = 4; m.looterNetId = 2; m.counters = {{5, "box", {1, 2, 3}}, {6, "pack", {}, 3}}; Encode(w, m); });
    add([](Writer& w) { BagBind m; m.netId = 6; m.ownerNetId = 3; m.sid = "pack"; Encode(w, m); });
    add([](Writer& w) { DoorsMsg m; DoorState d; d.sid = "door"; d.flags = kDoorHasLock; d.lockLevel = 3; m.doors = {d, d}; Encode(w, m); });   // lot A
    add([](Writer& w) { DoorRequest m; m.sid = "door"; Encode(w, m); });   // lot A
    add([](Writer& w) { InvOp m; m.fromNetId = 5; m.toNetId = 2; m.item.templateSid = "x"; m.traderNetId = 4; m.price = 12; Encode(w, m); });
    add([](Writer& w) { BuildPlace m; m.netId = 3; m.sid = "hut"; m.parentSid = "house"; Encode(w, m); });
    add([](Writer& w) { BuildStateMsg m; m.entries = {{1, "hut", {1, 2, 3}, 5, 1}}; Encode(w, m); });
    add([](Writer& w) { Encode(w, BuildRemove{4, "hut", {}}); });
    add([](Writer& w) { BuildAction m; m.sid = "shop"; Encode(w, m); });
    add([](Writer& w) { Encode(w, StallMsg{90}); });   // fix G6
    add([](Writer& w) { Encode(w, JoinQueueMsg{2, 3, JoinPhase::Editor, "Joueur2"}); });
    add([](Writer& w) { FloorsMsg m; m.entries = {{3, 10}, {9, 9}}; Encode(w, m); });
    add([](Writer& w) {   // lot B
        FactionsMsg m; FactionRelationEntry e; e.factionSid = "f"; e.hasOurs = true; e.ours.relation = 5; m.factions = {e, e}; Encode(w, m);
    });
    add([](Writer& w) {
        BountiesMsg m; CharBounties c; c.netId = 3; c.bounties = {{"f", 10, 1u, false, 2}}; m.chars = {c}; Encode(w, m);
    });
    add([](Writer& w) { DiplomacyMsg m; m.part = DiploPart::Pairs; m.pairs = {{"a", "b", {}}}; Encode(w, m); });   // diplomacy
    add([](Writer& w) { DiplomacyMsg m; m.part = DiploPart::Uniques; m.uniques = {{"u", 0, true}}; Encode(w, m); });
    add([](Writer& w) { DiplomacyMsg m; m.part = DiploPart::Towns; m.towns = {{"t", "f", "o"}}; Encode(w, m); });
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
        case Msg::ContainerOpen: { ContainerOpen m; Decode(r, m); break; }
        case Msg::ContainerOpened: { ContainerOpened m; Decode(r, m); break; }
        case Msg::ContainerClose: { ContainerClose m; Decode(r, m); break; }
        case Msg::TradeOpen: { TradeOpen m; Decode(r, m); break; }
        case Msg::BagBind: { BagBind m; Decode(r, m); break; }
        case Msg::BuildPlace: { BuildPlace m; Decode(r, m); break; }
        case Msg::BuildState: { BuildStateMsg m; Decode(r, m); break; }
        case Msg::BuildRemove: { BuildRemove m; Decode(r, m); break; }
        case Msg::BuildAction: { BuildAction m; Decode(r, m); break; }
        case Msg::Stall: { StallMsg m; Decode(r, m); break; }
        case Msg::JoinQueue: { JoinQueueMsg m; if (Decode(r, m) && (m.position < 1 || m.position > m.total)) std::abort(); break; }
        case Msg::Floors: { FloorsMsg m; Decode(r, m); break; }
        case Msg::Shots: { ShotsMsg m; Decode(r, m); break; }      // lot C
        case Msg::Ranged: { RangedMsg m; Decode(r, m); break; }
        case Msg::Captives: { CaptivesMsg m; Decode(r, m); break; }
        case Msg::Factions: { FactionsMsg m; Decode(r, m); break; }
        case Msg::Bounties: { BountiesMsg m; Decode(r, m); break; }
        case Msg::Diplomacy: { DiplomacyMsg m; Decode(r, m); break; }
        case Msg::Doors: { DoorsMsg m; Decode(r, m); break; }          // lot A
        case Msg::DoorRequest: { DoorRequest m; Decode(r, m); break; } // lot A
        case Msg::MapMarkers: { MapMarkersMsg m; Decode(r, m); break; }
        case Msg::MapPing: { MapPingMsg m; Decode(r, m); break; }
        default: break;
        }
    };
    for (int i = 0; i < 300000; ++i) {
        std::vector<uint8_t> p;
        if (i % 2) {
            p.resize(rng() % 64);
            for (auto& b : p) b = uint8_t(rng());
            if (!p.empty()) p[0] = uint8_t(1 + rng() % 82);
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

// Same world after a gap (a window resized, a zone loading), or a new one (a load)?
static void TestWorldIdentity() {
    std::printf("world identity: a gap is not a new world; stand-ins never decide; a load is, and names leftovers\n");
    auto P = [](uintptr_t v) { return reinterpret_cast<const void*>(v); };
    auto O = [&](uint32_t serial, uintptr_t ptr) { WorldIdentity::Object o; o.handle = FakeWorld::H(serial); o.ptr = P(ptr); return o; };
    std::set<uintptr_t> alive;   // object addresses in the game now
    auto there = [&](const WorldIdentity::Object& o) { return alive.count(reinterpret_cast<uintptr_t>(o.ptr)) != 0; };
    WorldIdentity id;
    const void* player = P(0x9000);
    CHECK(id.Observe(player, {P(0x10), P(0x20)}, {}, there).newWorld);   // the first world
    CHECK(id.generation() == 1);
    CHECK(!id.Observe(player, {P(0x20), P(0x10)}, {}, there).newWorld);  // order does not matter
    // another player joins: we make a stand-in for their character (in our squad, not in `own`)
    alive = {0x10, 0x20, 0x30};
    CHECK(!id.Observe(player, {P(0x10), P(0x20)}, {O(90000, 0x30)}, there).newWorld);
    // a few frames without the main loop (a window resized), then the same world: not new
    id.Gap();
    id.Gap();
    WorldIdentity::Verdict v = id.Observe(player, {P(0x10), P(0x20)}, {O(90000, 0x30)}, there);
    CHECK(!v.newWorld && v.leftovers.empty() && id.generation() == 1);
    // our own squad changing while the game runs (a recruit): not a load either
    CHECK(!id.Observe(player, {P(0x10), P(0x20), P(0x40)}, {O(90000, 0x30)}, there).newWorld);
    // a load: other objects for our squad after a gap; a stand-in of ours still there is named
    id.Gap();
    v = id.Observe(player, {P(0x110), P(0x120), P(0x140)}, {}, there);
    CHECK(v.newWorld && id.generation() == 2);
    CHECK(v.leftovers.size() == 1 && !v.leftovers.empty() && v.leftovers[0].handle.serial == 90000);
    // after a load nothing of the previous world is named again
    id.Gap();
    alive.clear();
    v = id.Observe(P(0x9100), {P(0x210)}, {}, there);   // another player object: a new world too
    CHECK(v.newWorld && v.leftovers.empty() && id.generation() == 3);
}

// A client world that, like KenshiWorld, gives a character it recreates a local handle of its own
// (an alias), puts recreated player characters into its own squad, and tells worlds apart with
// kc::WorldIdentity (Frame); on a new world it forgets every alias and removes leftover stand-ins.
struct AliasWorld : FakeWorld {
    std::map<uint32_t, uint32_t> alias;   // host serial -> local serial
    std::set<uint32_t> made;              // local serials of the stand-ins Spawn made
    uint32_t nextLocal = 90000;
    uintptr_t epoch = 1;                  // a load gives our own characters other addresses
    WorldIdentity identity;
    int newWorlds = 0, leftoversRemoved = 0;
    uint32_t Local(uint32_t s) const { auto it = alias.find(s); return it == alias.end() ? s : it->second; }
    const void* Ptr(uint32_t s) const {   // stand-ins keep theirs (one left behind by a load: a leftover)
        return reinterpret_cast<const void*>((made.count(s) ? uintptr_t(0) : epoch << 32) | s);
    }
    bool Exists(const Handle& h) override { return chars.count(Local(h.serial)) != 0; }
    bool Read(const Handle& h, EntityState& out) override { return FakeWorld::Read(H(Local(h.serial)), out); }
    void Apply(const Handle& h, const EntityState& t, const EntityState& l) override { FakeWorld::Apply(H(Local(h.serial)), t, l); }
    bool Spawn(const Handle& h, const SpawnInfo& info, const EntityState& at) override {
        if (alias.count(h.serial) || info.templateSid != "tmpl-" + std::to_string(h.serial)) return false;
        const uint32_t local = nextLocal++;
        FakeChar c;
        c.squad = true;   // a player's character: in our squad, as in the game
        c.pos = at.pos;
        c.dest = at.pos;
        chars[local] = c;
        alias[h.serial] = local;
        made.insert(local);
        ++spawns;
        return true;
    }
    void Despawn(const Handle& h) override {
        auto it = alias.find(h.serial);
        if (it == alias.end()) return;
        if (chars.erase(it->second)) ++despawns;
        made.erase(it->second);
        alias.erase(it);
    }
    void Frame(bool live) {
        if (!live || !ready) { identity.Gap(); return; }
        std::vector<const void*> own;
        for (auto& [s, c] : chars) if (c.squad && !made.count(s)) own.push_back(Ptr(s));
        std::vector<WorldIdentity::Object> standIns;
        for (uint32_t s : made) if (chars.count(s)) { WorldIdentity::Object o; o.handle = H(s); o.ptr = Ptr(s); standIns.push_back(o); }
        const WorldIdentity::Verdict v = identity.Observe(reinterpret_cast<const void*>(uintptr_t(0x77)), own, standIns,
                                                          [&](const WorldIdentity::Object& o) { return chars.count(o.handle.serial) && Ptr(o.handle.serial) == o.ptr; });
        if (!v.newWorld || identity.generation() == 1) return;
        for (auto& o : v.leftovers) if (chars.erase(o.handle.serial)) ++leftoversRemoved;
        alias.clear();
        made.clear();
        ++newWorlds;
    }
    size_t Copies() const { size_t n = 0; for (auto& [s, c] : chars) n += s >= 90000 ? 1 : 0; return n; }
    bool HasCopyOf(uint32_t hostSerial) const { auto it = alias.find(hostSerial); return it != alias.end() && chars.count(it->second); }
};

static void TestNoDuplicatePlayers() {
    std::printf("session: players who join later have exactly one copy on an earlier client, across resizes and zone loads\n");
    FakeWorld hw, bw, cw3;
    AliasWorld aw;
    SetupHost(hw);
    AtMenu(aw);
    AtMenu(bw);
    AtMenu(cw3);
    for (FakeWorld* w : {static_cast<FakeWorld*>(&aw), &bw, &cw3}) w->editorSupported = true;   // each player confirms their character at once
    SessionConfig hc; hc.name = "Host"; hc.port = ++g_port;
    Session host(hw, hc, Now, Quiet("host"));
    std::string err;
    CHECK(host.Host(&err));
    SessionConfig ac; ac.name = "First"; ac.port = hc.port;
    SessionConfig bc; bc.name = "Second"; bc.port = hc.port;
    SessionConfig c3; c3.name = "Third"; c3.port = hc.port;
    Session cliA(aw, ac, Now, Quiet("cliA"));
    Session cliB(bw, bc, Now, Quiet("cliB"));
    Session cliC(cw3, c3, Now, Quiet("cliC"));
    bool aLive = true;
    auto run = [&](double seconds, std::function<bool()> until) {
        const double end = Now() + seconds;
        double last = Now();
        while (Now() < end) {
            const double n = Now();
            for (FakeWorld* w : {&hw, static_cast<FakeWorld*>(&aw), &bw, &cw3}) w->Simulate(float(n - last));
            for (FakeWorld* w : {static_cast<FakeWorld*>(&aw), &bw, &cw3}) w->editorOpen = false;
            aw.Frame(aLive);
            host.Tick(hw.ready);
            cliA.Tick(aw.ready && aLive);
            cliB.Tick(bw.ready);
            cliC.Tick(cw3.ready);
            last = n;
            if (until && until()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };
    auto gap = [&](int frames) {   // a window resized / a zone loading: the main loop does not run
        aLive = false;
        for (int i = 0; i < frames; ++i) run(0.02, nullptr);
        aLive = true;
    };
    CHECK(cliA.Join("127.0.0.1", hc.port, &err));
    run(10.0, [&] { return cliA.state() == SessionState::Connected && aw.controllable.size() == 1; });
    CHECK(cliA.state() == SessionState::Connected);
    CHECK(cliB.Join("127.0.0.1", hc.port, &err));
    run(15.0, [&] { return cliB.state() == SessionState::Connected && hw.named.count("Second") && aw.HasCopyOf(hw.named["Second"]); });
    const uint32_t second = hw.named.count("Second") ? hw.named["Second"] : 0;
    CHECK(aw.HasCopyOf(second) && aw.Copies() == 1);
    gap(5);   // the stress test's window rearrangement right after the joins
    run(4.0, nullptr);
    CHECK(cliC.Join("127.0.0.1", hc.port, &err));
    run(15.0, [&] { return cliC.state() == SessionState::Connected && hw.named.count("Third") && aw.HasCopyOf(hw.named["Third"]); });
    const uint32_t third = hw.named.count("Third") ? hw.named["Third"] : 0;
    for (int round = 0; round < 3; ++round) {   // zone loads, hitches
        gap(3 + round);
        run(4.0, nullptr);   // more than the spawn grace and the presence check
    }
    CHECK(aw.newWorlds == 0);
    CHECK(aw.HasCopyOf(second) && aw.HasCopyOf(third));
    if (aw.Copies() != 2) std::printf("    first client holds %zu copies of the 2 players who joined later\n", aw.Copies());
    CHECK(aw.Copies() == 2);   // exactly one each, nothing left without an alias
    CHECK(bw.chars.count(third) == 1);   // and the second player has the third one's too
    // A real load (our own characters are other objects now) with stand-ins of the previous world
    // still in the game: they are removed, and the session makes them again: still exactly one each.
    ++aw.epoch;
    gap(3);
    run(5.0, [&] { return aw.HasCopyOf(second) && aw.HasCopyOf(third); });
    CHECK(aw.newWorlds == 1 && aw.leftoversRemoved == 2);
    CHECK(aw.HasCopyOf(second) && aw.HasCopyOf(third));
    CHECK(aw.Copies() == 2);
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

static void TestGroundDrops() {
    std::printf("session: a client's drop to the ground (its character, a chest it has open) is done by the host; others' refused\n");
    FakeWorld hw, cw;
    SetupHost(hw);
    auto item = [](const char* sid, int q, const char* sec, int x, int y) {
        ItemState i; i.templateSid = sid; i.quantity = q; i.section = sec; i.x = int16_t(x); i.y = int16_t(y); return i;
    };
    hw.chars[1].items = {item("bread", 2, "main", 0, 0)};
    hw.chars[2].items = {item("ore", 12, "main", 0, 0), item("sword", 1, "main", 2, 0)};
    hw.boxes[700] = {"chest", {210, 0, 5}, {item("ore", 5, "main", 0, 0)}};
    AtMenu(cw);
    cw.boxes = hw.boxes;
    SessionConfig hc; hc.characterPerPlayer = false; hc.port = ++g_port; SessionConfig cc; cc.port = hc.port; cc.name = "C";
    Session host(hw, hc, Now, Quiet("host"));
    Session cli(cw, cc, Now, Quiet("cli"));
    std::string err;
    host.Host(&err);
    CHECK(JoinAndWait(host, hw, cli, cw, hc.port, 3));
    host.Assign(FakeWorld::H(2), 2);
    Run({{&host, &hw}, {&cli, &cw}}, 1.5);
    // from its own character: the host's game drops it (the stack leaves the host's inventory)
    cw.localDrops.push_back({FakeWorld::H(2), item("ore", 12, "main", 0, 0)});
    Run({{&host, &hw}, {&cli, &cw}}, 3.0, [&] { return hw.chars[2].items.size() == 1; });
    CHECK(hw.chars[2].items.size() == 1 && hw.chars[2].items[0].templateSid == "sword");
    CHECK(hw.drops == 1 && hw.lastDropper == FakeWorld::H(2));
    // from a chest it opened: dropped by its character standing by the chest
    cw.containerReqs.push_back({FakeWorld::H(2), "chest", {210, 0, 5}});
    Run({{&host, &hw}, {&cli, &cw}}, 3.0, [&] { return cli.entityCount() == 4; });
    CHECK(cli.entityCount() == 4);
    cw.localDrops.push_back({FakeWorld::B(700), item("ore", 5, "main", 0, 0)});
    Run({{&host, &hw}, {&cli, &cw}}, 3.0, [&] { return hw.boxes[700].items.empty(); });
    CHECK(hw.boxes[700].items.empty());
    CHECK(hw.drops == 2 && hw.lastDropper == FakeWorld::H(2));
    // from the host's own character: refused, nothing leaves it
    cw.localDrops.push_back({FakeWorld::H(1), item("bread", 2, "main", 0, 0)});
    Run({{&host, &hw}, {&cli, &cw}}, 2.0);
    CHECK(hw.chars[1].items.size() == 1 && hw.drops == 2);
    // actor safety: a drop from a chest names the character that drops it; forged ones are refused
    std::map<Handle, uint32_t, bool (*)(const Handle&, const Handle&)> net([](const Handle& a, const Handle& b) { return a.serial < b.serial || (a.serial == b.serial && a.type < b.type); });
    host.ForEachEntity([&](uint32_t id, const Handle& h, uint8_t, bool, bool) { net[h] = id; });
    CHECK(net.count(FakeWorld::B(700)) && net.count(FakeWorld::H(1)) && net.count(FakeWorld::H(2)));
    hw.boxes[700].items = {item("ore", 5, "main", 0, 0)};
    const uint8_t me = cli.localId();
    auto forge = [&](uint32_t actor) {
        InvOp op; op.kind = InvOpKind::Drop; op.fromNetId = net[FakeWorld::B(700)]; op.toNetId = actor; op.item = item("ore", 5, "main", 0, 0);
        Writer w; Encode(w, op); CHECK(host.InjectForTest(me, w));
        Run({{&host, &hw}, {&cli, &cw}}, 0.5);
    };
    const uint32_t refused0 = host.actorRefusals();
    forge(net[FakeWorld::H(1)]);   // the host's character
    forge(0);                      // nobody
    CHECK(hw.drops == 2 && hw.boxes[700].items.size() == 1 && host.actorRefusals() == refused0 + 2);
    forge(net[FakeWorld::H(2)]);   // ours, standing by it: dropped by it
    CHECK(hw.drops == 3 && hw.lastDropper == FakeWorld::H(2) && hw.boxes[700].items.empty());
    hw.boxes[700].items = {item("ore", 5, "main", 0, 0)};
    hw.chars[2].pos = {5000, 0, 5000};   // ours, but far from the chest
    hw.chars[2].dest = hw.chars[2].pos;
    forge(net[FakeWorld::H(2)]);
    CHECK(hw.drops == 3 && hw.boxes[700].items.size() == 1);
}

// ---- fix G2: swaps, merges and refusals in looting
static void TestInventorySwaps() {
    std::printf("session: loot swaps, stack merges, moves into a freed slot, refusals\n");
    FakeWorld hw, cw;
    SetupHost(hw);
    auto item = [](const char* sid, int q, const char* sec, int x, int y) {
        ItemState i; i.templateSid = sid; i.quantity = q; i.section = sec; i.x = int16_t(x); i.y = int16_t(y); return i;
    };
    FakeChar body; body.squad = false; body.pos = {130, 0, 0}; body.dest = body.pos; body.vit.flags = kVitUnconscious;
    body.items = {item("leather_boots", 1, "boots", 0, 0), item("bread", 2, "main", 1, 0), item("sword", 1, "main", 3, 0)};
    hw.chars[20] = body;
    FakeChar guard; guard.squad = false; guard.pos = {140, 0, 0}; guard.dest = guard.pos;   // awake
    hw.chars[21] = guard;
    hw.chars[2].items = {item("sandals", 1, "boots", 0, 0), item("bread", 3, "main", 0, 0), item("knife", 1, "weapon", 0, 0)};
    AtMenu(cw);
    SessionConfig hc; hc.characterPerPlayer = false; hc.port = ++g_port; SessionConfig cc; cc.port = hc.port; cc.name = "C";
    Session host(hw, hc, Now, Quiet("host"));
    Session cli(cw, cc, Now, Quiet("cli"));
    std::string err;
    host.Host(&err);
    CHECK(JoinAndWait(host, hw, cli, cw, hc.port, 5));
    host.Assign(FakeWorld::H(2), 2);
    Run({{&host, &hw}, {&cli, &cw}}, 2.0, [&] { return cw.chars[20].items == hw.chars[20].items && cw.chars[2].items == hw.chars[2].items; });
    CHECK(cw.chars[20].items == hw.chars[20].items);
    auto count = [](const std::vector<ItemState>& v, const char* sid) {
        int n = 0;
        for (const auto& i : v) n += i.templateSid == sid ? i.quantity : 0;
        return n;
    };
    auto find = [](std::vector<ItemState>& v, const char* sid) -> ItemState& {
        for (auto& i : v)
            if (i.templateSid == sid) return i;
        static ItemState none;
        return none;
    };
    // 1. boots dropped on a character already wearing boots: the game swaps them
    find(cw.chars[20].items, "leather_boots").templateSid = "sandals";
    find(cw.chars[2].items, "sandals").templateSid = "leather_boots";
    Run({{&host, &hw}, {&cli, &cw}}, 4.0, [&] { return count(hw.chars[2].items, "leather_boots") == 1 && cw.chars[2].items == hw.chars[2].items; });
    CHECK(hw.invSwaps == 1 && hw.invRefusals == 0);
    CHECK(count(hw.chars[2].items, "leather_boots") == 1 && count(hw.chars[2].items, "sandals") == 0);
    CHECK(count(hw.chars[20].items, "sandals") == 1 && count(hw.chars[20].items, "leather_boots") == 0);
    CHECK(find(hw.chars[2].items, "leather_boots").section == "boots" && find(hw.chars[20].items, "sandals").section == "boots");
    CHECK(cw.chars[2].items == hw.chars[2].items && cw.chars[20].items == hw.chars[20].items);
    // 2. the body's bread dropped on our bread: one pile of 5 where ours was
    cw.chars[20].items.erase(std::remove_if(cw.chars[20].items.begin(), cw.chars[20].items.end(), [](const ItemState& i) { return i.templateSid == "bread"; }),
                             cw.chars[20].items.end());
    find(cw.chars[2].items, "bread").quantity = 5;
    Run({{&host, &hw}, {&cli, &cw}}, 4.0, [&] { return count(hw.chars[2].items, "bread") == 5 && cw.chars[2].items == hw.chars[2].items; });
    CHECK(count(hw.chars[2].items, "bread") == 5 && count(hw.chars[20].items, "bread") == 0);
    int piles = 0;
    for (const auto& i : hw.chars[2].items) piles += i.templateSid == "bread";
    CHECK(piles == 1 && find(hw.chars[2].items, "bread").x == 0);
    CHECK(cw.chars[2].items == hw.chars[2].items && cw.chars[20].items == hw.chars[20].items);
    // 3. the knife goes to the bag and the body's sword into the hand it freed, in one go: the
    //    move that frees the slot runs first, whatever order the diff found them in
    ItemState& knife = find(cw.chars[2].items, "knife");
    knife.section = "main"; knife.x = 5; knife.y = 0;
    ItemState sword = find(cw.chars[20].items, "sword");
    cw.chars[20].items.erase(std::remove_if(cw.chars[20].items.begin(), cw.chars[20].items.end(), [](const ItemState& i) { return i.templateSid == "sword"; }),
                             cw.chars[20].items.end());
    sword.section = "weapon"; sword.x = 0; sword.y = 0;
    cw.chars[2].items.push_back(sword);
    Run({{&host, &hw}, {&cli, &cw}}, 4.0, [&] { return count(hw.chars[2].items, "sword") == 1 && cw.chars[2].items == hw.chars[2].items; });
    CHECK(hw.invRefusals == 0);
    CHECK(find(hw.chars[2].items, "sword").section == "weapon" && find(hw.chars[2].items, "knife").section == "main");
    CHECK(count(hw.chars[20].items, "sword") == 0);
    CHECK(cw.chars[2].items == hw.chars[2].items && cw.chars[20].items == hw.chars[20].items);
    // 4. putting something on a knocked-out body is allowed (the game allows it)
    ItemState k2 = find(cw.chars[2].items, "knife");
    cw.chars[2].items.erase(std::remove_if(cw.chars[2].items.begin(), cw.chars[2].items.end(), [](const ItemState& i) { return i.templateSid == "knife"; }),
                            cw.chars[2].items.end());
    k2.x = 7;
    cw.chars[20].items.push_back(k2);
    Run({{&host, &hw}, {&cli, &cw}}, 4.0, [&] { return count(hw.chars[20].items, "knife") == 1 && cw.chars[20].items == hw.chars[20].items; });
    CHECK(count(hw.chars[20].items, "knife") == 1 && count(hw.chars[2].items, "knife") == 0);
    // 5. ...but not on an awake NPC: refused, the client goes back to the host's state, nothing lost
    Run({{&host, &hw}, {&cli, &cw}}, 1.0, [&] { return cw.chars[21].items == hw.chars[21].items; });
    ItemState s2 = find(cw.chars[2].items, "sword");
    cw.chars[2].items.erase(std::remove_if(cw.chars[2].items.begin(), cw.chars[2].items.end(), [](const ItemState& i) { return i.templateSid == "sword"; }),
                            cw.chars[2].items.end());
    s2.section = "main";
    cw.chars[21].items.push_back(s2);
    Run({{&host, &hw}, {&cli, &cw}}, 6.0, [&] { return cw.chars[2].items == hw.chars[2].items && cw.chars[21].items == hw.chars[21].items; });
    CHECK(count(hw.chars[2].items, "sword") == 1 && hw.chars[21].items.empty());
    CHECK(cw.chars[2].items == hw.chars[2].items && cw.chars[21].items == hw.chars[21].items);
}

// ---- lot A: doors and locks
static void TestDoors() {
    std::printf("session: doors and locks near the players follow the host; door buttons go to the host; a locked chest stays shut\n");
    FakeWorld hw, cw;
    SetupHost(hw);   // squad at x = 100, 200, 300
    auto door = [](const char* sid, Vec3 p, DoorKind k, uint8_t state, uint8_t flags, int level) {
        DoorState d; d.sid = sid; d.pos = p; d.kind = k; d.state = state; d.flags = flags; d.lockLevel = level; return d;
    };
    hw.doors["gate"] = door("gate", {120, 0, 10}, DoorKind::Door, 0, kDoorHasLock, 30);
    hw.doors["chest"] = door("chest", {210, 0, 5}, DoorKind::Lock, 0, kDoorHasLock | kDoorLocked, 60);
    hw.doors["far"] = door("far", {9000, 0, 0}, DoorKind::Door, 0, 0, 0);
    hw.boxes[700] = {"chest", {210, 0, 5}, {}};
    hw.lockedBoxes.insert(700);
    AtMenu(cw);
    cw.doors = hw.doors;                       // the client's save has the same doors...
    cw.doors["gate"].state = 1;                // ...in another state (the host played since)
    cw.doors["chest"].flags = kDoorHasLock;
    cw.boxes = hw.boxes;
    SessionConfig hc; hc.characterPerPlayer = false; hc.port = ++g_port; SessionConfig cc; cc.port = hc.port; cc.name = "C";
    Session host(hw, hc, Now, Quiet("host"));
    Session cli(cw, cc, Now, Quiet("cli"));
    std::string err;
    host.Host(&err);
    CHECK(JoinAndWait(host, hw, cli, cw, hc.port, 3));
    Run({{&host, &hw}, {&cli, &cw}}, 3.0, [&] { return cw.doors["gate"].state == 0 && (cw.doors["chest"].flags & kDoorLocked); });
    CHECK(cw.doors["gate"].state == 0 && cw.doors["gate"].lockLevel == 30);
    CHECK((cw.doors["chest"].flags & kDoorLocked) && cw.doors["chest"].lockLevel == 60);
    CHECK(host.doorsKnown() == 2);             // the far door is not sent
    // the host opens the gate, then locks the chest's twin... the client follows
    hw.doors["gate"].state = 1;
    hw.doors["gate"].flags |= kDoorBroken;
    Run({{&host, &hw}, {&cli, &cw}}, 3.0, [&] { return cw.doors["gate"].state == 1 && (cw.doors["gate"].flags & kDoorBroken); });
    CHECK(cw.doors["gate"].state == 1 && (cw.doors["gate"].flags & kDoorBroken));
    // a local change on the client is undone by the host's state (re-imposed now and then)
    cw.doors["gate"].state = 0;
    Run({{&host, &hw}, {&cli, &cw}}, 5.0, [&] { return cw.doors["gate"].state == 1; });
    CHECK(cw.doors["gate"].state == 1);
    // the client player clicks the gate's lock button: the host's game runs it, everyone sees it
    hw.doors["gate"].state = 0;
    hw.doors["gate"].flags = kDoorHasLock;
    Run({{&host, &hw}, {&cli, &cw}}, 2.0, [&] { return cw.doors["gate"].state == 0 && !(cw.doors["gate"].flags & kDoorBroken); });
    DoorRequest rq; rq.sid = "gate"; rq.pos = {120, 0, 10}; rq.action = DoorAction::LockButton;
    cw.doorReqs.push_back(rq);
    Run({{&host, &hw}, {&cli, &cw}}, 3.0, [&] { return (hw.doors["gate"].flags & kDoorLocked) && (cw.doors["gate"].flags & kDoorLocked); });
    CHECK((hw.doors["gate"].flags & kDoorLocked) && (cw.doors["gate"].flags & kDoorLocked));
    // a locked chest is not opened for a client player (its lock must be picked first)
    host.Assign(FakeWorld::H(2), 2);
    Run({{&host, &hw}, {&cli, &cw}}, 1.5);
    const size_t chatBefore = cli.chatLog().size();
    cw.containerReqs.push_back({FakeWorld::H(2), "chest", {210, 0, 5}});
    Run({{&host, &hw}, {&cli, &cw}}, 2.0, [&] { return cli.chatLog().size() > chatBefore; });
    CHECK(cli.chatLog().size() > chatBefore && cli.chatLog().back().find("verrouill") != std::string::npos);
    CHECK(!cw.tradeWindow);
}

// fix G6
static void TestFloorsAndStall() {
    std::printf("session: a character upstairs on the host is upstairs on clients; a far teleport warns the player's game\n");
    FakeWorld hw, cw;
    SetupHost(hw);
    AtMenu(cw);
    hw.floors[2] = 11;   // already upstairs when the client joins
    SessionConfig hc; hc.characterPerPlayer = false; hc.port = ++g_port; SessionConfig cc; cc.port = hc.port; cc.name = "C";
    Session host(hw, hc, Now, Quiet("host"));
    Session cli(cw, cc, Now, Quiet("cli"));
    std::string err;
    host.Host(&err);
    CHECK(JoinAndWait(host, hw, cli, cw, hc.port, 3));
    Run({{&host, &hw}, {&cli, &cw}}, 3.0, [&] { return cw.floors.count(2) && cw.floors[2] == 11; });
    CHECK(cw.floors.count(2) && cw.floors[2] == 11);
    CHECK(!cw.floors.count(1) || cw.floors[1] == 9);
    // it takes the stairs down, another goes up: the client follows both
    hw.floors[2] = 9;
    hw.floors[3] = 10;
    Run({{&host, &hw}, {&cli, &cw}}, 3.0, [&] { return cw.floors[2] == 9 && cw.floors.count(3) && cw.floors[3] == 10; });
    CHECK(cw.floors[2] == 9 && cw.floors.count(3) && cw.floors[3] == 10);
    // the client's game moved it on its own (a local stair step): the host's floor comes back
    cw.floors[3] = 9;
    Run({{&host, &hw}, {&cli, &cw}}, 1.0, [&] { return cw.floors[3] == 10; });
    CHECK(cw.floors[3] == 10);
    // a far teleport: the client is told, and both stay connected
    host.ExpectStall(cli.localId(), 120.0);
    Run({{&host, &hw}, {&cli, &cw}}, 1.0);
    CHECK(cli.state() == SessionState::Connected && host.players().size() >= 1);
}

static void TestTrade() {
    std::printf("session: trading with a merchant: the window opens on the player's screen, purchases and sales replayed with their price\n");
    FakeWorld hw, cw;
    SetupHost(hw);
    auto item = [](const char* sid, int q, const char* sec, int x, int y) {
        ItemState i; i.templateSid = sid; i.quantity = q; i.section = sec; i.x = int16_t(x); i.y = int16_t(y); return i;
    };
    // the client player's character arrives at (0, 0, -20): the shop is there
    FakeChar merchant; merchant.squad = false; merchant.pos = {10, 0, -20}; merchant.dest = merchant.pos;
    hw.chars[50] = merchant;
    hw.boxes[900] = {"counter", {15, 0, -15}, {item("bread", 5, "main", 0, 0), item("sword", 1, "main", 2, 0)}};
    hw.shops[50] = {900};
    hw.money = 1000;
    hw.merchantMoney[50] = 500;
    AtMenu(cw);
    cw.boxes = hw.boxes;               // the save the client loads has the same shop
    cw.boxes[900].items.clear();       // (its stock comes from the host anyway)
    SessionConfig hc; hc.port = ++g_port; SessionConfig cc; cc.port = hc.port; cc.name = "C";
    Session host(hw, hc, Now, Quiet("host"));
    Session cli(cw, cc, Now, Quiet("cli"));
    std::string err;
    host.Host(&err);
    CHECK(JoinAndWait(host, hw, cli, cw, hc.port, 5));
    const uint32_t me = 5000;   // the client player's own character (the host's EnsurePlayerCharacter made it)
    Run({{&host, &hw}, {&cli, &cw}}, 2.0, [&] { return cw.money == 1000; });
    CHECK(cw.money == 1000);
    // the merchant's "let's trade" in the client player's conversation
    hw.tradeReqs.push_back({FakeWorld::H(me), FakeWorld::H(50)});
    Run({{&host, &hw}, {&cli, &cw}}, 3.0, [&] { return cli.tradeView().open; });
    CHECK(cli.tradeView().open && cli.tradeView().counters == 1 && host.hostTrades() == 1);
    CHECK(cw.boxes[900].items == hw.boxes[900].items && cw.merchantMoney[50] == 500 && cw.tradeWindow);
    // the client's game buys 2 breads for 60 cats (part of a stack, into the bag)
    cw.boxes[900].items[0].quantity = 3;
    cw.chars[me].items.push_back(item("bread", 2, "main", 4, 1));
    cw.money -= 60;
    cw.merchantMoney[50] += 60;
    Run({{&host, &hw}, {&cli, &cw}}, 4.0, [&] { return hw.money == 940 && cw.chars[me].items == hw.chars[me].items && cw.money == 940; });
    CHECK(hw.money == 940 && hw.merchantMoney[50] == 560);
    CHECK(hw.boxes[900].items.size() == 2 && hw.boxes[900].items[0].quantity == 3);
    CHECK(hw.chars[me].items.size() == 1 && hw.chars[me].items[0].templateSid == "bread" && hw.chars[me].items[0].quantity == 2 && hw.chars[me].items[0].x == 4);
    CHECK(cw.money == 940 && cw.chars[me].items == hw.chars[me].items && cw.boxes[900].items == hw.boxes[900].items);
    // it sells one back for 25 (onto the shop's stack)
    cw.chars[me].items[0].quantity = 1;
    cw.boxes[900].items[0].quantity = 4;
    cw.money += 25;
    Run({{&host, &hw}, {&cli, &cw}}, 4.0, [&] { return hw.money == 965 && cw.money == 965 && cw.boxes[900].items == hw.boxes[900].items; });
    CHECK(hw.money == 965 && hw.merchantMoney[50] == 535);
    int breadInShop = 0;
    for (const auto& i : hw.boxes[900].items) if (i.templateSid == "bread") breadInShop += i.quantity;
    CHECK(breadInShop == 4 && hw.chars[me].items.size() == 1 && hw.chars[me].items[0].quantity == 1);
    CHECK(cw.money == 965 && cw.chars[me].items == hw.chars[me].items);
    // a purchase it cannot pay is refused: the sword goes back to the shop, the cats back to 965
    {
        auto& box = cw.boxes[900].items;
        box.erase(std::remove_if(box.begin(), box.end(), [](const ItemState& i) { return i.templateSid == "sword"; }), box.end());
    }
    cw.chars[me].items.push_back(item("sword", 1, "main", 0, 3));
    cw.money -= 5000;
    Run({{&host, &hw}, {&cli, &cw}}, 5.0, [&] { return cw.money == 965 && cw.chars[me].items == hw.chars[me].items && cw.boxes[900].items == hw.boxes[900].items; });
    CHECK(hw.money == 965 && hw.merchantMoney[50] == 535 && hw.chars[me].items.size() == 1);
    CHECK(cw.money == 965 && cw.chars[me].items == hw.chars[me].items && cw.boxes[900].items == hw.boxes[900].items);
    // the host buys the sword itself: the client's window shows the new stock
    const int opens = cw.tradeWindowOpens;
    {
        auto& box = hw.boxes[900].items;
        box.erase(std::remove_if(box.begin(), box.end(), [](const ItemState& i) { return i.templateSid == "sword"; }), box.end());
    }
    Run({{&host, &hw}, {&cli, &cw}}, 4.0, [&] { return cw.boxes[900].items == hw.boxes[900].items && cw.tradeWindowOpens > opens; });
    CHECK(cw.boxes[900].items == hw.boxes[900].items && cw.tradeWindowOpens > opens);
    // the merchant's cats change on the host: the client's copy follows
    hw.merchantMoney[50] = 777;
    Run({{&host, &hw}, {&cli, &cw}}, 3.0, [&] { return cw.merchantMoney[50] == 777; });
    CHECK(cw.merchantMoney[50] == 777);
    // the player closes the window: the host forgets the trade
    cw.tradeWindow = false;
    Run({{&host, &hw}, {&cli, &cw}}, 4.0, [&] { return host.hostTrades() == 0 && !cli.tradeView().open; });
    CHECK(host.hostTrades() == 0 && !cli.tradeView().open);
    // a merchant without a shop: the player is told, nothing opens
    hw.chars[51] = merchant;
    hw.merchantMoney[51] = 10;
    Run({{&host, &hw}, {&cli, &cw}}, 1.5);
    hw.tradeReqs.push_back({FakeWorld::H(me), FakeWorld::H(51)});
    Run({{&host, &hw}, {&cli, &cw}}, 1.5);
    CHECK(!cli.tradeView().open && !cli.tradeView().pending && host.hostTrades() == 0);
}

static void TestTravellingTrade() {
    std::printf("session: travelling merchants sell from their squad's worn backpacks; backpacks synced; pack beasts robbed and given\n");
    FakeWorld hw, cw;
    SetupHost(hw);
    auto item = [](const char* sid, int q, const char* sec, int x, int y) {
        ItemState i; i.templateSid = sid; i.quantity = q; i.section = sec; i.x = int16_t(x); i.y = int16_t(y); return i;
    };
    FakeChar trader; trader.squad = false; trader.pos = {10, 0, -20}; trader.dest = trader.pos;
    FakeChar beast = trader; beast.pos = {14, 0, -24}; beast.dest = beast.pos; beast.animal = true;
    hw.chars[60] = trader;
    hw.chars[61] = beast;
    hw.money = 1000;
    hw.merchantMoney[60] = 500;
    AtMenu(cw);
    SessionConfig hc; hc.port = ++g_port; SessionConfig cc; cc.port = hc.port; cc.name = "C";
    Session host(hw, hc, Now, Quiet("host"));
    Session cli(cw, cc, Now, Quiet("cli"));
    std::string err;
    host.Host(&err);
    CHECK(JoinAndWait(host, hw, cli, cw, hc.port, 6));
    const uint32_t me = 5000;
    // the caravan's bull carries the stock in its pack; the client's game has the same pack (another
    // local handle), empty for now
    hw.chars[61].bagSid = "bull-pack"; hw.chars[61].bagSerial = 7001;
    hw.chars[61].bag = {item("bread", 5, "main", 0, 0), item("sword", 1, "main", 2, 0)};
    hw.caravans[60] = {61};
    cw.chars[61].bagSid = "bull-pack"; cw.chars[61].bagSerial = 9001;
    Run({{&host, &hw}, {&cli, &cw}}, 4.0, [&] { return cw.chars[61].bag == hw.chars[61].bag; });
    CHECK(cw.chars[61].bag == hw.chars[61].bag);
    // the trader asks the client's character to trade: its window sells from the pack
    hw.tradeReqs.push_back({FakeWorld::H(me), FakeWorld::H(60)});
    Run({{&host, &hw}, {&cli, &cw}}, 4.0, [&] { return cli.tradeView().open; });
    CHECK(cli.tradeView().open && cli.tradeView().counters == 1 && host.hostTrades() == 1 && cw.tradeWindow);
    // a purchase while the caravan walks on: 2 breads for 60 cats, paid on the host
    hw.chars[60].dest = {40, 0, -20};
    hw.chars[61].dest = {44, 0, -24};
    cw.chars[61].bag[0].quantity = 3;
    cw.chars[me].items.push_back(item("bread", 2, "main", 4, 1));
    cw.money -= 60;
    Run({{&host, &hw}, {&cli, &cw}}, 4.0, [&] { return hw.money == 940 && cw.money == 940 && cw.chars[61].bag == hw.chars[61].bag; });
    CHECK(hw.money == 940 && hw.merchantMoney[60] == 560 && hw.chars[61].bag.size() == 2 && hw.chars[61].bag[0].quantity == 3);
    CHECK(cw.money == 940 && cw.chars[61].bag == hw.chars[61].bag && cw.chars[me].items == hw.chars[me].items);
    // the bull is knocked out mid-trade: the window closes everywhere, nothing else moves
    hw.chars[61].vit.flags = kVitUnconscious;
    Run({{&host, &hw}, {&cli, &cw}}, 4.0, [&] { return host.hostTrades() == 0 && !cli.tradeView().open && !cw.tradeWindow; });
    CHECK(host.hostTrades() == 0 && !cli.tradeView().open && !cw.tradeWindow && hw.money == 940);
    // it wakes up; the client steals the sword from the NPC's pack: the host's crime check decides
    hw.chars[61].vit.flags = 0;
    Run({{&host, &hw}, {&cli, &cw}}, 1.5);
    {
        auto& b = cw.chars[61].bag;
        b.erase(std::remove_if(b.begin(), b.end(), [](const ItemState& i) { return i.templateSid == "sword"; }), b.end());
    }
    cw.chars[me].items.push_back(item("sword", 1, "main", 0, 3));
    Run({{&host, &hw}, {&cli, &cw}}, 4.0, [&] { return hw.chars[me].items.size() == 2 && cw.chars[61].bag == hw.chars[61].bag; });
    CHECK(hw.thefts == 1 && hw.chars[me].items.size() == 2 && hw.chars[61].bag.size() == 1 && cw.chars[61].bag == hw.chars[61].bag);
    // the client's own backpack: synced, and its player moves things in and out of it
    hw.chars[me].bagSid = "small-pack"; hw.chars[me].bagSerial = 7002;
    cw.chars[me].bagSid = "small-pack"; cw.chars[me].bagSerial = 9002;
    hw.chars[me].bag = {item("ration", 3, "main", 0, 0)};
    Run({{&host, &hw}, {&cli, &cw}}, 4.0, [&] { return cw.chars[me].bag == hw.chars[me].bag; });
    CHECK(cw.chars[me].bag == hw.chars[me].bag);
    cw.chars[me].bag.clear();
    cw.chars[me].items.push_back(item("ration", 3, "main", 5, 5));
    Run({{&host, &hw}, {&cli, &cw}}, 4.0, [&] { return hw.chars[me].bag.empty() && hw.chars[me].items.size() == 3; });
    CHECK(hw.chars[me].bag.empty() && hw.chars[me].items.size() == 3 && cw.chars[me].items == hw.chars[me].items);
    // a backpack swapped for another one (a new local item): it is bound again and refilled
    hw.chars[me].bagSid = "big-pack"; hw.chars[me].bagSerial = 7003; hw.chars[me].bag = {item("ration", 1, "main", 1, 1)};
    cw.chars[me].bagSid = "big-pack"; cw.chars[me].bagSerial = 9003; cw.chars[me].bag.clear();
    Run({{&host, &hw}, {&cli, &cw}}, 5.0, [&] { return cw.chars[me].bag == hw.chars[me].bag; });
    CHECK(cw.chars[me].bag == hw.chars[me].bag);
    // a pack beast given to the client's player joins the squad of its own character
    FakeChar pet; pet.squad = true; pet.animal = true; pet.pos = {0, 0, -25}; pet.dest = pet.pos;
    hw.chars[62] = pet;
    Run({{&host, &hw}, {&cli, &cw}}, 2.0);
    host.Assign(FakeWorld::H(62), cli.localId());
    CHECK(!hw.joins.empty() && hw.joins.back().first == 62 && hw.joins.back().second == me);
    // the wearer goes: its backpack is no longer followed
    hw.chars.erase(61);
    Run({{&host, &hw}, {&cli, &cw}}, 4.0, [&] { return cli.entityCount() == host.entityCount(); });
    CHECK(cli.entityCount() == host.entityCount());
}

static void TestCrashRejoin() {
    std::printf("session: a client whose game froze, then restarted (same Steam account): replaced cleanly, same character, nothing left open\n");
    FakeWorld hw, cw, cw2;
    SetupHost(hw);
    FakeChar merchant; merchant.squad = false; merchant.pos = {10, 0, -20}; merchant.dest = merchant.pos;
    hw.chars[50] = merchant;
    ItemState bread; bread.templateSid = "bread"; bread.quantity = 3; bread.section = "main";
    hw.boxes[900] = {"counter", {15, 0, -15}, {bread}};
    hw.shops[50] = {900};
    hw.merchantMoney[50] = 100;
    AtMenu(cw);
    AtMenu(cw2);
    cw.boxes = cw2.boxes = hw.boxes;
    SessionConfig hc; hc.port = ++g_port; hc.steamId = 1;
    SessionConfig cc; cc.port = hc.port; cc.name = "Crashy"; cc.steamId = 4242;
    Session host(hw, hc, Now, Quiet("host"));
    std::string err;
    CHECK(host.Host(&err));
    auto first = std::make_unique<Session>(cw, cc, Now, Quiet("cli"));
    CHECK(JoinAndWait(host, hw, *first, cw, hc.port, 5));
    const uint32_t me = hw.named.count("Crashy") ? hw.named["Crashy"] : 0;
    hw.tradeReqs.push_back({FakeWorld::H(me), FakeWorld::H(50)});
    Run({{&host, &hw}, {first.get(), &cw}}, 3.0, [&] { return first->tradeView().open; });
    CHECK(host.hostTrades() == 1);
    // the client's game freezes (not ticked): its connection keeps answering, the host keeps it
    Run({{&host, &hw}}, 7.0);
    CHECK(host.players().size() == 1);
    CHECK(host.hostTrades() == 1);
    // the player kills the game and starts it again: a new connection from the same account while
    // the old one is still up
    Session second(cw2, cc, Now, Quiet("cli2"));
    CHECK(second.Join("127.0.0.1", hc.port, &err));
    Run({{&host, &hw}, {&second, &cw2}}, 10.0, [&] { return second.state() == SessionState::Connected && cw2.controllable.size() == 1; });
    CHECK(second.state() == SessionState::Connected);
    CHECK(host.players().size() == 1 && host.players().begin()->second.name == "Crashy");   // not "Crashy 3"
    CHECK(hw.named.size() == 1);                       // no second character made for them
    CHECK(cw2.controllable.size() == 1 && !cw2.controllable.empty() && cw2.controllable[0].serial == me);
    CHECK(host.hostTrades() == 0);                     // the old trade window is gone
    CHECK(hw.halts >= 1);                              // its character stood still meanwhile
    CHECK(host.ownerOf(FakeWorld::H(me)) == host.players().begin()->first);
    first.reset();   // the old process is gone for good: nothing changes for the new one
    Run({{&host, &hw}, {&second, &cw2}}, 2.0);
    CHECK(second.state() == SessionState::Connected && host.players().size() == 1);
}

static void TestBuildings() {
    std::printf("session: buildings placed by a client or the host are built by everyone; progress, dismantling, removal, purchase\n");
    FakeWorld hw, cw;
    SetupHost(hw);
    hw.money = 1000;
    // a building for sale, in both worlds (from the save), with other handles on each machine
    FakeWorld::FakeBldg shop; shop.sid = "shop"; shop.pos = {5, 100, -15}; shop.forSale = true; shop.ours = false; shop.flags = kSiteComplete;
    hw.bldgs[19000] = shop;
    AtMenu(cw);
    cw.bldgs[29000] = shop;
    cw.bldgSerial = 30000;
    SessionConfig hc; hc.port = ++g_port; SessionConfig cc; cc.port = hc.port; cc.name = "C";
    Session host(hw, hc, Now, Quiet("host"));
    Session cli(cw, cc, Now, Quiet("cli"));
    std::string err;
    host.Host(&err);
    CHECK(JoinAndWait(host, hw, cli, cw, hc.port, 4));
    auto findSid = [](FakeWorld& w, const std::string& sid) -> FakeWorld::FakeBldg* {
        for (auto& [s, b] : w.bldgs) if (b.sid == sid) return &b;
        return nullptr;
    };
    // the client's build mode places a hut: nothing built here, the host builds it, then the client
    BuildPlace hut; hut.sid = "hut"; hut.pos = {30, 0, -20}; hut.rot = {1, 0, 0, 0};
    cw.placements.push_back({hut, Handle{}});
    Run({{&host, &hw}, {&cli, &cw}}, 4.0, [&] { return findSid(hw, "hut") && findSid(cw, "hut"); });
    CHECK(findSid(hw, "hut") && findSid(cw, "hut"));
    CHECK(hw.placementsBuilt == 1 && cw.placementsBuilt == 1);
    if (findSid(hw, "hut") && findSid(cw, "hut")) CHECK(Dist(findSid(hw, "hut")->pos, findSid(cw, "hut")->pos) < 0.01f);
    CHECK(host.buildingCount() == 1);
    // the host's workers build: the client sees the progress, then the end
    if (auto* b = findSid(hw, "hut")) b->progress = 40;
    Run({{&host, &hw}, {&cli, &cw}}, 4.0, [&] { auto* c = findSid(cw, "hut"); return c && c->progress == 40; });
    CHECK(findSid(cw, "hut") && findSid(cw, "hut")->progress == 40);
    if (auto* b = findSid(hw, "hut")) { b->progress = 100; b->flags = kSiteComplete; }
    Run({{&host, &hw}, {&cli, &cw}}, 4.0, [&] { auto* c = findSid(cw, "hut"); return c && c->flags == kSiteComplete; });
    CHECK(findSid(cw, "hut") && findSid(cw, "hut")->flags == kSiteComplete && findSid(cw, "hut")->progress == 100);
    // the host places a wall itself: the client builds it too
    BuildPlace wall; wall.sid = "wall"; wall.pos = {-30, 0, -20};
    Handle wallH;
    Vec3 wallAt;
    CHECK(hw.ExecutePlacement(wall, wallH, wallAt));
    hw.placements.push_back({wall, wallH});
    Run({{&host, &hw}, {&cli, &cw}}, 4.0, [&] { return findSid(cw, "wall") != nullptr; });
    CHECK(findSid(cw, "wall") && Dist(findSid(cw, "wall")->pos, wallAt) < 0.01f && cw.placementsBuilt == 2);
    // a placement the host's game refuses: nothing anywhere
    BuildPlace bad; bad.sid = "refused";
    cw.placements.push_back({bad, Handle{}});
    Run({{&host, &hw}, {&cli, &cw}}, 2.0);
    CHECK(!findSid(hw, "refused") && !findSid(cw, "refused") && hw.placementsBuilt == 2);
    // a placement build mode would have refused (in the acid): the host checks it, builds nothing,
    // tells the player why
    const int checked = hw.placementsChecked;
    BuildPlace acid; acid.sid = "inwater"; acid.pos = {54612.5f, 0, 40575.9f};
    cw.placements.push_back({acid, Handle{}});
    Run({{&host, &hw}, {&cli, &cw}}, 2.0, [&] { return !cli.chatLog().empty() && cli.chatLog().back().find("eau") != std::string::npos; });
    CHECK(hw.placementsChecked > checked);
    CHECK(!findSid(hw, "inwater") && !findSid(cw, "inwater") && hw.placementsBuilt == 2 && cw.placementsBuilt == 2);
    CHECK(!cli.chatLog().empty() && cli.chatLog().back().find("dans l'eau ou l'acide") != std::string::npos);
    // the client dismantles the hut: the host's game does it, the client sees it being dismantled
    {
        auto* c = findSid(cw, "hut");
        BuildAction d; d.kind = BuildActionKind::Dismantle; d.arg = 2; d.sid = "hut"; d.pos = c ? c->pos : Vec3{};
        cw.bldgActions.push_back(d);
    }
    Run({{&host, &hw}, {&cli, &cw}}, 4.0, [&] { auto* c = findSid(cw, "hut"); return c && (c->flags & kSiteDismantling); });
    CHECK(findSid(hw, "hut") && (findSid(hw, "hut")->flags & kSiteDismantling));
    CHECK(findSid(cw, "hut") && (findSid(cw, "hut")->flags & kSiteDismantling));
    // dismantled for good on the host: gone on the client
    for (auto it = hw.bldgs.begin(); it != hw.bldgs.end(); ++it)
        if (it->second.sid == "hut") { hw.bldgRemoved.push_back(FakeWorld::Bh(it->first)); hw.bldgs.erase(it); break; }
    Run({{&host, &hw}, {&cli, &cw}}, 3.0, [&] { return findSid(cw, "hut") == nullptr; });
    CHECK(!findSid(cw, "hut") && host.buildingCount() == 1);
    // the client buys the shop: the host buys it, every client replays the purchase
    cw.bldgActions.push_back({BuildActionKind::Buy, 2, "shop", shop.pos});
    Run({{&host, &hw}, {&cli, &cw}}, 4.0, [&] { auto* c = findSid(cw, "shop"); return c && c->ours; });
    CHECK(findSid(hw, "shop") && findSid(hw, "shop")->ours && !findSid(hw, "shop")->forSale && hw.money == 900);
    CHECK(findSid(cw, "shop") && findSid(cw, "shop")->ours && !findSid(cw, "shop")->forSale);
}

// ---- lot B
static void TestFactions() {
    std::printf("session: faction relations and bounties are the host's, everywhere\n");
    FakeWorld hw, cw;
    SetupHost(hw);
    FactionRelationEntry holy; holy.factionSid = "holy"; holy.hasOurs = holy.hasTheirs = true; holy.ours.relation = -10; holy.theirs.relation = -12;
    FactionRelationEntry shek; shek.factionSid = "shek"; shek.hasOurs = true; shek.ours.relation = 30; shek.ours.alliance = true;
    hw.factions.factions = {holy, shek};
    hw.factions.playerRank = 2;
    hw.bounties[1].bounties = {{"holy", 2000, 8u, false, 5}};
    hw.bounties[1].crime = 3;
    hw.bounties[1].crimeFactionSid = "holy";
    AtMenu(cw);
    SessionConfig hc; hc.characterPerPlayer = false; hc.port = ++g_port;
    SessionConfig cc; cc.port = hc.port; cc.name = "C";
    Session host(hw, hc, Now, Quiet("host"));
    Session cli(cw, cc, Now, Quiet("cli"));
    std::string err;
    host.Host(&err);
    CHECK(JoinAndWait(host, hw, cli, cw, hc.port, 3));
    Run({{&host, &hw}, {&cli, &cw}}, 4.0, [&] { return cw.factions == hw.factions && cw.bounties[1] == hw.bounties[1]; });
    CHECK(cw.factions == hw.factions);
    CHECK(cw.bounties[1] == hw.bounties[1] && cw.bounties[1].bounties.size() == 1 && cw.bounties[1].bounties[0].amount == 2000);
    CHECK(cli.factionsView().received >= 1 && cli.factionsView().bountiesReceived >= 1 && host.factionsView().sent >= 1);
    // the host's game changes them (war declared, bounty paid off): everyone follows within a second or two
    hw.factions.factions[0].ours.relation = -100;
    hw.factions.factions[0].ours.war = true;
    hw.bounties[1].bounties.clear();
    hw.bounties[1].crime = 0;
    hw.bounties[1].crimeFactionSid.clear();
    Run({{&host, &hw}, {&cli, &cw}}, 4.0, [&] { return cw.factions == hw.factions && cw.bounties[1] == hw.bounties[1]; });
    CHECK(cw.factions.factions.size() == 2 && cw.factions.factions[0].ours.war && cw.factions.factions[0].ours.relation == -100);
    CHECK(cw.bounties[1].bounties.empty() && cw.bounties[1].crime == 0);
    // the client's game drifts by itself: put back to the host's values
    cw.factions.factions[1].ours.relation = 99;
    const size_t before = cli.factionsView().corrected;
    Run({{&host, &hw}, {&cli, &cw}}, 4.0, [&] { return cw.factions == hw.factions; });
    CHECK(cw.factions == hw.factions && cli.factionsView().corrected > before);
}

// diplomacy: relations between factions, leaders, towns; French news for players
static bool ChatHas(const Session& s, const std::string& part) {
    for (const auto& l : s.chatLog()) if (l.find(part) != std::string::npos) return true;
    return false;
}
static const FactionPairRelation* PairOf(const FakeWorld& w, const std::string& a, const std::string& b) {
    for (const auto& p : w.diplo.pairs) if (p.from == a && p.to == b) return &p;
    return nullptr;
}
static void TestDiplomacy() {
    std::printf("session: diplomacy (wars between factions, leaders, towns) is the host's, everywhere, with news in French\n");
    FakeWorld hw, cw;
    SetupHost(hw);
    // the save both games start from
    DiplomacyState start;
    RelationState calm; calm.relation = 10;
    for (const char* a : {"holy", "shek", "bandits"})
        for (const char* b : {"holy", "shek", "bandits"})
            if (std::string(a) != b) start.pairs.push_back({a, b, calm});
    start.uniques = {{"phoenix", kUniqueAlive, false}, {"tinfist", kUniqueAlive, false}};   // in id order, as games list them
    start.towns = {{"squin", "shek", ""}, {"stoat", "holy", ""}};
    hw.diplo = start;
    FactionRelationEntry holy; holy.factionSid = "holy"; holy.hasOurs = holy.hasTheirs = true; holy.ours.relation = 0; holy.theirs.relation = 0;
    hw.factions.factions = {holy};
    hw.bounties[1].bounties = {};
    AtMenu(cw);
    cw.diplo = start;   // the host's save, loaded
    SessionConfig hc; hc.characterPerPlayer = false; hc.port = ++g_port;
    SessionConfig cc; cc.port = hc.port; cc.name = "C";
    Session host(hw, hc, Now, Quiet("host"));
    Session cli(cw, cc, Now, Quiet("cli"));
    std::string err;
    host.Host(&err);
    CHECK(JoinAndWait(host, hw, cli, cw, hc.port, 3));
    Run({{&host, &hw}, {&cli, &cw}}, 8.0, [&] { return cli.diplomacyView().received >= 3; });
    CHECK(cli.diplomacyView().received >= 3);   // the three parts, once (nothing changed yet)
    CHECK(cli.hostDiplomacy().pairs.empty() && cli.hostDiplomacy().uniques.size() == 2 && cli.hostDiplomacy().towns.size() == 2);
    CHECK(cw.diplo == start);
    CHECK(!ChatHas(cli, "Monde") && !ChatHas(cli, "Diplomatie"));   // the state as it was is not news
    // the Shek chief dies by the players' hand: the Shek go to war with the Holy Nation, Squin is
    // taken over; the Holy Nation turns on the players; a bounty is put on one of them
    hw.diplo.uniques[1] = {"tinfist", kUniqueDead, true};
    for (auto& p : hw.diplo.pairs)
        if ((p.from == "shek" && p.to == "holy") || (p.from == "holy" && p.to == "shek")) { p.rel.war = true; p.rel.relation = -100; }
    hw.diplo.towns[0] = {"squin", "holy", "squin-occupied"};
    hw.factions.factions[0].theirs.relation = -60;
    hw.bounties[1].bounties = {{"holy", 3000, 8u, false, 5}};
    Run({{&host, &hw}, {&cli, &cw}}, 8.0, [&] {
        const auto* p = PairOf(cw, "shek", "holy");
        return p && p->rel.war && cw.diplo.uniques == hw.diplo.uniques && cw.diplo.towns == hw.diplo.towns && cw.bounties[1] == hw.bounties[1];
    });
    CHECK(PairOf(cw, "shek", "holy") && PairOf(cw, "shek", "holy")->rel.war && PairOf(cw, "holy", "shek")->rel.war);
    CHECK(PairOf(cw, "bandits", "holy") && !PairOf(cw, "bandits", "holy")->rel.war);
    CHECK(cw.diplo.uniques == hw.diplo.uniques && cw.diplo.towns == hw.diplo.towns);
    CHECK(cli.hostDiplomacy().pairs.size() == 2);   // only the pairs that changed travel
    for (const Session* s : {&host, &cli}) {         // the same news on both sides, in French
        CHECK(ChatHas(*s, "Diplomatie : guerre entre"));
        CHECK(ChatHas(*s, "Monde : tinfist est mort (de la main des joueurs)"));
        CHECK(ChatHas(*s, "Monde : squin appartient maintenant à holy"));
        CHECK(ChatHas(*s, "Monde : squin a changé"));
        CHECK(ChatHas(*s, "Diplomatie : holy vous considère maintenant comme ennemi"));
        CHECK(ChatHas(*s, "Prime : "));
    }
    // peace comes back: the pair is sent again (it moved since the host started), clients follow
    for (auto& p : hw.diplo.pairs)
        if (p.from == "shek" && p.to == "holy") { p.rel.war = false; p.rel.relation = 5; }
    hw.bounties[1].bounties.clear();
    Run({{&host, &hw}, {&cli, &cw}}, 8.0, [&] { const auto* p = PairOf(cw, "shek", "holy"); return p && !p->rel.war && cw.bounties[1].bounties.empty(); });
    CHECK(!PairOf(cw, "shek", "holy")->rel.war && PairOf(cw, "holy", "shek")->rel.war);
    CHECK(ChatHas(cli, "n'est plus recherché par holy"));
    // the client's game changes them by itself: put back to the host's within a few seconds
    for (auto& p : cw.diplo.pairs) if (p.from == "holy" && p.to == "shek") p.rel.war = false;
    cw.diplo.uniques[1].state = kUniqueAlive;
    const size_t before = cli.diplomacyView().corrected;
    Run({{&host, &hw}, {&cli, &cw}}, 8.0, [&] { return PairOf(cw, "holy", "shek")->rel.war && cw.diplo.uniques == hw.diplo.uniques; });
    CHECK(PairOf(cw, "holy", "shek")->rel.war && cw.diplo.uniques == hw.diplo.uniques && cli.diplomacyView().corrected > before);
    // float noise in the host's relations between NPC factions is not news, nor sent again
    const size_t sent = host.diplomacyView().sent;
    for (auto& p : hw.diplo.pairs) p.rel.relation += 0.3f;
    Run({{&host, &hw}, {&cli, &cw}}, 4.0);
    CHECK(host.diplomacyView().sent == sent);
}

// lot D: prisons
static void TestCaptives() {
    std::printf("session: captive characters (cage, shackles, slavery, sentence) follow the host, and their release\n");
    FakeWorld hw, cw;
    SetupHost(hw);
    FakeChar npc; npc.squad = false; npc.pos = {130, 0, 0}; npc.dest = npc.pos;
    hw.chars[20] = npc;
    AtMenu(cw);
    SessionConfig hc; hc.characterPerPlayer = false; hc.port = ++g_port; SessionConfig cc; cc.port = hc.port; cc.name = "C";
    Session host(hw, hc, Now, Quiet("host"));
    Session cli(cw, cc, Now, Quiet("cli"));
    std::string err;
    host.Host(&err);
    CHECK(JoinAndWait(host, hw, cli, cw, hc.port, 4));
    // a hostile NPC locks squad member 2 in a cage, with shackles, as a slave serving a sentence
    CaptiveState s; s.caged = true; s.cageSid = "cage-a"; s.cagePos = {205, 0, 3}; s.chained = true; s.slaveState = 1; s.slaveOf = "slavers";
    s.slaveOwner = FakeWorld::H(20); s.sentenceBegan = 77; s.sentence = 24;
    hw.chars[2].cap = s;
    // and an NPC prisoner in another cage
    CaptiveState p; p.caged = true; p.cageSid = "cage-b"; p.cagePos = {131, 0, 0};
    hw.chars[20].cap = p;
    Run({{&host, &hw}, {&cli, &cw}}, 3.0, [&] { return cw.chars[2].cap == s && cw.chars[20].cap == p; });
    CHECK(cw.chars[2].cap == s);
    CHECK(cw.chars[20].cap == p);
    CHECK(host.captiveCount() == 2 && cli.captiveCount() == 2);
    CHECK(cw.chars[1].cap.free() && cw.chars[3].cap.free());
    // our copy drifting (the local game would have let it out): imposed again
    cw.chars[2].cap = CaptiveState{};
    Run({{&host, &hw}, {&cli, &cw}}, 4.0, [&] { return cw.chars[2].cap == s; });
    CHECK(cw.chars[2].cap == s);
    // the lock is picked: out of the cage, shackles still on
    hw.chars[2].cap.caged = false; hw.chars[2].cap.cageSid.clear(); hw.chars[2].cap.cagePos = {};
    Run({{&host, &hw}, {&cli, &cw}}, 3.0, [&] { return !cw.chars[2].cap.caged; });
    CHECK(!cw.chars[2].cap.caged && cw.chars[2].cap.chained && cw.chars[2].cap.slaveState == 1);
    // set free: everything cleared on the client too, and forgotten on both sides
    hw.chars[2].cap = CaptiveState{};
    hw.chars[20].cap = CaptiveState{};
    Run({{&host, &hw}, {&cli, &cw}}, 3.0, [&] { return cw.chars[2].cap.free() && cw.chars[20].cap.free(); });
    CHECK(cw.chars[2].cap.free() && cw.chars[20].cap.free());
    Run({{&host, &hw}, {&cli, &cw}}, 0.6);
    CHECK(host.captiveCount() == 0 && cli.captiveCount() == 0);
    // nothing is resent for characters that stay free
    const int applies = cw.captiveApplies;
    Run({{&host, &hw}, {&cli, &cw}}, 2.5);
    CHECK(cw.captiveApplies == applies);
}

// The overlay's GUI -> display conversion (plugin/map_view.h): MyGUI view pixels to back buffer pixels.
static void TestGuiToDisplay() {
    std::printf("overlay: MyGUI coordinates converted to the display, per axis, every frame\n");
    using kcp::MapScene;
    // same size: unchanged
    auto g = kcp::MakeGuiToDisplay(844, 774, 844, 774);
    CHECK(g.known && g.fx == 1.0f && g.fy == 1.0f);
    // the window was resized, the GUI still has its old view: stretched per axis
    g = kcp::MakeGuiToDisplay(1280, 720, 640, 540);
    CHECK(g.known && std::fabs(g.fx - 0.5f) < 1e-6f && std::fabs(g.fy - 0.75f) < 1e-6f);
    const kcp::SceneRect r = kcp::GuiRectToDisplay({100, 600, 60, 60, 2}, g);
    CHECK(std::fabs(r.x - 50) < 1e-4f && std::fabs(r.y - 450) < 1e-4f && std::fabs(r.w - 30) < 1e-4f && std::fabs(r.h - 45) < 1e-4f && r.owner == 2);
    // unknown or absurd sizes: 1:1
    g = kcp::MakeGuiToDisplay(0, 0, 800, 600);
    CHECK(!g.known && g.fx == 1.0f && g.fy == 1.0f);
    g = kcp::MakeGuiToDisplay(1280, 720, 0, 600);
    CHECK(!g.known);
    // a scene: map image, its visible part and the frames move together, once only
    MapScene s;
    s.guiW = 1600; s.guiH = 900;
    s.mapOpen = true; s.boundsOk = true;
    s.minX = 0; s.minZ = 0; s.sizeX = 1000; s.sizeZ = 1000;
    s.imgX = 400; s.imgY = 100; s.imgW = 800; s.imgH = 800;
    s.clipX0 = 400; s.clipY0 = 100; s.clipX1 = 1200; s.clipY1 = 900;
    s.portraits.push_back({800, 850, 40, 40, 1});
    kcp::SceneToDisplay(s, 800, 450);
    CHECK(s.inDisplay && s.imgX == 200 && s.imgY == 50 && s.imgW == 400 && s.imgH == 400 && s.clipX1 == 600 && s.clipY1 == 450);
    CHECK(s.portraits[0].x == 400 && s.portraits[0].y == 425 && s.portraits[0].w == 20 && s.portraits[0].h == 20);
    kcp::SceneToDisplay(s, 800, 450);   // a second call changes nothing
    CHECK(s.imgX == 200 && s.portraits[0].x == 400);
    // the world point at the image's centre lands at the centre of the converted image
    float sx = 0, sy = 0;
    CHECK(kcp::WorldToMapScreen(s, {500, 0, 500}, sx, sy) && std::fabs(sx - 400) < 1e-3f && std::fabs(sy - 250) < 1e-3f);
}

static void TestMap() {
    std::printf("session: map markers (players' characters, hostile squads) and pings reach every player\n");
    FakeWorld hw;
    SetupHost(hw);
    hw.threats = {MapThreat{{150, 0, 40}, 5, ThreatKind::Attacking, "Bandits"}};
    SessionConfig hc; hc.characterPerPlayer = false; hc.port = ++g_port; hc.name = "Hote";
    Session host(hw, hc, Now, Quiet("host"));
    std::string err;
    CHECK(host.Host(&err));
    FakeWorld w1, w2;
    AtMenu(w1); AtMenu(w2);
    SessionConfig c1; c1.port = hc.port; c1.name = "Un";
    SessionConfig c2; c2.port = hc.port; c2.name = "Deux";
    Session s1(w1, c1, Now, Quiet("c1")), s2(w2, c2, Now, Quiet("c2"));
    CHECK(s1.Join("127.0.0.1", hc.port, &err));
    CHECK(s2.Join("127.0.0.1", hc.port, &err));
    std::vector<std::pair<Session*, FakeWorld*>> all{{&host, &hw}, {&s1, &w1}, {&s2, &w2}};
    Run(all, 15.0, [&] { return s1.state() == SessionState::Connected && s2.state() == SessionState::Connected && s1.entityCount() == 3 && s2.entityCount() == 3; });
    CHECK(s1.state() == SessionState::Connected && s2.state() == SessionState::Connected);
    host.Assign(FakeWorld::H(2), s1.localId());
    host.Assign(FakeWorld::H(3), s2.localId());
    Run(all, 3.0, [&] {
        const auto& m = s2.mapMarkers();
        return m.chars.size() == 3 && m.players.size() == 3 && m.chars[2].owner == s2.localId() && !m.threats.empty();
    });
    const MapMarkersMsg& m = s2.mapMarkers();
    CHECK(m.chars.size() == 3 && m.players.size() == 3);
    {   // each squad character once (the nearby-characters list also holds the squad: it must not leak in)
        std::set<uint32_t> ids;
        for (const auto& c : host.mapMarkers().chars) CHECK(ids.insert(c.netId).second);
        CHECK(host.mapMarkers().chars.size() == 3);
    }
    CHECK(s2.mapMarkersAge() < 2.0);
    CHECK(m == host.mapMarkers() || m.chars.size() == host.mapMarkers().chars.size());
    if (m.chars.size() == 3) {
        CHECK(m.chars[0].owner == 1 && m.chars[1].owner == s1.localId() && m.chars[2].owner == s2.localId());
        // every player has one character marked as their own (here: their first one)
        CHECK((m.chars[0].flags & kMapAvatar) && (m.chars[1].flags & kMapAvatar) && (m.chars[2].flags & kMapAvatar));
        CHECK(Dist(m.chars[1].pos, hw.chars[2].pos) < 1.0f);
        Handle h; CHECK(s2.netIdHandle(m.chars[1].netId, h) && h == FakeWorld::H(2));
    }
    CHECK(m.threats.size() == 1 && m.threats[0].label == "Bandits" && m.threats[0].kind == ThreatKind::Attacking && m.threats[0].count == 5);
    // characters far from a client are on its map too: positions follow the host's
    hw.chars[3].pos = hw.chars[3].dest = {90000, 0, -90000};
    Run(all, 2.0, [&] { return s1.mapMarkers().chars.size() == 3 && Dist(s1.mapMarkers().chars[2].pos, hw.chars[3].pos) < 1.0f; });
    CHECK(s1.mapMarkers().chars.size() == 3 && Dist(s1.mapMarkers().chars[2].pos, hw.chars[3].pos) < 1.0f);
    // a client pings: the host and the other client show it, in the pinger's name
    CHECK(s1.PlaceMapPing({10, 0, 20}, PingKind::Danger));
    CHECK(!s1.PlaceMapPing({11, 0, 20}, PingKind::Go));   // too soon
    Run(all, 2.0, [&] { return host.pings().size() == 1 && s2.pings().size() == 1 && s1.pings().size() == 1; });
    CHECK(host.pings().size() == 1 && s2.pings().size() == 1 && s1.pings().size() == 1);
    if (s2.pings().size() == 1) {
        CHECK(s2.pings()[0].ping.owner == s1.localId() && s2.pings()[0].ping.kind == PingKind::Danger && s2.pings()[0].ping.pos.z == 20);
        CHECK(host.pings()[0].ping.id == s2.pings()[0].ping.id);
    }
    // the host's own ping, and the cap of 5 alive per player
    CHECK(host.PlaceMapPing({1, 0, 1}, PingKind::Help));
    for (int i = 0; i < 6; ++i) {
        Run(all, Session::kPingInterval + 0.05);
        s1.PlaceMapPing({float(100 + i), 0, 0}, PingKind::Go);
    }
    Run(all, 1.0);
    size_t ofS1 = 0;
    for (const auto& p : s2.pings()) ofS1 += p.ping.owner == s1.localId() ? 1 : 0;
    CHECK(ofS1 == Session::kPingsPerPlayer);
    CHECK(s2.pings().size() == Session::kPingsPerPlayer + 1);
    // they fade away
    Run(all, Session::kPingLife + 0.5, [&] { return host.pings().empty() && s2.pings().empty(); });
    CHECK(host.pings().empty() && s2.pings().empty());
    CHECK(hw.threatCalls > 0);
}

static void TestJobs() {   // fix G5
    std::printf("session: a job the host's character no longer has is gone from the client's list too\n");
    FakeWorld hw, cw;
    SetupHost(hw);
    AtMenu(cw);
    SessionConfig hc; hc.characterPerPlayer = false; hc.port = ++g_port; SessionConfig cc; cc.port = hc.port; cc.name = "C";
    Session host(hw, hc, Now, Quiet("host"));
    Session cli(cw, cc, Now, Quiet("cli"));
    std::string err;
    host.Host(&err);
    CHECK(JoinAndWait(host, hw, cli, cw, hc.port, 3));
    hw.chars[2].jobs = {16, 87};
    cw.chars[2].jobs = {16, 87, 16};
    Run({{&host, &hw}, {&cli, &cw}}, 3.0, [&] { return cw.chars[2].jobs.size() == 2; });
    CHECK((cw.chars[2].jobs == std::vector<int32_t>{16, 87}));
    hw.chars[2].jobs = {87};   // the host's character stops following
    Run({{&host, &hw}, {&cli, &cw}}, 3.0, [&] { return cw.chars[2].jobs.size() == 1; });
    CHECK((cw.chars[2].jobs == std::vector<int32_t>{87}));
}

static void TestSquadWire() {
    std::printf("squad window: SquadState, SquadRequest and JobState round trips; names cleaned; malformed requests refused\n");
    SquadStateMsg m;
    m.rev = 7;
    m.squads.push_back({FakeWorld::SquadId(5), "Les Ma\xc3\xaetres", {3, 1, 2}});
    m.squads.push_back({FakeWorld::SquadId(6), "", {}});
    m.members.push_back({3, "Beep", kMemberShared});
    Writer w; Encode(w, m);
    Reader r(w.data(), w.size());
    CHECK(PeekType(r) == Msg::SquadState);
    SquadStateMsg back;
    CHECK(Decode(r, back) && SameSquadState(m, back) && back.rev == 7);
    SquadRequest q; q.seq = 3; q.actor = 9; q.op = SquadOp::Rename; q.squad = FakeWorld::SquadId(5); q.name = "Rats";
    Writer w2; Encode(w2, q);
    Reader r2(w2.data(), w2.size());
    SquadRequest q2;
    CHECK(PeekType(r2) == Msg::SquadRequest && Decode(r2, q2) && q2.op == SquadOp::Rename && q2.squad == q.squad && q2.name == "Rats" && q2.actor == 9);
    auto refused = [](SquadRequest x) { Writer ww; Encode(ww, x); Reader rr(ww.data(), ww.size()); PeekType(rr); SquadRequest y; return !Decode(rr, y); };
    SquadRequest bad = q; bad.actor = 0; CHECK(refused(bad));                   // no actor named
    bad = q; bad.squad = Handle{}; CHECK(refused(bad));                         // rename of no squad
    bad = q; bad.name = "  \x01 "; CHECK(refused(bad));                          // no name left
    bad = q; bad.index = -1; CHECK(refused(bad));
    CHECK(CleanSquadName("  a\tb  ") == "ab");
    CHECK(CleanSquadName(std::string(100, 'x')).size() == kMaxSquadName);
    CHECK(CleanSquadName(std::string(63, 'x') + "\xc3\xae").size() == 63);    // a cut UTF-8 letter is dropped
    JobStateMsg js;
    JobEntry e; e.task = 87; e.subjectSid = "mine"; e.subjectPos = {1, 2, 3}; e.location = {4, 5, 6};
    js.chars.push_back({12, {e}});
    Writer w3; Encode(w3, js);
    Reader r3(w3.data(), w3.size());
    JobStateMsg js2;
    CHECK(PeekType(r3) == Msg::JobState && Decode(r3, js2) && js2.chars.size() == 1 && js2.chars[0].jobs.size() == 1 && js2.chars[0].jobs[0] == e);
    // the RESCUE button's task (148) and its kin are accepted without a subject
    CHECK(TaskTargetAllowed(TaskVia::NewTask, 148, 0, nullptr) && TaskTargetAllowed(TaskVia::AddJob, 105, 0, nullptr));
    // settings commands: what a nobody's character takes from any player
    Command c; c.kind = CommandKind::Task; c.via = TaskVia::SetOrder; CHECK(Session::IsSettingsCommand(c));
    c.via = TaskVia::AddJob; c.shift = false; CHECK(!Session::IsSettingsCommand(c));
    c.shift = true; CHECK(Session::IsSettingsCommand(c));
    c.kind = CommandKind::MoveTo; CHECK(!Session::IsSettingsCommand(c));
}

static void TestSquadFallback() {
    std::printf("squad window: a host whose squads have no id (empty SquadState) still has its squads followed (legacy Squads)\n");
    FakeWorld hw, cw;
    SetupHost(hw);   // no FSquad: ReadSquadViews gives nothing, as when the game gave no squad id
    AtMenu(cw);
    SessionConfig hc; hc.characterPerPlayer = false; hc.port = ++g_port; SessionConfig cc; cc.port = hc.port; cc.name = "C";
    Session host(hw, hc, Now, Quiet("host"));
    Session cli(cw, cc, Now, Quiet("cli"));
    std::string err;
    host.Host(&err);
    CHECK(JoinAndWait(host, hw, cli, cw, hc.port, 3));
    Run({{&host, &hw}, {&cli, &cw}}, 5.0, [&] { return cw.legacyApplies > 0 && !cli.squadState().members.empty(); });
    CHECK(!cli.squadState().members.empty() && cli.squadState().squads.empty());
    CHECK(cw.legacyApplies > 0);
}

static void TestDeadSquad() {
    std::printf("squad window: the game's dead squad (__DEAD_SQUAD__) is never sent, never asked of the host, never made\n");
    CHECK(IsDeadSquadName("__DEAD_SQUAD__") && !IsDeadSquadName("__dead_squad__ ") && !IsDeadSquadName("Alpha"));
    FakeWorld hw, cw;
    SetupHost(hw);
    // the host's world as the 20:20 soak had it: its squad of the dead read like the others
    hw.squads = {{FakeWorld::SquadId(500), "Alpha", {1, 2, 3}}, {FakeWorld::SquadId(501), kDeadSquadName, {}}};
    AtMenu(cw);
    SessionConfig hc; hc.characterPerPlayer = false; hc.port = ++g_port; hc.name = "H";
    SessionConfig cc; cc.port = hc.port; cc.name = "C";
    Session host(hw, hc, Now, Quiet("host"));
    Session cli(cw, cc, Now, Quiet("cli"));
    std::string err;
    host.Host(&err);
    CHECK(JoinAndWait(host, hw, cli, cw, hc.port, 3));
    host.Assign(FakeWorld::H(2), cli.localId());
    auto all = std::vector<std::pair<Session*, FakeWorld*>>{{&host, &hw}, {&cli, &cw}};
    Run(all, 4.0, [&] { return !cli.squadState().squads.empty() && cw.squads.size() == 1; });
    CHECK(cli.squadState().squads.size() == 1 && cli.squadState().squads[0].name == "Alpha");
    CHECK(cw.squads.size() == 1 && cw.squads[0].name == "Alpha");   // no regular "__DEAD_SQUAD__" made here
    // the client's own dead squad, empty and matched to nothing: never asked of the host as a new squad
    cw.squads.push_back({FakeWorld::SquadId(900), kDeadSquadName, {}});
    Run(all, 4.0);
    CHECK(hw.squads.size() == 2);
    // a client asking for a squad under that name (an earlier build): refused
    std::map<uint32_t, uint32_t> net;
    host.ForEachEntity([&](uint32_t id, const Handle& h, uint8_t, bool, bool) { net[h.serial] = id; });
    SquadRequest q; q.seq = 91; q.actor = net[2]; q.op = SquadOp::Create; q.name = kDeadSquadName;
    Writer w; Encode(w, q);
    CHECK(host.InjectForTest(cli.localId(), w));
    Run(all, 1.0);
    CHECK(hw.squads.size() == 2);
}

static void TestSquadWindow() {
    std::printf("squad window: moves, new squads, renames, order, leader and names go through the host; concurrent edits end the same everywhere\n");
    FakeWorld hw, cw, cw2;
    SetupHost(hw);
    hw.chars[4] = hw.chars[3];   // a recruit nobody owns
    hw.chars[5] = hw.chars[3];   // the second player's
    hw.chars[1].name = "H";      // the host's avatar (named like the host)
    hw.squads = {{FakeWorld::SquadId(500), "Alpha", {1, 2, 4}}, {FakeWorld::SquadId(501), "Beta", {3, 5}}};
    AtMenu(cw);
    AtMenu(cw2);
    SessionConfig hc; hc.characterPerPlayer = false; hc.port = ++g_port; hc.name = "H";
    SessionConfig cc; cc.port = hc.port; cc.name = "C";
    SessionConfig cc2; cc2.port = hc.port; cc2.name = "D";
    Session host(hw, hc, Now, Quiet("host"));
    Session cli(cw, cc, Now, Quiet("cli"));
    Session cli2(cw2, cc2, Now, Quiet("cli2"));
    std::string err;
    host.Host(&err);
    CHECK(JoinAndWait(host, hw, cli, cw, hc.port, 5));
    CHECK(cli2.Join("127.0.0.1", hc.port, &err));
    auto all = std::vector<std::pair<Session*, FakeWorld*>>{{&host, &hw}, {&cli, &cw}, {&cli2, &cw2}};
    Run(all, 10.0, [&] { return cli2.state() == SessionState::Connected && cli2.entityCount() == 5; });
    host.Assign(FakeWorld::H(2), cli.localId());
    host.Assign(FakeWorld::H(5), cli2.localId());
    host.Assign(FakeWorld::H(3), cli2.localId());
    std::map<uint32_t, uint32_t> net;
    host.ForEachEntity([&](uint32_t id, const Handle& h, uint8_t, bool, bool) { net[h.serial] = id; });
    auto names = [](const FakeWorld& w) { std::vector<std::string> v; for (auto& q : w.squads) v.push_back(q.name); return v; };
    auto members = [](const FakeWorld& w) { std::vector<std::vector<uint32_t>> v; for (auto& q : w.squads) v.push_back(q.members); return v; };
    auto same = [&] { return names(cw) == names(hw) && members(cw) == members(hw) && names(cw2) == names(hw) && members(cw2) == members(hw); };
    Run(all, 4.0, [&] { return same() && cli.squadState().members.size() == 5 && cw.sharedSet.size() == 1; });
    CHECK(same());
    CHECK((names(cw) == std::vector<std::string>{"Alpha", "Beta"}));
    // nobody's: the recruit (4), not the host's avatar (1) nor a player's character
    CHECK(host.IsShared(net[4]) && !host.IsShared(net[1]) && !host.IsShared(net[2]) && cli.IsShared(net[4]));
    CHECK(cw.sharedSet.size() == 1 && !cw.sharedSet.empty() && cw.sharedSet[0].serial == 4);

    // 1. the client renames the squad holding its character in its squad window: the host takes it
    cw.squads[0].name = "Les Rats";
    Run(all, 4.0, [&] { return hw.squads[0].name == "Les Rats" && same(); });
    CHECK(hw.squads[0].name == "Les Rats" && same());
    // ... a squad without any of its characters: put back
    cw.squads[1].name = "Pas a toi";
    Run(all, 2.5);
    CHECK(hw.squads[1].name == "Beta" && cw.squads[1].name == "Beta");

    // 2. a new squad made in the client's squad window: the host makes it, everyone has it
    cw.squads.push_back({FakeWorld::SquadId(800), "Eclaireurs", {}});
    Run(all, 5.0, [&] { return hw.squads.size() == 3 && same(); });
    CHECK(hw.squads.size() == 3 && hw.squads.back().name == "Eclaireurs" && same());
    if (hw.squads.size() != 3) return;
    {   // the client drops its portrait on it
        IWorld::LocalSquadRequest r; r.actor = FakeWorld::H(2); r.op = SquadOp::Move; r.squad = hw.squads[2].id; r.index = 0;
        cw.squadReqs.push_back(r);
    }
    Run(all, 3.0, [&] { return hw.squads[2].members == std::vector<uint32_t>{2} && same(); });
    CHECK(hw.squads[2].members == std::vector<uint32_t>{2} && same());
    {   // back in the first squad at index 0: it leads it
        IWorld::LocalSquadRequest r; r.actor = FakeWorld::H(2); r.op = SquadOp::Move; r.squad = hw.squads[0].id; r.index = 0;
        cw.squadReqs.push_back(r);
    }
    Run(all, 3.0, [&] { return hw.squads[0].members.size() == 3 && hw.squads[0].members[0] == 2 && same(); });
    CHECK(hw.squads[0].members.size() == 3 && hw.squads[0].members[0] == 2 && same());
    // the empty squad removed in the client's window (its cross): gone everywhere
    cw.squads.erase(cw.squads.begin() + 2);
    Run(all, 3.0, [&] { return hw.squads.size() == 2 && same(); });
    CHECK(hw.squads.size() == 2 && same());

    // 3. only one's own characters move: the host's character is refused before sending, and on the host
    const uint32_t refusedBefore = host.actorRefusals();
    {
        SquadRequest q; q.actor = net[1]; q.op = SquadOp::Move; q.squad = hw.squads[1].id;
        CHECK(!cli.RequestSquadChange(q));
        q.seq = 77;
        Writer w; Encode(w, q);
        CHECK(host.InjectForTest(cli.localId(), w));
        Run(all, 1.0);
        CHECK(host.actorRefusals() == refusedBefore + 1);
        CHECK(std::find(hw.squads[0].members.begin(), hw.squads[0].members.end(), 1u) != hw.squads[0].members.end());
    }

    // 4. squad order: the client drags its squad last
    std::swap(cw.squads[0], cw.squads[1]);
    Run(all, 3.0, [&] { return hw.squads[0].name == "Beta" && same(); });
    CHECK(hw.squads[0].name == "Beta" && same());

    // 5. concurrent: the host and the client rename the same squad at once; the host runs them in
    //    order (the client's arrives last: it wins) and everyone ends with the same name
    for (auto& q : hw.squads) if (q.name == "Les Rats") q.name = "Hote";
    for (auto& q : cw.squads) if (q.name == "Les Rats") q.name = "Client";
    Run(all, 5.0, [&] { return same() && names(hw)[1] == "Client"; });
    CHECK(same() && names(hw)[1] == "Client");
    // concurrent moves of one character: the host puts the client's character in Beta, the client
    // asks for a new squad of its own: the last one wins, everyone the same
    hw.TakeOut(2);
    hw.squads[0].members.push_back(2);
    {
        IWorld::LocalSquadRequest r; r.actor = FakeWorld::H(2); r.op = SquadOp::Move; r.name = "Solo";
        cw.squadReqs.push_back(r);
    }
    Run(all, 4.0, [&] { return hw.squads.size() == 3 && same(); });
    CHECK(hw.squads.size() == 3 && same() && hw.squads.back().name == "Solo");

    // 6. character names: one's own goes through the host, another's is put back
    cw.chars[2].name = "Kenji";
    cw.chars[1].name = "Vole";
    Run(all, 4.0, [&] { return hw.chars[2].name == "Kenji" && cw2.chars[2].name == "Kenji" && cw.chars[1].name == "H"; });
    CHECK(hw.chars[2].name == "Kenji" && cw2.chars[2].name == "Kenji" && cw.chars[1].name == "H" && hw.chars[1].name == "H");

    // 7. AI settings of the recruit nobody owns: any player, the last one wins; the host's avatar: refused
    auto passive = [](FakeWorld& w, uint32_t actor, bool on) {
        Command c; c.kind = CommandKind::Task; c.via = TaskVia::SetOrder; c.task = 13; c.shift = true; c.add = on;
        w.localOrders.push_back({FakeWorld::H(actor), c});
    };
    passive(cw, 4, true);
    Run(all, 2.0, [&] { return hw.chars[4].passive; });
    CHECK(hw.chars[4].passive && hw.sharedOrders >= 1);
    passive(cw2, 4, false);
    passive(cw, 4, true);
    Run(all, 1.5);
    passive(cw2, 4, false);   // the last one
    Run(all, 2.0, [&] { return !hw.chars[4].passive; });
    CHECK(!hw.chars[4].passive);
    Run(all, 1.0);   // every order in flight has arrived
    CHECK(!hw.chars[4].passive);
    const size_t ordersBefore = hw.orderedSerials.size();
    passive(cw, 1, true);   // the host's own character: never sent
    Run(all, 1.5);
    CHECK(!hw.chars[1].passive && hw.orderedSerials.size() == ordersBefore);
    {   // a move order on the recruit is not a setting: refused
        Command c; c.seq = 501; c.netId = net[4]; c.kind = CommandKind::MoveTo; c.pos = {9, 0, 9};
        Writer w; Encode(w, c);
        CHECK(host.InjectForTest(cli.localId(), w));
        Run(all, 1.0);
        CHECK(hw.chars[4].dest.x != 9.0f);
    }

    // 8. jobs with their targets: the same list on every client (added and ordered, not only removed)
    JobEntry j1; j1.task = 31; j1.subject = FakeWorld::H(1);
    JobEntry j2; j2.task = 87; j2.subjectSid = "mine"; j2.subjectPos = {10, 0, 10};
    hw.chars[2].jobList = {j1, j2};
    Run(all, 3.0, [&] { return cw.chars[2].jobList.size() == 2 && cw2.chars[2].jobList.size() == 2; });
    CHECK(cw.chars[2].jobList == hw.chars[2].jobList && cw2.chars[2].jobList == hw.chars[2].jobList);
    hw.chars[2].jobList = {j2, j1};
    Run(all, 3.0, [&] { return cw.chars[2].jobList == hw.chars[2].jobList; });
    CHECK(cw.chars[2].jobList == hw.chars[2].jobList);
}

static void TestRanged() {   // lot C
    std::printf("session: ranged combat: the host's shots are fired again on clients, aims and turrets follow\n");
    FakeWorld hw, cw;
    SetupHost(hw);
    FakeChar archer; archer.squad = false; archer.pos = {150, 0, 0}; archer.dest = archer.pos;
    hw.chars[30] = archer;
    AtMenu(cw);
    SessionConfig hc; hc.characterPerPlayer = false; hc.port = ++g_port; SessionConfig cc; cc.port = hc.port; cc.name = "C";
    Session host(hw, hc, Now, Quiet("host"));
    Session cli(cw, cc, Now, Quiet("cli"));
    std::string err;
    host.Host(&err);
    CHECK(JoinAndWait(host, hw, cli, cw, hc.port, 4));
    Run({{&host, &hw}, {&cli, &cw}}, 1.0);
    // the archer shoots at squad member 1; someone far away (not followed) shoots too
    IWorld::WorldShot s;
    s.shooter = FakeWorld::H(30); s.target = FakeWorld::H(1); s.stat = 17; s.aimPos = {100, 15, 0}; s.dir = {0.5f, 0.5f, 0.5f, 0.5f};
    IWorld::WorldShot far = s; far.shooter = FakeWorld::H(999);
    IWorld::WorldShot turret = s; turret.target = Handle{}; turret.turretSid = "turret-sid"; turret.turretPos = {140, 5, 3};
    hw.shotsOut = {s, far, turret};
    Run({{&host, &hw}, {&cli, &cw}}, 2.0, [&] { return cw.shotsFired.size() >= 2; });
    CHECK(cw.shotsFired.size() == 2 && host.rangedStats().shotsSent == 2);
    if (cw.shotsFired.size() == 2) {
        const auto& a = cw.shotsFired[0];
        CHECK(a.shooter.serial == 30 && a.target.serial == 1 && a.stat == 17 && a.aimPos.x == 100 && a.dir.y == 0.5f && a.turretSid.empty());
        const auto& b = cw.shotsFired[1];
        CHECK(b.turretSid == "turret-sid" && b.turretPos.x == 140 && !b.target.valid());
    }
    CHECK(cli.rangedStats().shotsReplayed == 2);
    // the archer aims at member 2: the client imposes it, every tick
    IWorld::WorldAim aim; aim.state = 0; aim.aimPos = {200, 15, 0}; aim.target = FakeWorld::H(2);
    hw.aims[30] = aim;
    Run({{&host, &hw}, {&cli, &cw}}, 2.0, [&] { return cw.aimsApplied.count(30) != 0; });
    CHECK(cw.aimsApplied.count(30) && cw.aimsApplied[30].first && cw.aimsApplied[30].second.aimPos.x == 200 && cw.aimsApplied[30].second.target.serial == 2);
    // it stops: the client is told
    hw.aims.clear();
    Run({{&host, &hw}, {&cli, &cw}}, 2.0, [&] { return cw.aimsApplied.count(30) && !cw.aimsApplied[30].first; });
    CHECK(cw.aimsApplied.count(30) && !cw.aimsApplied[30].first);
    // a turret near the players turns: the client turns it; unchanged, it is not sent again at once
    hw.turrets = {TurretAim{"turret-sid", {140, 5, 3}, {300, 10, 40}}};
    Run({{&host, &hw}, {&cli, &cw}}, 2.0, [&] { return !cw.turretsApplied.empty(); });
    CHECK(!cw.turretsApplied.empty() && cw.turretsApplied.back().sid == "turret-sid" && cw.turretsApplied.back().target.z == 40);
    const size_t n = cw.turretsApplied.size();
    Run({{&host, &hw}, {&cli, &cw}}, 0.6);
    CHECK(cw.turretsApplied.size() <= n + 1);   // at most the 2 s refresh, not one every 0.2 s
}

// Host administration: the command syntax, the game's experience arithmetic, the god-mode registry
// (kept by player identity across rejoins) and the notice sent to one player.
static void TestAdmin() {
    std::printf("admin: commands, experience, god mode registry, notices\n");
    namespace adm = kc::admin;
    adm::Command c;
    std::string err;
    CHECK(adm::Parse("god 2 on", c, err) && c.verb == adm::Verb::God && c.who.id == 2 && c.on);
    CHECK(adm::Parse("GOD all off", c, err) && c.who.all && !c.on);
    CHECK(adm::Parse("god host", c, err) && c.who.host && c.on);
    CHECK(!adm::Parse("god bob on", c, err) && !err.empty());
    CHECK(!adm::Parse("god 2 maybe", c, err));
    CHECK(adm::Parse("xp 2 melee_attack 100", c, err) && c.verb == adm::Verb::Xp && c.skill == 1 && c.amount == 100 && !c.levels);
    CHECK(adm::Parse("xp all all 5 levels", c, err) && c.who.all && c.skill == adm::kAllSkills && c.amount == 5 && c.levels);
    CHECK(adm::Parse("xp host Attaque 2.5 niveaux", c, err) && c.who.host && c.skill == 1 && c.amount == 2.5 && c.levels);
    CHECK(!adm::Parse("xp 2 bogus 5", c, err));
    CHECK(!adm::Parse("xp 2 1 30000", c, err));       // above the per-command limit
    CHECK(!adm::Parse("xp 2 1 101 levels", c, err));
    CHECK(!adm::Parse("xp 2 1 -5", c, err));
    CHECK(!adm::Parse("xp 2 1 1,5", c, err));         // the decimal point only, whatever the locale
    CHECK(adm::Parse("tp 2 host", c, err) && c.verb == adm::Verb::Tp && c.who.id == 2 && c.tp == adm::TpTo::Host);
    CHECK(adm::Parse("tp host 3", c, err) && c.who.host && c.tp == adm::TpTo::Player && c.to.id == 3);
    CHECK(adm::Parse("tp 2 3", c, err) && c.tp == adm::TpTo::Player && c.to.id == 3);
    CHECK(adm::Parse("tp all point", c, err) && c.who.all && c.tp == adm::TpTo::Point);
    CHECK(adm::Parse("tp 2 1.5 -2 300", c, err) && c.tp == adm::TpTo::Pos && c.pos.x == 1.5f && c.pos.y == -2.0f && c.pos.z == 300.0f);
    CHECK(!adm::Parse("tp 2 all", c, err));
    CHECK(!adm::Parse("tp 2", c, err));
    CHECK(adm::Parse("money 5000", c, err) && c.verb == adm::Verb::Money && c.money == 5000);
    CHECK(adm::Parse("money -200", c, err) && c.money == -200);
    CHECK(!adm::Parse("money 0", c, err));
    CHECK(adm::Parse("heal all", c, err) && c.verb == adm::Verb::Heal && c.who.all);
    CHECK(adm::Parse("list", c, err) && c.verb == adm::Verb::List);
    CHECK(!adm::Parse("", c, err) && !adm::Parse("explode 2", c, err));
    // skills
    CHECK(adm::FindSkill("melee_attack") == 1 && adm::FindSkill("Attaque") == 1 && adm::FindSkill("33") == 33);
    CHECK(adm::FindSkill("34") == -1 && adm::FindSkill("TOUTES") == adm::kAllSkills && adm::FindSkill("armes_lourdes") == 24);
    for (size_t i = 0; i < kStatCount; ++i) CHECK(adm::FindSkill(adm::kSkills[i].key) == int(i));
    // the game's increaseStat: amount * ((100 - stat) / 100)^2, nothing above 20 per call
    CHECK(adm::IncreaseStatModel(0, 20) == 20.0f);
    CHECK(std::fabs(adm::IncreaseStatModel(50, 20) - 55.0f) < 1e-4f);
    CHECK(adm::IncreaseStatModel(10, 21) == 10.0f && adm::IncreaseStatModel(10, 0) == 10.0f && adm::IncreaseStatModel(100, 5) == 100.0f);
    std::vector<float> calls;
    float stat = 0;
    auto inc = [&](float a) { calls.push_back(a); stat = adm::IncreaseStatModel(stat, a); };
    auto read = [&] { return stat; };
    CHECK(adm::GiveXp(45, inc) == 3 && calls.size() == 3 && calls[0] == 20.0f && calls[2] == 5.0f);
    CHECK(stat > 20.0f && stat < 45.0f);   // diminishing returns, as in the game
    calls.clear();
    stat = 10;
    int n = 0;
    CHECK(adm::RaiseTo(15, read, inc, 600, &n) && std::fabs(stat - 15.0f) < 2e-3f && n >= 1);
    for (float a : calls) CHECK(a > 0 && a <= 20.0f);
    stat = 60;
    CHECK(adm::RaiseTo(70, read, inc, 600) && std::fabs(stat - 70.0f) < 2e-3f);
    stat = 99;
    CHECK(!adm::RaiseTo(100, read, inc, 50) && stat > 99.0f && stat < 100.0f);   // the caller writes the rest
    calls.clear();
    stat = 5;
    CHECK(!adm::RaiseTo(10, read, [&](float a) { calls.push_back(a); }, 600, &n) && calls.size() == 1);   // refused (client): stops at once
    // god mode follows the player, not the player id
    adm::GodRegistry g;
    const std::string alice = adm::PlayerKey(76561198000000001ull, "Alice"), bob = adm::PlayerKey(0, "Bob");
    CHECK(alice == "steam:76561198000000001" && bob == "name:Bob");
    CHECK(!g.any() && !g.On(alice));
    g.Set(alice, true, {adm::kHostKey, alice, bob});
    CHECK(g.On(alice) && !g.On(bob) && !g.On(adm::kHostKey));
    CHECK(g.On(adm::PlayerKey(76561198000000001ull, "Alice2")));   // renamed, same Steam account: still on
    g.SetAll(true);
    CHECK(g.all() && g.On("name:Newcomer") && g.On(adm::kHostKey));
    g.Set(alice, false, {adm::kHostKey, alice, bob});   // one off while everyone is on: the others stay on
    CHECK(!g.all() && !g.On(alice) && g.On(bob) && g.On(adm::kHostKey) && !g.On("name:Newcomer"));
    g.SetAll(false);
    CHECK(!g.any() && !g.On(bob));
    // the notice: to that player alone, never from a client
    FakeWorld hw, cw;
    SetupHost(hw);
    AtMenu(cw);
    SessionConfig hc; hc.characterPerPlayer = false; hc.port = ++g_port; SessionConfig cc; cc.port = hc.port; cc.name = "C";
    Session host(hw, hc, Now, Quiet("host"));
    Session cli(cw, cc, Now, Quiet("cli"));
    host.Host(&err);
    CHECK(JoinAndWait(host, hw, cli, cw, hc.port, 3));
    CHECK(!host.SendNotice(99, "personne"));
    CHECK(!cli.SendNotice(1, "un client ne peut pas"));
    const size_t hostLines = host.chatLog().size();
    CHECK(host.SendNotice(cli.localId(), "L'hôte t'a téléporté près de lui."));
    Run({{&host, &hw}, {&cli, &cw}}, 2.0, [&] { return !cli.chatLog().empty() && cli.chatLog().back() == "* L'hôte t'a téléporté près de lui."; });
    CHECK(!cli.chatLog().empty() && cli.chatLog().back() == "* L'hôte t'a téléporté près de lui.");
    CHECK(host.chatLog().size() == hostLines);   // not a chat line of the host's
}

// ---------------------------------------------------------------- actor safety
// What every task accepts as its subject: the table the host checks before the game's order
// functions get an order (a BUILD order aimed at an NPC crashed the host in Character::addJob).
static void TestTaskTargets() {
    std::printf("actor safety: every task id is checked against what its subject is\n");
    struct Kind { const char* name; uint32_t f; };
    const uint32_t F = kTgtNamed | kTgtFound;
    const Kind kinds[] = {
        {"none", 0},
        {"self", F | kTgtCharacter | kTgtSelf | kTgtConscious | kTgtSquad},
        {"npc standing", F | kTgtCharacter | kTgtConscious},
        {"npc down", F | kTgtCharacter | kTgtDown},
        {"npc dead", F | kTgtCharacter | kTgtDead | kTgtDown},
        {"squad mate standing", F | kTgtCharacter | kTgtConscious | kTgtSquad},
        {"item", F | kTgtItem},
        {"container", F | kTgtContainer | kTgtBuilding},
        {"site of ours", F | kTgtBuilding | kTgtUnfinished | kTgtOurs},
        {"finished building of ours", F | kTgtBuilding | kTgtOurs},
        {"site of an NPC faction", F | kTgtBuilding | kTgtUnfinished},
        {"bed", F | kTgtBuilding | kTgtBed},
        {"cage", F | kTgtBuilding | kTgtCage},
        {"machine", F | kTgtBuilding | kTgtMachine},
        {"door", F | kTgtBuilding | kTgtDoor},
        {"stale handle", kTgtNamed},
    };
    // the kinds each task accepts, for the orders the UI gives
    const std::map<int, std::set<std::string>> expect = {
        {2, {"site of ours"}},
        {12, {"npc standing"}},
        {98, {"bed"}}, {258, {"bed"}},
        {87, {"machine"}},
        {3, {"item"}},
        {4, {"npc standing", "npc down", "squad mate standing"}},
        {26, {"npc standing", "npc down", "npc dead", "squad mate standing", "container"}},
        {25, {"self", "npc standing", "npc down", "squad mate standing"}},
        {44, {"npc standing", "npc down", "squad mate standing"}},
        {225, {"npc down", "npc dead"}},
        {72, {"door"}},
        {107, {"cage"}},
        {6, {"none", "self"}},
        {96, {"site of ours", "finished building of ours"}},
    };
    for (const auto& [task, ok] : expect)
        for (const auto& k : kinds) {
            const bool want = ok.count(k.name) > 0;
            for (TaskVia via : {TaskVia::AddOrder, TaskVia::NewTask, TaskVia::TaskNearest, TaskVia::AddJob}) {
                std::string why;
                const bool got = TaskTargetAllowed(via, task, k.f, &why);
                if (got != want) std::printf("    task %d on %s: %s (%s)\n", task, k.name, got ? "allowed" : "refused", why.c_str());
                CHECK(got == want);
                CHECK(got || !why.empty());
            }
        }
    // every task id: a stale or unknown subject never reaches the game; unknown ids are refused
    std::set<int> known;
    for (int task = -5; task < 600; ++task) {
        CHECK(!TaskTargetAllowed(TaskVia::AddJob, task, kTgtNamed, nullptr));
        CHECK(!TaskTargetAllowed(TaskVia::TaskNearest, task, kTgtNamed, nullptr));
        bool any = false;
        for (const auto& k : kinds) any |= TaskTargetAllowed(TaskVia::AddOrder, task, k.f, nullptr);
        if (any) known.insert(task);
    }
    CHECK(!known.count(0) && !known.count(1) && !known.count(55) && !known.count(118) && !known.count(124) && !known.count(599));
    CHECK(known.count(2) && known.count(12) && known.count(258) && known.count(87));
    // the squad bar and the Tâches panel act on the actor itself
    CHECK(TaskTargetAllowed(TaskVia::SetOrder, 13, 0, nullptr) && !TaskTargetAllowed(TaskVia::SetOrder, 18, 0, nullptr) &&
          !TaskTargetAllowed(TaskVia::SetOrder, -1, 0, nullptr));
    CHECK(TaskTargetAllowed(TaskVia::RemovePermajob, 87, 0, nullptr) && !TaskTargetAllowed(TaskVia::RemoveJob, -3, 0, nullptr));
    CHECK(!TaskTargetAllowed(TaskVia(99), 2, kTgtNamed | kTgtFound | kTgtBuilding | kTgtUnfinished | kTgtOurs, nullptr));
    // the rules table: the requests naming an actor check it; host->client messages have no rule
    size_t n = 0;
    const MessageRule* rules = MessageRules(n);
    CHECK(n >= 10);
    for (Msg m : {Msg::Command, Msg::ContainerOpen, Msg::Appearance}) CHECK(MessageRuleFor(m) && MessageRuleFor(m)->subject == AuthSubject::OwnCharacter);
    CHECK(MessageRuleFor(Msg::DialogReply)->subject == AuthSubject::OwnConversation);
    CHECK(MessageRuleFor(Msg::InvOp)->subject == AuthSubject::Inventory);
    for (Msg m : {Msg::Snapshot, Msg::Bind, Msg::Welcome, Msg::Result, Msg::Resync}) CHECK(MessageRuleFor(m) && MessageRuleFor(m)->role == AuthRole::HostOnly);
    for (size_t i = 0; i < n; ++i) CHECK(rules[i].name && *rules[i].name);
    // Result round trip
    Result r;
    r.request = Msg::Command; r.seq = 77; r.netId = 5; r.state = ResultState::Rejected; r.reason = ResultReason::WrongTarget; r.text = "Action refusée";
    Writer w;
    Encode(w, r);
    Reader rd(w.data(), w.size());
    Result back;
    CHECK(PeekType(rd) == Msg::Result && Decode(rd, back) && back.seq == 77 && back.netId == 5 && back.state == ResultState::Rejected &&
          back.reason == ResultReason::WrongTarget && back.text == r.text);
}

// The crash at kenshi_x64+0x883B78 (10 oct.): a client's local copy of a job with a wrong subject
// (task 27 on a squad mate) raised an exception inside the game; the tick caught it, but the
// destructor of the "KenshiCoop is calling" scope opened around the call never ran, and from then on
// the client's game ran every order itself, until a BUILD on an NPC crashed it.
extern "C" __declspec(dllimport) void __stdcall RaiseException(unsigned long code, unsigned long flags, unsigned long n, const unsigned __int64* args);
static thread_local ScopeCounts t_scopes;
struct TestCallScope {
    TestCallScope() { ++t_scopes.host; }
    ~TestCallScope() { --t_scopes.host; }
};
static void RaiseInGame() { RaiseException(0xE04B4301u, 0, 0, nullptr); }   // not a C++ exception, like an access violation
static void (*volatile g_gameCall)() = RaiseInGame;
__declspec(noinline) static void ScopedGameCall() {
    TestCallScope scope;
    g_gameCall();
}
static bool GuardedGameCall() {   // the tick's barrier (TickSEH)
    __try {
        ScopedGameCall();
        return true;
    } __except (1) {
        return false;
    }
}
static void TestCallScopeRepair() {
    std::printf("call scopes: an exception caught by __except leaves no scope counted, and orders are checked before our own copy runs them\n");
    // the repair itself
    ScopeCounts live{3, 1}, mark{1, 0};
    CHECK(RepairScopeCounts(live, mark) == 2 && live.host == 1 && live.anim == 0);
    live = {1, 0};
    CHECK(RepairScopeCounts(live, mark) == 0 && live.host == 1);
    live = {0, 0};   // closed twice: put back too, nothing "left open"
    CHECK(RepairScopeCounts(live, mark) == 0 && live.host == 1 && live.anim == 0);
    // what /EHsc does: the scope's destructor is skipped by the __except (the count stays up), and
    // the repair brings it back to the mark
    t_scopes = {};
    const ScopeCounts before = t_scopes;
    CHECK(!GuardedGameCall());
    std::printf("  scopes left open by the caught exception: %d\n", t_scopes.host - before.host);
    CHECK(t_scopes.host >= before.host);
    RepairScopeCounts(t_scopes, before);
    CHECK(t_scopes.host == 0 && t_scopes.anim == 0);
    // the two orders of the crash, checked the way the client's local copy (RouteOrder) and the
    // debug commands now check them before the game sees them
    const uint32_t F = kTgtNamed | kTgtFound;
    const uint32_t mate = F | kTgtCharacter | kTgtConscious | kTgtSquad, npc = F | kTgtCharacter | kTgtConscious;
    std::string why;
    CHECK(!TaskTargetAllowed(TaskVia::AddJob, 27, mate, &why) && !why.empty());   // the job that faulted on the client
    for (TaskVia via : {TaskVia::AddOrder, TaskVia::NewTask, TaskVia::TaskNearest, TaskVia::AddJob}) {
        CHECK(!TaskTargetAllowed(via, 2, npc, nullptr));                           // BUILD on an NPC (stress4, the crash)
        CHECK(!TaskTargetAllowed(via, 2, F | kTgtCharacter | kTgtDown, nullptr));
        CHECK(!TaskTargetAllowed(via, 2, mate, nullptr));
        CHECK(!TaskTargetAllowed(via, 2, 0, nullptr));                              // BUILD on nothing
        CHECK(!TaskTargetAllowed(via, 2, F | kTgtBuilding | kTgtOurs, nullptr));    // a finished building
        CHECK(TaskTargetAllowed(via, 2, F | kTgtBuilding | kTgtUnfinished | kTgtOurs, nullptr));
    }
    // the host's job lists copied to a client copy (ApplyJobList): a known task with a subject of the
    // wrong kind is never handed to the game; tasks the table does not know are not judged
    CHECK(TaskTargetWrongKind(TaskVia::AddJob, 2, npc, &why) && !why.empty());
    CHECK(TaskTargetWrongKind(TaskVia::AddJob, 87, npc, nullptr));                  // operate a machine: an NPC
    CHECK(!TaskTargetWrongKind(TaskVia::AddJob, 87, F | kTgtBuilding | kTgtMachine, nullptr));
    CHECK(!TaskTargetWrongKind(TaskVia::AddJob, 2, F | kTgtBuilding | kTgtUnfinished | kTgtOurs, nullptr));
    CHECK(!TaskTargetWrongKind(TaskVia::AddJob, 31, mate, nullptr));                // follow a squad mate
    for (int task : {0, 1, 17, 22, 40, 55, 118, 124, 599}) CHECK(!TaskTargetWrongKind(TaskVia::AddJob, task, npc, nullptr));
}

// Forged requests naming a character the sender does not own (the host's, another player's, an NPC,
// none, unknown) are refused on the host for every request type; the client's own pass; the client
// refuses to send them in the first place; orders aimed at the wrong kind of target are refused.
static void TestActorSafety() {
    std::printf("actor safety: a client never makes a character it does not own act\n");
    FakeWorld hw, cw, cw2;
    SetupHost(hw);
    FakeChar npc; npc.squad = false; npc.pos = {120, 0, 10}; npc.dest = npc.pos;
    hw.chars[10] = npc;
    AtMenu(cw);
    AtMenu(cw2);
    SessionConfig hc; hc.characterPerPlayer = false; hc.port = ++g_port;
    SessionConfig cc; cc.port = hc.port; cc.name = "C";
    SessionConfig cc2; cc2.port = hc.port; cc2.name = "D";
    std::vector<std::string> hostLog;
    Session host(hw, hc, Now, [&](const std::string& l) { hostLog.push_back(l); });
    Session cli(cw, cc, Now, Quiet("cli"));
    Session cli2(cw2, cc2, Now, Quiet("cli2"));
    std::string err;
    host.Host(&err);
    CHECK(JoinAndWait(host, hw, cli, cw, hc.port, 4));
    CHECK(cli2.Join("127.0.0.1", hc.port, &err));
    auto all = std::vector<std::pair<Session*, FakeWorld*>>{{&host, &hw}, {&cli, &cw}, {&cli2, &cw2}};
    Run(all, 10.0, [&] { return cli2.state() == SessionState::Connected && cli2.entityCount() == 4; });
    host.Assign(FakeWorld::H(2), cli.localId());    // the client's character
    host.Assign(FakeWorld::H(3), cli2.localId());   // another player's
    Run(all, 1.5);
    std::map<uint32_t, uint32_t> net;   // serial -> netId
    host.ForEachEntity([&](uint32_t id, const Handle& h, uint8_t, bool, bool) { net[h.serial] = id; });
    CHECK(net.size() == 4 && net.count(1) && net.count(2) && net.count(3) && net.count(10));
    const uint8_t me = cli.localId();
    CHECK(host.CheckActor(me, net[2]) == Session::ActorVerdict::Ok);
    CHECK(host.CheckActor(me, net[1]) == Session::ActorVerdict::NotOwned);    // the host's
    CHECK(host.CheckActor(me, net[3]) == Session::ActorVerdict::NotOwned);    // another player's
    CHECK(host.CheckActor(me, net[10]) == Session::ActorVerdict::NotSquad);   // an NPC
    CHECK(host.CheckActor(me, 0) == Session::ActorVerdict::Missing);
    CHECK(host.CheckActor(me, 9999) == Session::ActorVerdict::Unknown);
    CHECK(host.CheckActor(1, net[1]) == Session::ActorVerdict::NotOwned);     // nobody acts "as the host" from the network
    auto rejectedFor = [&](uint32_t seq, ResultReason why) {
        for (const auto& r : cli.results()) if (r.seq == seq && r.state == ResultState::Rejected && r.reason == why) return true;
        return false;
    };
    // 1. forged orders, one per order kind and way of giving it, naming every actor that is not ours
    const Vec3 far{5000, 0, 5000};
    uint32_t seq = 1000;
    uint32_t forged = 0;
    for (uint32_t actor : {net[1], net[3], net[10], 0u, 9999u}) {
        for (int kind = 0; kind < 7; ++kind) {
            Command c;
            c.seq = ++seq;
            c.netId = actor;
            c.pos = far;
            if (kind == 0) c.kind = CommandKind::MoveTo;
            else if (kind == 1) c.kind = CommandKind::Stop;
            else if (kind == 2) { c.kind = CommandKind::PickUp; c.itemSid = "x"; }
            else if (kind == 3) { c.kind = CommandKind::Task; c.via = TaskVia::TaskNearest; c.task = 258; }   // sleep in a bed
            else if (kind == 4) { c.kind = CommandKind::Task; c.via = TaskVia::TaskNearest; c.task = 12; c.subject = FakeWorld::H(10); }   // talk
            else if (kind == 5) { c.kind = CommandKind::Task; c.via = TaskVia::SetOrder; c.task = 13; }
            else { c.kind = CommandKind::SquadMove; }
            Writer w;
            Encode(w, c);
            CHECK(host.InjectForTest(me, w));
            ++forged;
        }
    }
    hw.orderedSerials.clear();
    Run(all, 1.5);
    CHECK(hw.orderedSerials.empty());   // nothing ran, on anyone
    CHECK(Dist(hw.chars[1].dest, far) > 1 && Dist(hw.chars[3].dest, far) > 1 && Dist(hw.chars[10].dest, far) > 1);
    CHECK(host.actorRefusals() >= forged);
    CHECK(rejectedFor(1001, ResultReason::NotYourCharacter));   // the host's character, answered by seq
    bool anyNoActor = false;
    for (const auto& r : cli.results()) anyNoActor |= r.reason == ResultReason::NoActor;
    CHECK(anyNoActor);
    bool logged = false;
    for (const auto& l : hostLog) logged |= l.find("refused: actor " + std::to_string(net[1]) + " not owned by player " + std::to_string(me)) != std::string::npos;
    CHECK(logged);
    bool noticed = false;
    for (const auto& l : cli.chatLog()) noticed |= l.find("Action refusée") != std::string::npos;
    CHECK(noticed);
    // 2. the other request types naming a character that is not ours: looking into a container, new
    //    looks, dropping an item, answering someone else's conversation
    const uint32_t r0 = host.actorRefusals();
    { ContainerOpen m; m.looterNetId = net[1]; m.sid = "chest"; Writer w; Encode(w, m); CHECK(host.InjectForTest(me, w)); }
    { AppearanceMsg m; m.netId = net[1]; m.name = "Hacked"; Writer w; Encode(w, m); CHECK(host.InjectForTest(me, w)); }
    { InvOp op; op.kind = InvOpKind::Drop; op.fromNetId = net[1]; op.item.templateSid = "sword"; op.item.quantity = 1; Writer w; Encode(w, op); CHECK(host.InjectForTest(me, w)); }
    { DialogReply a; a.dialogId = 4242; a.index = 0; Writer w; Encode(w, a); CHECK(host.InjectForTest(me, w)); }
    { ContainerOpen m; m.looterNetId = net[3]; m.sid = "chest"; Writer w; Encode(w, m); CHECK(host.InjectForTest(me, w)); }
    Run(all, 1.0);
    CHECK(host.actorRefusals() >= r0 + 5);
    CHECK(host.recentRefusals(me) > 5.0);
    // 3. the client's own character: obeyed, answered Done
    hw.orderedSerials.clear();
    { Command c; c.seq = 5000; c.netId = net[2]; c.kind = CommandKind::MoveTo; c.pos = {222, 0, 0}; Writer w; Encode(w, c); CHECK(host.InjectForTest(me, w)); }
    Run(all, 1.5, [&] { return Dist(hw.chars[2].dest, {222, 0, 0}) < 1e-3f; });
    CHECK(Dist(hw.chars[2].dest, {222, 0, 0}) < 1e-3f);
    CHECK((hw.orderedSerials == std::vector<uint32_t>{2}));
    Run(all, 0.5);
    bool done = false;
    for (const auto& r : cli.results()) done |= r.seq == 5000 && r.state == ResultState::Done;
    CHECK(done);
    // 4. the client refuses to send an order for a character it does not own (French notice)
    const uint32_t r1 = host.actorRefusals();
    const size_t chat0 = cli.chatLog().size();
    Command mv; mv.kind = CommandKind::MoveTo; mv.pos = far;
    cw.localOrders.push_back({FakeWorld::H(1), mv});
    cw.localOrders.push_back({FakeWorld::H(3), mv});
    Run(all, 3.0);   // past the notice's rate limit
    CHECK(host.actorRefusals() == r1);   // never sent
    CHECK(cli.chatLog().size() > chat0);
    CHECK(Dist(hw.chars[1].dest, far) > 1 && Dist(hw.chars[3].dest, far) > 1);
    // ... and one sent as is anyway (forged) is refused by the host
    Command forgedRaw = mv;
    forgedRaw.netId = net[1];
    CHECK(cli.SendRawCommandForTest(forgedRaw));
    Run(all, 1.0);
    CHECK(host.actorRefusals() == r1 + 1 && Dist(hw.chars[1].dest, far) > 1);
    // 5. orders of our own character aimed at the wrong kind of target: refused, nothing runs
    hw.subjectFlags[10] = kTgtCharacter | kTgtConscious;   // the NPC
    hw.subjectFlags[1] = kTgtCharacter | kTgtConscious | kTgtSquad;
    const int run0 = hw.tasksRun;
    uint32_t s2 = 6000;
    for (int task : {2, 87, 98, 258, 72, 107, 3, 284}) {   // build, operate, sleep, door, cage, pick up, loot a container: on an NPC
        Command c; c.seq = ++s2; c.netId = net[2]; c.kind = CommandKind::Task; c.via = TaskVia::TaskNearest; c.task = task; c.subject = FakeWorld::H(10);
        Writer w; Encode(w, c); CHECK(host.InjectForTest(me, w));
    }
    { Command c; c.seq = ++s2; c.netId = net[2]; c.kind = CommandKind::Task; c.via = TaskVia::AddJob; c.task = 2; c.subject = FakeWorld::H(77); Writer w; Encode(w, c); CHECK(host.InjectForTest(me, w)); }   // stale
    { Command c; c.seq = ++s2; c.netId = net[2]; c.kind = CommandKind::Task; c.via = TaskVia::AddJob; c.task = 2; Writer w; Encode(w, c); CHECK(host.InjectForTest(me, w)); }   // no subject at all
    { Command c; c.seq = ++s2; c.netId = net[2]; c.kind = CommandKind::Task; c.via = TaskVia::AddOrder; c.task = 12; c.subject = FakeWorld::H(1); Writer w; Encode(w, c); CHECK(host.InjectForTest(me, w)); }   // talk to a squad mate
    Run(all, 1.5);
    CHECK(hw.tasksRun == run0);
    for (uint32_t q = 6001; q <= s2; ++q) CHECK(rejectedFor(q, ResultReason::WrongTarget));
    // ... while the right target is obeyed
    hw.subjectFlags[50] = kTgtBuilding | kTgtUnfinished | kTgtOurs;
    { Command c; c.seq = 7000; c.netId = net[2]; c.kind = CommandKind::Task; c.via = TaskVia::NewTask; c.task = 2; c.subject = FakeWorld::B(50); Writer w; Encode(w, c); CHECK(host.InjectForTest(me, w)); }
    { Command c; c.seq = 7001; c.netId = net[2]; c.kind = CommandKind::Task; c.via = TaskVia::TaskNearest; c.task = 25; c.subject = FakeWorld::H(1); Writer w; Encode(w, c); CHECK(host.InjectForTest(me, w)); }   // first aid on the host's character: the actor is ours
    Run(all, 1.5);
    CHECK(hw.tasksRun == run0 + 2);
    CHECK(cli.rejectedCount() > 0);
    CHECK(host.state() == SessionState::Hosting && cli.state() == SessionState::Connected);   // everyone alive
}

// Conversations: the window of a client's character opens on that client only, its answers name
// their actor and line and reach the host's game once; one conversation per NPC ("occupé"); walking
// away, a player leaving, a recruit given to the player who recruited it.
static void TestDialogue() {
    std::printf("dialogue: windows on their player's screen, answers with their actor, one conversation per NPC\n");
    FakeWorld hw, cw, cw2;
    SetupHost(hw);
    for (uint32_t n : {10u, 11u, 12u}) {
        FakeChar npc; npc.squad = false; npc.pos = {110.0f + float(n), 0, 10}; npc.dest = npc.pos;
        hw.chars[n] = npc;
    }
    AtMenu(cw);
    AtMenu(cw2);
    SessionConfig hc; hc.characterPerPlayer = false; hc.port = ++g_port;
    SessionConfig cc; cc.port = hc.port; cc.name = "C";
    SessionConfig cc2; cc2.port = hc.port; cc2.name = "D";
    std::vector<std::string> hostLog;
    Session host(hw, hc, Now, [&](const std::string& l) { hostLog.push_back(l); });
    Session cli(cw, cc, Now, Quiet("cli"));
    Session cli2(cw2, cc2, Now, Quiet("cli2"));
    std::string err;
    host.Host(&err);
    CHECK(JoinAndWait(host, hw, cli, cw, hc.port, 6));
    CHECK(cli2.Join("127.0.0.1", hc.port, &err));
    auto all = std::vector<std::pair<Session*, FakeWorld*>>{{&host, &hw}, {&cli, &cw}, {&cli2, &cw2}};
    Run(all, 10.0, [&] { return cli2.state() == SessionState::Connected && cli2.entityCount() == 6; });
    host.Assign(FakeWorld::H(2), cli.localId());
    host.Assign(FakeWorld::H(3), cli2.localId());
    Run(all, 1.5);
    std::map<uint32_t, uint32_t> net;
    host.ForEachEntity([&](uint32_t id, const Handle& h, uint8_t, bool, bool) { net[h.serial] = id; });
    CHECK(net.size() == 6);
    const uint8_t me = cli.localId(), other = cli2.localId();
    auto ev = [](DialogKind k, uint32_t id, uint32_t speaker, uint32_t pc, std::string text, std::vector<std::string> replies = {}) {
        IWorld::WorldDialog d;
        d.kind = k; d.dialogId = id; d.speaker = FakeWorld::H(speaker); d.pc = FakeWorld::H(pc); d.text = std::move(text); d.replies = std::move(replies);
        return d;
    };
    auto has = [](const std::deque<std::string>& lines, const char* what) {
        for (const auto& l : lines) if (l.find(what) != std::string::npos) return true;
        return false;
    };

    // 1. a guard (NPC 10) stops the client's character: its window opens on that client only, with the
    //    host's line and answers, named after the client's own character; the bubble goes to everyone
    hw.dialogEvents.push_back(ev(DialogKind::Open, 1, 10, 2, "Garde"));
    hw.dialogEvents.push_back(ev(DialogKind::Text, 1, 10, 2, "Halte ! Qui va la ?", {"Un voyageur", "Ca ne te regarde pas"}));
    IWorld::WorldDialog say = ev(DialogKind::Say, 0, 10, 0, "Halte !");
    hw.dialogEvents.push_back(say);
    Run(all, 2.0, [&] { return cli.dialog().open && !cli.dialog().replies.empty() && cw2.saysShown > 0; });
    CHECK(cli.dialog().open && cli.dialog().id == 1 && cli.dialog().name == "Garde" && cli.dialog().replies.size() == 2);
    CHECK(cli.dialog().actor == net[2] && cli.dialog().turn == 1);
    CHECK(!cli2.dialog().open);                       // not on the other player's screen
    CHECK(cw.saysShown == 1 && cw2.saysShown == 1);   // the bubble: everyone
    CHECK(host.openDialogs() == 1);

    // 2. answers that are not the client's: another player answering it, a forged actor (the host's
    //    character, the other player's, none): refused before the host's game sees them
    const uint32_t r0 = host.actorRefusals();
    { DialogReply a; a.dialogId = 1; a.actor = net[3]; a.turn = 1; a.index = 0; Writer w; Encode(w, a); CHECK(host.InjectForTest(other, w)); }
    { DialogReply a; a.dialogId = 1; a.actor = net[2]; a.turn = 1; a.index = 0; Writer w; Encode(w, a); CHECK(host.InjectForTest(other, w)); }
    { DialogReply a; a.dialogId = 1; a.actor = net[1]; a.turn = 1; a.index = 0; Writer w; Encode(w, a); CHECK(host.InjectForTest(me, w)); }
    { DialogReply a; a.dialogId = 1; a.actor = net[3]; a.turn = 1; a.index = 0; Writer w; Encode(w, a); CHECK(host.InjectForTest(me, w)); }
    { DialogReply a; a.dialogId = 1; a.actor = 0; a.turn = 1; a.index = 0; Writer w; Encode(w, a); CHECK(host.InjectForTest(me, w)); }
    Run(all, 1.0);
    CHECK(hw.answers.empty());
    CHECK(host.actorRefusals() >= r0 + 5);

    // 3. the client answers: once, on the host, as its own character
    cli.AnswerDialog(1);
    CHECK(cli.dialog().waiting);
    Run(all, 2.0, [&] { return !hw.answers.empty(); });
    CHECK((hw.answers == std::vector<std::pair<uint32_t, int>>{{1u, 1}}));
    cli.AnswerDialog(0);   // waiting for the next line: not sent twice
    Run(all, 0.5);
    CHECK(hw.answers.size() == 1);
    // ... an answer to an older line (the conversation moved on: speed 3) is ignored
    hw.dialogEvents.push_back(ev(DialogKind::Text, 1, 10, 2, "Bon, circule.", {"Merci"}));
    Run(all, 2.0, [&] { return cli.dialog().turn == 2; });
    CHECK(cli.dialog().turn == 2 && !cli.dialog().waiting && cli.dialog().replies.size() == 1);
    // the game fills the window twice per line: the same line again is not a new turn
    hw.dialogEvents.push_back(ev(DialogKind::Text, 1, 10, 2, "Bon, circule.", {"Merci"}));
    Run(all, 1.0);
    CHECK(cli.dialog().turn == 2);
    { DialogReply a; a.dialogId = 1; a.actor = net[2]; a.turn = 1; a.index = 0; Writer w; Encode(w, a); CHECK(host.InjectForTest(me, w)); }
    { DialogReply a; a.dialogId = 1; a.actor = net[2]; a.turn = 2; a.index = 5; Writer w; Encode(w, a); CHECK(host.InjectForTest(me, w)); }   // not offered
    Run(all, 1.0);
    CHECK(hw.answers.size() == 1);

    // 4. one conversation per NPC: the other player's character asking the guard to talk is refused
    //    ("occupé"); the client's own character may; an NPC the game has talking with the host's
    //    character too
    hw.subjectFlags[10] = kTgtCharacter | kTgtConscious;
    hw.subjectFlags[11] = kTgtCharacter | kTgtConscious;
    const int run0 = hw.tasksRun;
    { Command c; c.seq = 900; c.netId = net[3]; c.kind = CommandKind::Task; c.via = TaskVia::TaskNearest; c.task = kTaskTalk; c.subject = FakeWorld::H(10); Writer w; Encode(w, c); CHECK(host.InjectForTest(other, w)); }
    Run(all, 1.5);
    CHECK(hw.tasksRun == run0);
    bool busy = false;
    for (const auto& r : cli2.results()) busy |= r.seq == 900 && r.state == ResultState::Rejected && r.reason == ResultReason::Busy && r.text.find("occupé") != std::string::npos;
    CHECK(busy);
    CHECK(has(cli2.chatLog(), "occupé"));
    CHECK(host.dialogBusyRefusals() == 1);
    { Command c; c.seq = 901; c.netId = net[2]; c.kind = CommandKind::Task; c.via = TaskVia::TaskNearest; c.task = kTaskTalk; c.subject = FakeWorld::H(10); Writer w; Encode(w, c); CHECK(host.InjectForTest(me, w)); }
    Run(all, 1.0);
    CHECK(hw.tasksRun == run0 + 1);
    hw.talking[11] = 1;   // the host's character talks with NPC 11
    { Command c; c.seq = 902; c.netId = net[3]; c.kind = CommandKind::Task; c.via = TaskVia::TaskNearest; c.task = kTaskTalkNearest; c.subject = FakeWorld::H(11); Writer w; Encode(w, c); CHECK(host.InjectForTest(other, w)); }
    Run(all, 1.5);
    CHECK(hw.tasksRun == run0 + 1 && host.dialogBusyRefusals() == 2);
    hw.talking.clear();
    // ... and the host's game refusing a conversation because the NPC is busy tells that player
    const size_t chat2 = cli2.chatLog().size();
    hw.dialogEvents.push_back(ev(DialogKind::Busy, 0, 10, 3, "Garde"));
    Run(all, 2.0, [&] { return cli2.chatLog().size() > chat2; });
    CHECK(cli2.chatLog().size() > chat2 && has(cli2.chatLog(), "Garde est occupé"));
    CHECK(!cli.chatLog().empty() ? !has(cli.chatLog(), "Garde est occupé") : true);
    const size_t hostChat = host.chatLog().size();
    hw.dialogEvents.push_back(ev(DialogKind::Busy, 0, 10, 1, "Garde"));   // the host's own character: on the host's screen
    Run(all, 1.0, [&] { return host.chatLog().size() > hostChat; });
    CHECK(host.chatLog().size() > hostChat && has(host.chatLog(), "occupé"));

    // 5. a recruit: the NPC the client was talking with joins the player faction; it is the client's,
    //    even though the other player answered a conversation meanwhile
    hw.dialogEvents.push_back(ev(DialogKind::Open, 2, 11, 3, "Mercenaire"));
    hw.dialogEvents.push_back(ev(DialogKind::Text, 2, 11, 3, "Tu cherches du travail ?", {"Non"}));
    hw.dialogEvents.push_back(ev(DialogKind::Open, 3, 12, 2, "Vagabond"));
    hw.dialogEvents.push_back(ev(DialogKind::Text, 3, 12, 2, "Je peux te suivre ?", {"Oui, rejoins-nous", "Non"}));
    Run(all, 2.0, [&] { return cli2.dialog().id == 2 && cli2.dialog().turn == 1 && cli.dialog().id == 3 && cli.dialog().turn == 1; });
    CHECK(cli2.dialog().open && cli2.dialog().actor == net[3] && cli.dialog().id == 3);
    cli2.AnswerDialog(0);
    cli.AnswerDialog(0);
    Run(all, 2.0, [&] { return hw.answers.size() == 3; });
    CHECK(hw.answers.size() == 3);
    hw.chars[12].squad = true;   // recruited
    Run(all, 3.0, [&] { return std::find_if(cw.controllable.begin(), cw.controllable.end(), [](const Handle& h) { return h.serial == 12; }) != cw.controllable.end(); });
    CHECK(std::find_if(cw.controllable.begin(), cw.controllable.end(), [](const Handle& h) { return h.serial == 12; }) != cw.controllable.end());
    CHECK(std::find_if(cw2.controllable.begin(), cw2.controllable.end(), [](const Handle& h) { return h.serial == 12; }) == cw2.controllable.end());
    CHECK(host.CheckActor(me, net[12]) == Session::ActorVerdict::Ok);

    // 6. walking away: the host ends it in its game; the game's Close shuts the window
    cli.LeaveDialog();
    Run(all, 2.0, [&] { return !hw.ended.empty(); });
    CHECK((hw.ended == std::vector<uint32_t>{3}));
    hw.dialogEvents.push_back(ev(DialogKind::Close, 3, 12, 2, ""));
    Run(all, 2.0, [&] { return !cli.dialog().open; });
    CHECK(!cli.dialog().open);
    // the first conversation closes too
    hw.dialogEvents.push_back(ev(DialogKind::Close, 1, 10, 2, ""));
    Run(all, 1.0);
    CHECK(host.openDialogs() == 1);   // the other player's

    // 7. a player leaving mid-conversation: it ends in the host's game, nothing stays open
    cli2.Leave();
    Run(all, 3.0, [&] { return hw.ended.size() == 2; });
    CHECK((hw.ended == std::vector<uint32_t>{3, 2}));
    CHECK(host.openDialogs() == 0);
    bool cleaned = false;
    for (const auto& l : hostLog) cleaned |= l.find("1 conversation(s)") != std::string::npos;
    CHECK(cleaned);
    CHECK(host.state() == SessionState::Hosting && cli.state() == SessionState::Connected);
}

// Every message has exactly one authority rule; PeekType accepts exactly the messages; host->client
// messages sent by a client are refused; a client's map pings are rate-limited by their rule.
// Workshop: the research, the crafting benches, the machines and the towns' power are the host's;
// a client's requests run on the host (in order, paid once, last writer wins) and everyone follows.
static void TestWorkshop() {
    std::printf("session: research, crafting benches, machines and power follow the host; requests run there once\n");
    FakeWorld hw, cw;
    SetupHost(hw);   // squad at x = 100, 200, 300
    auto item = [](const char* sid, int qty) { ItemState s; s.templateSid = sid; s.quantity = qty; s.section = "main"; return s; };
    hw.research.deskLevel = 2;
    hw.research.finished = {"tech-a"};
    hw.research.queue = {{"tech-b", 5.0f}};
    hw.boxes[800] = {"bench", {150, 0, 0}, {item("iron plates", 4)}};
    FakeWorld::FakeMachine bench;
    bench.s.sid = "bench"; bench.s.pos = {150, 0, 0}; bench.s.flags = kMachCrafting | kMachPowerOn; bench.s.maxOperators = 3;
    bench.s.crafts = {{"sword", "iron", "item-sword", 0.25f}};
    bench.box = 800;
    bench.ops = {FakeWorld::H(1), FakeWorld::H(2)};
    hw.machines["bench"] = bench;
    FakeWorld::FakeMachine far = bench;
    far.s.sid = "far-bench"; far.s.pos = {9000, 0, 9000}; far.box = 0; far.ops.clear();
    hw.machines["far-bench"] = far;
    TownPower tp; tp.sid = "bench"; tp.pos = {150, 0, 0}; tp.values[0] = 12; tp.values[3] = 7; tp.onBattery = false;
    hw.townPower = {tp};
    std::vector<float> st(kStatCount, 1.0f);
    st[3] = 5.5f;   // science
    hw.skills[2] = st;
    AtMenu(cw);
    cw.boxes = hw.boxes;
    cw.boxes[800].items = {item("iron plates", 1)};   // the client's copy of the bench is behind
    cw.machines["bench"].s.sid = "bench";
    cw.machines["bench"].s.pos = bench.s.pos;
    cw.machines["far-bench"].s.sid = "far-bench";
    SessionConfig hc; hc.characterPerPlayer = false; hc.port = ++g_port; SessionConfig cc; cc.port = hc.port; cc.name = "C";
    std::vector<std::string> hostLog;
    Session host(hw, hc, Now, [&](const std::string& l) { hostLog.push_back(l); });
    Session cli(cw, cc, Now, Quiet("cli"));
    std::string err;
    host.Host(&err);
    CHECK(JoinAndWait(host, hw, cli, cw, hc.port, 3));
    host.Assign(FakeWorld::H(2), cli.localId());
    auto all = std::vector<std::pair<Session*, FakeWorld*>>{{&host, &hw}, {&cli, &cw}};
    Run(all, 3.0, [&] { return cw.research == hw.research && cw.machineApplies > 0 && cw.boxes[800].items == hw.boxes[800].items; });
    // 1. the research, the machine (operators, crafts, power) and its inventory: the host's, unopened
    CHECK(cw.research == hw.research);
    CHECK(cw.machines["bench"].s.operatorCount == 2 && cw.machines["bench"].ops.size() == 2 && cw.machines["bench"].s.crafts.size() == 1 &&
          cw.machines["bench"].s.crafts[0].progress == 0.25f);
    CHECK(cw.machines["far-bench"].s.crafts.empty());   // far from every player: not sent
    CHECK(cw.boxes[800].items == hw.boxes[800].items);
    CHECK(!cw.townPower.empty() && cw.townPower[0] == tp);
    // a worker leaves: everyone sees it
    hw.machines["bench"].ops = {FakeWorld::H(1)};
    Run(all, 3.0, [&] { return cw.machines["bench"].ops.size() == 1; });
    CHECK(cw.machines["bench"].s.operatorCount == 1 && cw.machines["bench"].ops.size() == 1 && cw.machines["bench"].ops[0] == FakeWorld::H(1));
    // 2. a client queues a tech without the artifacts: refused, nothing paid, the reason in French
    auto ask = [&](ResearchAction a, const std::string& sid) {
        IWorld::LocalResearchAsk r;
        r.req.action = a;
        r.req.sid = sid;
        cw.researchAsks.push_back(r);
    };
    size_t refusedBefore = host.workshopView().requestsRefused;
    ask(ResearchAction::Queue, "tech-c");
    Run(all, 2.0, [&] { return host.workshopView().requestsRefused > refusedBefore; });
    CHECK(host.workshopView().requestsRefused == refusedBefore + 1 && hw.research.queue.size() == 1);
    // 3. with one artifact, the same tech asked twice at once: queued once, paid once
    hw.artifacts = 1;
    ask(ResearchAction::Queue, "tech-c");
    ask(ResearchAction::Queue, "tech-c");
    Run(all, 3.0, [&] { return cw.research.queue.size() == 2; });
    CHECK(hw.research.queue.size() == 2 && hw.researchPays == 1 && hw.artifacts == 0 && cw.research == hw.research);
    // cancelled, then asked again: back in the queue without paying a second time
    ask(ResearchAction::Cancel, "tech-c");
    Run(all, 3.0, [&] { return cw.research.queue.size() == 1; });
    ask(ResearchAction::Queue, "tech-c");
    Run(all, 3.0, [&] { return cw.research.queue.size() == 2; });
    CHECK(hw.researchPays == 1 && cw.research == hw.research);
    // a local change on the client is undone by the host's research
    cw.research.queue.clear();
    Run(all, 5.0, [&] { return cw.research == hw.research; });
    CHECK(cw.research == hw.research);
    // 4. a blueprint the client's character carries: learnt on the host, used up, known everywhere
    hw.chars[2].items.push_back(item("bp-forge", 1));
    Run(all, 2.0, [&] { return !cw.chars[2].items.empty(); });
    IWorld::LocalResearchAsk bp;
    bp.req.action = ResearchAction::LearnBlueprint;
    bp.req.sid = "bp-forge";
    bp.req.item = item("bp-forge", 1);
    bp.actor = FakeWorld::H(2);
    cw.researchAsks.push_back(bp);
    Run(all, 3.0, [&] { return std::count(cw.research.finished.begin(), cw.research.finished.end(), "tech-of-bp-forge") == 1 && cw.chars[2].items.empty(); });
    CHECK(std::count(hw.research.finished.begin(), hw.research.finished.end(), "tech-of-bp-forge") == 1 && hw.chars[2].items.empty() && cw.chars[2].items.empty());
    // 5. a blueprint "carried" by the host's character: refused by the central check, nothing learnt
    hw.chars[1].items.push_back(item("bp-other", 1));
    const uint64_t refusals = host.actorRefusals();
    bp.actor = FakeWorld::H(1);
    bp.req.sid = "bp-other";
    bp.req.item = item("bp-other", 1);
    cw.researchAsks.push_back(bp);
    Run(all, 2.0);
    CHECK(hw.chars[1].items.size() == 1 && host.actorRefusals() == refusals);   // never sent: the client's own check (ClientMaySend)
    ResearchRequest forged; forged.seq = 99; forged.actorNetId = 0; forged.action = ResearchAction::LearnBlueprint; forged.sid = "bp-other";
    host.ForEachEntity([&](uint32_t id, const Handle& h, uint8_t, bool, bool) { if (h == FakeWorld::H(1)) forged.actorNetId = id; });
    forged.item = item("bp-other", 1);
    Writer fw; Encode(fw, forged);
    CHECK(host.InjectForTest(cli.localId(), fw));
    Run(all, 1.0);
    CHECK(hw.chars[1].items.size() == 1 && host.actorRefusals() == refusals + 1);
    // 6. crafting orders: added by the client, run on the host, seen by everyone; two orders at once
    //    on the same bench: the last one wins everywhere
    auto order = [&](MachineAction a, const std::string& base, int index, bool value) {
        IWorld::LocalMachineAsk m;
        m.req.action = a; m.req.sid = "bench"; m.req.pos = {150, 0, 0}; m.req.baseSid = base; m.req.index = index; m.req.value = value;
        cw.machineAsks.push_back(m);
    };
    order(MachineAction::AddCraft, "helmet", 0, false);
    Run(all, 3.0, [&] { return cw.machines["bench"].s.crafts.size() == 2; });
    CHECK(hw.machines["bench"].s.crafts.size() == 2 && hw.machines["bench"].s.crafts[1].baseSid == "helmet" && cw.machines["bench"].s.crafts.size() == 2);
    order(MachineAction::SetRepeat, "", 0, true);
    order(MachineAction::SetRepeat, "", 0, false);
    Run(all, 3.0, [&] { return host.workshopView().requestsDone >= 7; });
    Run(all, 1.5);
    CHECK(!(hw.machines["bench"].s.flags & kMachRepeat) && !(cw.machines["bench"].s.flags & kMachRepeat));
    order(MachineAction::RemoveCraft, "", 0, false);
    order(MachineAction::RemoveCraft, "", 5, false);   // gone already: refused, nothing else touched
    Run(all, 3.0, [&] { return cw.machines["bench"].s.crafts.size() == 1; });
    CHECK(hw.machines["bench"].s.crafts.size() == 1 && hw.machines["bench"].s.crafts[0].baseSid == "helmet" && cw.machines["bench"].s.crafts.size() == 1);
    // 7. power: switched off by the client, off everywhere; the town panel follows the host
    order(MachineAction::SetPower, "", 0, false);
    hw.townPower[0].values[0] = 0;
    Run(all, 3.0, [&] { return !(cw.machines["bench"].s.flags & kMachPowerOn) && cw.townPower[0].values[0] == 0; });
    CHECK(!(hw.machines["bench"].s.flags & kMachPowerOn) && !(cw.machines["bench"].s.flags & kMachPowerOn) && cw.townPower[0].values[0] == 0);
    // 8. a request from a character far from the machine: refused
    hw.chars[2].pos = {5000, 0, 5000};
    hw.chars[2].dest = hw.chars[2].pos;
    const size_t crafts = hw.machines["bench"].s.crafts.size();
    order(MachineAction::AddCraft, "axe", 0, false);
    Run(all, 2.0);
    CHECK(hw.machines["bench"].s.crafts.size() == crafts);
    // 9. learning by doing: the host's level up is told to the player (its game gains nothing itself)
    const size_t chat0 = cli.chatLog().size();
    hw.skills[2][3] = 6.25f;
    Run(all, 4.0, [&] { for (auto& l : cli.chatLog()) if (l.find("Science 5 -> 6") != std::string::npos) return true; return false; });
    bool told = false;
    for (size_t i = chat0; i < cli.chatLog().size(); ++i) told |= cli.chatLog()[i].find("Science 5 -> 6") != std::string::npos;
    CHECK(told);
    bool logged = false;
    for (auto& l : hostLog) logged |= l.find("research: queue tech-c") != std::string::npos;
    CHECK(logged);
}

static void TestMessageRules() {
    std::printf("authority: every message type has its rule; host-only ones refused from clients; pings rate-limited\n");
    size_t n = 0;
    const MessageRule* rules = MessageRules(n);
    std::set<int> ruled;
    for (size_t i = 0; i < n; ++i) {
        CHECK(ruled.insert(int(rules[i].type)).second);   // one rule per message
        CHECK(MsgName(rules[i].type) != nullptr);          // ... and only for messages
        CHECK(rules[i].name && *rules[i].name);
    }
    int messages = 0;
    for (int t = 0; t < 256; ++t) {   // every Msg value (MsgName has a case for each enumerator)
        const Msg m = Msg(t);
        const bool isMsg = MsgName(m) != nullptr;
        const MessageRule* r = MessageRuleFor(m);
        if (isMsg) {
            ++messages;
            if (!r) std::printf("    no authority rule for message %d (%s)\n", t, MsgName(m));
            CHECK(r != nullptr && r->type == m);
        } else {
            CHECK(r == nullptr);
        }
        const uint8_t b = uint8_t(t);
        Reader rd(&b, 1);
        CHECK(PeekType(rd).has_value() == isMsg);
    }
    CHECK(messages == int(n) && messages >= 59);
    // the ids of the messages merged together (protocol 33): no clash, the right direction
    static_assert(uint8_t(Msg::JoinQueue) == 72 && uint8_t(Msg::BagBind) == 80 && uint8_t(Msg::MapMarkers) == 82 && uint8_t(Msg::MapPing) == 83 &&
                  uint8_t(Msg::Diplomacy) == 85 && uint8_t(Msg::Result) == 90, "message ids of protocol 33");
    for (Msg m : {Msg::Diplomacy, Msg::MapMarkers, Msg::JoinQueue, Msg::BagBind, Msg::Result, Msg::Welcome, Msg::Pong})
        CHECK(MessageRuleFor(m)->role == AuthRole::HostOnly);
    CHECK(MessageRuleFor(Msg::Hello)->role == AuthRole::Handshake);
    CHECK(MessageRuleFor(Msg::MapPing)->role == AuthRole::InGame && MessageRuleFor(Msg::MapPing)->minInterval == Session::kPingInterval);
    for (size_t i = 0; i < n; ++i)
        if (rules[i].type != Msg::MapPing) CHECK(rules[i].minInterval == 0);
    // on a host: a client sending host->client messages is refused; its pings pass at most once per interval
    FakeWorld hw, cw;
    SetupHost(hw);
    AtMenu(cw);
    SessionConfig hc; hc.characterPerPlayer = false; hc.port = ++g_port; SessionConfig cc; cc.port = hc.port; cc.name = "C";
    Session host(hw, hc, Now, Quiet("host"));
    Session cli(cw, cc, Now, Quiet("cli"));
    std::string err;
    host.Host(&err);
    CHECK(JoinAndWait(host, hw, cli, cw, hc.port, 3));
    const uint8_t me = cli.localId();
    const uint32_t r0 = host.actorRefusals();
    const size_t ents0 = host.entityCount();
    { DiplomacyMsg m; m.part = DiploPart::Towns; m.towns = {{"town-1", "holy", "town-1-destroyed"}}; Writer w; Encode(w, m); CHECK(host.InjectForTest(me, w)); }
    { MapMarkersMsg m; m.players = {{me, "C"}}; Writer w; Encode(w, m); CHECK(host.InjectForTest(me, w)); }
    { JoinQueueMsg m; m.position = 1; m.total = 1; m.current = "C"; Writer w; Encode(w, m); CHECK(host.InjectForTest(me, w)); }
    { BagBind m{77, 1, "backpack"}; Writer w; Encode(w, m); CHECK(host.InjectForTest(me, w)); }
    { Result m; m.state = ResultState::Done; Writer w; Encode(w, m); CHECK(host.InjectForTest(me, w)); }
    CHECK(host.actorRefusals() == r0 + 5);
    CHECK(host.entityCount() == ents0);   // the forged BagBind made nothing
    const size_t pings0 = host.pings().size();
    const uint32_t limited0 = host.rateLimited();
    for (int i = 0; i < 5; ++i) {
        MapPingMsg p; p.kind = PingKind::Danger; p.pos = {float(i), 0, 0};
        Writer w; Encode(w, p); CHECK(host.InjectForTest(me, w));
    }
    CHECK(host.pings().size() == pings0 + 1 && host.rateLimited() == limited0 + 4);
    CHECK(host.actorRefusals() == r0 + 5);   // too fast is not a refusal
    Run({{&host, &hw}, {&cli, &cw}}, Session::kPingInterval + 0.2);
    { MapPingMsg p; p.kind = PingKind::Go; p.pos = {9, 0, 9}; Writer w; Encode(w, p); CHECK(host.InjectForTest(me, w)); }
    CHECK(host.pings().size() == pings0 + 2);
    Run({{&host, &hw}, {&cli, &cw}}, 1.0, [&] { return cli.pings().size() >= 2; });
    CHECK(cli.pings().size() == 2);
}

// Players join one at a time; the others wait in a queue and are told their place.
static void TestJoinQueue() {
    std::printf("session: join queue: 3 near-simultaneous joins (mid-save, mid-transfer), one at a time through the editor\n");
    {
        FakeWorld hw;
        SetupHost(hw);
        hw.exportFrames = 30;   // a save that takes a while: the second player arrives during it
        SessionConfig hc; hc.name = "Host"; hc.port = ++g_port;
        Session host(hw, hc, Now, Quiet("host"));
        std::string err;
        CHECK(host.Host(&err));
        const char* names[3] = {"Alpha", "Bravo", "Charlie"};
        std::vector<std::unique_ptr<FakeWorld>> ws;
        std::vector<std::unique_ptr<Session>> cs;
        for (int k = 0; k < 3; ++k) {
            ws.push_back(std::make_unique<FakeWorld>());
            AtMenu(*ws.back());
            ws.back()->editorSupported = true;
            SessionConfig cc; cc.name = names[k]; cc.port = hc.port;
            cs.push_back(std::make_unique<Session>(*ws.back(), cc, Now, Quiet(names[k])));
        }
        auto all = [&] {
            std::vector<std::pair<Session*, FakeWorld*>> v{{&host, &hw}};
            for (size_t k = 0; k < cs.size(); ++k) if (cs[k]) v.push_back({cs[k].get(), ws[k].get()});
            return v;
        };
        CHECK(cs[0]->Join("127.0.0.1", hc.port, &err));
        Run(all(), 2.0, [&] { return host.joinQueue().size() == 1; });
        CHECK(cs[1]->Join("127.0.0.1", hc.port, &err));   // the host is saving its world for Alpha
        Run(all(), 10.0, [&] { return cs[0]->state() == SessionState::Loading; });
        CHECK(cs[0]->state() == SessionState::Loading);
        CHECK(cs[2]->Join("127.0.0.1", hc.port, &err));   // Alpha is loading the world
        Run(all(), 10.0, [&] { return ws[0]->editorOpen && cs[2]->queueStatus(); });
        // Alpha is in the world, making their character; Bravo and Charlie wait their turn
        CHECK(cs[0]->state() == SessionState::Connected && ws[0]->editorOpen);
        Run(all(), 3.0, [&] { auto q = cs[2]->queueStatus(); return q && q->phase == JoinPhase::Editor; });
        for (int k = 1; k < 3; ++k) {
            const JoinQueueMsg* q = cs[k]->queueStatus();
            CHECK(q != nullptr);
            if (!q) continue;
            CHECK(q->position == k + 1 && q->total == 3 && q->current == "Alpha" && q->phase == JoinPhase::Editor);
            CHECK(cs[k]->state() == SessionState::Downloading && ws[k]->imports == 0);
        }
        auto list = host.joinQueue();
        CHECK(list.size() == 3);
        if (list.size() == 3) {
            CHECK(list[0].name == "Alpha" && !list[0].waiting && list[0].phase == JoinPhase::Editor);
            CHECK(list[1].name == "Bravo" && list[1].waiting && list[2].name == "Charlie" && list[2].waiting);
        }
        CHECK(hw.holding);                              // the game waits for Alpha's editor
        CHECK(hw.named.size() == 1);                    // only Alpha's character so far
        // nobody is dropped while waiting, however long the turn ahead lasts
        Run(all(), 3.0);
        CHECK(cs[1]->state() == SessionState::Downloading && cs[2]->state() == SessionState::Downloading);
        // Alpha closes the editor: Bravo's turn
        ws[0]->editorOpen = false;
        Run(all(), 10.0, [&] { return ws[1]->editorOpen; });
        CHECK(cs[1]->state() == SessionState::Connected && ws[1]->editorOpen);
        Run(all(), 3.0, [&] { auto q = cs[2]->queueStatus(); return q && q->current == "Bravo" && q->phase == JoinPhase::Editor; });
        {
            const JoinQueueMsg* q = cs[2]->queueStatus();
            CHECK(q && q->position == 2 && q->total == 2 && q->current == "Bravo");
        }
        // Bravo's save had Alpha's character in it
        CHECK(hw.named.count("Alpha") && ws[1]->chars.count(hw.named["Alpha"]) == 1);
        ws[1]->editorOpen = false;
        Run(all(), 10.0, [&] { return ws[2]->editorOpen; });
        CHECK(cs[2]->state() == SessionState::Connected && !cs[2]->queueStatus());
        ws[2]->editorOpen = false;
        Run(all(), 5.0, [&] { return !hw.holding && host.joinQueue().empty(); });
        CHECK(!hw.holding && host.joinQueue().empty() && host.joiningPlayers() == 0);
        CHECK(hw.named.size() == 3);
        // the ones who came earlier get a stand-in for the later ones' characters
        Run(all(), 10.0, [&] {
            for (auto& w : ws) for (auto& [n, serial] : hw.named) if (!w->chars.count(serial)) return false;
            return true;
        });
        for (int k = 0; k < 3; ++k) {
            CHECK(cs[k]->state() == SessionState::Connected);
            CHECK(ws[k]->imports == 1 && ws[k]->editorOpened == 1);
            for (auto& [n, serial] : hw.named) CHECK(ws[k]->chars.count(serial) == 1);   // everyone's character, everywhere
        }
        CHECK(ws[2]->fp == hw.fp);   // the last save is the host's current world
    }
    std::printf("session: join queue: the player in the editor crashes, a queued player leaves: the queue moves on, renumbered\n");
    {
        FakeWorld hw;
        SetupHost(hw);
        SessionConfig hc; hc.name = "Host"; hc.port = ++g_port;
        Session host(hw, hc, Now, Quiet("host"));
        std::string err;
        CHECK(host.Host(&err));
        const char* names[4] = {"Alpha", "Bravo", "Charlie", "Delta"};
        std::vector<std::unique_ptr<FakeWorld>> ws;
        std::vector<std::unique_ptr<Session>> cs;
        for (int k = 0; k < 4; ++k) {
            ws.push_back(std::make_unique<FakeWorld>());
            AtMenu(*ws.back());
            ws.back()->editorSupported = true;
            SessionConfig cc; cc.name = names[k]; cc.port = hc.port;
            cs.push_back(std::make_unique<Session>(*ws.back(), cc, Now, Quiet(names[k])));
        }
        auto all = [&] {
            std::vector<std::pair<Session*, FakeWorld*>> v{{&host, &hw}};
            for (size_t k = 0; k < cs.size(); ++k) if (cs[k]) v.push_back({cs[k].get(), ws[k].get()});
            return v;
        };
        CHECK(cs[0]->Join("127.0.0.1", hc.port, &err));
        Run(all(), 2.0, [&] { return host.joinQueue().size() == 1; });
        for (int k = 1; k < 4; ++k) CHECK(cs[k]->Join("127.0.0.1", hc.port, &err));
        Run(all(), 10.0, [&] { auto q = cs[3]->queueStatus(); return ws[0]->editorOpen && q && q->phase == JoinPhase::Editor; });
        CHECK(ws[0]->editorOpen && host.joinQueue().size() == 4);
        // Alpha's game dies with the editor open: Bravo's turn, with a save made after that
        cs[0].reset();
        Run(all(), 10.0, [&] { return ws[1]->editorOpen; });
        CHECK(cs[1]->state() == SessionState::Connected && ws[1]->editorOpen);
        CHECK(host.players().size() == 3);
        Run(all(), 3.0, [&] { auto q = cs[3]->queueStatus(); return q && q->position == 3 && q->current == "Bravo" && q->phase == JoinPhase::Editor; });
        {
            const JoinQueueMsg* q = cs[2]->queueStatus();
            CHECK(q && q->position == 2 && q->total == 3 && q->current == "Bravo");
            q = cs[3]->queueStatus();
            CHECK(q && q->position == 3 && q->total == 3);
        }
        // Charlie gives up while queued: Delta moves up
        cs[2]->Leave();
        Run(all(), 3.0, [&] { auto q = cs[3]->queueStatus(); return q && q->position == 2; });
        {
            const JoinQueueMsg* q = cs[3]->queueStatus();
            CHECK(q && q->position == 2 && q->total == 2 && q->current == "Bravo");
            auto list = host.joinQueue();
            CHECK(list.size() == 2 && list[0].name == "Bravo" && list[1].name == "Delta");
        }
        ws[1]->editorOpen = false;
        Run(all(), 10.0, [&] { return ws[3]->editorOpen; });
        CHECK(cs[3]->state() == SessionState::Connected);
        ws[3]->editorOpen = false;
        Run(all(), 5.0, [&] { return !hw.holding && host.joinQueue().empty(); });
        CHECK(!hw.holding && host.joinQueue().empty());
        CHECK(cs[1]->state() == SessionState::Connected && cs[3]->state() == SessionState::Connected);
        CHECK(ws[3]->fp == hw.fp && ws[3]->chars.count(hw.named["Bravo"]) == 1);
    }
    std::printf("session: several players in the character editor at once: the game waits for the last one\n");
    {
        FakeWorld hw, aw, bw;
        SetupHost(hw);
        AtMenu(aw); AtMenu(bw);
        SessionConfig hc; hc.name = "Host"; hc.port = ++g_port;
        SessionConfig ac; ac.name = "Alpha"; ac.port = hc.port;
        SessionConfig bc; bc.name = "Bravo"; bc.port = hc.port;
        Session host(hw, hc, Now, Quiet("host"));
        Session a(aw, ac, Now, Quiet("a")), b(bw, bc, Now, Quiet("b"));
        std::string err;
        CHECK(host.Host(&err));
        aw.editorSupported = bw.editorSupported = true;
        CHECK(a.Join("127.0.0.1", hc.port, &err));
        Run({{&host, &hw}, {&a, &aw}, {&b, &bw}}, 10.0, [&] { return aw.editorOpen; });
        aw.editorOpen = false;   // Alpha's join turn ends
        CHECK(b.Join("127.0.0.1", hc.port, &err));
        Run({{&host, &hw}, {&a, &aw}, {&b, &bw}}, 10.0, [&] { return bw.editorOpen; });
        bw.editorOpen = false;
        Run({{&host, &hw}, {&a, &aw}, {&b, &bw}}, 3.0, [&] { return !hw.holding; });
        CHECK(a.state() == SessionState::Connected && b.state() == SessionState::Connected && !hw.holding);
        // both reopen their editor (Multijoueur window, "edit my character")
        CHECK(a.EditOwnCharacter() && b.EditOwnCharacter());
        Run({{&host, &hw}, {&a, &aw}, {&b, &bw}}, 3.0, [&] { return hw.holding && host.joinQueue().empty(); });
        CHECK(hw.holding && host.joinQueue().empty());
        aw.editorOpen = false;   // one closes: still paused for the other
        Run({{&host, &hw}, {&a, &aw}, {&b, &bw}}, 1.0);
        CHECK(hw.holding);
        bw.editorOpen = false;
        Run({{&host, &hw}, {&a, &aw}, {&b, &bw}}, 3.0, [&] { return !hw.holding; });
        CHECK(!hw.holding);
    }
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

// The client's clock follows the host's: a client a little behind (or ahead) runs a few percent
// faster (or slower) until both read the same hours; far apart (a stall, a zone loading) it is set
// to the host's hours at once; in step, the host's speed is left as it is.
static void TestClockSync() {
    struct Run { double behind; float speed; bool snap; };
    for (Run run : {Run{0.003, 2.0f, false}, Run{-0.0035, 3.0f, false}, Run{0.0, 1.0f, false}, Run{0.0, 3.0f, false},
                    Run{0.162, 3.0f, true}, Run{-0.13, 3.0f, true}, Run{0.3, 1.0f, true}, Run{-2.0, 2.0f, true}}) {
        ClockSync cs;
        const double rate = 0.0091, delay = 0.06, dt = 1.0 / 30;
        double host = 40.0, mine = 40.0 - run.behind, nextSend = 0, closeAt = -1;
        std::deque<std::pair<double, TimeState>> inFlight;
        bool everTrimmed = false;
        float clientSpeed = run.speed;
        for (double t = 0; t < 90.0; t += dt) {
            host += rate * run.speed * dt;
            mine += rate * clientSpeed * dt;
            if (t >= nextSend) { nextSend = t + 0.5; inFlight.push_back({t + delay, TimeState{run.speed, false, host}}); }
            while (!inFlight.empty() && inFlight.front().first <= t) { cs.OnHost(inFlight.front().second, t, delay); inFlight.pop_front(); }
            if (!cs.have()) continue;   // nothing from the host yet: the local speed stays
            const TimeState want = cs.Target(t, mine);
            if (cs.Snap() >= 0.0) mine = cs.Snap();
            CHECK(!want.paused);
            clientSpeed = want.speed;
            everTrimmed |= cs.trim() != 0.0f;
            CHECK(std::fabs(clientSpeed - run.speed) <= run.speed * 0.101f);
            if (closeAt < 0 && std::fabs(host - mine) < 0.002) closeAt = t;
        }
        if (std::fabs(host - mine) >= 0.001 || cs.trim() != 0.0f)
            std::printf("    clock run %.3f h x%.0f: still %.4f h apart, trim %.2f, rate %.5f\n", run.behind, run.speed, host - mine, cs.trim(), cs.rate());
        CHECK(std::fabs(host - mine) < 0.001);
        CHECK(cs.trim() == 0.0f);
        CHECK((cs.snaps() > 0) == run.snap);
        CHECK(cs.snaps() <= 2);   // one, and maybe a second once the rate is known better
        if (run.behind != 0.0 && !run.snap) CHECK(everTrimmed);
        // far apart: close within seconds (the rate is measured over the first 2 s), not minutes
        if (run.snap) CHECK(closeAt >= 0 && closeAt < 6.0);
        CHECK(std::fabs(cs.rate() - rate) < rate * 0.05);
        // paused: the host's pause and speed, never a trim; its hours are exact: a client off by
        // more than kPausedSnap is set to them
        cs.OnHost(TimeState{run.speed, true, host}, 100.0, delay);
        const TimeState p = cs.Target(100.1, mine - 0.05);
        CHECK(p.paused && p.speed == run.speed && cs.trim() == 0.0f);
        CHECK(cs.Snap() == host);
        cs.OnHost(TimeState{run.speed, true, host}, 102.0, delay);
        cs.Target(102.1, host - 0.001);   // close enough: left alone
        CHECK(cs.Snap() < 0.0);
    }
    // another world's hours (hours apart): set as well, once the rate is known; never twice in a row
    ClockSync cs;
    cs.OnHost(TimeState{1.0f, false, 10.0}, 0.0, 0.0);
    cs.Target(0.1, 5.0);
    CHECK(cs.Snap() < 0.0);   // the rate is not known yet: the target cannot be extrapolated
    cs.OnHost(TimeState{1.0f, false, 10.03}, 3.0, 0.0);
    CHECK(cs.Target(3.0, 5.0).speed == 1.0f && cs.trim() == 0.0f && cs.Snap() > 10.0);
    CHECK(cs.Target(3.05, 10.03).speed == 1.0f && cs.Snap() < 0.0);   // the same message: no second snap
    std::puts("clock sync ok");
}

// ---------------------------------------------------------------- 0.3.1: per-player streaming
static void TestStreamingTools() {
    std::printf("streaming: retry backoff, log limits, stand-in loss delays, automatic zone resync decisions\n");
    Backoff<std::string> b(30.0, 600.0);
    CHECK(b.Allowed("Cat", 0.0));
    CHECK(b.Fail("Cat", 0.0) == 30.0);
    CHECK(!b.Allowed("Cat", 29.0) && b.Allowed("Cat", 30.0));
    CHECK(b.Fail("Cat", 30.0) == 60.0);
    CHECK(b.Fail("Cat", 90.0) == 120.0);
    for (int i = 0; i < 10; ++i) b.Fail("Cat", 1000.0);
    CHECK(b.next("Cat") == 1600.0);          // never more than the ceiling
    CHECK(b.Allowed("Ruka", 0.0));           // per kind
    b.Succeed("Cat");
    CHECK(b.Allowed("Cat", 1000.0) && b.failures("Cat") == 0);

    LogLimiter lim(3, 10.0);
    int written = 0;
    for (int i = 0; i < 10; ++i) written += lim.Allow(1.0) ? 1 : 0;
    CHECK(written == 3 && lim.held() == 7);
    uint32_t held = 0;
    CHECK(lim.Allow(11.5, &held) && held == 7);   // the next window says what was held back
    CHECK(lim.held() == 0);

    CHECK(StandInLossDelay(0) == 0.0 && StandInLossDelay(1) == 2.0 && StandInLossDelay(2) == 10.0);
    CHECK(StandInLossDelay(3) == 30.0 && StandInLossDelay(4) == 60.0 && StandInLossDelay(9) == 120.0);

    ZoneHealth z;
    std::string why;
    double t = 0;
    CHECK(z.Report(2, 41, 0, t += 5, why) == ZoneHealth::Action::None);
    CHECK(z.Report(2, 41, 0, t += 5, why) == ZoneHealth::Action::None);
    CHECK(z.Report(2, 41, 0, t += 5, why) == ZoneHealth::Action::Resync);   // 15 s of missing NPCs
    CHECK(why.find("41 NPCs missing") != std::string::npos);
    CHECK(z.Report(3, 0, 0, t, why) == ZoneHealth::Action::None);           // another player: untouched
    for (int i = 0; i < 6; ++i) CHECK(z.Report(2, 41, 0, t += 5, why) == ZoneHealth::Action::None);   // not again within a minute
    ZoneHealth::Action a = ZoneHealth::Action::None;
    for (int i = 0; i < 6 && a == ZoneHealth::Action::None; ++i) a = z.Report(2, 41, 0, t += 5, why);
    CHECK(a == ZoneHealth::Action::Resync && z.tries(2) == 2);
    CHECK(z.Report(2, 1, 0, t += 5, why) == ZoneHealth::Action::None && z.tries(2) == 0);   // a good report: all forgotten
    t += 100;   // (a minute after the last zone resync)
    CHECK(z.Report(2, 0, 4, t += 5, why) == ZoneHealth::Action::None);
    CHECK(z.Report(2, 0, 4, t += 5, why) == ZoneHealth::Action::None);
    CHECK(z.Report(2, 0, 4, t += 5, why) == ZoneHealth::Action::Resync && why.find("4 characters off") != std::string::npos);
    // tried maxTries times without a good report: said once, then tried again only every 5 min
    ZoneHealthConfig quick;
    quick.minGap = 0;
    ZoneHealth q(quick);
    int resyncs = 0, still = 0;
    for (int i = 0; i < 100; ++i) {
        const ZoneHealth::Action r = q.Report(4, 20, 0, 1000.0 + i * 5.0, why);
        resyncs += r == ZoneHealth::Action::Resync;
        still += r == ZoneHealth::Action::StillDiffers;
    }
    CHECK(still == 1);
    CHECK(resyncs == 3 + 1);   // three quick ones, then one more 5 min after the last (in 500 s)
}

// The host sends each client the characters around that client's own characters, never the crowd
// around the host or another player; a client recreates a few per frame, near its characters only;
// a client that keeps missing NPCs gets its zone again by itself.
static void TestPerPlayerStreaming() {
    std::printf("streaming: each client is sent the characters around its own characters (not the host's crowd)\n");
    FakeWorld hw, aw, bw;
    SetupHost(hw);
    AtMenu(aw);
    AtMenu(bw);
    SessionConfig hc; hc.name = "Host"; hc.port = ++g_port;   // a character per player: the default
    Session host(hw, hc, Now, Quiet("host"));
    std::string err;
    CHECK(host.Host(&err));
    SessionConfig ac; ac.name = "First"; ac.port = hc.port;
    SessionConfig bc; bc.name = "Second"; bc.port = hc.port;
    Session cliA(aw, ac, Now, Quiet("cliA"));
    std::vector<std::string> bLog;
    Session cliB(bw, bc, Now, [&](const std::string& s) { bLog.push_back(s); });
    std::vector<std::pair<Session*, FakeWorld*>> all = {{&host, &hw}, {&cliA, &aw}, {&cliB, &bw}};
    aw.editorSupported = bw.editorSupported = true;   // each player confirms their character at once
    auto confirm = [&] { aw.editorOpen = bw.editorOpen = false; };
    CHECK(cliA.Join("127.0.0.1", hc.port, &err));
    Run(all, 10.0, [&] { confirm(); return cliA.state() == SessionState::Connected && aw.controllable.size() == 1; });
    CHECK(cliB.Join("127.0.0.1", hc.port, &err));
    Run(all, 15.0, [&] { confirm(); return cliB.state() == SessionState::Connected && bw.controllable.size() == 1 && hw.named.count("Second"); });
    CHECK(cliB.state() == SessionState::Connected);
    const uint8_t idA = cliA.localId(), idB = cliB.localId();
    const uint32_t first = hw.named["First"], second = hw.named["Second"];
    // the second player walks 2 km away; one of the host's characters is 2 km the other way
    hw.chars[second].pos = hw.chars[second].dest = {20000, 0, 0};
    hw.chars[3].pos = hw.chars[3].dest = {-20000, 0, 0};
    Run(all, 2.0);
    // NPCs appear after the joins: next to the first player and the host, next to the second player,
    // next to the host's far character only
    auto npc = [&](uint32_t s, Vec3 p) { FakeChar c; c.squad = false; c.pos = c.dest = p; c.vit.blood = 50; hw.chars[s] = c; };
    npc(70, {150, 0, 30});
    npc(71, {20100, 0, 0});
    npc(72, {-20050, 0, 0});
    Run(all, 8.0, [&] { return aw.chars.count(70) && bw.chars.count(71); });
    CHECK(host.streamsTo(FakeWorld::H(70), idA) && !host.streamsTo(FakeWorld::H(70), idB));
    CHECK(host.streamsTo(FakeWorld::H(71), idB) && !host.streamsTo(FakeWorld::H(71), idA));
    CHECK(!host.streamsTo(FakeWorld::H(72), idA) && !host.streamsTo(FakeWorld::H(72), idB));   // the host's crowd: nobody else's
    CHECK(host.streamsTo(FakeWorld::H(second), idA) && host.streamsTo(FakeWorld::H(3), idB));   // every player's characters: everyone
    CHECK(aw.chars.count(70) == 1 && aw.chars.count(71) == 0 && aw.chars.count(72) == 0);
    CHECK(bw.chars.count(71) == 1 && bw.chars.count(70) == 0 && bw.chars.count(72) == 0);
    CHECK(cliA.missingNpcs() == 0 && cliB.missingNpcs() == 0);   // what is not sent is not "missing"
    CHECK(host.streamedTo(idA) < host.npcCount() + 6 && host.streamedTo(idB) < host.npcCount() + 6);
    // damage numbers: made for characters near a client player's own characters, not the host's far crowd
    auto interest = [&](uint32_t s) {
        for (const Handle& h : hw.remoteInterest) if (h.serial == s) return true;
        return false;
    };
    CHECK(interest(70) && interest(71) && interest(first) && !interest(72));
    // the second player comes back: the NPCs there are sent to them at once, with their state
    hw.chars[second].pos = hw.chars[second].dest = {60, 0, -20};
    Run(all, 8.0, [&] { return bw.chars.count(70) && Dist(bw.chars[70].pos, hw.chars[70].pos) < 1e-3f && bw.chars[70].vit.blood == 50; });
    CHECK(host.streamsTo(FakeWorld::H(70), idB));
    CHECK(bw.chars.count(70) == 1 && bw.chars[70].vit.blood == 50);

    std::printf("streaming: a crowd appearing at once is created a few characters per frame, nearest first\n");
    const int peakBefore = cliA.spawnPeak();
    (void)peakBefore;
    for (uint32_t i = 0; i < 12; ++i) npc(100 + i, {200.0f + float(i) * 10, 0, 50});
    Run(all, 10.0, [&] {
        for (uint32_t i = 0; i < 12; ++i) if (!aw.chars.count(100 + i)) return false;
        return true;
    });
    for (uint32_t i = 0; i < 12; ++i) CHECK(aw.chars.count(100 + i) == 1);
    CHECK(cliA.spawnPeak() >= 1 && cliA.spawnPeak() <= 2);   // never more than spawnsPerTick in one frame

    std::printf("streaming: a stand-in our game keeps dropping comes back after a growing delay\n");
    int remade = 0;
    Run(all, 9.0, [&] {
        if (bw.chars.count(70)) { bw.chars.erase(70); ++remade; }   // unloaded at once, every time
        return false;
    });
    // 0 s, +2 s, +10 s: at most three in 9 s whatever the spawn grace; the 0.3.0 loop made one every ~3 s
    if (remade > 3) std::printf("    stand-in made again %d times in 9 s\n", remade);
    CHECK(remade <= 3);
    bool told = false;
    for (const auto& l : bLog) told = told || l.find("stand-in(s) vanished here lately") != std::string::npos;
    CHECK(told);

    std::printf("streaming: a client that keeps missing NPCs gets its zone sent again, by itself\n");
    aw.refuseSpawn = true;   // its game will not make them (as the factory refusing a kind)
    for (uint32_t i = 0; i < 6; ++i) npc(130 + i, {120, 0, float(i) * 10});
    Run(all, 30.0, [&] { return host.zoneResyncs() >= 1; });
    CHECK(host.zoneResyncs() >= 1);
    Run(all, 1.0, [&] { return cliA.resyncedHere() > 0; });
    CHECK(cliA.resyncedHere() > 0);
    CHECK(cliB.resyncedHere() == 0);   // nobody else is touched
    CHECK(aw.resyncs > 0);             // the characters it has are put on the host's spot
    aw.refuseSpawn = false;
    Run(all, 6.0, [&] {
        for (uint32_t i = 0; i < 6; ++i) if (!aw.chars.count(130 + i)) return false;
        return true;
    });
    for (uint32_t i = 0; i < 6; ++i) CHECK(aw.chars.count(130 + i) == 1);
}

// A 0.3.0 host (or stream_radius = 0) sends everything to everyone: the client still recreates only
// what is near its own characters, and does not count the rest as missing.
static void TestStandInArea() {
    std::printf("streaming: a client recreates host characters only near its own characters\n");
    FakeWorld hw, cw;
    SetupHost(hw);
    AtMenu(cw);
    SessionConfig hc; hc.name = "Host"; hc.port = ++g_port; hc.streamRadius = 0;   // everything to everyone
    SessionConfig cc; cc.name = "Client"; cc.port = hc.port;
    Session host(hw, hc, Now, Quiet("host"));
    Session cli(cw, cc, Now, Quiet("cli"));
    std::string err;
    CHECK(host.Host(&err));
    CHECK(cli.Join("127.0.0.1", hc.port, &err));
    Run({{&host, &hw}, {&cli, &cw}}, 10.0, [&] { return cli.state() == SessionState::Connected && cw.controllable.size() == 1; });
    FakeChar near; near.squad = false; near.pos = near.dest = {150, 0, 30};
    FakeChar far = near; far.pos = far.dest = {30000, 0, 0};
    hw.chars[80] = near;
    hw.chars[81] = far;
    Run({{&host, &hw}, {&cli, &cw}}, 6.0, [&] { return cw.chars.count(80) != 0; });
    Run({{&host, &hw}, {&cli, &cw}}, 3.0);
    CHECK(cw.chars.count(80) == 1);
    CHECK(cw.chars.count(81) == 0);    // its land is not loaded by our game: never made there
    CHECK(cli.missingNpcs() == 0);     // nor counted as missing
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // every line out before a crash
    if (std::getenv("KC_ONLY_STREAMING")) {   // the 0.3.1 streaming tests alone
        TestStreamingTools();
        TestPerPlayerStreaming();
        TestStandInArea();
        std::printf("%d checks, %d failed\n", g_checks, g_failed);
        return g_failed ? 1 : 0;
    }
    TestWire();
    TestClockSync();
    TestFuzz();
    TestJoinFromMenu();
    TestSessionReplication();
    TestDivergenceIsCorrected();
    TestRejections();
    TestWorldAuthority();
    TestOwnCharacter();
    TestWorldIdentity();
    TestNoDuplicatePlayers();
    TestSpawnReplication();
    TestStreamingTools();      // 0.3.1: per-player streaming
    TestPerPlayerStreaming();
    TestStandInArea();
    TestInventories();
    TestInventorySwaps();
    TestGroundDrops();
    TestTrade();
    TestTravellingTrade();
    TestCrashRejoin();
    TestBuildings();
    TestRanged();   // lot C
    TestCaptives();
    TestFactions();
    TestDiplomacy();
    TestDoors();   // lot A
    TestFloorsAndStall();   // fix G6
    TestJobs();   // fix G5
    TestSquadWire();
    TestSquadWindow();   // squad window and AI settings
    TestSquadFallback();
    TestDeadSquad();
    TestAdmin();
    TestTaskTargets();   // actor safety
    TestCallScopeRepair();   // crash at kenshi_x64+0x883B78
    TestActorSafety();
    TestGuiToDisplay();
    TestDialogue();
    TestMap();
    TestManyPlayers();
    TestJoinQueue();
    TestWorkshop();   // research, crafting benches, machines, power
    TestMessageRules();   // the authority table covers every message
    std::printf("\n%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
