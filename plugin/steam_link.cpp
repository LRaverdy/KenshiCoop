#include "steam_link.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "kc/session.h"
#include "util.h"

namespace kcp::steam {

namespace {

// ---- the Steam API Kenshi loaded (flat C entry points of steam_api64.dll)
using FnPtr = void* (*)();
using FnInt = int (*)();
using FnGetIface = void* (*)(void* client, int user, int pipe, const char* version);
using FnU64 = uint64_t (*)(void* self);
using FnFriendCount = int (*)(void* self, int flags);
using FnFriendByIndex = uint64_t (*)(void* self, int index, int flags);
using FnFriendName = const char* (*)(void* self, uint64_t id);
using FnName = const char* (*)(void* self);
using FnRichGet = const char* (*)(void* self, uint64_t id, const char* key);
using FnRichSet = bool (*)(void* self, const char* key, const char* value);
using FnSelf = void (*)(void* self);
using FnSend = bool (*)(void* self, uint64_t to, const void* data, uint32_t size, int sendType, int channel);
using FnAvail = bool (*)(void* self, uint32_t* size, int channel);
using FnRead = bool (*)(void* self, void* dest, uint32_t cap, uint32_t* size, uint64_t* from, int channel);
using FnPeer = bool (*)(void* self, uint64_t id);
using FnRegister = void (*)(void* cb, int id);
using FnRun = void (*)();

struct Api {
    void* friends = nullptr;
    void* net = nullptr;
    void* user = nullptr;
    FnU64 getSteamId = nullptr;
    FnFriendCount friendCount = nullptr;
    FnFriendByIndex friendByIndex = nullptr;
    FnFriendName friendName = nullptr;
    FnName personaName = nullptr;
    FnRichGet richGet = nullptr;
    FnRichSet richSet = nullptr;
    FnSelf richClear = nullptr;
    FnSend send = nullptr;
    FnAvail avail = nullptr;
    FnRead read = nullptr;
    FnPeer accept = nullptr;
    FnPeer close = nullptr;
    FnRegister registerCb = nullptr;
    FnRun runCallbacks = nullptr;
};
Api g_api;
bool g_steam = false;          // the Steam API answered
bool g_loopback = false;       // [debug] steam_loopback=1
uint64_t g_myId = 0;
std::string g_lastAddress;   // what was joined last: a resync joins it again
uint16_t g_lastPort = 0;

constexpr int kFriendFlagImmediate = 4;   // k_EFriendFlagImmediate
constexpr int kSendUnreliable = 0;        // k_EP2PSendUnreliable (ENet does the reliability)
constexpr uint16_t kLoopbackHostPort = 28100;

template <typename T>
bool Get(HMODULE m, const char* name, T& out) {
    out = reinterpret_cast<T>(GetProcAddress(m, name));
    return out != nullptr;
}

// Steam's callback object (CCallbackBase in the Steamworks headers): Steam calls Run(param).
class CallbackBase {
public:
    virtual void Run(void* param) = 0;
    virtual void Run(void* param, bool ioFailure, uint64_t call) = 0;
    virtual int GetCallbackSizeBytes() = 0;

protected:
    uint8_t flags_ = 0;
    int id_ = 0;
};

std::mutex g_reqMutex;
std::optional<uint64_t> g_joinRequest;

// P2PSessionRequest_t (1202): someone opened a P2P session to us: a joining player.
class SessionRequest final : public CallbackBase {
public:
    void Run(void* param) override {
        uint64_t from = 0;
        std::memcpy(&from, param, 8);
        if (g_api.accept) g_api.accept(g_api.net, from);
        Log("steam: P2P session from %llu accepted", static_cast<unsigned long long>(from));
    }
    void Run(void* param, bool, uint64_t) override { Run(param); }
    int GetCallbackSizeBytes() override { return 8; }
};
// GameRichPresenceJoinRequested_t (337): "Rejoindre la partie" in the Steam friends list.
class JoinRequested final : public CallbackBase {
public:
    void Run(void* param) override {
        const char* connect = static_cast<const char*>(param) + 8;
        uint64_t id = 0;
        if (const char* p = std::strstr(connect, "+kc_join "); p && ParseAddress(p + 9, id)) {
            std::lock_guard<std::mutex> lk(g_reqMutex);
            g_joinRequest = id;
        }
    }
    void Run(void* param, bool, uint64_t) override { Run(param); }
    int GetCallbackSizeBytes() override { return 8 + 256; }
};
SessionRequest g_sessionRequest;
JoinRequested g_joinRequested;

// ---- the transport: Steam P2P, or local UDP in loopback test mode (a "Steam id" is then a port)
SOCKET g_loopSock = INVALID_SOCKET;

bool TransportSend(uint64_t to, const void* data, uint32_t size) {
    if (g_loopback) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(uint16_t(to));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        return sendto(g_loopSock, static_cast<const char*>(data), int(size), 0, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == int(size);
    }
    return g_api.send(g_api.net, to, data, size, kSendUnreliable, 0);
}

// One packet, or 0 when none is waiting.
uint32_t TransportRecv(uint64_t& from, uint8_t* buf, uint32_t cap) {
    if (g_loopback) {
        sockaddr_in a{};
        int alen = sizeof(a);
        const int n = recvfrom(g_loopSock, reinterpret_cast<char*>(buf), int(cap), 0, reinterpret_cast<sockaddr*>(&a), &alen);
        if (n <= 0) return 0;
        from = ntohs(a.sin_port);
        return uint32_t(n);
    }
    uint32_t size = 0;
    if (!g_api.avail(g_api.net, &size, 0) || size == 0) return 0;
    uint32_t got = 0;
    if (!g_api.read(g_api.net, buf, cap, &got, &from, 0)) return 0;
    return got;
}

SOCKET LocalSocket(uint16_t port, bool connectTo, uint16_t& boundPort) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return s;
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = connectTo ? 0 : htons(port);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    int alen = sizeof(a);
    getsockname(s, reinterpret_cast<sockaddr*>(&a), &alen);
    boundPort = ntohs(a.sin_port);
    if (connectTo) {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        to.sin_port = htons(port);
        connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to));
    }
    return s;
}

