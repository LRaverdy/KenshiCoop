#include "bug_report.h"

#include <windows.h>

#include <cstdio>
#include <map>
#include <vector>

#include "kc/bug_report.h"
#include "kenshi.h"
#include "util.h"

namespace kcp {

namespace {

std::string g_lastReport;

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string Key(const kc::Handle& h) {
    char b[96];
    snprintf(b, sizeof(b), "%u:%u:%u:%u:%u", h.type, h.container, h.containerSerial, h.index, h.serial);
    return b;
}

// Everything above the log: when, who, what the game was doing, every player and their characters.
std::string Header(kc::Session& s, KenshiWorld& w, const kc::Session::BugRequest& r, const std::string& hostName, const char* version,
                   const SYSTEMTIME& st, double now) {
    std::string o;
    char b[512];
    snprintf(b, sizeof(b), "KenshiCoop bug report\ntime: %04d-%02d-%02d %02d:%02d:%02d (asked %.1f s before)\n", st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond, now - r.at);
    o += b;
    snprintf(b, sizeof(b), "mod version: %s, protocol %u\n", version, unsigned(kc::kProtocolVersion));
    o += b;
    o += "reporter: " + r.name + " (player " + std::to_string(r.playerId) + ")\n";
    o += "note: " + (r.note.empty() ? std::string("(none)") : r.note) + "\n";
    const bool ready = w.Ready();
    double hours = -1;
    if (ready) kenshi::GetGameHours(hours);
    snprintf(b, sizeof(b), "game: speed %.2f, paused %d, world loaded %d, game clock %.2f h\n", double(kenshi::GetFrameSpeed()), int(kenshi::GetPaused()),
             int(ready), hours);
    o += b;
    std::string save;
    const bool haveSave = kenshi::LastSaveLocation(save);
    snprintf(b, sizeof(b), "world: %s (fingerprint %016llx)\n", haveSave ? save.c_str() : "(unknown)", ready ? (unsigned long long)w.Fingerprint() : 0ull);
    o += b;
    snprintf(b, sizeof(b), "session: %s, %zu entities, %zu NPCs\n", s.isHost() ? "hosting" : "not hosting any more", s.entityCount(), s.npcCount());
    o += b;

    // the characters of the squad by owner (the host's game has them all), with where they stand
    std::map<uint8_t, std::vector<std::string>> chars;
    if (ready) {
        std::vector<kc::Handle> hs;
        w.PlayerCharacters(hs);
        for (const auto& h : hs) {
            std::string name;
            if (!w.ReadCharacterName(h, name)) name = "?";
            kc::EntityState es;
            std::string line = "    " + name + "  [" + Key(h) + "]";
            if (w.Read(h, es)) {
                snprintf(b, sizeof(b), "  at (%.1f, %.1f, %.1f)", double(es.pos.x), double(es.pos.y), double(es.pos.z));
                line += b;
            }
            chars[s.ownerOf(h)].push_back(std::move(line));
        }
    }
    auto list = [&](uint8_t id) {
        auto it = chars.find(id);
        if (it == chars.end()) { o += "    (no character)\n"; return; }
        for (const auto& l : it->second) o += l + "\n";
        chars.erase(it);
    };
    o += "\n== players ==\n";
    o += std::to_string(s.localId()) + ". " + hostName + " (host)\n";
    list(s.localId());
    for (const auto& [id, p] : s.players()) {
        snprintf(b, sizeof(b), "%u. %s  ping %u ms  %s%s\n", unsigned(id), p.name.c_str(), unsigned(p.rttMs), p.inGame ? "in game" : "joining (world not loaded yet)",
                 p.editing ? ", in the character editor" : "");
        o += b;
        if (p.reportAt >= 0) {
            const auto& m = p.report;
            snprintf(b, sizeof(b), "   last sync report %.0f s ago: %u characters followed, %u NPCs not there yet, %u squad members missing, %u off, max offset %.1f, %u fps\n",
                     now - p.reportAt, unsigned(m.entities), unsigned(m.missingNpcs), unsigned(m.missingSquad), unsigned(m.farOff), double(m.maxErr), unsigned(m.fps));
            o += b;
        } else {
            o += "   no sync report yet\n";
        }
        list(id);
    }
    for (const auto& [id, lines] : chars) {   // owned by a player no longer here
        o += "player " + std::to_string(id) + " (not connected):\n";
        for (const auto& l : lines) o += l + "\n";
    }
    return o;
}

} // namespace

void BugReportUpkeep(kc::Session& s, KenshiWorld& w, const std::string& hostName, const char* version) {
    for (const auto& r : s.TakeBugRequests()) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        const double now = NowSeconds();
        const std::string file = kc::bug::FileName(st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, r.name);
        const std::wstring dir = GameDir() + L"KenshiCoop_bugs";
        CreateDirectoryW(dir.c_str(), nullptr);
        std::wstring path = dir + L"\\";
        for (char c : file) path.push_back(wchar_t(static_cast<unsigned char>(c)));   // (ASCII: kc::bug::SafeName)
        // the marker first: it is in the report's own log tail
        Log("==== BUG REPORT from %s: %s -> %s ====", r.name.c_str(), r.note.empty() ? "(no note)" : r.note.c_str(), file.c_str());
        const std::string head = Header(s, w, r, hostName, version, st, now);
        const std::vector<std::string> tail = LogTail(kc::bug::kTailSeconds);
        FILE* f = _wfopen(path.c_str(), L"w");
        bool ok = f != nullptr;
        if (f) {
            ok = fputs(head.c_str(), f) >= 0;
            fprintf(f, "\n== host log, last %d minutes (%zu lines; the clients' lines start with [name]) ==\n", int(kc::bug::kTailSeconds / 60), tail.size());
            for (const auto& l : tail) ok = fputs(l.c_str(), f) >= 0 && fputc('\n', f) != EOF && ok;
            ok = fclose(f) == 0 && ok;
        }
        if (ok) {
            g_lastReport = Utf8(path);
            Log("bug report written: %s (%zu log lines)", g_lastReport.c_str(), tail.size());
            s.Announce("Rapport de bug de " + r.name + " enregistré (" + file + ").", "bug report of " + r.name + " saved (" + file + "), everyone told");
        } else {
            Log("bug report: cannot write %s", Utf8(path).c_str());
            s.Announce("Rapport de bug de " + r.name + " : fichier impossible à écrire chez l'hôte (voir KenshiCoop.log).",
                       "bug report of " + r.name + " NOT saved: cannot write " + file);
        }
    }
}

std::string LastBugReport() { return g_lastReport; }

} // namespace kcp
