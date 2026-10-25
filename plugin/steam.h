// Steam Remote Storage access from inside the game: waits for the game's Steam session,
// obtains ISteamRemoteStorage (v014) through the flat steam_api64 exports, binds the plugin's
// own I/O to it (storage.h) and patches the hooked vtable slots (vtable.h).
#pragma once

#include <windows.h>

namespace plugin {
namespace steam {

// Called on the wait thread once the interface is obtained and before anything is patched;
// returning false leaves the game untouched (no hooks).
using PrepareCallback = bool (*)();

// Called on the wait thread right after the hooks are installed (the "active" log line), before
// the heartbeat loop starts.
using ActiveCallback = void (*)();

// Starts the background wait thread, which holds a reference to the plugin DLL while it runs.
// Returns false if the thread could not be started.
bool start(PrepareCallback prepare, ActiveCallback on_active = nullptr);

// Asks the wait thread to stop. Does not wait (safe under the loader lock).
void stop();

}  // namespace steam
}  // namespace plugin