// ---- the relay (its own thread: a packet waits at most a millisecond, whatever the frame rate)
struct Link {
    bool host = false;
    uint16_t gamePort = 0;          // host: the session's ENet port
    uint64_t hostId = 0;            // client: who we play with
    SOCKET local = INVALID_SOCKET;  // client: the port ENet connects to
    sockaddr_in enetAddr{};         // client: where our ENet socket is
    bool haveEnet = false;
    struct Peer { SOCKET s = INVALID_SOCKET; double last = 0; };
    std::unordered_map<uint64_t, Peer> peers;   // host: one local socket per player
    std::atomic<bool> run{false};
    std::thread thread;
    std::atomic<uint64_t> packetsIn{0}, packetsOut{0};
};
Link* g_link = nullptr;
std::mutex g_linkMutex;

void Pump(Link& l) {
    uint8_t buf[2048];
    double nextSweep = 0;
    while (l.run) {
        // Steam -> local
        for (int i = 0; i < 256; ++i) {
            uint64_t from = 0;
            const uint32_t n = TransportRecv(from, buf, sizeof(buf));
            if (!n) break;
            ++l.packetsIn;
            if (l.host) {
                auto it = l.peers.find(from);
                if (it == l.peers.end()) {
                    if (l.peers.size() >= 16) continue;
                    uint16_t bound = 0;
                    Link::Peer p;
                    p.s = LocalSocket(l.gamePort, true, bound);
                    if (p.s == INVALID_SOCKET) continue;
                    it = l.peers.emplace(from, p).first;
                    Log("steam: player %llu relayed through local port %u", static_cast<unsigned long long>(from), bound);
                }
                it->second.last = NowSeconds();
                send(it->second.s, reinterpret_cast<const char*>(buf), int(n), 0);
            } else if (from == l.hostId && l.haveEnet) {
                sendto(l.local, reinterpret_cast<const char*>(buf), int(n), 0, reinterpret_cast<sockaddr*>(&l.enetAddr), sizeof(l.enetAddr));
            }
        }
        // local -> Steam
        fd_set rd;
        FD_ZERO(&rd);
        if (l.host) for (auto& [id, p] : l.peers) FD_SET(p.s, &rd);
        else FD_SET(l.local, &rd);
        if (rd.fd_count == 0) { Sleep(1); continue; }
        timeval tv{0, 1000};
        if (select(0, &rd, nullptr, nullptr, &tv) > 0) {
            if (l.host) {
                for (auto& [id, p] : l.peers)
                    for (int i = 0; i < 64; ++i) {
                        const int n = recv(p.s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
                        if (n <= 0) break;
                        if (TransportSend(id, buf, uint32_t(n))) ++l.packetsOut;
                    }
            } else {
                for (int i = 0; i < 64; ++i) {
                    sockaddr_in a{};
                    int alen = sizeof(a);
                    const int n = recvfrom(l.local, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&a), &alen);
                    if (n <= 0) break;
                    l.enetAddr = a;
                    l.haveEnet = true;
                    if (TransportSend(l.hostId, buf, uint32_t(n))) ++l.packetsOut;
                }
            }
        }
        // players gone quiet for a minute: their relay closes (ENet gave up long before)
        const double now = NowSeconds();
        if (l.host && now > nextSweep) {
            nextSweep = now + 5.0;
            for (auto it = l.peers.begin(); it != l.peers.end();) {
                if (now - it->second.last > 60.0) {
                    closesocket(it->second.s);
                    if (!g_loopback) g_api.close(g_api.net, it->first);
                    it = l.peers.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }
}

bool StartLink(Link* l, std::string* err) {
    if (g_loopback && g_loopSock == INVALID_SOCKET) {
        uint16_t bound = 0;
        g_loopSock = LocalSocket(l->host ? kLoopbackHostPort : 0, false, bound);
        if (g_loopSock == INVALID_SOCKET) {
            if (err) *err = "cannot open the loopback test port";
            return false;
        }
        g_myId = bound;
    }
    std::lock_guard<std::mutex> lk(g_linkMutex);
    l->run = true;
    l->thread = std::thread([l] { Pump(*l); });
    g_link = l;
    return true;
}

std::string IdText(uint64_t id) { return std::to_string(static_cast<unsigned long long>(id)); }

double g_nextFriendScan = 0;

} // namespace

void Init(bool loopbackTest) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_loopback = loopbackTest;
    if (loopbackTest) {
        g_steam = true;
        Log("steam: loopback test mode (no Steam: relayed over local UDP)");
    } else if (HMODULE m = GetModuleHandleW(L"steam_api64.dll")) {
        FnPtr steamClient = nullptr;
        FnInt hUser = nullptr, hPipe = nullptr;
        FnGetIface getFriends = nullptr, getNet = nullptr, getUser = nullptr;
        bool ok = Get(m, "SteamClient", steamClient) && Get(m, "SteamAPI_GetHSteamUser", hUser) && Get(m, "SteamAPI_GetHSteamPipe", hPipe) &&
                  Get(m, "SteamAPI_ISteamClient_GetISteamFriends", getFriends) &&
                  Get(m, "SteamAPI_ISteamClient_GetISteamNetworking", getNet) && Get(m, "SteamAPI_ISteamClient_GetISteamUser", getUser) &&
                  Get(m, "SteamAPI_ISteamUser_GetSteamID", g_api.getSteamId) &&
                  Get(m, "SteamAPI_ISteamFriends_GetFriendCount", g_api.friendCount) &&
                  Get(m, "SteamAPI_ISteamFriends_GetFriendByIndex", g_api.friendByIndex) &&
                  Get(m, "SteamAPI_ISteamFriends_GetFriendPersonaName", g_api.friendName) &&
                  Get(m, "SteamAPI_ISteamFriends_GetPersonaName", g_api.personaName) &&
                  Get(m, "SteamAPI_ISteamFriends_GetFriendRichPresence", g_api.richGet) &&
                  Get(m, "SteamAPI_ISteamFriends_SetRichPresence", g_api.richSet) &&
                  Get(m, "SteamAPI_ISteamFriends_ClearRichPresence", g_api.richClear) &&
                  Get(m, "SteamAPI_ISteamNetworking_SendP2PPacket", g_api.send) &&
                  Get(m, "SteamAPI_ISteamNetworking_IsP2PPacketAvailable", g_api.avail) &&
                  Get(m, "SteamAPI_ISteamNetworking_ReadP2PPacket", g_api.read) &&
                  Get(m, "SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser", g_api.accept) &&
                  Get(m, "SteamAPI_ISteamNetworking_CloseP2PSessionWithUser", g_api.close) &&
                  Get(m, "SteamAPI_RegisterCallback", g_api.registerCb) && Get(m, "SteamAPI_RunCallbacks", g_api.runCallbacks);
        void* client = ok ? steamClient() : nullptr;
        if (client) {
            const int u = hUser(), p = hPipe();
            g_api.friends = getFriends(client, u, p, "SteamFriends015");
            g_api.net = getNet(client, u, p, "SteamNetworking005");
            g_api.user = getUser(client, u, p, "SteamUser019");
        }
        if (g_api.friends && g_api.net && g_api.user) {
            g_myId = g_api.getSteamId(g_api.user);
            g_steam = g_myId != 0;
        }
        if (g_steam) {
            g_api.registerCb(&g_sessionRequest, 1202);   // P2PSessionRequest_t
            g_api.registerCb(&g_joinRequested, 337);     // GameRichPresenceJoinRequested_t
            Log("steam: ready (id %llu)", static_cast<unsigned long long>(g_myId));
        } else {
            Log("steam: the Steam API did not answer: joining by Steam is off (IP still works)");
        }
    }
    // Steam starts the game with "+kc_join <id>" when "Rejoindre la partie" is used while it is closed
    if (const char* cl = GetCommandLineA()) {
        uint64_t id = 0;
        if (const char* p = std::strstr(cl, "+kc_join "); p && ParseAddress(p + 9, id)) {
            std::lock_guard<std::mutex> lk(g_reqMutex);
            g_joinRequest = id;
        }
    }
}

bool Available() { return g_steam; }
uint64_t MyId() { return g_myId; }
std::string MyName() {
    if (g_loopback || !g_steam) return {};
    const char* n = g_api.personaName(g_api.friends);
    return n ? n : "";
}

bool ParseAddress(const std::string& address, uint64_t& id) {
    std::string s = address;
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    const bool prefixed = s.rfind("steam:", 0) == 0;
    if (prefixed) s = s.substr(6);
    size_t n = 0;
    while (n < s.size() && s[n] >= '0' && s[n] <= '9') ++n;
    if (n == 0 || n > 20 || (!prefixed && n != 17)) return false;   // a bare number must look like a Steam id
    id = std::stoull(s.substr(0, n));
    return id != 0;
}

bool StartHost(uint16_t gamePort, std::string* err) {
    if (!g_steam) {
        if (err) *err = "Steam is not available";
        return false;
    }
    Stop();
    auto* l = new Link;
    l->host = true;
    l->gamePort = gamePort;
    if (!StartLink(l, err)) { delete l; return false; }
    if (!g_loopback) {
        const std::string me = IdText(g_myId);
        g_api.richSet(g_api.friends, "kc_host", me.c_str());
        g_api.richSet(g_api.friends, "connect", ("+kc_join " + me).c_str());
        g_api.richSet(g_api.friends, "status", "KenshiCoop : partie ouverte");
    }
    Log("steam: hosting through Steam (id %llu)", static_cast<unsigned long long>(g_myId));
    return true;
}

bool Join(kc::Session& s, uint64_t hostId, std::string* err) {
    if (!g_steam) {
        if (err) *err = "Steam is not available";
        return false;
    }
    if (hostId == g_myId) {
        if (err) *err = "this Steam id is your own";
        return false;
    }
    g_lastAddress = "steam:" + std::to_string(static_cast<unsigned long long>(hostId));
    Stop();
    auto* l = new Link;
    l->hostId = hostId;
    uint16_t localPort = 0;
    l->local = LocalSocket(0, false, localPort);
    if (l->local == INVALID_SOCKET) {
        delete l;
        if (err) *err = "cannot open a local port";
        return false;
    }
    if (!StartLink(l, err)) { closesocket(l->local); delete l; return false; }
    if (!g_loopback) {
        g_api.accept(g_api.net, hostId);   // the host's answers are welcome
        g_api.richSet(g_api.friends, "kc_join", IdText(hostId).c_str());
    }
    Log("steam: joining %llu through Steam (local port %u)", static_cast<unsigned long long>(hostId), localPort);
    return s.Join("127.0.0.1", localPort, err, kMtu);
}

bool LastJoin(std::string& address, uint16_t& port) {
    address = g_lastAddress;
    port = g_lastPort;
    return !address.empty();
}

bool JoinAddress(kc::Session& s, const std::string& address, uint16_t port, std::string* err) {
    g_lastAddress = address;
    g_lastPort = port;
    uint64_t id = 0;
    if (ParseAddress(address, id)) return Join(s, id, err);
    Stop();
    return s.Join(address, port, err);
}

void Stop() {
    Link* l = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_linkMutex);
        l = g_link;
        g_link = nullptr;
    }
    if (!l) return;
    l->run = false;
    if (l->thread.joinable()) l->thread.join();
    for (auto& [id, p] : l->peers) {
        closesocket(p.s);
        if (!g_loopback) g_api.close(g_api.net, id);
    }
    if (l->local != INVALID_SOCKET) closesocket(l->local);
    if (!l->host && !g_loopback) g_api.close(g_api.net, l->hostId);
    if (!g_loopback && g_steam) g_api.richClear(g_api.friends);
    Log("steam: link closed (%llu packets in, %llu out)", static_cast<unsigned long long>(l->packetsIn.load()),
        static_cast<unsigned long long>(l->packetsOut.load()));
    delete l;
}

