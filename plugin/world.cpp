#include "world.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

#include "hooks.h"

namespace kcp {

namespace {
float Dist(const kc::Vec3& a, const kc::Vec3& b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
constexpr double kClockTolerance = 0.02;  // in-game hours (~1 game minute) before the clock is corrected
// Save slot names (<= 15 chars: passed to the game as inline VS2010 strings).
constexpr const char* kExportSlot = "KenshiCoopHost";
constexpr const char* kImportSlot = "KenshiCoopJoin";

namespace fs = std::filesystem;

// Kenshi stores paths as narrow strings in the system code page.
fs::path FromGamePath(const std::string& s) {
    const int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), int(s.size()), w.data(), n);
    fs::path p(w);
    return p.is_absolute() ? p : fs::path(GameDir()) / p;
}
std::string ToUtf8(const fs::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}
fs::path FromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
} // namespace

KenshiWorld::KenshiWorld(const Config& cfg) : cfg_(cfg) {}

void KenshiWorld::BeginFrame(bool live) {
    live_ = live;
    squad_.clear();
    resolved_.clear();
    if (!live) {   // menus and loading screens: nothing in the world may be touched
        wasReady_ = false;
        return;
    }
    kenshi::PlayerCharacters(scratch_);
    for (kenshi::Character* c : scratch_) {
        kc::Handle h;
        if (kenshi::GetHandle(c, h) && h.valid()) squad_[h] = c;
    }
    // A (re)loaded world: a new generation, which tells a joining client its load completed.
    void* player = kenshi::Player();
    const bool ready = player && !squad_.empty();
    if (ready && (!wasReady_ || player != lastPlayer_)) ++generation_;
    wasReady_ = ready;
    lastPlayer_ = player;
}

void KenshiWorld::EndFrame() {
    // Clients run the host's clock: speed and pause are imposed every frame, so local keys
    // (space, F2/F3/F4) have no lasting effect.
    if (active_ && client_ && haveHostTime_) {
        if (std::fabs(kenshi::GetFrameSpeed() - hostTime_.speed) > 1e-3f) kenshi::CallSetFrameSpeed(hostTime_.speed);
        if (kenshi::GetPaused() != hostTime_.paused) kenshi::CallTogglePause(hostTime_.paused);
    }
    // While players join, the host's world stays frozen even if someone presses unpause.
    if (holding_ && !kenshi::GetPaused()) {
        kenshi::CallTogglePause(true);
        pausedByHold_ = true;
    }
    auto v = std::make_shared<HookView>();
    v->active = active_;
    v->client = client_;
    if (active_) {
        v->controllable = controllable_;
        for (auto& [h, c] : squad_) if (!controllable_.count(h)) v->squadForeign.insert(c);
    }
    clientActive_.store(active_ && client_, std::memory_order_relaxed);
    view_.store(std::shared_ptr<const HookView>(std::move(v)), std::memory_order_release);
}

bool KenshiWorld::Ready() { return live_ && kenshi::Player() != nullptr && !squad_.empty(); }

uint64_t KenshiWorld::Fingerprint() {
    // The set of squad handles identifies "the same save" on every machine.
    std::vector<kc::Handle> hs;
    PlayerCharacters(hs);
    std::sort(hs.begin(), hs.end(), [](const kc::Handle& a, const kc::Handle& b) {
        return std::tie(a.type, a.container, a.containerSerial, a.index, a.serial) <
               std::tie(b.type, b.container, b.containerSerial, b.index, b.serial);
    });
    uint64_t f = 0xcbf29ce484222325ull;
    for (const auto& h : hs) {
        const uint32_t v[5] = {h.type, h.container, h.containerSerial, h.index, h.serial};
        f = kc::Fnv1a64(v, sizeof(v), f);
    }
    return f;
}

uint64_t KenshiWorld::ModsHash() { return HashFile(GameDir() + L"data\\mods.cfg"); }

void KenshiWorld::PlayerCharacters(std::vector<kc::Handle>& out) {
    for (auto& [h, c] : squad_) out.push_back(h);
}

void KenshiWorld::NearbyCharacters(const std::vector<kc::Vec3>& centers, float radius, std::vector<kc::Handle>& out) {
    kenshi::ActiveCharacters(scratch_);
    for (kenshi::Character* c : scratch_) {
        kc::Vec3 p;
        if (!kenshi::GetPosition(c, p)) continue;
        bool inRange = false;
        for (const auto& ctr : centers) if (Dist(ctr, p) <= radius) { inRange = true; break; }
        if (!inRange) continue;
        kc::Handle h;
        if (kenshi::GetHandle(c, h) && h.valid()) {
            out.push_back(h);
            resolved_[h] = c;
        }
    }
}

kenshi::Character* KenshiWorld::FindSquad(const kc::Handle& h) const {
    auto it = squad_.find(h);
    return it == squad_.end() ? nullptr : it->second;
}

