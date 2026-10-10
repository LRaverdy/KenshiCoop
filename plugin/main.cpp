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
#include "host_console.h"
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
bool g_leftHostWorld = false;   // a client left the host's world: what it shows is only a copy
bool JoinAndRemember(const std::string& address, uint16_t port, std::string* err);
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

std::string FrenchError(const std::string& e);
const char* FrenchState(kc::SessionState s);

void Toast(const std::string& msg, double seconds = 4.0) {
    const double now = NowSeconds();
    for (auto& t : g_toasts) if (t.first == msg) { t.second = now + seconds; return; }
    g_toasts.emplace_back(msg, now + seconds);
    while (g_toasts.size() > 4) g_toasts.pop_front();
    Log("toast shown: \"%s\"", msg.c_str());
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
Hotkey g_hkHost{'H'}, g_hkJoin{'J'}, g_hkLeave{'L'}, g_hkGive{'G'}, g_hkOverlay{'O'}, g_hkDiag{'D'}, g_hkMultiplayer{'M'}, g_hkConsoleWindow{'W'},
    g_hkConsole{'K'};

bool Pressed(Hotkey& k, bool modifiers) {
    const bool now = modifiers && KeyDown(k.vk);
    const bool edge = now && !k.down;
    k.down = now;
    return edge;
}

void GiveSelectedToNextPlayer() {
    if (!g_session->isHost()) { Toast("Seul l'hôte peut confier des personnages aux joueurs."); return; }
    std::vector<kc::Handle> sel;
    kenshi::SelectedHandles(sel);
    std::vector<kc::Handle> mine;
    for (auto& h : sel) if (g_world->FindSquad(h)) mine.push_back(h);
    if (mine.empty()) { Toast("Sélectionne d'abord un membre de ton escouade."); return; }
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
        if (g_session->isHost()) Toast("Tu héberges déjà la partie.");
        else if (g_session->Host(&err)) Toast("Partie hébergée (port " + std::to_string(g_cfg.port) + ").");
        else Toast("Impossible d'héberger : " + FrenchError(err));
    }
    if (Pressed(g_hkJoin, mods)) {
        if (g_session->isClient()) Toast("Tu es déjà connecté.");
        else if (JoinAndRemember(g_cfg.joinAddress, g_cfg.port, &err)) Toast("Connexion à " + g_cfg.joinAddress + "...");
        else Toast("Impossible de rejoindre : " + FrenchError(err));
    }
    if (Pressed(g_hkLeave, mods)) {
        g_session->Leave();
        Toast("Tu as quitté la session.");
    }
    if (Pressed(g_hkGive, mods)) GiveSelectedToNextPlayer();
    if (Pressed(g_hkDiag, mods)) { DumpDiagnostics("hotkey"); Toast("Diagnostic écrit dans KenshiCoop.log"); }
    if (Pressed(g_hkConsoleWindow, mods)) HostConsoleShow(!HostConsoleVisible());
}

// ---- the Multijoueur window and the console (French: what the players read)
const char* FrenchPhase(kc::JoinPhase p) {
    switch (p) {
    case kc::JoinPhase::Saving: return "préparation du monde";
    case kc::JoinPhase::Loading: return "chargement du monde";
    case kc::JoinPhase::Editor: return "création du personnage";
    }
    return "";
}

