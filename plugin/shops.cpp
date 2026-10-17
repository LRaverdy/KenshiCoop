// Animal shops and the prospecting window, on the game's side.
//
// Animal shops (A_FAIRE 3i). The dialogue action CHOOSE_ANIMALS_BUYING (0x30 in Dialogue::_doActions,
// 0x6817F4) gathers the seller's animals around it, builds an AnimalTradingHandler (0x6A9440: seller,
// buyer, one entry per animal with its price) and shows it in a CharacterTradingWindow (0x72A880,
// then show(true)). In a client's conversation all of this runs in the host's game.
// Host: the handler made for another player's character is noted (hk_animalCtor); its window is
// never shown (hk_tradeWindowShow: the handler is taken out of the window, which stays hidden until
// the next one replaces it) and the handler is kept as the offer the session sends to that player
// (TakeAnimalOffers). The player's choice is sold with that same handler: its entries ticked, its
// confirm (0x6AAC20) run, which recruits each animal and takes the cats (SellAnimals); then it is
// freed the way the window frees it (DropAnimalOffer).
// Client: the same window is made with our copies of the seller, our character and the animals
// (OpenAnimalShop); its confirm never sells anything here (hk_animalConfirm): the ticked animals go
// to the host (TakeAnimalChoices), and the window closes as after a purchase.
//
// Prospecting (A_FAIRE 3p). The orders bar's PROSPECT button (OrdersPanel::prospectingButton,
// 0x721800) closes the window when open; otherwise it gives task 150 to the selected character
// nearest the camera with Character::addOrder (no PlayerInterface order function: none of our order
// hooks saw it), and the task, once its short action is over (0x348610), opens the window with the
// character's place, science skill and name (ProspectingWindow::showT). On a client the task never
// runs (the host drives our characters): nothing opened, nothing logged. The window only reads the
// world's resource maps (game data, the same on every machine): a client now opens it at once for
// its own character (hk_prospectButton). On the host, the button no longer gives task 150 to another
// player's character (it bypassed every order hook).
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

#include "hooks.h"
#include "util.h"
#include "world.h"

