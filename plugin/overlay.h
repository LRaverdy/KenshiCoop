// Overlay drawn with Dear ImGui on top of Kenshi's D3D11 swap chain: a read-only status panel,
// plus two windows the player can use with mouse and keyboard (the "Multijoueur" window and the
// console). Kenshi reads input through DirectInput: while one of our windows has the mouse or a
// text field, the game's DirectInput reads come back empty, so typing never drives the game.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace kcp {

struct OverlayPlayer {
    uint8_t id = 0;
    std::string name;
    uint32_t pingMs = 0;
    size_t characters = 0;
    bool you = false;
    // host only (Administration section)
    bool god = false;
    size_t down = 0;    // characters lying (knocked out, dead...): a teleport asks to confirm
};

struct OverlayModel {
    bool visible = true;
    std::string title;
    std::vector<std::string> lines;     // status block
    std::vector<std::string> chat;      // recent events
    std::vector<std::string> toasts;    // transient warnings (already filtered by age)
    // "Multijoueur" window
    bool worldLoaded = false;           // a game is loaded: hosting is possible
    bool active = false;                // a session is running (hosting or joined)
    bool hosting = false;
    std::string stateText;              // where the session stands, in French
    std::string errorText;              // why the last session ended, in French (empty: none)
    bool leftHostWorld = false;         // a client that left: what it sees is only a copy of the host's world
    float download = -1;                // 0..1 while the host's world is downloading
    std::vector<std::string> queueLines; // join queue: our place (client) / everyone in it (host)
    std::vector<OverlayPlayer> players; // everyone in the session, us included
    std::string name, address;          // current settings
    uint16_t port = 0;
    bool fullConsole = false;           // host: every console command; client: read-only ones
    // host: the Administration section (god mode, teleports, experience, healing, money)
    bool godAll = false;                // god mode for everyone is on
    std::string movePoint;              // "x, z" of the host's last move order (empty: none)
    // Steam: our id (the code friends join with) and friends hosting a session right now
    std::string steamId;
    std::vector<std::pair<std::string, std::string>> steamFriends;   // name, id
    // conversation of one of our characters, held in the host's world
    bool dialogOpen = false;
    uint32_t dialogId = 0;
    std::string dialogName, dialogText;
    std::vector<std::string> dialogReplies;
    bool dialogWaiting = false;
    // "Diplomatie" window: the host's relations, bounties and world, ready to show (French)
    struct DiploRelation {
        std::string name;
        int relation = 0;
        int standing = 0;               // kc::Standing: 0 neutral, 1 ally, 2 enemy
        bool war = false;
    };
    bool diploHave = false;             // the host's values are known here
    std::string diploHeader;            // rank, reputation
    std::vector<DiploRelation> diploRelations;
    std::vector<std::string> diploBounties, diploWorld;
    // "Affichage" settings (map markers, minimap, pings...), shown in the Multijoueur window
    bool optMap = true, optHeads = true, optPortraits = true, optMinimap = true, optMinimapRotate = false, optPings = true;
    int optMinimapCorner = 1;
};

// What the player did in our windows; carried out on the game thread.
struct OverlayAction {
    enum class Kind { Host, Join, Leave, Command, DialogAnswer, EditCharacter, QuitGame, Ping, SetOption } kind = Kind::Command;
    std::string name, address, text;
    uint16_t port = 0;
    int index = 0;                      // DialogAnswer; Ping: kc::PingKind
    float x = 0, y = 0, z = 0;          // Ping: where
    float value = 0;                    // SetOption: text = the option's key ([ui] in KenshiCoop.ini)
};

// The host's item spawner list (Administration section): every item template the game can make,
// published once per loaded world (empty: none, not hosting).
struct OverlayItem {
    std::string sid, name;
    int category = 0;   // kc::admin::ItemCat
    int stack = 1;      // the most one stack holds
};
struct OverlayMaker {
    std::string sid, name;
    std::vector<std::string> weapons;                        // weapon template sids it makes
    std::vector<std::pair<std::string, std::string>> models; // sid, name; best first
};
void OverlayPublishItems(std::vector<OverlayItem> items, std::vector<OverlayMaker> makers);

bool OverlayInstall(std::string* err);      // hooks IDXGISwapChain::Present / ResizeBuffers, DirectInput reads
void OverlayPublish(OverlayModel model);    // game thread -> render thread
std::vector<OverlayAction> OverlayTakeActions();
void OverlayToggleMultiplayer();
void OverlayToggleConsole();
void OverlayToggleDiplomacy();
bool OverlayDiplomacyOpen();
void OverlayOpenMultiplayer();
bool OverlayTyping();                       // a text field of ours has the keyboard
// The map layer (plugin/map_view.h): game thread -> render thread, every frame.
struct MapScene;
void OverlayPublishScene(MapScene scene);
void OverlayScreenSize(float& w, float& h);  // the back buffer, as last drawn
void OverlayWindowInfo(int& clientW, int& clientH, int& dpi);   // the game window's client area and DPI, as last drawn
void OverlayPushAction(OverlayAction a);     // from the overlay's own layers
void OverlayShutdown();

} // namespace kcp