// The join queue in French: a waiting client's place, or (host) everyone in it.
std::vector<std::string> QueueLines() {
    std::vector<std::string> out;
    if (const kc::JoinQueueMsg* q = g_session->queueStatus()) {
        out.push_back("File d'attente : position " + std::to_string(q->position) + "/" + std::to_string(q->total) + " — en attente de " + q->current +
                      " (" + FrenchPhase(q->phase) + ")…");
    } else if (g_session->isHost()) {
        const auto list = g_session->joinQueue();
        if (!list.empty()) out.push_back("File d'attente des arrivées :");
        for (size_t i = 0; i < list.size(); ++i)
            out.push_back("  " + std::to_string(i + 1) + ". " + list[i].name + " — " + (list[i].waiting ? std::string("attend son tour") : FrenchPhase(list[i].phase)));
    }
    return out;
}

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
        out("fenetre              ouvrir / fermer la console hors du jeu (aussi Ctrl+Shift+W)");
        out("players              joueurs : id, nom, ping, personnages");
        out("status               état de la session et de la synchro");
        if (!client) {
            out("kick <id>            retirer un joueur (hôte)");
            out("give <id>            donner les personnages sélectionnés au joueur <id> (hôte)");
            out("save                 sauvegarder la partie");
            out("pause [0|1]          mettre en pause / reprendre");
            out("speed <x>            vitesse du jeu (1, 2, 3...)");
            out("tp <id> [vers <id>]  téléporter les persos du joueur <id> près de ton perso sélectionné (ou d'un autre joueur)");
            out("resync [id]          le joueur <id> (sans id : tout le monde) recharge ton monde tel qu'il est");
            out("heal <id|all>        soigne complètement les persos du joueur <id> (all : toute l'escouade)");
            out("xp <id|all> <n>      +n niveaux dans toutes les compétences (ex. xp 2 10)");
            out("god <id|all> [off]   mode dieu : plus aucun dégât ni K.-O. (off pour l'enlever)");
            out("money <n>            ajoute n cats à l'argent commun (négatif pour en retirer)");
        }
        return;
    }
    if (cmd == "fenetre" || cmd == "window") {
        HostConsoleShow(!HostConsoleVisible());
        out(HostConsoleVisible() ? "console externe ouverte" : "console externe fermee");
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
        for (const auto& q : QueueLines()) out(q);
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
    if (cmd == "resync") {
        int id = 0;
        in >> id;
        if (!host) { out("pas de partie hébergée"); return; }
        const size_t n = g_session->RequestResync(uint8_t(std::clamp(id, 0, 255)));
        out(n ? std::to_string(n) + " joueur(s) rechargent ton monde" : "aucun joueur en jeu");
        return;
    }
    if (cmd == "money" || cmd == "argent") {   // admin: cats for the player faction (everyone shares them)
        long long n = 0;
        in >> n;
        if (!host) { out("seul l'hôte peut faire ça"); return; }
        int32_t cur = 0;
        if (n == 0 || !kenshi::ReadPlayerMoney(cur)) { out("usage : money <n>  (ex. money 5000)"); return; }
        const long long next = std::max(0LL, std::min(2000000000LL, (long long)cur + n));
        kenshi::WritePlayerMoney(int32_t(next));
        out("argent : " + std::to_string(cur) + " -> " + std::to_string(next) + " cats");
        Log("admin: money %lld -> %lld", (long long)cur, next);
        return;
    }
    if (cmd == "heal" || cmd == "xp" || cmd == "god") {   // admin: heal / xp / god mode on a player's characters
        std::string who, arg;
        in >> who >> arg;
        if (!host) { out("seul l'hôte peut faire ça"); return; }
        const bool all = who == "all" || who == "tous";
        int id = -1;
        if (!all) { try { id = std::stoi(who); } catch (...) { id = -1; } }
        if (!all && (id < 0 || id > 255)) { out("usage : " + cmd + " <id|all> ...  (tape players pour les numéros)"); return; }
        std::vector<kc::Handle> squad;
        g_world->PlayerCharacters(squad);
        int n = 0;
        for (const auto& h : squad) {
            if (!all && g_session->ownerOf(h) != id) continue;
            kenshi::Character* c = g_world->FindSquad(h);
            if (!c) continue;
            HostCallScope scope;
            if (cmd == "heal") n += kenshi::HealCompletely(c) ? 1 : 0;
            else if (cmd == "god") { kenshi::SetGodMode(c, arg != "off"); if (arg != "off") kenshi::HealCompletely(c); ++n; }
            else {
                float levels = 0;
                try { levels = std::stof(arg); } catch (...) { levels = 0; }
                std::vector<float> stats;
                if (levels <= 0 || !kenshi::ReadStats(c, stats)) continue;
                for (auto& v : stats) v = std::min(100.0f, v + levels);
                kenshi::WriteStats(c, stats);
                ++n;
            }
        }
        const std::string what = cmd == "heal" ? "soigné(s)" : cmd == "xp" ? "monté(s) de niveau" : (arg == "off" ? "sans mode dieu" : "en mode dieu");
        out(std::to_string(n) + " personnage(s) " + what);
        Log("admin: %s %s %s -> %d character(s)", cmd.c_str(), who.c_str(), arg.c_str(), n);
        return;
    }
    if (cmd == "tp") {   // tp <id> [<toId>]: unstick a player's characters next to my selection (or another player's)
        int id = -1, to = -1;
        in >> id >> to;
        if (!host || id < 0 || id > 255) { out("usage : tp <id> [vers <id>]  (tape players pour les numéros)"); return; }
        std::vector<kc::Handle> squad, who;
        g_world->PlayerCharacters(squad);
        for (const auto& h : squad) if (g_session->ownerOf(h) == id) who.push_back(h);
        if (who.empty()) { out("ce joueur n'a pas de personnage"); return; }
        kc::Vec3 dest;
        bool have = false;
        if (to >= 0) {
            for (const auto& h : squad)
                if (g_session->ownerOf(h) == to)
                    if (kenshi::Character* c = g_world->FindSquad(h); c && kenshi::GetPosition(c, dest)) { have = true; break; }
        } else {
            std::vector<kc::Handle> sel;
            kenshi::SelectedHandles(sel);
            for (const auto& h : sel)
                if (kenshi::Character* c = g_world->FindSquad(h); c && kenshi::GetPosition(c, dest)) { have = true; break; }
            for (const auto& h : squad)
                if (!have && g_session->ownerOf(h) == g_session->localId())
                    if (kenshi::Character* c = g_world->FindSquad(h); c && kenshi::GetPosition(c, dest)) have = true;
        }
        if (!have) { out("destination introuvable : sélectionne un de tes persos"); return; }
        // its game loads the destination zone at once and freezes meanwhile: the link must wait
        if (id != g_session->localId()) g_session->ExpectStall(uint8_t(id), 120.0);
        const int n = g_world->TeleportCharacters(who, dest);
        out(std::to_string(n) + " personnage(s) du joueur " + std::to_string(id) + " téléporté(s)");
        Toast(std::to_string(n) + " personnage(s) téléporté(s).");
        return;
    }
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
            if (JoinAndRemember(g_cfg.joinAddress, g_cfg.port, &err)) Toast("Connexion à " + g_cfg.joinAddress + "...");
            else Toast("Impossible de rejoindre : " + FrenchError(err));
            break;
        case OverlayAction::Kind::Leave:
            g_session->Leave();
            Toast("Session quittée.");
            break;
        case OverlayAction::Kind::Command:
            ConsoleCommand(a.text);
            break;
        case OverlayAction::Kind::QuitGame:
            Log("the player quits the game from the Multijoueur window");
            LogClose();
            ExitProcess(0);
        case OverlayAction::Kind::EditCharacter:
            if (!g_session->EditOwnCharacter()) Toast("Ton personnage n'est pas encore là.");
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
    m.title = std::string("KenshiCoop ") + kVersion + "  -  " + FrenchState(g_session->state());
    const auto st = g_session->state();
    m.queueLines = QueueLines();
    if (st == kc::SessionState::Downloading && g_session->queueStatus()) {
        m.lines.insert(m.lines.end(), m.queueLines.begin(), m.queueLines.end());
    } else if (st == kc::SessionState::Downloading) {
        m.lines.push_back("Réception du monde de l'hôte : " + std::to_string(int(g_session->downloadProgress() * 100)) + " %");
    } else if (st == kc::SessionState::Loading || st == kc::SessionState::Connecting || st == kc::SessionState::Handshake) {
        m.lines.push_back("Patiente...");
    } else if (st == kc::SessionState::Idle || st == kc::SessionState::Failed) {
        if (st == kc::SessionState::Failed && !g_session->lastError().empty()) m.lines.push_back(FrenchError(g_session->lastError()));
        m.lines.push_back("Ctrl+Shift+H  héberger cette partie");
        m.lines.push_back("Ctrl+Shift+J  rejoindre " + g_cfg.joinAddress + ":" + std::to_string(g_cfg.port) + " (marche depuis le menu principal)");
        m.lines.push_back("Ctrl+Shift+M  fenêtre Multijoueur   Ctrl+Shift+K  console");
        m.lines.push_back("Ctrl+Shift+O  masquer ce panneau");
    } else {
        m.lines.push_back("Toi : " + g_cfg.name + " (joueur " + std::to_string(g_session->localId()) + ")" +
                          (g_session->isHost() ? "" : "   ping " + std::to_string(g_session->pingMs()) + " ms"));
        for (auto& [id, p] : g_session->players())
            m.lines.push_back("  joueur " + std::to_string(id) + " : " + p.name + (g_session->isHost() ? "   ping " + std::to_string(p.rttMs) + " ms" : ""));
        size_t mine = 0;
        std::vector<kc::Handle> hs;
        g_world->PlayerCharacters(hs);
        for (auto& h : hs) if (g_session->ownerOf(h) == g_session->localId()) ++mine;
        m.lines.push_back("Escouade : " + std::to_string(hs.size()) + " personnages, dont " + std::to_string(mine) + " à toi");
        m.lines.push_back("Monde : " + std::to_string(g_session->npcCount()) + " PNJ synchronisés" +
                          (g_session->isClient() && g_session->missingNpcs() ? " (" + std::to_string(g_session->missingNpcs()) + " pas encore là)" : ""));
        if (g_session->missingSquad())
            m.lines.push_back("ATTENTION : " + std::to_string(g_session->missingSquad()) + " membres de l'escouade manquent ici (charge la sauvegarde de l'hôte)");
        if (g_session->isHost() && g_session->joiningPlayers())
            m.lines.push_back(std::to_string(g_session->joiningPlayers()) + " joueur(s) en train d'arriver : partie en pause");
        if (g_session->isHost()) m.lines.insert(m.lines.end(), m.queueLines.begin(), m.queueLines.end());
        if (g_session->isHost()) m.lines.push_back("Ctrl+Shift+G  confier la sélection au joueur suivant");
        m.lines.push_back("Ctrl+Shift+L  quitter la session");
    }
    const auto& chat = g_session->chatLog();
    for (size_t i = chat.size() > 6 ? chat.size() - 6 : 0; i < chat.size(); ++i) m.chat.push_back(chat[i]);
    // the Multijoueur window
    m.worldLoaded = g_world->Ready();
    m.hosting = g_session->isHost();
    m.active = m.hosting || g_session->isClient();
    m.stateText = FrenchState(st);
    if (st == kc::SessionState::Failed && !g_session->lastError().empty()) m.errorText = FrenchError(g_session->lastError());
    m.leftHostWorld = g_leftHostWorld && g_world->Ready();
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
        else if (JoinAndRemember("steam:" + std::to_string(*id), g_cfg.port, &err)) Toast("Connexion à ton ami via Steam...");
        else Toast("Impossible de rejoindre : " + FrenchError(err));
    }
}