namespace kenshi {

namespace {
constexpr uintptr_t kRvaTradingWindowInstance = 0x2132C30;   // CharacterTradingWindow::Instance
constexpr uintptr_t kRvaCrtNew = 0x22471A0;                  // import: the CRT's operator new (what _doActions allocates them with)
constexpr uintptr_t kRvaCrtDelete = 0x2247198;               // import: its operator delete (what the window frees the handler with)
constexpr uintptr_t kRvaWidgetGetVisible = 0x2246A30;        // import: MyGUI::Widget::getVisible
constexpr uintptr_t kVtLektorCharacters = 0x16EFE88;         // lektor<Character*> (the list _doActions passes)
constexpr size_t kHandlerSize = 0x88, kWindowSize = 0x110;
// CharacterTradingHandler
constexpr uintptr_t CTH_seller = 0x08, CTH_buyer = 0x10, CTH_entries = 0x20, CTH_count = 0x28;
constexpr size_t kEntrySize = 0x68;
constexpr uintptr_t ENT_price = 0x20, ENT_ticked = 0x24;   // entry +0: the animal's hand
// CharacterTradingWindow
constexpr uintptr_t CTW_widget = 0x38, CTW_handler = 0xD0;
constexpr uintptr_t kWidgetSetVisible = 0x18;   // MyGUI::Widget vtable: setVisible(bool)
// ProspectingWindow
constexpr uintptr_t PW_widget = 0x08, PW_lastPos = 0x108, PW_lastSkill = 0x114, PW_lastName = 0x118;
constexpr size_t kStatScience = 3;   // kStatOffsets[3] = CharStats+0xE0, what task 150 passes

bool Copy(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <class T>
bool Get(const void* p, uintptr_t off, T& out) {
    return p && Copy(&out, static_cast<const uint8_t*>(p) + off, sizeof(T));
}
template <class T>
bool Put(void* p, uintptr_t off, const T& v) {
    return p && Copy(static_cast<uint8_t*>(p) + off, &v, sizeof(T));
}
void* Import(uintptr_t rva) {
    void* f = nullptr;
    Get(reinterpret_cast<void*>(Addr(rva)), 0, f);
    return f;
}

struct Lektor {   // what the constructors read: count (+8) and data (+0x10)
    uintptr_t vt = 0;
    uint32_t count = 0, capacity = 0;
    Character** data = nullptr;
};

using NewFn = void* (*)(size_t);
using DelFn = void (*)(void*);
using AnimalCtorFn = void* (*)(void*, Character*, Character*, Lektor*, int);
using WindowCtorFn = void* (*)(void*, void*);
using ShowFn = void (*)(void*, bool);
using ThisFn = void (*)(void*);
using BoolThisFn = bool (*)(void*);
using ButtonFn = void (*)(void*, void*);
using WidgetBoolFn = bool (*)(const void*);
using WidgetSetFn = void (*)(void*, bool);
using SingletonFn = void* (*)();
using ShowTFn = void (*)(void*, const float*, float, const void*);
using CameraFn = float* (*)(void*, float*);
using NearestFn = Character* (*)(void*, const float*);

void* CallNew(size_t n) {
    void* f = Import(kRvaCrtNew);
    if (!f) return nullptr;
    __try { return reinterpret_cast<NewFn>(f)(n); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
bool CallDelete(void* p) {
    void* f = Import(kRvaCrtDelete);
    if (!f) return false;
    __try { reinterpret_cast<DelFn>(f)(p); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* AnimalCtorSeh(void* h, Character* seller, Character* buyer, Lektor* l, int price) {
    __try { return reinterpret_cast<AnimalCtorFn>(FnAddr(FnAnimalTradeCtor))(h, seller, buyer, l, price); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
void* WindowCtorSeh(void* w, void* h) {
    __try { return reinterpret_cast<WindowCtorFn>(FnAddr(FnCharTradeWindowCtor))(w, h); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
bool ShowSeh(void* w, bool on) {
    __try { reinterpret_cast<ShowFn>(FnAddr(FnCharTradeWindowShow))(w, on); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool BoolSeh(void* fn, void* self, bool& out) {
    __try { out = reinterpret_cast<BoolThisFn>(fn)(self); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ThisSeh(void* fn, void* self) {
    __try { reinterpret_cast<ThisFn>(fn)(self); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ButtonSeh(void* fn, void* self, void* sender) {
    __try { reinterpret_cast<ButtonFn>(fn)(self, sender); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool WidgetVisibleSeh(void* fn, const void* widget, bool& out) {
    __try { out = reinterpret_cast<WidgetBoolFn>(fn)(widget); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool WidgetSetSeh(void* fn, void* widget, bool on) {
    __try { reinterpret_cast<WidgetSetFn>(fn)(widget, on); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void* SingletonSeh() {
    __try { return reinterpret_cast<SingletonFn>(FnAddr(FnProspectSingleton))(); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
bool ShowTSeh(void* win, const float* pos, float skill, const void* name) {
    __try { reinterpret_cast<ShowTFn>(FnAddr(FnProspectShow))(win, pos, skill, name); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
Character* NearestSeh(void* gw, void* pi) {
    __try {
        float cam[3] = {};
        reinterpret_cast<CameraFn>(FnAddr(FnCameraTarget))(gw, cam);
        return reinterpret_cast<NearestFn>(FnAddr(FnNearestSelected))(pi, cam);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool WidgetVisible(const void* widget, bool& out) {
    void* fn = Import(kRvaWidgetGetVisible);
    return widget && fn && WidgetVisibleSeh(fn, widget, out);
}
} // namespace

namespace {
void* TradeCtorSeh(void* fn, void* h, Character* seller, Character* buyer, Lektor* l, int price) {
    __try { return reinterpret_cast<AnimalCtorFn>(fn)(h, seller, buyer, l, price); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
} // namespace

// The game's character trading window (animals, slaves bought, characters sold as slaves) made with
// that handler's constructor, its prices (when given: by the character each entry names) set before
// the window lays its rows out.
bool OpenCharTradeWindow(Fn ctor, Character* seller, Character* buyer, const std::vector<Character*>& chars, int32_t basePrice,
                         const std::vector<int32_t>* prices, void** handlerOut) {
    if (handlerOut) *handlerOut = nullptr;
    if (!IsCharacter(seller) || !IsCharacter(buyer) || chars.empty() || chars.size() > 64) return false;
    for (Character* a : chars) if (!IsCharacter(a)) return false;
    std::vector<Character*> list = chars;   // read by the constructors only, never kept
    Lektor l;
    l.vt = Addr(kVtLektorCharacters);
    l.count = l.capacity = uint32_t(list.size());
    l.data = list.data();
    void* h = CallNew(kHandlerSize);
    if (!h) return false;
    void* made = ctor == FnAnimalTradeCtor ? AnimalCtorSeh(h, seller, buyer, &l, std::max(basePrice, 0))
                                           : TradeCtorSeh(FnAddr(ctor), h, seller, buyer, &l, std::max(basePrice, 0));
    if (!made) {
        kcp::Log("character trading window: the game's handler could not be made (exception caught; its memory is left)");
        return false;
    }
    if (prices && prices->size() == chars.size()) {
        uint8_t* data = nullptr;
        uint64_t n = 0;
        if (Get(h, CTH_entries, data) && Get(h, CTH_count, n) && data && n <= 64)
            for (uint64_t i = 0; i < n; ++i) {
                kc::Handle eh;
                HandleFromHand(data + i * kEntrySize, eh);
                for (size_t k = 0; k < chars.size(); ++k)
                    if (kc::Handle ch; GetHandle(chars[k], ch) && ch == eh) Put(data + i * kEntrySize, ENT_price, std::max((*prices)[k], 0));
            }
    }
    void* w = CallNew(kWindowSize);
    if (!w || !WindowCtorSeh(w, h)) {
        kcp::Log("character trading window: the game's window could not be made");
        return false;
    }
    if (handlerOut) *handlerOut = h;
    return ShowSeh(w, true);
}

bool CallSlaveConfirm(void* handler, bool sell, bool& done) {
    done = false;
    return handler && BoolSeh(FnAddr(sell ? FnSlaveSellConfirm : FnSlaveBuyConfirm), handler, done);
}

bool OpenAnimalShopWindow(Character* seller, Character* buyer, const std::vector<Character*>& animals, int32_t basePrice, void** handlerOut) {
    return OpenCharTradeWindow(FnAnimalTradeCtor, seller, buyer, animals, basePrice, nullptr, handlerOut);
}

bool AnimalShopEntries(const void* handler, std::vector<ShopEntry>& out) {
    out.clear();
    uint8_t* data = nullptr;
    uint64_t n = 0;
    if (!Get(handler, CTH_entries, data) || !Get(handler, CTH_count, n) || (n && !data) || n > 64) return false;
    for (uint64_t i = 0; i < n; ++i) {
        const uint8_t* e = data + i * kEntrySize;
        ShopEntry s;
        uint8_t ticked = 0;
        if (!Get(e, ENT_price, s.price) || !Get(e, ENT_ticked, ticked)) return false;
        s.ticked = ticked != 0;
        HandleFromHand(e, s.animal);
        out.push_back(s);
    }
    return true;
}

bool AnimalShopParties(const void* handler, Character*& seller, Character*& buyer) {
    return Get(handler, CTH_seller, seller) && Get(handler, CTH_buyer, buyer);
}

bool TickAnimalEntry(void* handler, size_t index, bool on) {
    uint8_t* data = nullptr;
    uint64_t n = 0;
    if (!Get(handler, CTH_entries, data) || !Get(handler, CTH_count, n) || index >= n || !data) return false;
    return Put(data + index * kEntrySize, ENT_ticked, uint8_t(on ? 1 : 0));
}

bool CallAnimalConfirm(void* handler, bool& sold) {
    sold = false;
    return handler && BoolSeh(FnAddr(FnAnimalTradeConfirm), handler, sold);
}

bool FreeAnimalHandler(void* handler) {
    if (!handler || !ThisSeh(FnAddr(FnCharTradeHandlerDtor), handler)) return false;
    return CallDelete(handler);
}

void* TradingWindow() {
    void* w = nullptr;
    Get(reinterpret_cast<void*>(Addr(kRvaTradingWindowInstance)), 0, w);
    return w;
}
void* TradingWindowHandler(const void* window) {
    void* h = nullptr;
    Get(window, CTW_handler, h);
    return h;
}
bool DetachTradingWindowHandler(void* window) { return Put(window, CTW_handler, static_cast<void*>(nullptr)); }
bool TradingWindowVisible(const void* window) {
    void* widget = nullptr;
    bool v = false;
    return Get(window, CTW_widget, widget) && WidgetVisible(widget, v) && v;
}
bool HideTradingWindow(void* window) {
    void* widget = nullptr;
    void* vt = nullptr;
    void* fn = nullptr;
    return Get(window, CTW_widget, widget) && widget && Get(widget, 0, vt) && Get(vt, kWidgetSetVisible, fn) && fn && WidgetSetSeh(fn, widget, false);
}
bool PressTradingConfirm(void* window) { return window && ButtonSeh(FnAddr(FnCharTradeConfirmButton), window, nullptr); }

Character* ProspectingCharacter() {
    GameWorld* gw = World();
    PlayerInterface* pi = Player();
    if (!gw || !pi) return nullptr;
    Character* c = NearestSeh(gw, pi);
    return IsCharacter(c) ? c : nullptr;
}

bool ProspectingWindowVisible() {
    void* win = SingletonSeh();
    void* widget = nullptr;
    bool v = false;
    return win && Get(win, PW_widget, widget) && WidgetVisible(widget, v) && v;
}

bool ShowProspectingWindow(Character* c) {
    kc::Vec3 p;
    float skill = 0;
    std::string name;
    if (!IsCharacter(c) || !GetPosition(c, p)) return false;
    ReadStat(c, kStatScience, skill);
    CharacterName(c, name);
    void* win = SingletonSeh();
    if (!win) return false;
    const float pos[3] = {p.x, p.y, p.z};
    alignas(8) uint8_t gs[0x28];
    GameStringView(name, gs);
    return ShowTSeh(win, pos, skill, gs);
}

bool ProspectingState(bool& visible, kc::Vec3& pos, float& skill, std::string& name) {
    visible = ProspectingWindowVisible();
    void* win = SingletonSeh();
    float p[3] = {};
    if (!win || !Get(win, PW_lastPos, p) || !Get(win, PW_lastSkill, skill)) return false;
    pos = {p[0], p[1], p[2]};
    return ReadStdString(static_cast<uint8_t*>(win) + PW_lastName, name);
}

bool PressProspectButton() {
    // the button's code never reads its OrdersPanel (only globals): no panel is needed
    return ButtonSeh(FnAddr(FnProspectButton), nullptr, nullptr);
}

} // namespace kenshi

namespace kcp {

namespace {
// ---- host: the handlers the game made for another player's character, and the offers kept
struct HeldOffer {
    kenshi::Character* seller = nullptr;
    kenshi::Character* buyer = nullptr;
    kc::Handle sellerH, buyerH;
    int32_t basePrice = 0;
    std::vector<kc::Handle> animals;   // (slave shops: the characters offered)
    std::vector<int32_t> prices;        // slave shops: the game's price of each
    uint32_t generation = 0;
    int kind = -1;                      // -1 an animal shop, else kc::kSlaveBuy / kSlaveSell
};
std::mutex g_shopMutex;
std::map<void*, HeldOffer> g_noted;   // made, its window not shown yet
std::map<void*, HeldOffer> g_held;    // its window hidden: offered to the player
std::vector<kc::IWorld::AnimalOffer> g_newOffers;
std::vector<kc::IWorld::SlaveOffer> g_newSlaveOffers;
// ---- client: the window we opened for the host's offer
void* g_clientHandler = nullptr;
void* g_clientWindow = nullptr;
std::vector<kc::IWorld::AnimalChoice> g_choices;
void* g_slaveHandler = nullptr;   // the slave window we opened (0.3.5 lot 5)
void* g_slaveWindow = nullptr;
int g_slaveKind = 0;
std::vector<kc::IWorld::SlaveChoice> g_slaveChoices;

// Host: note a handler the game made for another player's character (its window will be kept hidden).
bool NoteForeignHandler(void* self, kenshi::Character* a, kenshi::Character* b, int basePrice, int kind) {
    auto v = KenshiWorld::View();
    KenshiWorld* w = TheWorld();
    if (InHostCall() || !v->active || v->client || !w) return false;
    // the dialogue passes (its owner, the other one): the seller is the dialogue's NPC; whichever of
    // the two is another player's character is the player's side
    const bool fb = b && v->squadForeign.count(b), fa = !fb && a && v->squadForeign.count(a);
    if (!fa && !fb) return false;
    HeldOffer o;
    o.buyer = fb ? b : a;
    o.seller = fb ? a : b;
    o.kind = kind;
    kenshi::GetHandle(o.buyer, o.buyerH);
    kenshi::GetHandle(o.seller, o.sellerH);
    o.basePrice = basePrice;
    o.generation = w->WorldGeneration();
    std::vector<kenshi::ShopEntry> entries;
    kenshi::AnimalShopEntries(self, entries);
    for (const auto& e : entries) {
        o.animals.push_back(e.animal);
        o.prices.push_back(e.price);
    }
    std::lock_guard<std::mutex> lk(g_shopMutex);
    if (g_noted.size() >= 8) g_noted.erase(g_noted.begin());   // (a handler whose window never came: forgotten, not freed)
    g_noted[self] = std::move(o);
    return true;
}

void ShopToast(const char* text) {
    if (KenshiWorld* w = TheWorld()) w->Toast(text);
}
} // namespace

namespace shophooks {
using AnimalCtorFn = void* (*)(void*, kenshi::Character*, kenshi::Character*, void*, int);
AnimalCtorFn o_animalCtor = nullptr;
using BoolFn = bool (*)(void*);
BoolFn o_animalConfirm = nullptr;
using ShowFn = void (*)(void*, bool);
ShowFn o_tradeWindowShow = nullptr;
using ButtonFn = void (*)(void*, void*);
ButtonFn o_prospectButton = nullptr;
AnimalCtorFn o_slaveBuyCtor = nullptr, o_slaveSellCtor = nullptr;   // 0.3.5 lot 5 (same signature)
BoolFn o_slaveBuyConfirm = nullptr, o_slaveSellConfirm = nullptr;

// Host: an animal shop the game makes for another player's character is noted (the window that shows
// it next is then kept hidden).
void* hk_animalCtor(void* self, kenshi::Character* a, kenshi::Character* b, void* list, int basePrice) {
    void* r = o_animalCtor(self, a, b, list, basePrice);
    if (r && NoteForeignHandler(self, a, b, basePrice, -1)) Log("animal shop made for another player's character: its window will not show here");
    return r;
}

// 0.3.5 lot 5 (A_FAIRE 16/10 point 4). Host: the slave window the dialogue makes for another player's
// character (action 0x2A: the slaver's slaves; 0x29: that character's squad mates to sell) is noted the
// same way: its window stays hidden, the offer goes to that player.
void* hk_slaveBuyCtor(void* self, kenshi::Character* a, kenshi::Character* b, void* list, int basePrice) {
    void* r = o_slaveBuyCtor(self, a, b, list, basePrice);
    if (r && NoteForeignHandler(self, a, b, basePrice, kc::kSlaveBuy)) Log("slave shop (buying) made for another player's character: its window will not show here");
    return r;
}
void* hk_slaveSellCtor(void* self, kenshi::Character* a, kenshi::Character* b, void* list, int basePrice) {
    void* r = o_slaveSellCtor(self, a, b, list, basePrice);
    if (r && NoteForeignHandler(self, a, b, basePrice, kc::kSlaveSell)) Log("slave shop (selling) made for another player's character: its window will not show here");
    return r;
}

// The character trading window (animals, slaves, bail, conscription: one class). Host: the window of
// an animal shop noted above stays hidden; its handler is taken out of it (the window would free it
// when replaced) and offered to the player.
void hk_tradeWindowShow(void* win, bool on) {
    auto v = KenshiWorld::View();
    if (on && v->active && !v->client) {
        void* h = kenshi::TradingWindowHandler(win);
        std::unique_lock<std::mutex> lk(g_shopMutex);
        auto it = h ? g_noted.find(h) : g_noted.end();
        if (it != g_noted.end()) {
            HeldOffer o = std::move(it->second);
            g_noted.erase(it);
            kenshi::DetachTradingWindowHandler(win);
            kenshi::HideTradingWindow(win);
            const int kind = o.kind;
            if (kind < 0) {
                kc::IWorld::AnimalOffer offer;
                offer.key = reinterpret_cast<uint64_t>(h);
                offer.seller = o.sellerH;
                offer.buyer = o.buyerH;
                offer.basePrice = o.basePrice;
                offer.animals = o.animals;
                if (g_newOffers.size() < 16) g_newOffers.push_back(std::move(offer));
            } else {
                kc::IWorld::SlaveOffer offer;
                offer.key = reinterpret_cast<uint64_t>(h);
                offer.kind = uint8_t(kind);
                offer.seller = o.sellerH;
                offer.buyer = o.buyerH;
                offer.basePrice = o.basePrice;
                offer.slaves = o.animals;
                offer.prices = o.prices;
                if (g_newSlaveOffers.size() < 16) g_newSlaveOffers.push_back(std::move(offer));
            }
            g_held[h] = std::move(o);
            lk.unlock();
            Log("%s window of another player's character kept hidden here: offered to that player", kind < 0 ? "animal shop" : "slave shop");
            return;
        }
    }
    o_tradeWindowShow(win, on);
}

// The animal window's confirm. Client: nothing is sold here; the ticked animals go to the host
// (the window then closes as after a purchase). Host: the game's own (its window, or SellAnimals).
bool hk_animalConfirm(void* handler) {
    if (InHostCall() || !KenshiWorld::ClientActive()) return o_animalConfirm(handler);
    std::vector<kenshi::ShopEntry> entries;
    std::lock_guard<std::mutex> lk(g_shopMutex);
    if (handler != g_clientHandler) {
        Log("animal shop confirm on a window the host did not offer: refused here");
        ShopToast("Achat d'animaux : demande-le au vendeur pendant la partie (le jeu de l'hôte vend).");
        return false;
    }
    kc::IWorld::AnimalChoice c;
    if (kenshi::AnimalShopEntries(handler, entries))
        for (const auto& e : entries)
            // our copy of a host animal (a stand-in) has its own local handle: the session knows the host's
            if (e.ticked) { KenshiWorld* w = TheWorld(); c.animals.push_back(w ? w->HostHandleOf(e.animal) : e.animal); c.price += e.price; }
    if (c.animals.empty()) return false;   // nothing ticked: the game does nothing either
    Log("animal shop: %zu animal(s) chosen here for %d cats, asked of the host", c.animals.size(), int(c.price));
    if (g_choices.size() < 8) g_choices.push_back(std::move(c));
    g_clientHandler = nullptr;   // the window frees it now
    g_clientWindow = nullptr;
    return true;
}

// 0.3.5 lot 5: the slave windows' confirm. Client: nothing is bought or sold here; the ticked characters
// go to the host (the window then closes as after a deal). Host: the game's own (its window, or
// TradeSlaves).
bool SlaveConfirm(void* handler, int kind, BoolFn original) {
    if (InHostCall() || !KenshiWorld::ClientActive()) return original(handler);
    std::vector<kenshi::ShopEntry> entries;
    std::lock_guard<std::mutex> lk(g_shopMutex);
    if (handler != g_slaveHandler || kind != g_slaveKind) {
        Log("slave window confirm on a window the host did not offer: refused here");
        ShopToast("Esclaves : passe par le marchand pendant la partie (le jeu de l'hôte fait l'échange).");
        return false;
    }
    kc::IWorld::SlaveChoice c;
    c.kind = uint8_t(kind);
    if (kenshi::AnimalShopEntries(handler, entries))
        for (const auto& e : entries)
            // our copy of a host character (a stand-in) has its own local handle: the session knows the host's
            if (e.ticked) { KenshiWorld* w = TheWorld(); c.slaves.push_back(w ? w->HostHandleOf(e.animal) : e.animal); c.price += e.price; }
    if (c.slaves.empty()) return false;   // nothing ticked: the game does nothing either
    Log("slave window: %zu character(s) chosen here for %d cats, asked of the host", c.slaves.size(), int(c.price));
    if (g_slaveChoices.size() < 8) g_slaveChoices.push_back(std::move(c));
    g_slaveHandler = nullptr;   // the window frees it now
    g_slaveWindow = nullptr;
    return true;
}
bool hk_slaveBuyConfirm(void* handler) { return SlaveConfirm(handler, kc::kSlaveBuy, o_slaveBuyConfirm); }
bool hk_slaveSellConfirm(void* handler) { return SlaveConfirm(handler, kc::kSlaveSell, o_slaveSellConfirm); }

// The PROSPECT button. Client: the window opens here at once for our character (the task the game
// would give it never runs on a client). Host: never task 150 on another player's character.
void hk_prospectButton(void* panel, void* sender) {
    auto v = KenshiWorld::View();
    if (InHostCall() || !v->active || kenshi::ProspectingWindowVisible()) return o_prospectButton(panel, sender);   // (open: it closes)
    kenshi::Character* c = kenshi::ProspectingCharacter();
    if (!c) return o_prospectButton(panel, sender);
    kc::Handle h;
    kenshi::GetHandle(c, h);
    std::string name;
    kenshi::CharacterName(c, name);
    if (v->squadForeign.count(c) && !v->controllable.count(h)) {
        Log("prospect: %s is another player's character: refused", name.c_str());
        ShopToast("Ce personnage appartient à un autre joueur.");
        return;
    }
    if (!v->client) return o_prospectButton(panel, sender);
    const bool ok = kenshi::ShowProspectingWindow(c);
    Log("prospect: window %s here for %s (the host's game is not asked: it only shows this place's resources)", ok ? "opened" : "could NOT be opened",
        name.c_str());
}
} // namespace shophooks

// ---- KenshiWorld (IWorld): animal shops
void KenshiWorld::TakeAnimalOffers(std::vector<AnimalOffer>& out) {
    std::lock_guard<std::mutex> lk(g_shopMutex);
    out.swap(g_newOffers);
    g_newOffers.clear();
}

bool KenshiWorld::SellAnimals(uint64_t key, const std::vector<kc::Handle>& animals, int32_t& price, std::string& why) {
    price = 0;
    void* h = reinterpret_cast<void*>(key);
    HeldOffer o;
    {
        std::lock_guard<std::mutex> lk(g_shopMutex);
        auto it = g_held.find(h);
        if (it == g_held.end()) { why = "l'offre a expiré"; return false; }
        o = it->second;
    }
    if (o.generation != generation_) { why = "le monde a été rechargé"; return false; }
    // the handler keeps raw pointers to both characters: they must still be those
    if (Find(o.sellerH) != o.seller || Find(o.buyerH) != o.buyer || kenshi::IsDead(o.buyer)) {
        why = "le vendeur ou ton personnage n'est plus là";
        return false;
    }
    std::vector<kenshi::ShopEntry> entries;
    if (!kenshi::AnimalShopEntries(h, entries)) { why = "offre illisible"; return false; }
    int32_t total = 0;
    size_t ticked = 0;
    std::vector<kenshi::Character*> chosen;
    for (size_t i = 0; i < entries.size(); ++i) {
        const bool want = std::find(animals.begin(), animals.end(), entries[i].animal) != animals.end();
        kenshi::Character* a = want ? kenshi::Resolve(entries[i].animal) : nullptr;
        const bool ok = want && a && !kenshi::IsDead(a) && !kenshi::InPlayerSquad(a);
        if (want && !ok) {
            why = "un des animaux n'est plus à vendre";
            for (size_t k = 0; k < entries.size(); ++k) kenshi::TickAnimalEntry(h, k, false);
            return false;
        }
        kenshi::TickAnimalEntry(h, i, ok);
        if (ok) { total += entries[i].price; ++ticked; chosen.push_back(a); }
    }
    if (ticked != animals.size()) { why = "un des animaux n'était pas proposé"; return false; }
    int32_t before = 0, after = 0;
    kenshi::ReadPlayerMoney(before);
    if (before < total) {
        why = "pas assez d'argent (" + std::to_string(total) + " cats demandés, " + std::to_string(before) + " en caisse)";
        return false;
    }
    bool sold = false, ran = false;
    {
        CallScopeGuard guard("animal purchase");
        HostCallScope scope;
        // 0.3.4: the confirm ends by opening the character editor on the bought animals (as for the
        // player's own purchase); here it would open on the host's screen, pause the game and keep
        // the animals out of the squad list until closed: not opened (hk_showEditor)
        NoEditorScope noEditor;
        ran = kenshi::CallAnimalConfirm(h, sold);
    }
    kenshi::ReadPlayerMoney(after);
    price = before - after;
    Log("animal shop: the game sold %zu animal(s) for %d cats (prices %d)%s", ticked, int(price), int(total), ran ? (sold ? "" : ", confirm said no") : ", confirm faulted");
    if (!ran || !sold) {
        why = ran ? "le jeu a refusé (escouade pleine ?)" : "erreur du jeu";
        return false;
    }
    // the cats taken but no animal recruited (the game's recruit refused them: a test-spawned goat of no
    // seller did so on 12/10): the player gets the money back
    const bool joined = std::any_of(chosen.begin(), chosen.end(), [](kenshi::Character* a) { return kenshi::IsCharacter(a) && kenshi::InPlayerSquad(a); });
    // 0.3.4: where each one is now. The animalshop test's goat (12/10, 13/10) was taken into a player
    // squad here, yet never became a squad character of the mod: the confirm had opened the character
    // editor on the host (see hk_showEditor). The game's squad list is rebuilt on its next frame:
    // "list 0" here is expected
    {
        std::vector<kenshi::Character*> listed;
        kenshi::PlayerCharacters(listed);
        for (kenshi::Character* a : chosen) {
            kc::Handle ah;
            kc::Vec3 ap{};
            const bool alive = kenshi::IsCharacter(a);
            char akey[80] = "gone";
            if (alive) {
                kenshi::GetHandle(a, ah);
                kenshi::GetPosition(a, ap);
                snprintf(akey, sizeof(akey), "%u:%u:%u:%u:%u", ah.type, ah.container, ah.containerSerial, ah.index, ah.serial);
            }
            Log("animal shop: bought animal now %s: player squad %d, player characters list %d (rebuilt next frame), at %.0f,%.0f, editor open here %d", akey,
                int(alive && kenshi::InPlayerSquad(a)), int(alive && std::find(listed.begin(), listed.end(), a) != listed.end()), ap.x, ap.z,
                int(kenshi::CharacterEditorOpen()));
        }
    }
    if (!joined) {
        if (price > 0) kenshi::WritePlayerMoney(before);
        Log("animal shop: no bought animal joined the player's squads: %d cats given back", int(price));
        price = 0;
        why = "le jeu n'a pas fait entrer l'animal dans l'escouade (argent rendu)";
        return false;
    }
    return true;
}

void KenshiWorld::DropAnimalOffer(uint64_t key) {
    void* h = reinterpret_cast<void*>(key);
    HeldOffer o;
    {
        std::lock_guard<std::mutex> lk(g_shopMutex);
        auto it = g_held.find(h);
        if (it == g_held.end()) return;
        o = it->second;
        g_held.erase(it);
    }
    // a handler of a world since unloaded is left alone (never touched again)
    if (o.generation == generation_ && !kenshi::FreeAnimalHandler(h)) Log("animal shop: the kept offer could not be freed (exception caught)");
}

bool KenshiWorld::OpenAnimalShop(const kc::Handle& seller, const kc::Handle& buyer, const std::vector<kc::Handle>& animals, int32_t basePrice) {
    kenshi::Character* s = Find(seller);
    kenshi::Character* b = Find(buyer);
    std::vector<kenshi::Character*> list;
    for (const auto& h : animals)
        if (kenshi::Character* a = Find(h); a && !kenshi::IsDead(a)) list.push_back(a);
    if (!s || !b || list.empty()) return false;
    void* handler = nullptr;
    const bool ok = kenshi::OpenAnimalShopWindow(s, b, list, basePrice, &handler);
    std::lock_guard<std::mutex> lk(g_shopMutex);
    g_clientHandler = ok ? handler : nullptr;
    g_clientWindow = ok ? kenshi::TradingWindow() : nullptr;
    return ok && g_clientWindow;
}

void KenshiWorld::TakeAnimalChoices(std::vector<AnimalChoice>& out) {
    std::lock_guard<std::mutex> lk(g_shopMutex);
    // our window closed without a purchase (cancel, the cross, another window replacing it)
    if (g_clientHandler) {
        void* now = kenshi::TradingWindow();
        if (now != g_clientWindow || kenshi::TradingWindowHandler(now) != g_clientHandler || !kenshi::TradingWindowVisible(now)) {
            Log("animal shop window closed without buying");
            g_clientHandler = g_clientWindow = nullptr;
            if (g_choices.size() < 8) g_choices.push_back({});
        }
    }
    out.swap(g_choices);
    g_choices.clear();
}

// ---- KenshiWorld (IWorld): slave shops (0.3.5 lot 5, A_FAIRE 16/10 point 4)
void KenshiWorld::TakeSlaveOffers(std::vector<SlaveOffer>& out) {
    std::lock_guard<std::mutex> lk(g_shopMutex);
    out.swap(g_newSlaveOffers);
    g_newSlaveOffers.clear();
}

// Host: the player's choice run by the game with the handler it made: the entries ticked, its confirm
// (buy 0x6ADD20: each slave freed, ordered to follow the buyer, its "freed" conversation started with
// the buyer, then the cats taken from the player faction, which it never checks; sell 0x6A7740: each
// character chained as the slaver's slave, the cats added to the player faction).
bool KenshiWorld::TradeSlaves(uint64_t key, const std::vector<kc::Handle>& slaves, int32_t& price, std::string& why) {
    price = 0;
    void* h = reinterpret_cast<void*>(key);
    HeldOffer o;
    {
        std::lock_guard<std::mutex> lk(g_shopMutex);
        auto it = g_held.find(h);
        if (it == g_held.end() || it->second.kind < 0) { why = "l'offre a expiré"; return false; }
        o = it->second;
    }
    const bool sell = o.kind == kc::kSlaveSell;
    if (o.generation != generation_) { why = "le monde a été rechargé"; return false; }
    // the handler keeps raw pointers to both characters: they must still be those
    if (Find(o.sellerH) != o.seller || Find(o.buyerH) != o.buyer || kenshi::IsDead(o.buyer) || kenshi::IsDead(o.seller)) {
        why = "le marchand ou ton personnage n'est plus là";
        return false;
    }
    std::vector<kenshi::ShopEntry> entries;
    if (!kenshi::AnimalShopEntries(h, entries)) { why = "offre illisible"; return false; }
    int32_t total = 0;
    size_t ticked = 0;
    std::vector<kenshi::Character*> chosen;
    for (size_t i = 0; i < entries.size(); ++i) {
        const bool want = std::find(slaves.begin(), slaves.end(), entries[i].animal) != slaves.end();
        kenshi::Character* a = want ? kenshi::Resolve(entries[i].animal) : nullptr;
        // a slave bought is still nobody's; a character sold is still in the player faction
        const bool ok = want && a && !kenshi::IsDead(a) && (sell ? kenshi::InPlayerSquad(a) : !kenshi::InPlayerSquad(a));
        if (want && !ok) {
            why = sell ? "un des personnages ne peut plus être vendu" : "un des esclaves n'est plus à vendre";
            for (size_t k = 0; k < entries.size(); ++k) kenshi::TickAnimalEntry(h, k, false);
            return false;
        }
        kenshi::TickAnimalEntry(h, i, ok);
        if (ok) { total += entries[i].price; ++ticked; chosen.push_back(a); }
    }
    if (ticked != slaves.size()) { why = "un des personnages n'était pas proposé"; return false; }
    int32_t before = 0, after = 0;
    kenshi::ReadPlayerMoney(before);
    if (!sell && before < total) {
        why = "pas assez d'argent (" + std::to_string(total) + " cats demandés, " + std::to_string(before) + " en caisse)";
        return false;
    }
    bool done = false, ran = false;
    {
        CallScopeGuard guard(sell ? "slave sale" : "slave purchase");
        HostCallScope scope;
        ran = kenshi::CallSlaveConfirm(h, sell, done);
    }
    kenshi::ReadPlayerMoney(after);
    price = before - after;
    Log("slave shop: the game %s %zu character(s) for %d cats (prices %d)%s", sell ? "bought from the player" : "sold to the player", ticked, int(price),
        int(total), ran ? (done ? "" : ", confirm said no") : ", confirm faulted");
    for (kenshi::Character* a : chosen) {   // where each one stands now (for the logs: freed and following, or chained)
        kenshi::Captivity cap;
        std::string name;
        const bool alive = kenshi::IsCharacter(a);
        if (alive) { kenshi::ReadCaptivity(a, cap); kenshi::CharacterName(a, name); }
        Log("slave shop: %s now: slave state %d, chained %d, in the player's squads %d", alive ? name.c_str() : "(gone)", alive ? cap.slaveState : -1,
            int(alive && cap.chained), int(alive && kenshi::InPlayerSquad(a)));
    }
    if (!ran || !done) {
        why = ran ? "le jeu a refusé" : "erreur du jeu";
        return false;
    }
    return true;
}

void KenshiWorld::DropSlaveOffer(uint64_t key) { DropAnimalOffer(key); }   // the same kept handlers (g_held)

bool KenshiWorld::OpenSlaveShop(uint8_t kind, const kc::Handle& seller, const kc::Handle& buyer, const std::vector<kc::Handle>& slaves,
                                const std::vector<int32_t>& prices, int32_t basePrice) {
    kenshi::Character* s = Find(seller);
    kenshi::Character* b = Find(buyer);
    std::vector<kenshi::Character*> list;
    std::vector<int32_t> listPrices;
    for (size_t i = 0; i < slaves.size(); ++i)
        if (kenshi::Character* a = Find(slaves[i]); a && !kenshi::IsDead(a)) {
            list.push_back(a);
            listPrices.push_back(i < prices.size() ? prices[i] : 0);
        }
    if (!s || !b || list.empty() || kind > kc::kSlaveSell) return false;
    void* handler = nullptr;
    const bool ok = kenshi::OpenCharTradeWindow(kind == kc::kSlaveSell ? kenshi::FnSlaveSellCtor : kenshi::FnSlaveBuyCtor, s, b, list, basePrice,
                                                &listPrices, &handler);
    std::lock_guard<std::mutex> lk(g_shopMutex);
    g_slaveHandler = ok ? handler : nullptr;
    g_slaveWindow = ok ? kenshi::TradingWindow() : nullptr;
    g_slaveKind = kind;
    return ok && g_slaveWindow;
}

void KenshiWorld::TakeSlaveChoices(std::vector<SlaveChoice>& out) {
    std::lock_guard<std::mutex> lk(g_shopMutex);
    // our window closed without a deal (cancel, the cross, another window replacing it)
    if (g_slaveHandler) {
        void* now = kenshi::TradingWindow();
        if (now != g_slaveWindow || kenshi::TradingWindowHandler(now) != g_slaveHandler || !kenshi::TradingWindowVisible(now)) {
            Log("slave window closed without a deal");
            g_slaveHandler = g_slaveWindow = nullptr;
            kc::IWorld::SlaveChoice none;
            none.kind = uint8_t(g_slaveKind);
            if (g_slaveChoices.size() < 8) g_slaveChoices.push_back(none);
        }
    }
    out.swap(g_slaveChoices);
    g_slaveChoices.clear();
}

} // namespace kcp
