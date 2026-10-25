#include "operations.h"

#include <algorithm>

#include "core/build.h"
#include "core/gvas.h"
#include "core/key.h"
#include "core/ops.h"
#include "plugin/log.h"
#include "plugin/storage.h"

namespace plugin {
namespace operations {
namespace {

using kakarot::Build;
using kakarot::Bytes;

// The GVAS header sits well inside the first 8 KiB; only that much is decrypted to check it.
constexpr size_t kHeaderPrefix = 8192;

Build header_build(const Bytes& encrypted, const kakarot::Key& key) {
    size_t n = std::min(encrypted.size(), kHeaderPrefix) & ~(kakarot::aes::kBlockSize - 1);
    Bytes plain(encrypted.begin(), encrypted.begin() + static_cast<ptrdiff_t>(n));
    kakarot::gvas::Header header;
    if (!kakarot::aes::decrypt_ecb(key, plain.data(), plain.size()) || !kakarot::gvas::parse_header(plain, header))
        return Build::Unknown;
    return kakarot::build::from_save_header(header);
}

struct Candidate {
    std::string name;
    int64_t time;
};

std::vector<Candidate> reference_candidates(const Model& model, Side target) {
    std::vector<Candidate> out;
    auto consider = [&](const std::string& name) {
        if (storage::exists(name)) out.push_back({name, storage::timestamp(name)});
    };
    for (int slot = 0; slot < kSlotCount; ++slot) consider(model.slot_name(slot, target));
    consider(model.system_name(target));
    std::stable_sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) { return a.time > b.time; });
    return out;
}

}  // namespace

bool read_save(const Session& session, const std::string& name, Bytes& out) {
    if (!storage::read(name, out)) return false;
    if (!kakarot::key::verify_key(session.key, out)) {
        log::write("  %s (%zu B) does not decrypt to GVAS with the key; not used", name.c_str(), out.size());
        out.clear();
        return false;
    }
    return true;
}

bool read_optional_save(const Session& session, const std::string& name, Bytes& out, bool& present) {
    present = storage::exists(name);
    return !present || read_save(session, name, out);
}

bool find_reference(const Session& session, Side target, Reference& ref) {
    const Build build = session.model.build(target);
    for (const Candidate& c : reference_candidates(session.model, target)) {
        Bytes data;
        if (!storage::read(c.name, data)) continue;
        Build found = header_build(data, session.key);
        if (found != build) {
            log::write("  reference: %s (time %lld) has a %s header, skipped", c.name.c_str(), c.time,
                       kakarot::build::name(found));
            continue;
        }
        ref = {c.name, std::move(data), c.time};
        log::write("  reference for %s: %s (time %lld, %zu B)", kakarot::build::name(build), ref.name.c_str(),
                   ref.file_time, ref.data.size());
        return true;
    }
    log::write("  reference for %s: none found", kakarot::build::name(build));
    return false;
}

bool header_timestamps(const Bytes& system_save, const kakarot::Key& key, std::map<int, int64_t>& out,
                       std::string* error) {
    Bytes plain;
    kakarot::gvas::Layout layout;
    if (!kakarot::ops::decrypt(system_save, key, plain, error) || !kakarot::gvas::parse_layout(plain, layout, error))
        return false;
    out.clear();
    for (const kakarot::gvas::Property& p : layout.top.properties) {
        int64_t ticks = 0;
        if (p.name == "Headers" && kakarot::gvas::read_struct_datetime(plain, p, "Timestamp", ticks))
            out[p.array_index] = ticks;
    }
    return true;
}

bool produce_slot(const Session& session, int slot, Side target, const Bytes& source, const Reference* ref,
                  manifest::Entry& entry) {
    log::Timer timer;
    const std::string& name = session.model.slot_name(slot, target);
    Bytes out;
    std::string error;
    if (!ref) {
        out = source;
    } else if (!kakarot::ops::convert_game_save(source, ref->data, session.key, out, &error)) {
        log::write("  convert to %s FAILED: %s", name.c_str(), error.c_str());
        return false;
    }
    if (!storage::write(name, out)) return false;
    entry = {slot, direction(session.model, target), manifest::fnv1a64(source), manifest::fnv1a64(out),
             manifest::utc_now()};
    log::write("  produced %s: %zu B from %zu B, %s | %.3f ms", name.c_str(), out.size(), source.size(),
               ref ? ("reference " + ref->name).c_str() : "no reference: source header and stamp kept",
               timer.ms());
    return true;
}

bool merge_system(const Session& session, Side target, const Bytes& source_system, const Bytes* target_system,
                  const std::vector<int>& slots, const Reference* ref) {
    log::Timer timer;
    const std::string& name = session.model.system_name(target);
    Bytes out;
    std::string error;
    bool ok;
    if (target_system) {
        ok = kakarot::ops::merge_headers(source_system, *target_system, session.key, slots, out, &error);
    } else {
        const bool to_hd = session.model.build(target) == Build::HD;
        ok = kakarot::ops::convert_system_save(source_system, ref ? ref->data : source_system, session.key, to_hd,
                                               out, &error);
    }
    const char* how = target_system ? "merge Headers into" : "convert whole to";
    if (!ok) {
        log::write("  %s %s FAILED: %s", how, name.c_str(), error.c_str());
        return false;
    }
    if (!storage::write(name, out)) return false;
    log::write("  %s %s: slots %s, %zu B%s | %.3f ms", how, name.c_str(), slot_list(slots).c_str(), out.size(),
               target_system || ref ? "" : " (no reference: source header and stamp kept)", timer.ms());
    return true;
}

std::string slot_list(const std::vector<int>& slots) {
    std::string out;
    for (int s : slots) out += (out.empty() ? "" : ",") + std::to_string(s);
    return out.empty() ? "none" : out;
}

}  // namespace operations
}  // namespace plugin