kenshi::Character* KenshiWorld::Find(const kc::Handle& h) {
    if (kenshi::Character* c = FindSquad(h)) return c;
    auto it = resolved_.find(h);
    if (it != resolved_.end()) return it->second;
    kenshi::Character* c = kenshi::Resolve(h);   // null when the character is not loaded here
    resolved_[h] = c;
    return c;
}

bool KenshiWorld::Read(const kc::Handle& h, kc::EntityState& out) {
    kenshi::Character* c = Find(h);
    if (!c || !kenshi::GetPosition(c, out.pos)) return false;
    if (!kenshi::GetRotation(c, out.rot)) out.rot = {};
    bool moving = false;
    float speed = 0;
    if (!kenshi::GetMovement(c, out.dest, moving, speed)) { out.dest = out.pos; moving = false; }
    out.flags = 0;
    if (moving) out.flags |= kc::kFlagMoving;
    if (kenshi::IsDown(c)) out.flags |= kc::kFlagDown;
    if (kenshi::IsDead(c)) out.flags |= kc::kFlagDead;
    return true;
}

bool KenshiWorld::ReadVitals(const kc::Handle& h, kc::EntityVitals& out) {
    kenshi::Character* c = Find(h);
    return c && kenshi::ReadVitals(c, out);
}

void KenshiWorld::Apply(const kc::Handle& h, const kc::EntityState& target, const kc::EntityState& latest) {
    kenshi::Character* c = Find(h);
    if (!c) return;
    // Bodies on the ground are moved by ragdoll physics, not by their legs.
    if ((target.flags | latest.flags) & (kc::kFlagDown | kc::kFlagDead)) { lastDest_.erase(h); return; }
    kc::Vec3 local;
    if (!kenshi::GetPosition(c, local)) return;

    // Far off (late join, lag spike, teleport on the host): snap straight to the host state.
    if (Dist(local, target.pos) > cfg_.snapDistance) {
        HostCallScope scope;
        kenshi::Teleport(c, target.pos, target.rot);
        lastDest_.erase(h);
        return;
    }
    // Otherwise let the game's own locomotion walk the character, so animations stay natural:
    // follow the host's destination while it moves, and settle on its exact position when idle.
    const kc::Vec3 want = (latest.flags & kc::kFlagMoving) ? latest.dest : target.pos;
    auto it = lastDest_.find(h);
    if (it == lastDest_.end() || Dist(it->second, want) > cfg_.destEpsilon) {
        HostCallScope scope;
        if (kenshi::SetDestination(c, want)) lastDest_[h] = want;
    }
}

void KenshiWorld::ApplyVitals(const kc::Handle& h, const kc::EntityVitals& v) {
    kenshi::Character* c = Find(h);
    if (!c) return;
    kenshi::WriteVitals(c, v);
    // Death and knockout are transitions, not values: replay them exactly when the host has them.
    HostCallScope scope;
    if ((v.flags & kc::kVitDead) && !kenshi::IsDead(c)) {
        kenshi::CallDeclareDead(c);
    } else if ((v.flags & kc::kVitUnconscious) && !kenshi::IsUnconscious(c) && !kenshi::IsDead(c)) {
        kenshi::CallKnockout(c);
        kenshi::WriteVitals(c, v);   // knockout() picks its own timer; the host's wins
    }
}

bool KenshiWorld::Order(const kc::Handle& h, const kc::Command& cmd) {
    kenshi::Character* c = FindSquad(h);
    if (!c) return false;
    HostCallScope scope;   // lets the call through our own order-blocking hooks
    switch (cmd.kind) {
    case kc::CommandKind::MoveTo: return CallPlayerMoveOrder(c, cmd.pos);
    case kc::CommandKind::Stop: return kenshi::Halt(c);
    }
    return false;
}

void KenshiWorld::QueueLocalOrder(const kc::Handle& h, const kc::Command& c) {
    std::lock_guard<std::mutex> lk(ordersMutex_);
    if (orders_.size() < 256) orders_.emplace_back(h, c);
}

void KenshiWorld::TakeLocalOrders(std::vector<std::pair<kc::Handle, kc::Command>>& out) {
    std::lock_guard<std::mutex> lk(ordersMutex_);
    out.swap(orders_);
    orders_.clear();
}

kc::TimeState KenshiWorld::GetTime() {
    kc::TimeState t{kenshi::GetFrameSpeed(), kenshi::GetPaused(), 0.0};
    kenshi::GetGameHours(t.gameHours);
    return t;
}

void KenshiWorld::SetTime(const kc::TimeState& t) {
    hostTime_ = t;
    haveHostTime_ = true;
    double local = 0;
    if (t.gameHours > 0 && kenshi::GetGameHours(local) && std::fabs(local - t.gameHours) > kClockTolerance)
        kenshi::SetGameHours(t.gameHours);
}

