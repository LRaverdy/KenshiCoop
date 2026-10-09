// KenshiCoop plugin entry point. Kenshi loads it as an Ogre plugin (Plugins_x64.cfg) and calls
// dllStartPlugin while the engine starts, before any save is loaded.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <deque>
#include <memory>
#include <sstream>
#include <string>

#include "debug.h"
#include "hooks.h"
#include "kc/session.h"
#include "kenshi.h"
#include "overlay.h"
#include "steam_link.h"
#include "util.h"
#include "world.h"

namespace kcp {

namespace {

constexpr const char* kVersion = "0.1.0";

Config g_cfg;
std::wstring g_iniPath;
std::unique_ptr<KenshiWorld> g_world;
std::unique_ptr<kc::Session> g_session;
bool g_overlayVisible = true;
std::deque<std::pair<std::string, double>> g_toasts;   // text, expiry time
bool g_wasReady = false;
bool g_menuWindowShown = false;   // the Multijoueur window opens by itself once, on the main menu
double g_startedAt = 0;

const char* StateName(kc::SessionState s) {
    switch (s) {
    case kc::SessionState::Idle: return "offline";
    case kc::SessionState::Hosting: return "hosting";
    case kc::SessionState::Connecting: return "connecting...";
    case kc::SessionState::Handshake: return "handshaking...";
    case kc::SessionState::Downloading: return "receiving the host's world...";
    case kc::SessionState::Loading: return "loading the host's world...";
    case kc::SessionState::Connected: return "connected";
    case kc::SessionState::Failed: return "disconnected";
    }
    return "?";
}

void Toast(const std::string& msg, double seconds = 4.0) {
    const double now = NowSeconds();
    for (auto& t : g_toasts) if (t.first == msg) { t.second = now + seconds; return; }
    g_toasts.emplace_back(msg, now + seconds);
    while (g_toasts.size() > 4) g_toasts.pop_front();
    Log("toast: %s", msg.c_str());
}

// ---- hotkeys: Ctrl+Shift+<key>, edge-triggered, only while Kenshi has focus
bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

bool OurWindowFocused() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

struct Hotkey {
    int vk;
    bool down = false;
};
Hotkey g_hkHost{'H'}, g_hkJoin{'J'}, g_hkLeave{'L'}, g_hkGive{'G'}, g_hkOverlay{'O'}, g_hkDiag{'D'}, g_hkMultiplayer{'M'},
    g_hkConsole{'K'};

bool Pressed(Hotkey& k, bool modifiers) {
    const bool now = modifiers && KeyDown(k.vk);
    const bool edge = now && !k.down;
    k.down = now;
    return edge;
}

void GiveSelectedToNextPlayer() {
    if (!g_session->isHost()) { Toast("Only the host can hand characters to players."); return; }
    std::vector<kc::Handle> sel;
    kenshi::SelectedHandles(sel);
    std::vector<kc::Handle> mine;
    for (auto& h : sel) if (g_world->FindSquad(h)) mine.push_back(h);
    if (mine.empty()) { Toast("Select one of your squad members first."); return; }
    // cycle owner: host -> each connected player -> host
    std::vector<uint8_t> order{g_session->localId()};
    for (auto& [id, p] : g_session->players()) order.push_back(id);
    const uint8_t cur = g_session->ownerOf(mine.front());
    size_t idx = 0;
    for (size_t i = 0; i < order.size(); ++i) if (order[i] == cur) idx = i;
    const uint8_t next = order[(idx + 1) % order.size()];
    for (auto& h : mine) g_session->Assign(h, next);
    const std::string who = next == g_session->localId() ? "you" : g_session->players().at(next).name;
    Toast(std::to_string(mine.size()) + " character(s) now controlled by " + who + ".");
}

// Writes everything KenshiCoop reads from the game to the log, to validate the memory layout.
void DumpDiagnostics(const char* why) {
    if (!g_world->Ready()) { Log("---- diagnostics (%s): no world loaded", why); return; }
    std::vector<kc::Handle> hs;
    g_world->PlayerCharacters(hs);
    Log("---- diagnostics (%s): %zu player characters, speed=%.2f paused=%d", why, hs.size(), kenshi::GetFrameSpeed(), int(kenshi::GetPaused()));
    for (const auto& h : hs) {
        kc::EntityState st;
        const bool ok = g_world->Read(h, st);
        Log("  hand{type=%u cont=%u cser=%u idx=%u ser=%u} read=%d pos=(%.1f, %.1f, %.1f) rot=(%.3f, %.3f, %.3f, %.3f) dest=(%.1f, %.1f, %.1f) flags=%u",
            h.type, h.container, h.containerSerial, h.index, h.serial, int(ok), st.pos.x, st.pos.y, st.pos.z,
            st.rot.w, st.rot.x, st.rot.y, st.rot.z, st.dest.x, st.dest.y, st.dest.z, unsigned(st.flags));
    }
    std::vector<kc::Handle> sel;
    kenshi::SelectedHandles(sel);
    Log("  selected: %zu", sel.size());
    for (const auto& h : sel) Log("    hand{type=%u idx=%u ser=%u} known=%d", h.type, h.index, h.serial, int(g_world->FindSquad(h) != nullptr));
    std::vector<kenshi::Character*> active;
    kenshi::ActiveCharacters(active);
    double hours = -1;
    kenshi::GetGameHours(hours);
    Log("  active characters: %zu, game clock: %.3f h", active.size(), hours);
    for (const auto& h : hs) {
        kc::EntityVitals v;
        if (g_world->ReadVitals(h, v))
            Log("  vitals idx=%u blood=%.1f ko=%.1f flags=%u parts=%zu first=(%.1f, %.1f, %.1f)", h.index, v.blood, v.koTimer, unsigned(v.flags),
                v.parts.size(), v.parts.empty() ? 0.f : v.parts[0].flesh, v.parts.empty() ? 0.f : v.parts[0].stun, v.parts.empty() ? 0.f : v.parts[0].bandage);
    }
    Log("  world fingerprint %016llx, mods hash %016llx", (unsigned long long)g_world->Fingerprint(), (unsigned long long)g_world->ModsHash());
}

void HandleHotkeys() {
    // while a KenshiCoop text field has the keyboard, keys are text, not commands
    const bool mods = OurWindowFocused() && !OverlayTyping() && KeyDown(VK_CONTROL) && KeyDown(VK_SHIFT);
    std::string err;
    if (Pressed(g_hkMultiplayer, mods)) OverlayToggleMultiplayer();
    if (Pressed(g_hkConsole, mods)) OverlayToggleConsole();
    if (Pressed(g_hkOverlay, mods)) g_overlayVisible = !g_overlayVisible;
    if (Pressed(g_hkHost, mods)) {
        if (g_session->isHost()) Toast("Already hosting.");
        else if (g_session->Host(&err)) Toast("Hosting on port " + std::to_string(g_cfg.port) + ".");
        else Toast("Cannot host: " + err);
    }
    if (Pressed(g_hkJoin, mods)) {
        if (g_session->isClient()) Toast("Already connected.");
        else if (steam::JoinAddress(*g_session, g_cfg.joinAddress, g_cfg.port, &err)) Toast("Joining " + g_cfg.joinAddress + "...");
        else Toast("Cannot join: " + err);
    }
    if (Pressed(g_hkLeave, mods)) {
        g_session->Leave();
        Toast("Left the session.");
    }
    if (Pressed(g_hkGive, mods)) GiveSelectedToNextPlayer();
    if (Pressed(g_hkDiag, mods)) { DumpDiagnostics("hotkey"); Toast("Diagnostics written to KenshiCoop.log"); }
}

// ---- the Multijoueur window and the console (French: what the players read)
const char* FrenchState(kc::SessionState s) {
    switch (s) {
    case kc::SessionState::Idle: return "Hors ligne";
    case kc::SessionState::Hosting: return "Partie hébergée";
    case kc::SessionState::Connecting: return "Connexion à l'hôte...";
    case kc::SessionState::Handshake: return "Connexion à l'hôte...";
    case kc::SessionState::Downloading: return "Téléchargement du monde de l'hôte...";
    case kc::SessionState::Loading: return "Chargement du monde de l'hôte...";
    case kc::SessionState::Connected: return "Connecté";
    case kc::SessionState::Failed: return "Déconnecté";
    }
    return "?";
}

std::string FrenchError(const std::string& e) {
    static const std::pair<const char*, const char*> table[] = {
        {"incompatible KenshiCoop version", "version de KenshiCoop différente de celle de l'hôte"},
        {"different Kenshi build", "version de Kenshi différente de celle de l'hôte"},
        {"different active mod list", "mods différents de ceux de l'hôte (mêmes mods, même ordre)"},
        {"different save loaded", "le monde chargé ne correspond pas à celui de l'hôte"},
        {"server full", "la partie est pleine"},
        {"invalid player name", "nom invalide (lettres et chiffres, pas d'espace au début ou à la fin)"},
        {"host has not loaded a world yet", "l'hôte n'a pas encore chargé de partie"},
        {"the host could not save its world", "l'hôte n'a pas pu sauvegarder son monde, réessaie"},
        {"joining took too long", "la connexion a pris trop de temps"},
        {"removed by the host", "l'hôte t'a retiré de la partie"},
        {"no answer from host", "pas de réponse de l'hôte (adresse, port UDP fermé ou pare-feu ?)"},
        {"lost connection to host", "connexion à l'hôte perdue"},
        {"could not connect to host", "impossible de joindre l'hôte"},
        {"downloading the host's world timed out", "le téléchargement du monde a expiré"},
        {"loading the host's world timed out", "le chargement du monde a expiré"},
        {"the loaded world differs from the host's", "le monde chargé diffère de celui de l'hôte"},
        {"bad world transfer from host", "transfert du monde corrompu, réessaie"},
        {"cannot load the host's world", "impossible de charger le monde de l'hôte"},
        {"world unloaded", "la partie a été quittée"},
        {"load a save before hosting", "charge d'abord une partie"},
        {"cannot listen on UDP port", "port UDP déjà utilisé (une autre partie ?)"},
        {"cannot resolve address", "adresse de l'hôte introuvable"},
        {"cannot create network socket", "réseau indisponible"},
        {"cannot start connection", "impossible de démarrer la connexion"},
        {"Steam is not available", "Steam n'est pas disponible (lance le jeu depuis Steam)"},
        {"this Steam id is your own", "c'est ton propre code Steam"},
        {"cannot open a local port", "réseau indisponible"},
    };
    for (const auto& [en, fr] : table)
        if (e.find(en) != std::string::npos) return fr;
    return e;
}

// Settings typed in the window: used for this session and kept for the next launch.
bool ApplyConnection(const std::string& name, const std::string& address, uint16_t port) {
    if (!kc::ValidName(name)) { Toast("Nom invalide : 1 à " + std::to_string(kc::kMaxNameLen) + " caractères, sans espace au début ni à la fin."); return false; }
    if (!g_session->Configure(name, port)) { Toast("Quitte d'abord la session en cours."); return false; }
    g_cfg.name = name;
    g_cfg.joinAddress = address;
    g_cfg.port = port;
    if (!SaveConnection(g_iniPath, name, address, port)) Log("cannot save the connection settings to KenshiCoop.ini");
    return true;
}

size_t CharactersOf(uint8_t playerId) {
    std::vector<kc::Handle> hs;
    g_world->PlayerCharacters(hs);
    size_t n = 0;
    for (auto& h : hs) n += g_session->ownerOf(h) == playerId ? 1 : 0;
    return n;
}

void ConsoleCommand(const std::string& line) {
    Log("> %s", line.c_str());
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    std::transform(cmd.begin(), cmd.end(), cmd.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    auto out = [](const std::string& s) { Log("  %s", s.c_str()); };
    const bool host = g_session->isHost();
    const bool client = g_session->isClient();
    if (cmd == "help") {
        out("help                 cette aide");
        out("players              joueurs : id, nom, ping, personnages");
        out("status               état de la session et de la synchro");
        if (!client) {
            out("kick <id>            retirer un joueur (hôte)");
            out("give <id>            donner les personnages sélectionnés au joueur <id> (hôte)");
            out("save                 sauvegarder la partie");
            out("pause [0|1]          mettre en pause / reprendre");
            out("speed <x>            vitesse du jeu (1, 2, 3...)");
        }
        return;
    }
    if (cmd == "players") {
        out(std::to_string(g_session->localId()) + ". " + g_cfg.name + " (toi)  " + std::to_string(CharactersOf(g_session->localId())) + " perso(s)");
        for (auto& [id, p] : g_session->players())
            out(std::to_string(id) + ". " + p.name + "  ping " + std::to_string(host ? p.rttMs : 0) + " ms  " + std::to_string(CharactersOf(id)) +
                " perso(s)" + (host && !p.inGame ? "  (en train de rejoindre)" : ""));
        if (client) out("ping vers l'hôte : " + std::to_string(g_session->pingMs()) + " ms");
        return;
    }
    if (cmd == "status") {
        char buf[200];
        snprintf(buf, sizeof(buf), "%s - %zu entités, %zu PNJ, %zu effets météo, vitesse %.1f%s", FrenchState(g_session->state()),
                 g_session->entityCount(), g_session->npcCount(), g_world->LiveEffects(), kenshi::GetFrameSpeed(),
                 kenshi::GetPaused() ? " (pause)" : "");
        out(buf);
        if (client && g_session->missingNpcs()) out(std::to_string(g_session->missingNpcs()) + " PNJ de l'hôte pas encore présents ici");
        if (client && g_session->missingSquad()) out(std::to_string(g_session->missingSquad()) + " membres de l'escouade introuvables ici");
        if (host && g_session->joiningPlayers()) out(std::to_string(g_session->joiningPlayers()) + " joueur(s) en train de rejoindre");
        if (!g_session->lastError().empty() && !host && !client) out("dernière erreur : " + FrenchError(g_session->lastError()));
        return;
    }
    if (client) { out("commande réservée à l'hôte (ici : help, players, status)"); return; }
    if (cmd == "kick" || cmd == "give") {
        int id = -1;
        in >> id;
        if (!host) { out("pas de partie hébergée"); return; }
        if (cmd == "kick") {
            if (id < 0 || id > 255 || !g_session->KickPlayer(uint8_t(id))) out("joueur inconnu : tape players pour la liste");
            else out("joueur " + std::to_string(id) + " retiré");
            return;
        }
        const bool known = id == g_session->localId() || (id >= 0 && id <= 255 && g_session->players().count(uint8_t(id)));
        if (!known) { out("joueur inconnu : tape players pour la liste"); return; }
        std::vector<kc::Handle> sel;
        kenshi::SelectedHandles(sel);
        size_t n = 0;
        for (auto& h : sel)
            if (g_world->FindSquad(h)) { g_session->Assign(h, uint8_t(id)); ++n; }
        out(n ? std::to_string(n) + " personnage(s) donné(s) au joueur " + std::to_string(id) : "sélectionne d'abord des membres de ton escouade");
        return;
    }
    if (!g_world->Ready()) { out("aucune partie chargée"); return; }
    if (cmd == "save") {
        char name[64];
        const std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        std::strftime(name, sizeof(name), "coop_%Y%m%d_%H%M", &tm);
        std::string folder;
        out(kenshi::RequestSave(name, &folder) ? std::string("sauvegarde ") + name + " demandée" : "la sauvegarde a échoué");
        return;
    }
    if (cmd == "pause") {
        int on = kenshi::GetPaused() ? 0 : 1;
        in >> on;
        out(kenshi::CallUserPause(on != 0) ? (on ? "pause" : "reprise") : "échec");
        return;
    }
    if (cmd == "speed") {
        float v = 1;
        if (!(in >> v) || v <= 0 || v > 10) { out("usage : speed <x> (entre 0.1 et 10)"); return; }
        out(kenshi::CallSetFrameSpeed(v) ? "vitesse " + std::to_string(v).substr(0, 4) : "échec");
        return;
    }
    out("commande inconnue : tape help");
}

void HandleOverlayActions() {
    std::string err;
    for (auto& a : OverlayTakeActions()) {
        switch (a.kind) {
        case OverlayAction::Kind::Host:
            if (g_session->isHost()) { Toast("Partie déjà hébergée."); break; }
            if (!ApplyConnection(a.name, a.address, a.port)) break;
            if (g_session->Host(&err)) Toast("Partie hébergée sur le port " + std::to_string(g_cfg.port) + ".");
            else Toast("Impossible d'héberger : " + FrenchError(err));
            break;
        case OverlayAction::Kind::Join:
            if (g_session->isClient()) { Toast("Déjà connecté."); break; }
            if (a.address.empty()) { Toast("Entre l'adresse ou le code Steam de l'hôte."); break; }
            if (!ApplyConnection(a.name, a.address, a.port)) break;
            if (steam::JoinAddress(*g_session, g_cfg.joinAddress, g_cfg.port, &err)) Toast("Connexion à " + g_cfg.joinAddress + "...");
            else Toast("Impossible de rejoindre : " + FrenchError(err));
            break;
        case OverlayAction::Kind::Leave:
            g_session->Leave();
            Toast("Session quittée.");
            break;
        case OverlayAction::Kind::Command:
            ConsoleCommand(a.text);
            break;
        case OverlayAction::Kind::DialogAnswer:
            g_session->AnswerDialog(a.index);
            break;
        }
    }
}

void PublishOverlay() {
    OverlayModel m;
    m.visible = g_overlayVisible;
    m.title = std::string("KenshiCoop ") + kVersion + "  -  " + StateName(g_session->state());
    const auto st = g_session->state();
    if (st == kc::SessionState::Downloading) {
        m.lines.push_back("Receiving the host's world: " + std::to_string(int(g_session->downloadProgress() * 100)) + "%");
    } else if (st == kc::SessionState::Loading || st == kc::SessionState::Connecting || st == kc::SessionState::Handshake) {
        m.lines.push_back("Please wait...");
    } else if (st == kc::SessionState::Idle || st == kc::SessionState::Failed) {
        if (st == kc::SessionState::Failed && !g_session->lastError().empty()) m.lines.push_back(g_session->lastError());
        m.lines.push_back("Ctrl+Shift+H  host this game");
        m.lines.push_back("Ctrl+Shift+J  join " + g_cfg.joinAddress + ":" + std::to_string(g_cfg.port) + " (works from the main menu)");
        m.lines.push_back("Ctrl+Shift+M  multiplayer window   Ctrl+Shift+K  console");
        m.lines.push_back("Ctrl+Shift+O  hide this panel");
    } else {
        m.lines.push_back("You: " + g_cfg.name + " (player " + std::to_string(g_session->localId()) + ")" +
                          (g_session->isHost() ? "" : "   ping " + std::to_string(g_session->pingMs()) + " ms"));
        for (auto& [id, p] : g_session->players())
            m.lines.push_back("  player " + std::to_string(id) + ": " + p.name + (g_session->isHost() ? "   ping " + std::to_string(p.rttMs) + " ms" : ""));
        size_t mine = 0;
        std::vector<kc::Handle> hs;
        g_world->PlayerCharacters(hs);
        for (auto& h : hs) if (g_session->ownerOf(h) == g_session->localId()) ++mine;
        m.lines.push_back("Squad: " + std::to_string(hs.size()) + " characters, " + std::to_string(mine) + " yours");
        m.lines.push_back("World: " + std::to_string(g_session->npcCount()) + " NPCs synced" +
                          (g_session->isClient() && g_session->missingNpcs() ? " (" + std::to_string(g_session->missingNpcs()) + " not spawned here yet)" : ""));
        if (g_session->missingSquad())
            m.lines.push_back("WARNING: " + std::to_string(g_session->missingSquad()) + " squad members missing here - load the host's save!");
        if (g_session->isHost() && g_session->joiningPlayers())
            m.lines.push_back(std::to_string(g_session->joiningPlayers()) + " player(s) joining - game paused");
        if (g_session->isHost()) m.lines.push_back("Ctrl+Shift+G  give selected to next player");
        m.lines.push_back("Ctrl+Shift+L  leave");
    }
    const auto& chat = g_session->chatLog();
    for (size_t i = chat.size() > 6 ? chat.size() - 6 : 0; i < chat.size(); ++i) m.chat.push_back(chat[i]);
    // the Multijoueur window
    m.worldLoaded = g_world->Ready();
    m.hosting = g_session->isHost();
    m.active = m.hosting || g_session->isClient();
    m.stateText = FrenchState(st);
    if (st == kc::SessionState::Failed && !g_session->lastError().empty()) m.errorText = FrenchError(g_session->lastError());
    if (st == kc::SessionState::Downloading) m.download = float(g_session->downloadProgress());
    if (m.active) {
        m.players.push_back({g_session->localId(), g_cfg.name, 0, CharactersOf(g_session->localId()), true});
        for (auto& [id, p] : g_session->players())
            m.players.push_back({id, p.name, m.hosting ? p.rttMs : (id == 1 ? g_session->pingMs() : 0u), CharactersOf(id), false});
    }
    m.name = g_cfg.name;
    m.address = g_cfg.joinAddress;
    m.port = g_cfg.port;
    m.fullConsole = !g_session->isClient();
    if (steam::Available() && steam::MyId()) {
        m.steamId = std::to_string(steam::MyId());
        static double nextScan = 0;
        static std::vector<std::pair<std::string, std::string>> friends;
        if (const double now = NowSeconds(); now >= nextScan) {
            nextScan = now + 2.0;
            friends.clear();
            for (const auto& f : steam::FriendsHosting()) friends.emplace_back(f.name, std::to_string(f.id));
        }
        m.steamFriends = friends;
    }
    if (const auto& d = g_session->dialog(); d.open && g_session->isClient()) {
        m.dialogOpen = true;
        m.dialogId = d.id;
        m.dialogName = d.name;
        m.dialogText = d.text;
        m.dialogReplies = d.replies;
        m.dialogWaiting = d.waiting;
    }
    const double now = NowSeconds();
    while (!g_toasts.empty() && g_toasts.front().second < now) g_toasts.pop_front();
    for (auto& t : g_toasts) m.toasts.push_back(t.first);
    OverlayPublish(std::move(m));
}

// Playing through Steam: a host is reachable by its Steam id as soon as it hosts; the link closes
// with the session; "Rejoindre la partie" from Steam joins.
bool g_steamHostTried = false;
void SteamUpkeep() {
    steam::Tick();
    const auto st = g_session->state();
    const bool idle = st == kc::SessionState::Idle || st == kc::SessionState::Failed;
    if (idle && steam::Active()) steam::Stop();
    if (!g_session->isHost()) g_steamHostTried = false;
    else if (!g_steamHostTried && steam::Available() && !steam::Active()) {
        g_steamHostTried = true;
        std::string err;
        if (!steam::StartHost(g_cfg.port, &err)) Log("steam: cannot host through Steam: %s", err.c_str());
    }
    if (auto id = steam::TakeJoinRequest()) {
        std::string err;
        if (g_session->isHost() || g_session->isClient()) Toast("Quitte la session en cours avant de rejoindre un ami.");
        else if (steam::Join(*g_session, *id, &err)) Toast("Connexion à ton ami via Steam...");
        else Toast("Impossible de rejoindre : " + FrenchError(err));
    }
}

void Tick(bool live) {
    g_world->BeginFrame(live);
    for (auto& t : g_world->TakeToasts()) Toast(t);

    const bool ready = g_world->Ready();
    if (live && ready != g_wasReady) {
        g_wasReady = ready;
        Log(ready ? "world ready: %zu player characters" : "world unloaded", g_world->CharacterCount());
        if (ready) DumpDiagnostics("world loaded");
    } else if (!live && g_wasReady && NowSeconds() - LastLiveTick() > 1.0) {
        g_wasReady = false;
        Log("world unloaded (menu or loading)");
    }
    HandleHotkeys();
    HandleOverlayActions();
    if (!live && !g_menuWindowShown && !g_wasReady && g_session->state() == kc::SessionState::Idle && NowSeconds() - g_startedAt > 5.0) {
        g_menuWindowShown = true;   // on the main menu: offer to join
        OverlayOpenMultiplayer();
    }
    if (live) g_menuWindowShown = true;
    if (g_cfg.debugCommands) DebugPoll(*g_session, *g_world, live);
    SteamUpkeep();
    g_session->Tick(live);
    if (live) g_world->EndFrame();
    PublishOverlay();
}

void TickEntry(bool live) { Tick(live); }

// ---- Ogre frame listener: keeps KenshiCoop running in menus and loading screens, where the
// game's main loop (and therefore our main-loop tick) does not run. Its vtable must match the
// FrameListener of Kenshi's Ogre build, verified in OgreMain_x64.dll (Root::_fireFrame*):
// [0] frameStarted, [1] frameRenderingQueued, [2] (extra slot, unused), [3] frameEnded, [4] dtor.
struct FrameEvent {
    float timeSinceLastEvent;
    float timeSinceLastFrame;
};
class CoopFrameListener {
public:
    virtual bool frameStarted(const FrameEvent&) {
        if (NowSeconds() - LastLiveTick() > 0.2) RunTick(false);
        return true;
    }
    virtual bool frameRenderingQueued(const FrameEvent&) { return true; }
    virtual bool extraSlot(const void*) { return true; }
    virtual bool frameEnded(const FrameEvent&) { return true; }
    virtual ~CoopFrameListener() = default;
};
CoopFrameListener g_frameListener;

bool AddFrameListenerSEH(void* getSingleton, void* addListener) {
    using GetRootFn = void* (*)();
    using AddFn = void (*)(void* root, void* listener);
    __try {
        void* root = reinterpret_cast<GetRootFn>(getSingleton)();
        if (!root) return false;
        reinterpret_cast<AddFn>(addListener)(root, &g_frameListener);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool InstallFrameListener() {
    HMODULE ogre = GetModuleHandleW(L"OgreMain_x64.dll");
    if (!ogre) return false;
    void* get = reinterpret_cast<void*>(GetProcAddress(ogre, "?getSingleton@Root@Ogre@@SAAEAV12@XZ"));
    void* add = reinterpret_cast<void*>(GetProcAddress(ogre, "?addFrameListener@Root@Ogre@@QEAAXPEAVFrameListener@2@@Z"));
    return get && add && AddFrameListenerSEH(get, add);
}

// Last-chance crash report: where the game died, written to KenshiCoop.log before Windows takes over.
std::string ModuleOf(uintptr_t addr) {
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(addr), &mod) || !mod)
        return "?";
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(mod, path, MAX_PATH);
    std::wstring w(path, n);
    const size_t slash = w.find_last_of(L"\\/");
    char buf[160];
    snprintf(buf, sizeof(buf), "%ls+0x%llx", slash == std::wstring::npos ? w.c_str() : w.c_str() + slash + 1,
             (unsigned long long)(addr - reinterpret_cast<uintptr_t>(mod)));
    return buf;
}

LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;
LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    static std::atomic<int> once{0};
    if (once.fetch_add(1) == 0 && ep && ep->ExceptionRecord && ep->ContextRecord) {
        const auto* er = ep->ExceptionRecord;
        const auto* cx = ep->ContextRecord;
        Log("CRASH code=%08lx at %p (%s) thread=%lu access=%llu addr=%p", er->ExceptionCode, er->ExceptionAddress,
            ModuleOf(reinterpret_cast<uintptr_t>(er->ExceptionAddress)).c_str(), GetCurrentThreadId(),
            er->NumberParameters > 0 ? (unsigned long long)er->ExceptionInformation[0] : 0ull,
            er->NumberParameters > 1 ? reinterpret_cast<void*>(er->ExceptionInformation[1]) : nullptr);
        // Return addresses on the stack that land in a loaded module: enough to see who jumped where.
        const auto* sp = reinterpret_cast<const uintptr_t*>(cx->Rsp);
        int shown = 0;
        for (int i = 0; i < 256 && shown < 24; ++i) {
            uintptr_t v = 0;
            if (IsBadReadPtr(sp + i, sizeof(v))) break;
            v = sp[i];
            const std::string m = ModuleOf(v);
            if (m != "?") { Log("  stack[%d] %s", i, m.c_str()); ++shown; }
        }
    }
    return g_prevFilter ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

bool Start() {
    const std::wstring dir = GameDir();
    LogOpen(dir + L"KenshiCoop.log");
    Log("KenshiCoop %s starting", kVersion);
    g_prevFilter = SetUnhandledExceptionFilter(&CrashFilter);

    // Never patch a game build we have not analysed.
    std::string sha;
    if (!Sha256File(dir + L"kenshi_x64.exe", sha)) { Log("cannot read kenshi_x64.exe, disabled"); return false; }
    if (sha != kenshi::kSupportedExeSha256) {
        Log("unsupported kenshi_x64.exe (sha256 %s), KenshiCoop disabled. Supported: Kenshi 1.0.68 Steam.", sha.c_str());
        return false;
    }
    std::string err;
    if (!kenshi::Init(&err)) { Log("disabled: %s", err.c_str()); return false; }

    g_iniPath = dir + L"KenshiCoop.ini";
    g_cfg = LoadConfig(g_iniPath);
    steam::Init(g_cfg.steamLoopback);
    g_startedAt = NowSeconds();
    g_overlayVisible = g_cfg.overlay;
    g_world = std::make_unique<KenshiWorld>(g_cfg);
    g_world->SetGameBuild(std::stoull(sha.substr(0, 16), nullptr, 16));

    kc::SessionConfig sc;
    sc.name = g_cfg.name;
    sc.port = g_cfg.port;
    sc.snapDistance = g_cfg.snapDistance;
    sc.interestRadius = g_cfg.interestRadius;
    sc.characterPerPlayer = g_cfg.characterPerPlayer;
    g_session = std::make_unique<kc::Session>(*g_world, sc, NowSeconds, [](const std::string& s) { Log("%s", s.c_str()); });

    if (!InstallHooks(&TickEntry, &err)) { Log("disabled: %s", err.c_str()); g_session.reset(); g_world.reset(); return false; }
    if (!OverlayInstall(&err)) Log("overlay unavailable: %s (multiplayer still works, see this log)", err.c_str());
    if (!InstallFrameListener()) Log("frame listener unavailable: joining from the main menu will not work");
    Log("ready. name='%s' port=%u join=%s", g_cfg.name.c_str(), g_cfg.port, g_cfg.joinAddress.c_str());
    return true;
}

} // namespace

KenshiWorld* TheWorld() { return g_world.get(); }

} // namespace kcp

extern "C" __declspec(dllexport) void dllStartPlugin() {
    // Keep the DLL mapped for the life of the process: hooks may still be on a thread's stack
    // while Ogre shuts plugins down.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&dllStartPlugin), &self);
    try {
        kcp::Start();
    } catch (const std::exception& e) {
        kcp::Log("startup exception: %s", e.what());
    } catch (...) {
        kcp::Log("startup exception");
    }
}

extern "C" __declspec(dllexport) void dllStopPlugin() {
    if (kcp::g_session) kcp::g_session->Leave();
    kcp::OverlayShutdown();
    kcp::RemoveHooks();
    kcp::Log("stopped");
    kcp::LogClose();
}