// Client: every new line of our log goes to the host's log too (what goes wrong on each machine is
// then in one place, live). Host: the console window shows the session and runs its commands.
uint64_t g_forwardedSeq = 0;
bool g_consoleAutoOpened = false;
void LogAndConsoleUpkeep() {
    uint64_t seq = 0;
    RecentLog(0, &seq);
    if (g_session->isClient() && seq > g_forwardedSeq) {
        const auto lines = RecentLog(size_t(std::min<uint64_t>(seq - g_forwardedSeq, 200)), &seq);
        for (const auto& l : lines) {
            if (l.find("debug: ") != std::string::npos) continue;   // the test channel
            g_session->QueueLog(l.size() > 10 ? l.substr(10) : l);   // without our clock: the host stamps it
        }
    }
    g_forwardedSeq = seq;
    if (g_session->isHost() && g_cfg.hostConsole && !g_consoleAutoOpened) {
        g_consoleAutoOpened = true;
        HostConsoleShow(true);
    }
    for (auto& cmd : HostConsoleTakeCommands()) ConsoleCommand(cmd);
    static double nextPublish = 0;
    const double now = NowSeconds();
    if (!HostConsoleVisible() || now < nextPublish) return;
    nextPublish = now + 0.5;
    ConsoleModel m;
    char status[256];
    snprintf(status, sizeof(status), "%s   |   %zu entites, %zu PNJ   |   vitesse %.1f%s   |   %s", FrenchState(g_session->state()), g_session->entityCount(),
             g_session->npcCount(), kenshi::GetFrameSpeed(), kenshi::GetPaused() ? " (PAUSE)" : "", g_cfg.name.c_str());
    m.status = status;
    if (g_session->isHost() || g_session->isClient()) {
        ConsolePlayer me;
        me.id = g_session->localId();
        me.name = g_cfg.name + " (toi)";
        me.characters = CharactersOf(me.id);
        me.state = g_session->isHost() ? "hote" : "en jeu";
        m.players.push_back(me);
    }
    for (auto& [id, p] : g_session->players()) {
        ConsolePlayer cp;
        cp.id = id;
        cp.name = p.name;
        cp.pingMs = p.rttMs;
        cp.characters = CharactersOf(id);
        cp.state = !p.inGame ? "arrive (telechargement)" : p.editing ? "cree son personnage" : "en jeu";
        if (p.reportAt >= 0) {
            const auto& r = p.report;
            char b[200];
            snprintf(b, sizeof(b), "%u suivis, %u PNJ en attente, %u escouade manquants, %u decales, corr. max %.1f, %u img/s (il y a %.0f s)",
                     unsigned(r.entities), unsigned(r.missingNpcs), unsigned(r.missingSquad), unsigned(r.farOff), double(r.maxErr), unsigned(r.fps), now - p.reportAt);
            cp.sync = b;
            cp.warn = r.missingSquad > 0 || r.farOff > 3 || r.maxErr > 30.0f || (r.fps > 0 && r.fps < 15) || now - p.reportAt > 20.0;
        } else {
            cp.sync = "pas encore de rapport";
        }
        m.players.push_back(cp);
    }
    HostConsolePublish(std::move(m));
}

