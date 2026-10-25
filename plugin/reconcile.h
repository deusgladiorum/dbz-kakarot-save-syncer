// The reconcile pass (docs/design.html, 5): once per process, compare every slot's own and
// foreign copies by the save timestamp in Headers[slot] and bring the older or missing side up to
// date. It is started on the plugin's thread as soon as the hooks are active, or, as a fallback,
// before the game's first call naming its own system save is let through (sync.h).
#pragma once

#include "plugin/session.h"

namespace plugin {
namespace reconcile {

void run(Session& session);

}  // namespace reconcile
}  // namespace plugin