void KenshiWorld::HoldForJoin(bool hold) {
    if (hold == holding_) return;
    holding_ = hold;
    if (hold) {
        pausedByHold_ = !kenshi::GetPaused();
        if (pausedByHold_) kenshi::CallTogglePause(true);
        Toast("A player is joining: the game is paused until they are in.");
    } else {
        if (pausedByHold_ && kenshi::GetPaused()) kenshi::CallTogglePause(false);
        pausedByHold_ = false;
    }
}

bool KenshiWorld::BeginWorldExport(std::string* err) {
    if (kenshi::SaveManagerBusy()) { if (err) *err = "the game is already saving or loading"; return false; }
    std::string folder;
    if (!kenshi::RequestSave(kExportSlot, &folder) || folder.empty()) {
        if (err) *err = "the game refused to save now";
        return false;
    }
    exportFolder_ = folder;
    exporting_ = true;
    exportLastSize_ = 0;
    exportStableSince_ = 0;
    return true;
}

kc::ExportStatus KenshiWorld::PollWorldExport(std::vector<kc::WorldFile>& files, std::string* err) {
    if (!exporting_) { if (err) *err = "no save in progress"; return kc::ExportStatus::Failed; }
    if (kenshi::SaveManagerBusy()) return kc::ExportStatus::Pending;
    // Kenshi clears its request before the files are all on disk (it copies its working folder
    // into the slot afterwards): wait until quick.save exists and the folder stops growing.
    std::error_code ec;
    const fs::path dir = FromGamePath(exportFolder_) / kExportSlot;
    uint64_t size = 0;
    bool haveMain = false;
    if (fs::is_directory(dir, ec)) {
        for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            size += it->file_size(ec);
            if (it->path().filename() == "quick.save") haveMain = true;
        }
    }
    const double now = NowSeconds();
    if (!haveMain || size != exportLastSize_) {
        exportLastSize_ = size;
        exportStableSince_ = now;
        return kc::ExportStatus::Pending;   // the session's export timeout bounds this wait
    }
    if (now - exportStableSince_ < 1.0) return kc::ExportStatus::Pending;
    exporting_ = false;
    files.clear();
    uint64_t total = 0;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        kc::WorldFile f;
        f.path = ToUtf8(fs::relative(it->path(), dir, ec));
        if (ec || !kc::ValidWorldPath(f.path)) { if (err) *err = "unexpected file in save: " + f.path; return kc::ExportStatus::Failed; }
        std::ifstream in(it->path(), std::ios::binary);
        f.data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        total += f.data.size();
        if (total > kc::kMaxWorldBytes || files.size() >= kc::kMaxWorldFiles) { if (err) *err = "save too large"; return kc::ExportStatus::Failed; }
        files.push_back(std::move(f));
    }
    if (ec || files.empty()) { if (err) *err = "cannot read the save folder"; return kc::ExportStatus::Failed; }
    return kc::ExportStatus::Done;
}

bool KenshiWorld::BeginWorldImport(const std::vector<kc::WorldFile>& files, std::string* err) {
    std::string folder;
    if (!kenshi::SaveFolder(folder)) { if (err) *err = "cannot find the save folder"; return false; }
    if (kenshi::SaveManagerBusy()) { if (err) *err = "the game is busy saving or loading"; return false; }
    const fs::path dir = FromGamePath(folder) / kImportSlot;
    std::error_code ec;
    if (dir.filename() != kImportSlot) { if (err) *err = "bad save folder"; return false; }
    fs::remove_all(dir, ec);   // only ever our own slot
    for (const auto& f : files) {
        if (!kc::ValidWorldPath(f.path)) { if (err) *err = "refused file path " + f.path; return false; }
        const fs::path target = dir / FromUtf8(f.path);
        fs::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(f.data.data()), std::streamsize(f.data.size()));
        if (!out) { if (err) *err = "cannot write " + ToUtf8(target); return false; }
    }
    if (!kenshi::RequestLoad(kImportSlot)) { if (err) *err = "the game refused to load the save"; return false; }
    return true;
}

void KenshiWorld::SetRole(bool client, bool active) {
    client_ = client;
    active_ = active;
    haveHostTime_ = false;
    lastDest_.clear();
    if (!active) controllable_.clear();
}

void KenshiWorld::SetControllable(const std::vector<kc::Handle>& handles) {
    controllable_.clear();
    controllable_.insert(handles.begin(), handles.end());
}

void KenshiWorld::Toast(const std::string& msg) {
    std::lock_guard<std::mutex> lk(toastMutex_);
    if (toasts_.size() < 16) toasts_.push_back(msg);
}

std::vector<std::string> KenshiWorld::TakeToasts() {
    std::lock_guard<std::mutex> lk(toastMutex_);
    return std::exchange(toasts_, {});
}

} // namespace kcp
