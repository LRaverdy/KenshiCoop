// Players coming back (night-join, A_FAIRE 16/10 points 6, 9, 16): the host's roster of every
// character each player had, and what a player gets when they come back.
//
// Until 0.3.5 the host remembered one character per Steam account (KenshiCoop-players.txt) and, in
// memory only, the netIds a player commanded when they left. A player back after the host loaded a
// save or restarted its session, or whose character Kenshi had given a new handle (moved to another
// squad, captured), got a new character and the character editor, and every other member of their
// squads (recruits, freed slaves) stayed the host's, hidden from their squad bar. Now:
//  - every character a player commands is recorded (kc::Roster), the file's handles follow Kenshi's
//    new handles while the session runs, and the file is written within 2 s of a change;
//  - coming back, their own character is the newest one recorded that still lives anywhere (in
//    their squads, captive, chained, in a cage, in another of their squads, or held outside the
//    player faction); a new one is made only when none does (dead, or not in this world), and the
//    player is told why; the old record stays in the file;
//  - every other recorded character in the player faction that is the host's now is theirs again;
//  - the character made for a player's editor (never finished) stays theirs, the editor opens again
//    when they come back (also after the host restarted its session), and the host's own give
//    (Ctrl+Shift+G, "give") never hands it, or anyone's own character, to another player.
// The host loading another save: every player in the world reloads the new one (a resync), those on
// their way in get a save of the new world.
#include "kc/session.h"

#include <algorithm>
#include <unordered_set>

