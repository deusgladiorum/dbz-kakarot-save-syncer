// The manifest: the plugin's record of which outputs it produced from which sources, and of
// slot deletions (docs/design.html, 6). It answers one question: when a slot exists on only one
// side, is the survivor linked to the missing file (deleted on purpose), or never produced?
//
// Stored as small JSON text through Remote Storage under kCloudName, so it travels with the
// saves, and mirrored to a local copy next to the DLL that is read when the cloud read fails.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/aes.h"

namespace plugin {
namespace manifest {

constexpr char kCloudName[] = "dbz-kakarot-save-syncer.manifest";
constexpr wchar_t kLocalName[] = L"dbz-kakarot-save-syncer.manifest";

struct Entry {
    int slot = -1;
    std::string direction;  // "HD->standard", "standard->HD", or "deleted:HD" / "deleted:standard"
    uint64_t source = 0;    // FNV-1a 64 of the source file (for a deletion: the deleted file)
    uint64_t output = 0;    // FNV-1a 64 of the output (for a deletion: the other build's copy, 0 if none)
    std::string time;       // UTC time the entry was recorded, ISO 8601
};

uint64_t fnv1a64(const kakarot::Bytes& data);

// Current UTC time as "YYYY-MM-DDTHH:MM:SSZ".
std::string utc_now();

class Manifest {
public:
    // Appends an entry; only the newest kKeepPerSlot entries of each slot are kept.
    void add(const Entry& entry);

    // True if an entry of `slot` names `hash` as its source or output.
    bool links(int slot, uint64_t hash) const;

    const std::vector<Entry>& entries() const { return entries_; }

    std::string serialize() const;

    // Replaces the entries with those in `text`; false (and no entries) on a syntax error.
    bool parse(const std::string& text);

    static constexpr size_t kKeepPerSlot = 8;

private:
    std::vector<Entry> entries_;
};

// Reads the cloud copy, else the local copy at `local_path`; empty if neither parses.
Manifest load(const std::wstring& local_path);

// Writes the cloud copy and the local copy; false only if both failed.
bool save(const Manifest& manifest, const std::wstring& local_path);

}  // namespace manifest
}  // namespace plugin