bool Active() {
    std::lock_guard<std::mutex> lk(g_linkMutex);
    return g_link != nullptr;
}

std::vector<Friend> FriendsHosting() {
    std::vector<Friend> out;
    if (!g_steam || g_loopback) return out;
    const int n = g_api.friendCount(g_api.friends, kFriendFlagImmediate);
    for (int i = 0; i < n && i < 2000; ++i) {
        const uint64_t id = g_api.friendByIndex(g_api.friends, i, kFriendFlagImmediate);
        const char* h = g_api.richGet(g_api.friends, id, "kc_host");
        if (!h || !*h) continue;
        const char* name = g_api.friendName(g_api.friends, id);
        out.push_back({id, name ? name : IdText(id)});
    }
    return out;
}

std::optional<uint64_t> TakeJoinRequest() {
    std::lock_guard<std::mutex> lk(g_reqMutex);
    auto r = g_joinRequest;
    g_joinRequest.reset();
    return r;
}

void Tick() {
    if (!g_steam || g_loopback) return;
    g_api.runCallbacks();
    // A joining friend also says so in its rich presence (in case Steam's session request callback
    // never reached us): accept them.
    bool hosting = false;
    {
        std::lock_guard<std::mutex> lk(g_linkMutex);
        hosting = g_link && g_link->host;
    }
    const double now = NowSeconds();
    if (!hosting || now < g_nextFriendScan) return;
    g_nextFriendScan = now + 1.0;
    const std::string me = IdText(g_myId);
    const int n = g_api.friendCount(g_api.friends, kFriendFlagImmediate);
    for (int i = 0; i < n && i < 2000; ++i) {
        const uint64_t id = g_api.friendByIndex(g_api.friends, i, kFriendFlagImmediate);
        const char* j = g_api.richGet(g_api.friends, id, "kc_join");
        if (j && me == j) g_api.accept(g_api.net, id);
    }
}

} // namespace kcp::steam
