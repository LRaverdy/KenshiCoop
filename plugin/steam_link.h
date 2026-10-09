// Playing over the internet without opening ports: the session's UDP traffic is relayed through
// Steam's peer-to-peer networking (the Steam API Kenshi ships: ISteamNetworking005, relays through
// Steam's servers when a direct path fails).
//
// Nothing changes for the session itself. The host's link takes each player's Steam packets and
// hands them to the host's ENet port as if they came from a local UDP socket; a client's link opens
// a local UDP port that ENet connects to, and carries everything to the host's Steam account.
//
// Friends find each other through Steam rich presence: the host advertises itself, so a friend can
// pick it in the Multijoueur window or use "Rejoindre la partie" in the Steam friends list.
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace kc { class Session; }

namespace kcp::steam {

// Largest datagram Steam's unreliable P2P sends carry (ENet must not build bigger ones).
constexpr uint32_t kMtu = 1200;

// loopbackTest: no Steam at all, the "Steam" packets travel over local UDP (two instances on one
// PC can then test the relaying; a Steam id is a local port number).
void Init(bool loopbackTest);
bool Available();
uint64_t MyId();
std::string MyName();

// "steam:<id>" or a bare 17-digit Steam id.
bool ParseAddress(const std::string& address, uint64_t& id);

// Host: relay Steam traffic to the session's UDP port, and advertise the game to friends.
bool StartHost(uint16_t gamePort, std::string* err);
// Client: join the host behind that Steam id (starts the relay, then the session).
bool Join(kc::Session& s, uint64_t hostId, std::string* err);
// Either kind of address: Steam id or IP.
bool JoinAddress(kc::Session& s, const std::string& address, uint16_t port, std::string* err);
void Stop();
bool Active();

struct Friend {
    uint64_t id = 0;
    std::string name;
};
std::vector<Friend> FriendsHosting();          // friends whose game advertises a KenshiCoop session
std::optional<uint64_t> TakeJoinRequest();     // "Rejoindre la partie" clicked in Steam (or +kc_join on the command line)
void Tick();                                   // game thread, every tick: Steam callbacks, link upkeep

} // namespace kcp::steam
