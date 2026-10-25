#include "manifest.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "plugin/log.h"
#include "plugin/storage.h"

namespace plugin {
namespace manifest {
namespace {

std::string hex64(uint64_t v) {
    char text[17];
    _snprintf_s(text, _TRUNCATE, "%016llx", static_cast<unsigned long long>(v));
    return text;
}

// Escapes the characters JSON requires; the plugin's own strings never need more.
std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
    }
    return out + "\"";
}

// Recursive-descent reader for the subset of JSON the manifest uses: objects, arrays, strings
// (\" and \\ escapes), integers, true/false/null. Unknown members are skipped.
class Parser {
public:
    explicit Parser(const std::string& text) : s_(text) {}

    bool document(std::vector<Entry>& entries) {
        if (!eat('{')) return false;
        if (eat('}')) return at_end();
        do {
            std::string key;
            if (!string(key) || !eat(':')) return false;
            if (key == "entries" ? !entry_list(entries) : !skip_value()) return false;
        } while (eat(','));
        return eat('}') && at_end();
    }

private:
    void ws() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r' || s_[pos_] == '\n'))
            ++pos_;
    }
    bool eat(char c) {
        ws();
        if (pos_ < s_.size() && s_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }
    bool at_end() {
        ws();
        return pos_ == s_.size();
    }

    bool string(std::string& out) {
        if (!eat('"')) return false;
        out.clear();
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos_ >= s_.size()) return false;
                c = s_[pos_++];
            }
            out += c;
        }
        return false;
    }

    bool integer(long long& out) {
        ws();
        size_t start = pos_;
        if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        if (pos_ == start) return false;
        out = std::strtoll(s_.substr(start, pos_ - start).c_str(), nullptr, 10);
        return true;
    }

    bool small_integer(int& out) {
        long long n = 0;
        if (!integer(n)) return false;
        out = static_cast<int>(n);
        return true;
    }

    bool hash(uint64_t& out) {
        std::string text;
        if (!string(text)) return false;
        out = std::strtoull(text.c_str(), nullptr, 16);
        return true;
    }

    bool entry_list(std::vector<Entry>& entries) {
        if (!eat('[')) return false;
        if (eat(']')) return true;
        do {
            Entry e;
            if (!entry(e)) return false;
            entries.push_back(e);
        } while (eat(','));
        return eat(']');
    }

    bool entry(Entry& e) {
        if (!eat('{')) return false;
        if (eat('}')) return true;
        do {
            std::string key;
            if (!string(key) || !eat(':')) return false;
            bool ok = key == "slot"        ? small_integer(e.slot)
                      : key == "direction" ? string(e.direction)
                      : key == "source"    ? hash(e.source)
                      : key == "output"    ? hash(e.output)
                      : key == "time"      ? string(e.time)
                                           : skip_value();
            if (!ok) return false;
        } while (eat(','));
        return eat('}');
    }

    bool skip_value() {
        ws();
        if (pos_ >= s_.size()) return false;
        char c = s_[pos_];
        std::string ignored;
        long long n = 0;
        if (c == '"') return string(ignored);
        if (c == '{' || c == '[') return skip_container();
        if (c == '-' || (c >= '0' && c <= '9')) return integer(n);
        for (const char* word : {"true", "false", "null"}) {
            if (s_.compare(pos_, strlen(word), word) == 0) {
                pos_ += strlen(word);
                return true;
            }
        }
        return false;
    }

    bool skip_container() {
        const char close = s_[pos_] == '{' ? '}' : ']';
        ++pos_;
        if (eat(close)) return true;
        do {
            if (close == '}') {
                std::string key;
                if (!string(key) || !eat(':')) return false;
            }
            if (!skip_value()) return false;
        } while (eat(','));
        return eat(close);
    }

    const std::string& s_;
    size_t pos_ = 0;
};

bool read_local(const std::wstring& path, std::string& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart < (16 << 20);
    if (ok) {
        text.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = text.empty() || (ReadFile(file, &text[0], static_cast<DWORD>(text.size()), &got, nullptr) &&
                              got == text.size());
    }
    CloseHandle(file);
    return ok;
}

// Writes a temporary file and renames it over the old copy, so a crash never leaves half a file.
bool write_local(const std::wstring& path, const std::string& text) {
    const std::wstring temp = path + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
              written == text.size();
    CloseHandle(file);
    return ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

bool parse_into(Manifest& manifest, const std::string& text, const char* where) {
    if (manifest.parse(text)) {
        log::write("manifest: loaded %zu entries from the %s copy (%zu B)", manifest.entries().size(), where,
                   text.size());
        return true;
    }
    log::write("manifest: the %s copy (%zu B) does not parse, ignored", where, text.size());
    return false;
}

}  // namespace

uint64_t fnv1a64(const kakarot::Bytes& data) {
    uint64_t h = 1469598103934665603ULL;
    for (uint8_t b : data) h = (h ^ b) * 1099511628211ULL;
    return h;
}

std::string utc_now() {
    SYSTEMTIME t;
    GetSystemTime(&t);
    char text[32];
    _snprintf_s(text, _TRUNCATE, "%04u-%02u-%02uT%02u:%02u:%02uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                t.wSecond);
    return text;
}

void Manifest::add(const Entry& entry) {
    entries_.push_back(entry);
    size_t kept = 0;
    for (size_t i = entries_.size(); i-- > 0;) {
        if (entries_[i].slot != entry.slot) continue;
        if (++kept > kKeepPerSlot) entries_.erase(entries_.begin() + static_cast<ptrdiff_t>(i));
    }
}

bool Manifest::links(int slot, uint64_t hash) const {
    if (hash == 0) return false;
    for (const Entry& e : entries_)
        if (e.slot == slot && (e.source == hash || e.output == hash)) return true;
    return false;
}

std::string Manifest::serialize() const {
    std::string out = "{\n  \"format\": 1,\n  \"entries\": [";
    for (size_t i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        out += i ? ",\n    " : "\n    ";
        out += "{\"slot\": " + std::to_string(e.slot) + ", \"direction\": " + quoted(e.direction) +
               ", \"source\": " + quoted(hex64(e.source)) + ", \"output\": " + quoted(hex64(e.output)) +
               ", \"time\": " + quoted(e.time) + "}";
    }
    out += entries_.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

bool Manifest::parse(const std::string& text) {
    std::vector<Entry> entries;
    if (!Parser(text).document(entries)) {
        entries_.clear();
        return false;
    }
    entries_ = std::move(entries);
    return true;
}

Manifest load(const std::wstring& local_path) {
    Manifest manifest;
    kakarot::Bytes cloud;
    if (storage::exists(kCloudName) && storage::read(kCloudName, cloud) &&
        parse_into(manifest, std::string(cloud.begin(), cloud.end()), "cloud"))
        return manifest;
    std::string text;
    if (!local_path.empty() && read_local(local_path, text) && parse_into(manifest, text, "local")) return manifest;
    log::write("manifest: no readable copy, starting empty");
    return manifest;
}

bool save(const Manifest& manifest, const std::wstring& local_path) {
    const std::string text = manifest.serialize();
    bool cloud = storage::write(kCloudName, kakarot::Bytes(text.begin(), text.end()));
    bool local = !local_path.empty() && write_local(local_path, text);
    log::write("manifest: saved %zu entries (%zu B): cloud %s, local %s", manifest.entries().size(), text.size(),
               cloud ? "ok" : "FAILED", local ? "ok" : "FAILED");
    return cloud || local;
}

}  // namespace manifest
}  // namespace plugin
