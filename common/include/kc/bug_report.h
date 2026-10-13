#pragma once
// Bug reports (0.3.5). Any player types "/bug <note>" (the in-game console, Ctrl+Shift+K, or the
// host's console window); a client sends it to the host as an ordinary Chat, its pending log lines
// flushed first so they arrive before it. The host does not show it as chat: a moment later it
// writes KenshiCoop_bugs\bug-<date>_<time>-<name>.txt (a header, the players, the last 10 minutes of
// its log, the clients' lines included) and tells everyone in the chat. Pure rules here (no game
// calls), unit tested in tests/test_main.cpp; the file itself is plugin/bug_report.cpp.
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace kc::bug {

constexpr double kThrottleSeconds = 30.0;   // one report per player per 30 s
constexpr double kWriteDelay = 1.5;         // host: the reporter's flushed log lines land first
constexpr double kTailSeconds = 600.0;      // the report holds the last 10 minutes of the host's log
constexpr size_t kTailMaxLines = 20000;
constexpr size_t kTailMaxBytes = 4u << 20;  // 4 MB of text at most in memory
constexpr size_t kMaxNoteLen = 160;

// "/bug", "/BUG la porte", "/Bug   x" are bug reports (note: what follows, trimmed); "/bugs", "bug x",
// " /bug" (a leading space: typed on purpose) are not.
inline bool ParseCommand(const std::string& text, std::string& note) {
    static const char kCmd[] = "/bug";
    if (text.size() < 4) return false;
    for (size_t i = 0; i < 4; ++i)
        if (std::tolower(static_cast<unsigned char>(text[i])) != kCmd[i]) return false;
    if (text.size() > 4 && !std::isspace(static_cast<unsigned char>(text[4]))) return false;
    size_t b = 4, e = text.size();
    while (b < e && std::isspace(static_cast<unsigned char>(text[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(text[e - 1]))) --e;
    note = text.substr(b, e - b);
    if (note.size() > kMaxNoteLen) note.resize(kMaxNoteLen);
    return true;
}

// The reporter's name in a file name: ASCII letters, digits, '-' and '_' kept, anything else (spaces,
// accents, '\', ':', '.') becomes '_'; 32 characters at most; "player" when nothing is left.
inline std::string SafeName(const std::string& name) {
    std::string out;
    for (unsigned char c : name) {
        if (out.size() >= 32) break;
        out.push_back((std::isalnum(c) && c < 0x80) || c == '-' || c == '_' ? char(c) : '_');
    }
    bool any = false;
    for (char c : out) any = any || c != '_';
    return any ? out : "player";
}

// bug-2026-10-13_22-15-04-rob1.txt (local time of the report)
inline std::string FileName(int year, int month, int day, int hour, int minute, int second, const std::string& reporter) {
    char b[48];
    snprintf(b, sizeof(b), "bug-%04d-%02d-%02d_%02d-%02d-%02d-", year, month, day, hour, minute, second);
    return b + SafeName(reporter) + ".txt";
}

// One report per player per kThrottleSeconds. Allow() records the report it allows; `ago`: seconds
// since that player's last one when refused.
class Throttle {
public:
    explicit Throttle(double window = kThrottleSeconds) : window_(window) {}
    bool Allow(uint8_t player, double now, double* ago = nullptr) {
        auto it = last_.find(player);
        if (it != last_.end() && now - it->second < window_) {
            if (ago) *ago = now - it->second;
            return false;
        }
        last_[player] = now;
        return true;
    }
    void Forget(uint8_t player) { last_.erase(player); }   // the id may be someone else's next
private:
    double window_;
    std::map<uint8_t, double> last_;
};

// The newest log lines in memory (fed by the log sink, so a report never re-reads the file), bounded
// by age, count and size; lines arrive in time order.
class LogRing {
public:
    LogRing(double maxAge = kTailSeconds, size_t maxLines = kTailMaxLines, size_t maxBytes = kTailMaxBytes)
        : maxAge_(maxAge), maxLines_(maxLines), maxBytes_(maxBytes) {}
    void Push(double t, std::string line) {
        bytes_ += line.size();
        lines_.push_back({t, std::move(line)});
        Trim(t);
    }
    // Drops what is older than maxAge before `now`, and the oldest beyond the count and size limits.
    void Trim(double now) {
        while (!lines_.empty() && (lines_.size() > maxLines_ || bytes_ > maxBytes_ || lines_.front().t < now - maxAge_)) {
            bytes_ -= lines_.front().text.size();
            lines_.pop_front();
        }
    }
    // The lines logged at or after `since`, oldest first.
    std::vector<std::string> Since(double since) const {
        std::vector<std::string> out;
        for (const auto& l : lines_)
            if (l.t >= since) out.push_back(l.text);
        return out;
    }
    size_t size() const { return lines_.size(); }
    size_t bytes() const { return bytes_; }
private:
    struct Line { double t; std::string text; };
    double maxAge_;
    size_t maxLines_, maxBytes_;
    std::deque<Line> lines_;
    size_t bytes_ = 0;
};

} // namespace kc::bug
