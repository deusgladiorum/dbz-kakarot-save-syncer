// The building blocks of the save-time hook and the reconcile pass: the two operations of
// docs/design.html section 3, applied to files in Remote Storage, plus reference discovery.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "core/aes.h"
#include "plugin/session.h"

namespace plugin {
namespace operations {

// A file of the target build that supplies its engine header and version stamp.
struct Reference {
    std::string name;
    kakarot::Bytes data;
    int64_t file_time = 0;
};

// Reads a save through Remote Storage and checks that the key decrypts it; logs why not.
bool read_save(const Session& session, const std::string& name, kakarot::Bytes& out);

// Reads `name` if it exists. False only if it exists but cannot be read or decrypted.
bool read_optional_save(const Session& session, const std::string& name, kakarot::Bytes& out, bool& present);

// The newest `target`-side file (slots and system save, by GetFileTimestamp) whose decrypted
// header is that side's build; false if there is none.
bool find_reference(const Session& session, Side target, Reference& ref);

// Headers[slot].Timestamp ticks by slot; entries without a readable Timestamp are left out.
bool header_timestamps(const kakarot::Bytes& system_save, const kakarot::Key& key, std::map<int, int64_t>& out,
                       std::string* error = nullptr);

// Converts game save `source` (the other side's format) to the `target` side with `ref`, or
// keeps its header and stamp when `ref` is null, and writes it as the target side's slot file.
bool produce_slot(const Session& session, int slot, Side target, const kakarot::Bytes& source, const Reference* ref,
                  manifest::Entry& entry);

// Merges Headers[slots] of `source_system` (the other side's system save) into `target_system`
// and writes the target side's system save. When the target has none (`target_system` null), the
// source is converted whole with `ref` (or with its own header and stamp when `ref` is null).
bool merge_system(const Session& session, Side target, const kakarot::Bytes& source_system,
                  const kakarot::Bytes* target_system, const std::vector<int>& slots, const Reference* ref);

// "0,2,10" for log lines.
std::string slot_list(const std::vector<int>& slots);

}  // namespace operations
}  // namespace plugin
