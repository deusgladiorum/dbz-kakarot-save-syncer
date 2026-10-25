#include "sync.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>

#include "core/build.h"
#include "core/key.h"
#include "plugin/log.h"
#include "plugin/operations.h"
#include "plugin/reconcile.h"
#include "plugin/storage.h"

namespace plugin {
namespace sync {
namespace {

using kakarot::Bytes;

enum ReconcileState : LONG { kNotRun, kRunning, kDone };

SRWLOCK g_lock = SRWLOCK_INIT;
std::unique_ptr<Session> g_session;
volatile LONG g_reconcile = kNotRun;

// A game call that waits longer than this for the reconcile pass is flagged in the log.
constexpr double kLongWaitMs = 2000.0;

// ------------------------------------------------------------------- failure containment

template <class F>
void seh_guard(const char* what, F& body) {
    __try {
        body();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log::write("%s: exception 0x%08lx, abandoned (the game's call is unaffected)", what, GetExceptionCode());
    }
}

// Runs `body` under the sync lock; C++ and SEH exceptions are logged and swallowed inside the
// lock, so the lock is always released.
template <class F>
void locked(const char* what, F&& body) {
    auto checked = [&]() {
        try {
            body();
        } catch (const std::exception& e) {
            log::write("%s: %s, abandoned (the game's call is unaffected)", what, e.what());
        } catch (...) {
            log::write("%s: unknown exception, abandoned (the game's call is unaffected)", what);
        }
    };
    AcquireSRWLockExclusive(&g_lock);
    seh_guard(what, checked);
    ReleaseSRWLockExclusive(&g_lock);
}

bool copy_raw(void* out, const void* data, size_t size) {
    __try {
        memcpy(out, data, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Copies the game's buffer; false (logged) if it is unusable or not a save under the key.
bool copy_game_buffer(const Session& session, const char* name, const void* data, int32_t size, Bytes& out) {
    out.assign(size > 0 ? static_cast<size_t>(size) : 0, 0);
    if (size <= 0 || !data || !copy_raw(out.data(), data, out.size())) {
        log::write("  %s: the game's buffer is unusable, skipped", name);
        return false;
    }
    if (!kakarot::key::verify_key(session.key, out)) {
        log::write("  %s: the game's buffer does not decrypt to GVAS with the key, skipped", name);
        return false;
    }
    return true;
}

void drop_pending(Session& session, int slot) {
    auto& pending = session.pending;
    pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const manifest::Entry& e) { return e.slot == slot; }),
                  pending.end());
}

// ------------------------------------------------------------------- save time (design 4)

// Converts the slot the game just wrote into the foreign format and marks it pending.
void on_slot_written(Session& session, int slot, const char* name, const void* data, int32_t size) {
    log::Timer timer;
    log::write("save-time: %s written, producing %s", name, session.model.slot_name(slot, Side::Foreign).c_str());
    Bytes source;
    if (!copy_game_buffer(session, name, data, size, source)) return;
    operations::Reference ref;
    const bool have_ref = operations::find_reference(session, Side::Foreign, ref);
    if (!have_ref) log::write("  no foreign reference: the output keeps the source header and stamp (design fallback)");
    manifest::Entry entry;
    if (!operations::produce_slot(session, slot, Side::Foreign, source, have_ref ? &ref : nullptr, entry)) {
        log::write("save-time: slot %d not produced | %.3f ms", slot, timer.ms());
        return;
    }
    drop_pending(session, slot);
    session.pending.push_back(entry);
    log::write("save-time: slot %d produced, waiting for the system save | %.3f ms", slot, timer.ms());
}

// Merges the pending slots' Headers entries into the foreign system save and records them.
void on_system_written(Session& session, const char* name, const void* data, int32_t size) {
    if (session.pending.empty()) return;
    log::Timer timer;
    std::vector<int> slots;
    for (const manifest::Entry& e : session.pending) slots.push_back(e.slot);
    log::write("save-time: %s written, merging slots %s", name, operations::slot_list(slots).c_str());

    Bytes own_system, foreign_system;
    bool foreign_present = false;
    bool merged = false;
    if (copy_game_buffer(session, name, data, size, own_system) &&
        operations::read_optional_save(session, session.model.system_name(Side::Foreign), foreign_system,
                                       foreign_present)) {
        operations::Reference ref;
        const bool have_ref = !foreign_present && operations::find_reference(session, Side::Foreign, ref);
        merged = operations::merge_system(session, Side::Foreign, own_system, foreign_present ? &foreign_system : nullptr,
                                          slots, have_ref ? &ref : nullptr);
    }
    // The slot files exist either way, so their links are recorded; an unmerged entry is
    // "foreign timestamp unknown" at the next launch and merged again then.
    ensure_manifest(session);
    for (const manifest::Entry& e : session.pending) session.manifest.add(e);
    session.pending.clear();
    save_manifest(session);
    log::write("save-time: system save step %s | %.3f ms",
               merged ? "done" : "FAILED (the next launch's reconcile pass retries)", timer.ms());
}

// ------------------------------------------------------------------- delete (design 5)

uint64_t hash_of(const std::string& name) {
    Bytes data;
    return storage::exists(name) && storage::read(name, data) ? manifest::fnv1a64(data) : 0;
}

void record_deletion(Session& session, int slot, uint64_t deleted_hash) {
    manifest::Entry entry;
    entry.slot = slot;
    entry.direction = std::string("deleted:") + kakarot::build::name(session.model.build(Side::Own));
    entry.source = deleted_hash;
    entry.output = hash_of(session.model.slot_name(slot, Side::Foreign));
    entry.time = manifest::utc_now();
    ensure_manifest(session);
    session.manifest.add(entry);
    drop_pending(session, slot);
    log::write("delete: slot %d recorded (the foreign copy %s is kept)", slot,
               session.model.slot_name(slot, Side::Foreign).c_str());
    save_manifest(session);
}

bool active() { return g_session && storage::ready(); }

// ------------------------------------------------------------------- reconcile (design 5)

// Files the reconcile pass reads or writes: calls naming them wait while it runs.
bool guarded(const char* name) {
    return g_session->model.is_save_file(name) || std::strcmp(name, manifest::kCloudName) == 0;
}

// Runs the pass on this thread unless it has already started (once per session). The state
// becomes kRunning only with the lock held, so a caller that sees kRunning and then takes the
// lock gets it once the pass is over.
void run_pass(bool early, const char* name) {
    log::Timer timer;
    bool ran = false;
    locked("reconcile", [&]() {
        if (!g_session || g_reconcile != kNotRun) return;
        g_reconcile = kRunning;
        ran = true;
        if (early)
            log::write("reconcile: early start, on the plugin's thread once the hooks are active");
        else
            log::write("reconcile: inline start (fallback), by the game's first call naming %s", name);
        reconcile::run(*g_session);
        g_reconcile = kDone;
    });
    if (!ran) return;
    g_reconcile = kDone;  // also after an abandoned pass: it runs once per process
    log::write("reconcile: %s pass finished, total %.3f ms", early ? "early" : "inline", timer.ms());
}

// A game call that arrived while the pass runs: blocks until the pass releases the lock.
void wait_for_pass(const char* name) {
    log::Timer timer;
    AcquireSRWLockShared(&g_lock);
    ReleaseSRWLockShared(&g_lock);
    const double ms = timer.ms();
    log::write("reconcile: the game's call naming %s waited %.3f ms for the pass%s", name, ms,
               ms > kLongWaitMs ? " (over 2 s)" : "");
}

}  // namespace

void init(const Config& config) {
    AcquireSRWLockExclusive(&g_lock);
    g_session = std::make_unique<Session>(config);
    g_reconcile = kNotRun;
    ReleaseSRWLockExclusive(&g_lock);
}

void shutdown() {
    AcquireSRWLockExclusive(&g_lock);
    g_session.reset();
    ReleaseSRWLockExclusive(&g_lock);
}

void reconcile_early() {
    if (active()) run_pass(true, nullptr);
}

void before_call(const char* name) {
    if (g_reconcile == kDone || !active() || !guarded(name)) return;
    if (g_reconcile == kRunning) {
        wait_for_pass(name);
    } else if (g_session->model.is_own_system(name)) {
        run_pass(false, name);  // the early start has not happened
    }
}

void after_write(const char* name, const void* data, int32_t size, bool written) {
    if (!written || !active()) return;
    const int slot = g_session->model.own_slot(name);
    if (slot >= 0) {
        locked("save-time", [&]() {
            if (g_session) on_slot_written(*g_session, slot, name, data, size);
        });
    } else if (g_session->model.is_own_system(name)) {
        locked("save-time", [&]() {
            if (g_session) on_system_written(*g_session, name, data, size);
        });
    }
}

uint64_t before_delete(const char* name) {
    if (!active() || g_session->model.own_slot(name) < 0) return 0;
    uint64_t hash = 0;
    locked("delete", [&]() {
        if (g_session) hash = hash_of(name);
    });
    return hash;
}

void after_delete(const char* name, bool deleted, uint64_t deleted_hash) {
    if (!deleted || !active()) return;
    const int slot = g_session->model.own_slot(name);
    if (slot < 0) return;
    locked("delete", [&]() {
        if (g_session) record_deletion(*g_session, slot, deleted_hash);
    });
}

}  // namespace sync
}  // namespace plugin