bool g_clientInWorld = false;
double g_rejoinAt = -1;          // resync: when to join again
bool JoinAndRemember(const std::string& address, uint16_t port, std::string* err) {
    return steam::JoinAddress(*g_session, address, port, err);   // which remembers it for a resync
}

// The host asked us to reload its world: leave, then join the same host again a moment later.
void ResyncUpkeep() {
    std::string address;
    uint16_t port = 0;
    if (g_session->TakeResyncRequest() && steam::LastJoin(address, port)) {
        g_clientInWorld = false;   // not a departure: no "you left" message
        g_session->Leave();
        g_rejoinAt = NowSeconds() + 1.5;
        Toast("L'hôte resynchronise la partie : rechargement de son monde...");
    }
    if (g_rejoinAt > 0 && NowSeconds() >= g_rejoinAt) {
        g_rejoinAt = -1;
        std::string err;
        if (!steam::LastJoin(address, port) || !JoinAndRemember(address, port, &err)) Toast("Resynchronisation impossible : " + FrenchError(err));
    }
}

void AfterLeavingHostWorld() {
    if (g_rejoinAt > 0) return;   // a resync is under way
    const bool inWorld = g_session->state() == kc::SessionState::Connected;
    if (inWorld || g_session->isHost()) g_leftHostWorld = false;
    if (inWorld) { g_clientInWorld = true; return; }
    if (!g_clientInWorld || g_session->isClient()) return;
    g_clientInWorld = false;
    Log("left the host's world: the local copy is paused (Escape > Load to play one of our own games)");
    if (g_world->Ready() && !kenshi::GetPaused()) kenshi::CallUserPause(true);
    g_leftHostWorld = true;
    OverlayOpenMultiplayer();
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
    if (live) g_session->CountFrame();
    LogAndConsoleUpkeep();
    g_session->Tick(live);
    ResyncUpkeep();
    AfterLeavingHostWorld();
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
        // The game's main loop stops while the game is paused: a loaded, paused world still ticks
        // as live here (clients keep imposing the host's state, the host keeps sending it).
        if (NowSeconds() - LastLiveTick() > 0.2) RunTick(g_wasReady && kenshi::GetPaused() && kenshi::Player() != nullptr);
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
    // tests: several games on one PC share one KenshiCoop.ini; the harness gives each its own name
    // (debug channel only, never in normal play; not written back to the ini)
    if (char buf[64]; g_cfg.debugCommands && GetEnvironmentVariableA("KC_PLAYER_NAME", buf, sizeof(buf)) > 0 && kc::ValidName(buf)) {
        g_cfg.name = buf;
        Log("test: player name from KC_PLAYER_NAME: '%s'", buf);
    }
    g_world = std::make_unique<KenshiWorld>(g_cfg);
    g_world->SetGameBuild(std::stoull(sha.substr(0, 16), nullptr, 16));

    kc::SessionConfig sc;
    sc.name = g_cfg.name;
    sc.port = g_cfg.port;
    sc.snapDistance = g_cfg.snapDistance;
    sc.interestRadius = g_cfg.interestRadius;
    sc.characterPerPlayer = g_cfg.characterPerPlayer;
    sc.steamId = steam::MyId();
    // tests: two games on one PC share one Steam account; the harness gives each a fake one
    if (char buf[32]; g_cfg.debugCommands && GetEnvironmentVariableA("KC_FAKE_STEAM_ID", buf, sizeof(buf)) > 0) sc.steamId = std::stoull(buf);
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
    kcp::HostConsoleShutdown();
    kcp::OverlayShutdown();
    kcp::RemoveHooks();
    kcp::Log("stopped");
    kcp::LogClose();
}
