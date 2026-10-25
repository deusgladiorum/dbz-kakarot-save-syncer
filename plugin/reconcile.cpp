#include "reconcile.h"

#include <climits>
#include <map>

#include "core/build.h"
#include "plugin/log.h"
#include "plugin/operations.h"
#include "plugin/storage.h"

namespace plugin {
namespace reconcile {
namespace {

using kakarot::Bytes;
using operations::Reference;

// A side's timestamp for a slot with no Headers entry: older than any real one.
constexpr int64_t kUnknown = LLONG_MIN;

int index(Side side) { return side == Side::Own ? 0 : 1; }

const char* side_name(Side side) { return side == Side::Own ? "own" : "foreign"; }

struct SystemSave {
    bool present = false;
    Bytes data;
    std::map<int, int64_t> times;  // Headers[slot].Timestamp
};

// One pass: both system saves as read at the start, references found once per side on demand,
// and the slots whose Headers entry must then be merged into each side's system save.
class Pass {
public:
    explicit Pass(Session& session) : session_(session) {}

    bool load_systems() { return load_system(Side::Own) && load_system(Side::Foreign); }
    void reconcile_slot(int slot);
    void merge_all();
    int converted() const { return converted_; }
    int left_alone() const { return left_alone_; }

private:
    bool load_system(Side side);
    bool pick_newer(int slot, Side& newer);
    void bring_over(int slot, Side source, bool only_side);
    const Reference* reference(Side target);
    int64_t time_of(Side side, int slot) const;
    const std::string& name(int slot, Side side) const { return session_.model.slot_name(slot, side); }

    Session& session_;
    SystemSave systems_[2];
    Reference references_[2];
    int reference_state_[2] = {0, 0};  // 0 not looked up, 1 found, 2 none
    std::vector<int> merges_[2];
    int converted_ = 0;
    int left_alone_ = 0;
};

bool Pass::load_system(Side side) {
    SystemSave& s = systems_[index(side)];
    const std::string& file = session_.model.system_name(side);
    std::string error;
    if (!operations::read_optional_save(session_, file, s.data, s.present)) {
        log::write("reconcile: %s exists but cannot be read; pass skipped", file.c_str());
        return false;
    }
    if (s.present && !operations::header_timestamps(s.data, session_.key, s.times, &error)) {
        log::write("reconcile: %s does not parse (%s); pass skipped", file.c_str(), error.c_str());
        return false;
    }
    std::vector<int> slots;
    for (const auto& t : s.times) slots.push_back(t.first);
    log::write("reconcile: %s %s, Headers slots %s", file.c_str(), s.present ? "present" : "absent",
               operations::slot_list(slots).c_str());
    return true;
}

int64_t Pass::time_of(Side side, int slot) const {
    const auto& times = systems_[index(side)].times;
    auto it = times.find(slot);
    return it == times.end() ? kUnknown : it->second;
}

const Reference* Pass::reference(Side target) {
    int i = index(target);
    if (reference_state_[i] == 0)
        reference_state_[i] = operations::find_reference(session_, target, references_[i]) ? 1 : 2;
    return reference_state_[i] == 1 ? &references_[i] : nullptr;
}

// Both sides have the file: the newer save timestamp wins; equal (or both unknown) means nothing to do.
bool Pass::pick_newer(int slot, Side& newer) {
    const int64_t own = time_of(Side::Own, slot), foreign = time_of(Side::Foreign, slot);
    if (own == foreign) {
        log::write("reconcile: slot %d: %s, nothing to do", slot,
                   own == kUnknown ? "both timestamps unknown" : "same timestamp");
        return false;
    }
    newer = foreign > own ? Side::Foreign : Side::Own;
    log::write("reconcile: slot %d: %s newer (own %lld, foreign %lld)", slot, side_name(newer),
               own == kUnknown ? -1LL : own, foreign == kUnknown ? -1LL : foreign);
    return true;
}

void Pass::bring_over(int slot, Side source, bool only_side) {
    const Side target = other(source);
    if (!systems_[index(source)].times.count(slot)) {
        log::write("reconcile: slot %d: %s has no Headers[%d] entry, nothing to convert", slot,
                   session_.model.system_name(source).c_str(), slot);
        return;
    }
    Bytes data;
    if (!operations::read_save(session_, name(slot, source), data)) return;
    if (only_side) {
        if (session_.manifest.links(slot, manifest::fnv1a64(data))) {
            log::write("reconcile: slot %d: only %s exists and the manifest links it: deleted on purpose, left alone",
                       slot, name(slot, source).c_str());
            ++left_alone_;
            return;
        }
        log::write("reconcile: slot %d: only %s exists, not in the manifest: new save", slot,
                   name(slot, source).c_str());
    }
    manifest::Entry entry;
    if (!operations::produce_slot(session_, slot, target, data, reference(target), entry)) return;
    session_.manifest.add(entry);
    merges_[index(target)].push_back(slot);
    ++converted_;
}

void Pass::reconcile_slot(int slot) {
    const bool own = storage::exists(name(slot, Side::Own));
    const bool foreign = storage::exists(name(slot, Side::Foreign));
    if (!own && !foreign) return;
    Side source = own ? Side::Own : Side::Foreign;
    if (own && foreign && !pick_newer(slot, source)) return;
    bring_over(slot, source, !(own && foreign));
}

void Pass::merge_all() {
    for (Side target : {Side::Own, Side::Foreign}) {
        const std::vector<int>& slots = merges_[index(target)];
        if (slots.empty()) continue;
        const SystemSave& source = systems_[index(other(target))];
        const SystemSave& dest = systems_[index(target)];
        operations::merge_system(session_, target, source.data, dest.present ? &dest.data : nullptr, slots,
                                 dest.present ? nullptr : reference(target));
    }
}

}  // namespace

void run(Session& session) {
    log::Timer timer;
    log::write("reconcile: start (own %s, foreign %s)", kakarot::build::name(session.model.build(Side::Own)),
               kakarot::build::name(session.model.build(Side::Foreign)));
    ensure_manifest(session);
    Pass pass(session);
    if (!pass.load_systems()) return;
    for (int slot = 0; slot < kSlotCount; ++slot) pass.reconcile_slot(slot);
    pass.merge_all();
    if (pass.converted() > 0) save_manifest(session);
    log::write("reconcile: done, %d slot(s) converted, %d left alone as deleted | %.3f ms", pass.converted(),
               pass.left_alone(), timer.ms());
}

}  // namespace reconcile
}  // namespace plugin
