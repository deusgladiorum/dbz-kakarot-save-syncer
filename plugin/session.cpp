#include "session.h"

#include "plugin/log.h"

namespace plugin {

using kakarot::Build;
namespace build = kakarot::build;

Model::Model(Build own) : own_(own), foreign_(own == Build::HD ? Build::Standard : Build::HD) {
    const Build builds[2] = {own_, foreign_};
    for (int side = 0; side < 2; ++side) {
        for (int slot = 0; slot < kSlotCount; ++slot) slots_[side][slot] = build::slot_file_name(slot, builds[side]);
        system_[side] = build::slot_file_name(build::kSystemSlot, builds[side]);
    }
}

int Model::own_slot(const char* name) const {
    for (int slot = 0; slot < kSlotCount; ++slot)
        if (slots_[0][slot] == name) return slot;
    return -1;
}

bool Model::is_save_file(const char* name) const {
    for (int side = 0; side < 2; ++side) {
        if (system_[side] == name) return true;
        for (int slot = 0; slot < kSlotCount; ++slot)
            if (slots_[side][slot] == name) return true;
    }
    return false;
}

Session::Session(const Config& config)
    : model(config.own), key(config.key), local_manifest_path(config.local_manifest_path) {}

void ensure_manifest(Session& session) {
    if (session.manifest_loaded) return;
    session.manifest = manifest::load(session.local_manifest_path);
    session.manifest_loaded = true;
}

void save_manifest(Session& session) {
    if (!manifest::save(session.manifest, session.local_manifest_path))
        log::write("manifest: not saved anywhere; it stays in memory for this session");
}

std::string direction(const Model& model, Side target) {
    return std::string(build::name(model.build(other(target)))) + "->" + build::name(model.build(target));
}

}  // namespace plugin
