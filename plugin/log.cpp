#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace plugin {
namespace log {
namespace {

constexpr wchar_t kLogFileName[] = L"kakarot-hd-save-syncer.log";
constexpr size_t kMaxLine = 1024;

SRWLOCK g_lock = SRWLOCK_INIT;
HANDLE g_file = INVALID_HANDLE_VALUE;

void write_raw(const char* text, size_t length) {
    AcquireSRWLockExclusive(&g_lock);
    if (g_file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_file, text, static_cast<DWORD>(length), &written, nullptr);
    }
    ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace

void open(HMODULE module) {
    wchar_t path[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(module, path, static_cast<DWORD>(sizeof(path) / sizeof(path[0])));
    if (n == 0 || n >= sizeof(path) / sizeof(path[0])) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    slash[1] = L'\0';
    if (wcslen(path) + wcslen(kLogFileName) + 1 > sizeof(path) / sizeof(path[0])) return;
    wcscat_s(path, kLogFileName);
    open_path(path);
}

void open_path(const wchar_t* path) {
    HANDLE file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    AcquireSRWLockExclusive(&g_lock);
    HANDLE previous = g_file;
    g_file = file;
    ReleaseSRWLockExclusive(&g_lock);
    if (previous != INVALID_HANDLE_VALUE) CloseHandle(previous);

    write("==================== session start, pid %lu ====================", GetCurrentProcessId());
}

Timer::Timer() { QueryPerformanceCounter(&start_); }

double Timer::ms() const {
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return double(now.QuadPart - start_.QuadPart) * 1000.0 / double(frequency.QuadPart);
}

void close() {
    AcquireSRWLockExclusive(&g_lock);
    HANDLE file = g_file;
    g_file = INVALID_HANDLE_VALUE;
    ReleaseSRWLockExclusive(&g_lock);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
}

const char* utf8(const wchar_t* text, char* out, size_t out_size) {
    if (out_size == 0) return out;
    out[0] = '\0';
    if (!text) return out;
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, out, static_cast<int>(out_size), nullptr, nullptr);
    if (n <= 0) out[out_size - 1] = '\0';
    return out;
}

void write(const char* format, ...) {
    char line[kMaxLine];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int prefix = _snprintf_s(line, sizeof(line), _TRUNCATE, "%04u-%02u-%02u %02u:%02u:%02u.%03u [tid %5lu] ",
                             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                             GetCurrentThreadId());
    if (prefix < 0) prefix = static_cast<int>(strlen(line));

    va_list args;
    va_start(args, format);
    int body = _vsnprintf_s(line + prefix, sizeof(line) - prefix - 2, _TRUNCATE, format, args);
    va_end(args);
    // On truncation _vsnprintf_s returns -1 and leaves a NUL-terminated prefix of the text.
    size_t length = body < 0 ? strlen(line) : static_cast<size_t>(prefix + body);
    line[length++] = '\r';
    line[length++] = '\n';
    write_raw(line, length);
}

}  // namespace log
}  // namespace plugin