namespace kc {

namespace {
constexpr double kRosterInterval = 1.0;   // the roster follows the squads this often
constexpr double kRosterSaveDelay = 2.0;  // a change is written at most this late
} // namespace

void Session::LoadRosterFile() {
    roster_.Clear();
    rosterHandles_.clear();
    std::string text;
    if (world_.LoadRoster(text)) roster_.Parse(text);
    // a character made for an editor never finished (before a restart of the session): owed still
    size_t owed = 0, chars = 0;
    for (const auto& [key, p] : roster_.players()) {
        chars += p.chars.size();
        const RosterChar* own = p.LatestOwn();
        if (!own || own->kind != RosterKind::Editor) continue;
        editorOwed_.insert(key);
        editorChars_[key] = EditorChar{0, own->handle, own->name};
        ++owed;
    }
    if (!roster_.players().empty())
        log_("players' roster: " + std::to_string(roster_.players().size()) + " player(s), " + std::to_string(chars) + " character(s) recorded" +
             (owed ? ", " + std::to_string(owed) + " character editor(s) still to finish" : std::string()));
}

void Session::SaveRosterFile(double now, bool force) {
    if (!roster_.dirty() || (!force && now < rosterSaveAt_)) return;
    rosterSaveAt_ = now + kRosterSaveDelay;
    world_.SaveRoster(roster_.Serialize());
    roster_.Saved();
}

// Every second: each character a player commands is recorded as theirs (their own character as Own,
// or Editor while its editor is owed), and a recorded character Kenshi gave a new handle (another
// squad, a capture) is recorded under it, whoever commands it now (its player may be away).
void Session::HostRoster(double now, bool force, bool touchWorld) {
    if (!force && now < rosterAt_) return;
    rosterAt_ = now + kRosterInterval;
    std::unordered_set<Handle, HandleHash> recorded;
    for (const auto& [key, p] : roster_.players())
        for (const auto& c : p.chars) recorded.insert(c.handle);
    for (auto& [nid, e] : entities_) {
        if (e.container || e.bag) continue;
        auto rh = rosterHandles_.find(nid);
        if (rh == rosterHandles_.end()) {
            if (recorded.count(e.handle)) rosterHandles_[nid] = e.handle;
        } else if (rh->second != e.handle) {
            const Handle before = rh->second;
            std::string name;
            if (touchWorld) world_.ReadCharacterName(e.handle, name);
            if (roster_.Rehandle(before, e.handle, name))
                log_("players' roster: " + (name.empty() ? std::string("a character") : name) + " has a new handle (" + RosterHandleText(before) + " -> " +
                     RosterHandleText(e.handle) + "): recorded under it");
            for (auto& [pid, s] : sync_) if (s.own == before) s.own = e.handle;   // a player's own character
            rh->second = e.handle;
        }
        if (!e.squad || e.owner == hostId_) continue;
        auto pit = players_.find(e.owner);
        if (pit == players_.end()) continue;
        const RemotePlayer& p = pit->second;
        const std::string key = PlayerKey(p);
        auto sit = sync_.find(p.id);
        RosterKind kind = RosterKind::Member;
        if (sit != sync_.end() && sit->second.own == e.handle) kind = editorOwed_.count(key) ? RosterKind::Editor : RosterKind::Own;
        std::string name;
        if (touchWorld) world_.ReadCharacterName(e.handle, name);
        const bool fresh = !roster_.Find(key) || roster_.AnyRecordOf(e.handle) != key;
        if (roster_.Note(key, p.steamId, p.name, e.handle, name, kind) && fresh)
            log_("players' roster: " + (name.empty() ? std::string("a character") : name) + " recorded as " + p.name + "'s" +
                 (kind == RosterKind::Member ? "" : kind == RosterKind::Own ? " (their own character)" : " (their own, editor not finished)"));
        rosterHandles_[nid] = e.handle;
    }
    SaveRosterFile(now, false);
}

std::string Session::ReservedFor(const Handle& h) const {
    const std::string key = roster_.ReservedFor(h);
    if (key.empty()) return {};
    const RosterPlayer* p = roster_.Find(key);
    return p && !p->playerName.empty() ? p->playerName : key;
}

bool Session::HostGive(const Handle& h, uint8_t playerId, std::string* why) {
    if (!isHost()) return false;
    const std::string name = world_.CharacterNameOf(h);
    const std::string to = playerId == hostId_ ? std::string("the host") : players_.count(playerId) ? players_[playerId].name : "player " + std::to_string(playerId);
    const std::string reserved = roster_.ReservedFor(h);
    const std::string toKey = playerId != hostId_ && players_.count(playerId) ? PlayerKey(players_[playerId]) : std::string();
    if (!reserved.empty() && reserved != toKey) {
        const std::string owner = ReservedFor(h);
        log_("host give refused: " + name + " is " + owner + "'s own character (or the one made for their character editor): not given to " + to);
        if (why) *why = name + " est le personnage de " + owner + " : il lui reste réservé.";
        return false;
    }
    Assign(h, playerId);
    log_("host gives " + name + " to " + to);
    return true;
}

// HostSaves, once per join: the player's own character (characterPerPlayer), then every other
// character recorded as theirs (or commanded when they left) that is the host's now.
void Session::ReturnPlayer(RemotePlayer& p, PlayerSync& s) {
    const std::string key = PlayerKey(p);
    const double now = clock_();
    HostRoster(now, true);   // the roster as the world is now (handles changed while they were away)
    auto dead = [&](const Handle& h) {
        EntityVitals v;
        return world_.ReadVitals(h, v) && (v.flags & kVitDead) != 0;
    };
    auto captive = [&](const Handle& h) {
        CaptiveState c;
        return world_.ReadCaptive(h, c) && !c.free();
    };
    auto nameOf = [&](const Handle& h, const std::string& recorded) {
        std::string n;
        return world_.ReadCharacterName(h, n) && !n.empty() ? n : recorded.empty() ? world_.CharacterNameOf(h) : recorded;
    };
    // A recorded character in this world: by its handle, else, for their own character (a handle the
    // file could not follow: the session was not running), the one squad character of that name nobody
    // else has a record of. Other characters only by handle (names like "Taureau de Somme" repeat, and
    // the file is the same for every save of this game).
    auto find = [&](const RosterChar& c) -> Entity* {
        if (Entity* e = entityByHandle(c.handle)) return e;
        if (c.name.empty() || c.kind == RosterKind::Member) return nullptr;
        Entity* found = nullptr;
        int n = 0;
        for (auto& [nid, e] : entities_) {
            if (!e.squad || e.container || e.bag || e.owner != hostId_) continue;
            const std::string other = roster_.AnyRecordOf(e.handle);
            if (!other.empty() && other != key) continue;
            std::string cn;
            if (world_.ReadCharacterName(e.handle, cn) && cn == c.name) { found = &e; ++n; }
        }
        if (n != 1) return nullptr;
        if (roster_.Rehandle(c.handle, found->handle))
            log_("players' roster: " + c.name + " found under another handle (" + RosterHandleText(c.handle) + " -> " + RosterHandleText(found->handle) + ")");
        return found;
    };

    const RosterPlayer* rp = roster_.Find(key);
    std::vector<RosterChar> records = rp ? rp->chars : std::vector<RosterChar>{};
    std::string note;        // what they are told (French)
    std::string ownLine;     // the log's account of their own character
    bool created = false;
    if (cfg_.characterPerPlayer) {
        Handle own;
        RosterKind ownKind = RosterKind::Own;
        std::string lostName, lostWhy;   // the newest own character recorded, when it is gone
        for (auto it = records.rbegin(); it != records.rend() && !own.valid(); ++it) {
            if (it->kind == RosterKind::Member) continue;
            Entity* e = find(*it);
            const Handle h = e ? e->handle : it->handle;
            const bool there = e || world_.Exists(h);
            if (there && !dead(h)) {
                own = h;
                ownKind = it->kind;
                const std::string n = nameOf(h, it->name);
                const bool held = captive(h);
                const bool outside = !e || !e->squad;
                ownLine = "their own character " + n + (held ? " (captive: still theirs)" : "") + (outside ? " (outside the player's squads: still theirs)" : "");
                if (held || outside)
                    note = "Ton personnage " + n + (held ? " est prisonnier" : " est hors de tes escouades") +
                           " : il reste à toi (comme en solo, libère-le ou évade-toi).";
                if (e && e->owner != hostId_ && e->owner != p.id && players_.count(e->owner))
                    log_(p.name + "'s own character " + n + " is commanded by " + players_[e->owner].name + " now: given back to " + p.name);
                break;
            }
            if (lostWhy.empty()) {
                lostName = nameOf(h, it->name);
                lostWhy = there ? "dead" : "not in this world";
            }
        }
        // no record: the squad character with the player's name nobody else has a record of
        if (!own.valid() && records.empty()) {
            for (auto& [nid, e] : entities_) {
                if (!e.squad || e.container || e.bag || e.owner != hostId_ || dead(e.handle)) continue;
                const std::string other = roster_.AnyRecordOf(e.handle);
                std::string cn;
                if ((other.empty() || other == key) && world_.ReadCharacterName(e.handle, cn) && cn == p.name.substr(0, 15)) {
                    own = e.handle;
                    ownLine = "their own character " + cn + " (a squad character with their name)";
                    break;
                }
            }
        }
        if (!own.valid()) {
            if (world_.EnsurePlayerCharacter(p.name, p.steamId, own, created) && own.valid()) {
                if (created) {
                    ownLine = "a new character made for them" +
                              (lostWhy.empty() ? std::string(" (none recorded)") : " (" + lostName + ", their last one, is " + lostWhy + "; its record is kept)");
                    if (!lostWhy.empty())
                        note = "Ton personnage " + lostName + (lostWhy == "dead" ? " est mort" : " n'existe pas dans ce monde (sauvegarde plus ancienne ?)") +
                               " : un nouveau personnage est créé pour toi.";
                } else {
                    ownLine = "their own character " + nameOf(own, {});
                }
            } else {
                log_("no character of their own for " + p.name);
            }
        }
        if (own.valid()) {
            s.own = own;
            Assign(own, p.id);
            if (created) {
                // one made for them before and never finished (0.3.2), no longer theirs: removed when the
                // game keeps it outside the squads, else kept as the host's
                if (editorChars_.count(key)) RetireEditorChar(key, "a new character was made for them");
                editorOwed_.insert(key);
                const Entity* ne = entityByHandle(own);
                editorChars_[key] = EditorChar{ne ? ne->netId : 0u, own, p.name};
            } else if (ownKind == RosterKind::Editor) {
                // made for their editor, never finished (gone before closing it, even before a restart
                // of the session): theirs, and the editor opens again
                editorOwed_.insert(key);
                if (!editorChars_.count(key) || editorChars_[key].handle != own) {
                    const Entity* ne = entityByHandle(own);
                    editorChars_[key] = EditorChar{ne ? ne->netId : 0u, own, p.name};
                }
                log_(p.name + " never finished making their character (gone before closing the editor): the editor opens again for them");
            } else {
                editorOwed_.erase(key);   // their own character, made through the editor before
            }
            s.ownCreated = created || editorOwed_.count(key);
            std::string n;
            world_.ReadCharacterName(own, n);
            roster_.Note(key, p.steamId, p.name, own, n, editorOwed_.count(key) ? RosterKind::Editor : RosterKind::Own);
        }
    }
    // every other character of theirs the host commands now: theirs again (the roster: whatever
    // happened meanwhile; the netIds they commanded when they left: the same session)
    size_t back = 0, deadOnes = 0, missing = 0, others = 0;
    std::vector<std::string> names;
    auto giveBack = [&](Entity& e) {
        if (!e.squad || e.container || e.owner == p.id) return;
        if (e.owner != hostId_) { ++others; return; }   // given to another player meanwhile: the host decided
        if (dead(e.handle)) { ++deadOnes; return; }
        Assign(e.handle, p.id);
        ++back;
        if (names.size() < 8) names.push_back(nameOf(e.handle, {}));
    };
    for (const RosterChar& c : records) {
        if (c.handle == s.own) continue;
        Entity* e = find(c);
        if (!e) { ++missing; continue; }
        if (e->handle == s.own) continue;
        giveBack(*e);
    }
    if (auto lo = leftOwned_.find(key); lo != leftOwned_.end()) {
        for (uint32_t nid : lo->second) {
            auto e = entities_.find(nid);
            if (e != entities_.end() && e->second.handle != s.own) giveBack(e->second);
        }
        leftOwned_.erase(lo);
    }
    const bool returning = !records.empty() || back;
    if (returning || !ownLine.empty()) {
        std::string list;
        for (const auto& n : names) list += (list.empty() ? "" : ", ") + n;
        log_(p.name + (returning ? " is back: " : " joins: ") + (ownLine.empty() ? std::string("no own character") : ownLine) + "; " +
             std::to_string(back) + " other character(s) of theirs given back" + (list.empty() ? "" : " (" + list + ")") +
             (deadOnes || missing || others
                  ? "; recorded but not given: " + std::to_string(deadOnes) + " dead, " + std::to_string(missing) + " not in this world, " +
                        std::to_string(others) + " another player's now"
                  : std::string()));
    }
    if (back && note.empty() && !records.empty()) note = "Bon retour : " + std::to_string(back + (s.own.valid() ? 1 : 0)) + " personnage(s) te sont rendus.";
    s.returnNote = note;
    SaveRosterFile(now, true);
}

// The host loaded another save (a quick load, or another game) while players were in: their world is
// the old one. Everyone in the world reloads the host's (a resync); a save being made for a player on
// their way in is of the old world (made again); a player loading one is sent the new one once in.
void Session::HostWorldChanged(uint32_t from, uint32_t to) {
    size_t inGame = 0, onTheWay = 0;
    for (auto& [pid, p] : players_) (p.inGame ? inGame : onTheWay)++;
    log_("the host's world changed (generation " + std::to_string(from) + " -> " + std::to_string(to) + ": another save loaded): " +
         std::to_string(inGame) + " player(s) in the world reload it, " + std::to_string(onTheWay) + " on their way in get it");
    // object identities are addresses in the world they were read in: never matched across worlds
    byIdentity_.clear();
    for (auto& [nid, e] : entities_) {
        if (e.container || e.bag) continue;
        e.identity = world_.Identity(e.handle);
        if (e.identity) byIdentity_[e.identity] = nid;
    }
    rosterHandles_.clear();
    joinFps_.clear();
    if (exporting_) {   // the save under way is of the old world: made again
        exporting_ = false;
        for (auto it = exportFor_.rbegin(); it != exportFor_.rend(); ++it) {
            joinQueue_.push_front(*it);
            sync_[*it].ownChecked = false;
            sync_[*it].saveStartedAt = -1;
        }
        exportFor_.clear();
    }
    for (auto& [pid, p] : players_) {
        if (p.inGame) continue;
        PlayerSync& s = sync_[pid];
        if (s.worldSent) s.staleWorld = true;   // loading the old world: told to reload once in
        else s.ownChecked = false;              // waiting for a save: their characters looked up in the new world
    }
    for (auto& [pid, p] : players_)
        if (p.inGame) SendNotice(pid, "L'hôte a chargé une autre partie : rechargement de son monde...");
    AddChat("* Autre partie chargée : les joueurs rechargent ton monde.", "another save loaded: the players reload the host's world");
    RequestResync(0);
}

} // namespace kc
