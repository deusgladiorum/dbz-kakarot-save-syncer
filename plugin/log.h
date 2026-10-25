// Plugin log: timestamped lines appended to kakarot-hd-save-syncer.log next to the plugin DLL.
//
// Every line carries local time (ms) and the calling thread id. Lines are formatted into a
// stack buffer outside the lock, then written with one WriteFile under an SRW lock, so a line
// is in the OS file cache (and survives a game crash) as soon as write() returns. Nothing here
// allocates or throws.
#pragma once

#include <windows.h>

namespace plugin {
namespace log {

// Opens (appends to) the log file next to `module` and writes a session banner.
void open(HMODULE module);

// Opens (appends to) the log file at `path` and writes a session banner.
void open_path(const wchar_t* path);

// Closes the log file; later write() calls are dropped.
void close();

// printf-style; appends one line (a trailing newline is added). Long lines are truncated.
void write(const char* format, ...);

// Converts `text` to UTF-8 in `out` (truncated to fit); returns `out`. For paths in log lines.
const char* utf8(const wchar_t* text, char* out, size_t out_size);

// Elapsed wall time since construction, for the "| x ms" part of log lines.
class Timer {
public:
    Timer();
    double ms() const;

private:
    LARGE_INTEGER start_;
};

}  // namespace log
}  // namespace plugin
