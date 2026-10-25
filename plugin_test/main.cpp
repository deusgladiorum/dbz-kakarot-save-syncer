// plugin_test: runs the plugin's real hooks and sync logic against a fake Remote Storage.
//
//   plugin_test --exe STANDARD_EXE --fixtures BACKUPS_DIR [--scratch DIR] [--cli KAKAROT_CLI]
//
// --exe and --fixtures fall back to the KAKAROT_EXE and KAKAROT_FIXTURES environment variables.
//
// Fixtures are read only; each case copies the files it needs into its own scratch folder.
// Expected bytes come from kakarot_cli.exe (the oracle-tested C++ CLI) wherever the CLI can
// express the operation. The AES key is located in --exe at runtime and never printed.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "core/aes_paths.h"
#include "core/build.h"
#include "core/key.h"
#include "core/ops.h"
#include "plugin/hooks.h"
#include "plugin/log.h"
#include "plugin/manifest.h"
#include "plugin/storage.h"
#include "plugin/sync.h"
#include "plugin/vtable.h"
#include "plugin_test/fake_storage.h"

namespace fs = std::filesystem;
using kakarot::Build;
using kakarot::Bytes;
using plugin_test::FakeStorage;

namespace {

const char kStd1[] = "_matrix_1_std_pre_transfer";
const char kHd2[] = "_matrix_2_hd_post_transfer";
const char kHd3[] = "_matrix_3_hd_post_transfer_new_save";
const char kBackport[] = "_20261012_backup_before_backport";

struct Options {
    fs::path exe;
    fs::path fixtures;
    fs::path scratch;
    fs::path cli;
};

Options g_options;
kakarot::Key g_key{};

Bytes read_file(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::string read_text(const fs::path& path) {
    Bytes b = read_file(path);
    return std::string(b.begin(), b.end());
}

void write_file(const fs::path& path, const Bytes& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

fs::path fixture(const char* set, const char* name) { return g_options.fixtures / set / name; }

std::wstring quote(const fs::path& p) { return L"\"" + p.wstring() + L"\""; }

// Runs kakarot_cli --exe EXE <args>; output goes to `log`. True on exit code 0.
bool run_cli(const std::wstring& args, const fs::path& log) {
    std::wstring command = quote(g_options.cli) + L" --exe " + quote(g_options.exe) + L" " + args;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE out = CreateFileW(log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out;
    si.hStdError = out;
    PROCESS_INFORMATION pi{};
    BOOL started = CreateProcessW(nullptr, &command[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                  &si, &pi);
    DWORD code = 1;
    if (started) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    CloseHandle(out);
    return started && code == 0;
}

// ------------------------------------------------------------------------- one test case

struct FixtureFile {
    const char* set;
    const char* name;
};

class Case {
public:
    Case(const char* name, Build own, const std::vector<FixtureFile>& files) : name_(name), own_(own) {
        dir_ = g_options.scratch / name;
        std::error_code ignored;
        fs::remove_all(dir_, ignored);
        fs::create_directories(dir_ / "remote");
        fs::create_directories(dir_ / "expected");
        for (const FixtureFile& f : files) fs::copy_file(fixture(f.set, f.name), dir_ / "remote" / f.name);
    }

    ~Case() { stop(); }

    // Opens the log and the fake, patches the hooks in and starts a sync session.
    void start() {
        plugin::log::open_path((dir_ / "plugin.log").c_str());
        fake_ = std::make_unique<FakeStorage>(dir_ / "remote");
        plugin::storage::bind(fake_->object());
        plugin::vtable::install(fake_->object());
        new_session();
    }

    // A fresh session on the same files: what the next launch of the game sees.
    void new_session() {
        plugin::Config config;
        config.own = own_;
        config.key = g_key;
        config.local_manifest_path = local_manifest().wstring();
        plugin::sync::init(config);
    }

    void stop() {
        if (!fake_) return;
        plugin::vtable::restore();
        plugin::storage::unbind();
        plugin::sync::shutdown();
        plugin::log::close();
        fake_.reset();
    }

    FakeStorage& fake() { return *fake_; }
    fs::path local_manifest() const { return dir_ / plugin::manifest::kLocalName; }

    // Expected outputs, produced by kakarot_cli.
    fs::path transfer(const fs::path& ref, const fs::path& src, const char* out) {
        return cli_output(out, L"transfer --ref " + quote(ref) + L" --out " + quote(expected(out)) + L" " + quote(src));
    }
    fs::path merge(const fs::path& src, const fs::path& dst, const wchar_t* slots, const char* out) {
        return cli_output(out, L"system-merge --src " + quote(src) + L" --dst " + quote(dst) + L" --slots " + slots +
                                   L" --out " + quote(expected(out)));
    }
    fs::path convert_system(const fs::path& ref, const fs::path& src, const char* out) {
        return cli_output(out, L"system-convert --ref " + quote(ref) + L" --out " + quote(expected(out)) + L" " +
                                   quote(src));
    }

    void check(bool ok, const std::string& what) {
        if (!ok) failures_.push_back(what);
    }
    void check_bytes(const std::string& file, const Bytes& expected, const std::string& what) {
        check(fake_->has(file) && fake_->get(file) == expected, file + " " + what);
    }
    void check_file(const std::string& file, const fs::path& expected_path) {
        check(!expected_path.empty(), "expected " + file + " could not be produced by kakarot_cli");
        if (!expected_path.empty()) check_bytes(file, read_file(expected_path), "== kakarot_cli output");
    }
    void check_log(const std::string& needle) {
        check(read_text(dir_ / "plugin.log").find(needle) != std::string::npos, "log contains \"" + needle + "\"");
    }
    // Offset of the first log line containing `needle`, or npos.
    size_t log_pos(const std::string& needle) { return read_text(dir_ / "plugin.log").find(needle); }
    size_t log_count(const std::string& needle) {
        const std::string text = read_text(dir_ / "plugin.log");
        size_t n = 0;
        for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
        return n;
    }

    // The cloud manifest as the fake holds it; also checks that the local copy matches.
    plugin::manifest::Manifest cloud_manifest() {
        plugin::manifest::Manifest m;
        Bytes cloud = fake_->get(plugin::manifest::kCloudName);
        std::string text(cloud.begin(), cloud.end());
        check(m.parse(text), "cloud manifest parses");
        check(read_text(local_manifest()) == text, "local manifest copy equals the cloud copy");
        return m;
    }

    const std::vector<std::string>& failures() const { return failures_; }
    const std::string& name() const { return name_; }

private:
    fs::path expected(const char* out) const { return dir_ / "expected" / out; }
    fs::path cli_output(const char* out, const std::wstring& args) {
        return run_cli(args, dir_ / "cli.log") ? expected(out) : fs::path();
    }

    std::string name_;
    Build own_;
    fs::path dir_;
    std::unique_ptr<FakeStorage> fake_;
    std::vector<std::string> failures_;
};

int64_t hook_count(int function) {
    int64_t counts[plugin::hooks::kFunctionCount];
    plugin::hooks::counts(counts);
    return counts[function];
}

uint64_t hash_of(const fs::path& path) { return plugin::manifest::fnv1a64(read_file(path)); }

const std::vector<FixtureFile> kHd2Files = {{kHd2, "AutoSaveR0"}, {kHd2, "ManualSaveR0"}, {kHd2, "SystemSaveR0"}};
const std::vector<FixtureFile> kHd3Files = {{kHd3, "AutoSaveR0"}, {kHd3, "ManualSaveR0"}, {kHd3, "SystemSaveR0"}};
const std::vector<FixtureFile> kStd1Files = {{kStd1, "AutoSave0"}, {kStd1, "ManualSave0"}, {kStd1, "SystemSave0"}};

std::vector<FixtureFile> join(std::vector<FixtureFile> a, const std::vector<FixtureFile>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// ------------------------------------------------------------------------- cases

void manifest_roundtrip(Case& c) {
    plugin::manifest::Manifest m;
    for (int i = 0; i < 10; ++i) m.add({0, "HD->standard", 100u + i, 200u + i, "2026-10-25T00:00:00Z"});
    m.add({3, "deleted:HD", 0xfedcba9876543210ULL, 0, "t \"q\""});
    c.check(m.entries().size() == plugin::manifest::Manifest::kKeepPerSlot + 1, "only the newest 8 per slot kept");
    c.check(m.links(0, 109) && m.links(0, 209) && !m.links(0, 100) && !m.links(1, 109), "links by slot and hash");
    plugin::manifest::Manifest back;
    c.check(back.parse(m.serialize()) && back.serialize() == m.serialize(), "serialize/parse round trip");
    c.check(back.links(3, 0xfedcba9876543210ULL) && back.entries().back().time == "t \"q\"", "hash and escapes kept");
    c.check(!back.parse("{\"entries\": [ {\"slot\": 1,"), "truncated text is rejected");
    c.check(back.entries().empty(), "a rejected parse leaves no entries");
}

// The AES-NI path gives the same bytes as the software path (and both match FIPS-197 C.3).
void aes_paths_equal(Case& c) {
    namespace paths = kakarot::aes::paths;
    kakarot::Key key;
    for (int i = 0; i < 32; ++i) key[i] = static_cast<uint8_t>(i);
    const uint8_t plain[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                               0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    const uint8_t cipher[16] = {0x8e, 0xa2, 0xb7, 0xca, 0x51, 0x67, 0x45, 0xbf,
                                0xea, 0xfc, 0x49, 0x90, 0x4b, 0x49, 0x60, 0x89};
    const bool hardware = paths::hardware_supported();
    if (!hardware) std::printf("        (no AES-NI on this CPU: only the software path is checked)\n");
    Bytes block(plain, plain + 16);
    c.check(paths::encrypt_ecb_software(key, block.data(), 16) && Bytes(cipher, cipher + 16) == block,
            "software encrypt == FIPS-197 C.3");
    c.check(paths::decrypt_ecb_software(key, block.data(), 16) && Bytes(plain, plain + 16) == block,
            "software decrypt == FIPS-197 C.3");
    if (hardware) {
        c.check(paths::encrypt_ecb_hardware(key, block.data(), 16) && Bytes(cipher, cipher + 16) == block,
                "AES-NI encrypt == FIPS-197 C.3");
        c.check(paths::decrypt_ecb_hardware(key, block.data(), 16) && Bytes(plain, plain + 16) == block,
                "AES-NI decrypt == FIPS-197 C.3");
    }
    uint8_t odd[17] = {};
    c.check(!paths::encrypt_ecb_software(key, odd, 17) && !paths::encrypt_ecb_hardware(key, odd, 17) &&
                !kakarot::aes::encrypt_ecb(key, odd, 17),
            "a size that is not a multiple of 16 is refused");

    // Random keys and buffers from 0 B to 1 MB + 1 block: 8-block batches with every tail length.
    std::mt19937_64 rng(20261025);
    const size_t sizes[] = {0, 16, 32, 112, 128, 144, 240, 256, 4096 + 48, (1u << 20) + 16};
    for (size_t size : sizes) {
        for (uint8_t& b : key) b = static_cast<uint8_t>(rng());
        Bytes data(size);
        for (uint8_t& b : data) b = static_cast<uint8_t>(rng());
        Bytes sw = data, hw = data, dispatch = data;
        const std::string what = std::to_string(size) + " B: ";
        c.check(paths::encrypt_ecb_software(key, sw.data(), sw.size()), what + "software encrypt");
        c.check(kakarot::aes::encrypt_ecb(key, dispatch.data(), dispatch.size()) && dispatch == sw,
                what + "aes::encrypt_ecb == software");
        if (hardware)
            c.check(paths::encrypt_ecb_hardware(key, hw.data(), hw.size()) && hw == sw,
                    what + "AES-NI == software (encrypt)");
        Bytes sw_back = sw, hw_back = sw;
        c.check(paths::decrypt_ecb_software(key, sw_back.data(), sw_back.size()) && sw_back == data,
                what + "software round trip");
        if (hardware)
            c.check(paths::decrypt_ecb_hardware(key, hw_back.data(), hw_back.size()) && hw_back == data,
                    what + "AES-NI == software (decrypt)");
    }

    // A real save under the game's key.
    const Bytes save = read_file(fixture(kHd3, "SystemSaveR0"));
    Bytes sw = save, hw = save;
    c.check(!save.empty() && paths::decrypt_ecb_software(g_key, sw.data(), sw.size()), "fixture decrypts (software)");
    c.check(sw.size() >= 4 && std::equal(sw.begin(), sw.begin() + 4, "GVAS"), "fixture decrypts to GVAS");
    if (hardware)
        c.check(paths::decrypt_ecb_hardware(g_key, hw.data(), hw.size()) && hw == sw, "fixture: AES-NI == software");
}

// (a) HD build: a manual save produces the standard slot and merges its Headers entry.
void save_time_hd(Case& c) {
    c.start();
    FakeStorage& f = c.fake();
    c.check(f.game_exists("SystemSaveR0"), "title-screen FileExists");
    c.check(f.writes("ManualSave0") == 0 && f.writes("SystemSave0") == 0, "reconcile: in step, nothing written");
    const int64_t writes_before = hook_count(plugin::hooks::kFileWrite);
    c.check(f.game_write("ManualSaveR0", read_file(fixture(kHd3, "ManualSaveR0"))), "game slot write succeeds");
    c.check(f.game_write("SystemSaveR0", read_file(fixture(kHd3, "SystemSaveR0"))), "game system write succeeds");
    c.check(hook_count(plugin::hooks::kFileWrite) - writes_before == 2, "hooks saw only the game's 2 writes");

    fs::path slot = c.transfer(fixture(kStd1, "SystemSave0"), fixture(kHd3, "ManualSaveR0"), "ManualSave0");
    c.check_file("ManualSave0", slot);
    c.check_file("SystemSave0", c.merge(fixture(kHd3, "SystemSaveR0"), fixture(kStd1, "SystemSave0"), L"0", "SystemSave0"));
    c.check(f.writes("ManualSave0") == 1 && f.writes("SystemSave0") == 1, "one write per output");

    auto m = c.cloud_manifest();
    c.check(m.entries().size() == 1, "one manifest entry");
    if (!m.entries().empty()) {
        const auto& e = m.entries()[0];
        c.check(e.slot == 0 && e.direction == "HD->standard", "entry slot and direction");
        c.check(e.source == hash_of(fixture(kHd3, "ManualSaveR0")) && e.output == hash_of(slot), "entry hashes");
    }
    c.check(f.game_write("SystemSaveR0", read_file(fixture(kHd3, "SystemSaveR0"))), "second system write");
    c.check(f.writes("SystemSave0") == 1, "a system-save write with nothing pending does nothing");
    c.check_log("reference for standard: SystemSave0");
}

// (a) standard build: the same in the other direction.
void save_time_standard(Case& c) {
    c.start();
    FakeStorage& f = c.fake();
    c.check(f.game_exists("SystemSave0"), "title-screen FileExists");
    c.check(f.game_write("ManualSave0", read_file(fixture(kBackport, "ManualSave0"))), "game slot write");
    c.check(f.game_write("SystemSave0", read_file(fixture(kBackport, "SystemSave0"))), "game system write");
    c.check_file("ManualSaveR0", c.transfer(fixture(kHd2, "SystemSaveR0"), fixture(kBackport, "ManualSave0"), "ManualSaveR0"));
    c.check_file("SystemSaveR0",
                 c.merge(fixture(kBackport, "SystemSave0"), fixture(kHd2, "SystemSaveR0"), L"0", "SystemSaveR0"));
    auto m = c.cloud_manifest();
    c.check(m.entries().size() == 1 && m.entries()[0].direction == "standard->HD", "entry direction standard->HD");
}

// (b) foreign newer: the standard slot 0 is newer, so it is converted in.
void reconcile_foreign_newer(Case& c) {
    c.start();
    FakeStorage& f = c.fake();
    c.check(f.game_exists("SystemSaveR0"), "trigger");
    c.check_file("ManualSaveR0", c.transfer(fixture(kHd2, "SystemSaveR0"), fixture(kBackport, "ManualSave0"), "ManualSaveR0"));
    c.check_file("SystemSaveR0",
                 c.merge(fixture(kBackport, "SystemSave0"), fixture(kHd2, "SystemSaveR0"), L"0", "SystemSaveR0"));
    c.check(f.writes("ManualSave0") + f.writes("SystemSave0") + f.writes("AutoSave0") + f.writes("AutoSaveR0") == 0,
            "foreign files and the in-step autosave are not written");
    auto m = c.cloud_manifest();
    c.check(m.entries().size() == 1 && m.entries()[0].direction == "standard->HD", "one standard->HD entry");
    c.check_log("slot 0: foreign newer");
    c.check_log("slot 10: same timestamp");
}

// (b) same: a converted pair is never converted again.
void reconcile_same(Case& c) {
    c.start();
    FakeStorage& f = c.fake();
    c.check(f.game_exists("SystemSaveR0"), "trigger");
    c.check(f.writes("ManualSave0") + f.writes("SystemSave0") + f.writes("ManualSaveR0") + f.writes("SystemSaveR0") +
                    f.writes(plugin::manifest::kCloudName) ==
                0,
            "nothing written");
    c.check(c.log_count("same timestamp, nothing to do") == 2, "slots 0 and 10 are in step");
}

// (b) own newer: the HD slot 0 is newer, so the standard copy is produced and merged.
void reconcile_own_newer(Case& c) {
    c.start();
    c.check(c.fake().game_exists("SystemSaveR0"), "trigger");
    c.check_file("ManualSave0", c.transfer(fixture(kStd1, "SystemSave0"), fixture(kHd3, "ManualSaveR0"), "ManualSave0"));
    c.check_file("SystemSave0", c.merge(fixture(kHd3, "SystemSaveR0"), fixture(kStd1, "SystemSave0"), L"0", "SystemSave0"));
    c.check(c.fake().writes("AutoSave0") == 0, "the in-step autosave is not written");
    c.check_log("slot 0: own newer");
}

// (b) only the own side exists and the manifest does not know it: a new save, produced.
void reconcile_only_own_unlinked(Case& c) {
    c.start();
    c.check(c.fake().game_exists("SystemSaveR0"), "trigger");
    c.check_file("ManualSave0", c.transfer(fixture(kStd1, "SystemSave0"), fixture(kHd3, "ManualSaveR0"), "ManualSave0"));
    c.check_file("SystemSave0", c.merge(fixture(kHd3, "SystemSaveR0"), fixture(kStd1, "SystemSave0"), L"0", "SystemSave0"));
    c.check_log("only ManualSaveR0 exists, not in the manifest");
}

// (b) only the own side exists and the (local) manifest links it: deleted on purpose, left alone.
void reconcile_only_own_linked(Case& c) {
    plugin::manifest::Manifest m;
    m.add({0, "deleted:standard", 0x1234, hash_of(fixture(kHd3, "ManualSaveR0")), "2026-10-25T00:00:00Z"});
    const std::string text = m.serialize();
    write_file(c.local_manifest(), Bytes(text.begin(), text.end()));
    c.start();
    FakeStorage& f = c.fake();
    c.check(f.game_exists("SystemSaveR0"), "trigger");
    c.check(!f.has("ManualSave0") && f.writes("SystemSave0") == 0, "nothing produced");
    c.check_log("from the local copy");
    c.check_log("deleted on purpose, left alone");
}

// (b) only the foreign side exists: converted in; matches the game's native transfer.
void reconcile_only_foreign_unlinked(Case& c) {
    c.start();
    c.check(c.fake().game_exists("SystemSaveR0"), "trigger");
    c.check_file("ManualSaveR0", c.transfer(fixture(kHd2, "SystemSaveR0"), fixture(kStd1, "ManualSave0"), "ManualSaveR0"));
    c.check_bytes("ManualSaveR0", read_file(fixture(kHd2, "ManualSaveR0")), "== the native transfer output");
    c.check_file("SystemSaveR0", c.merge(fixture(kStd1, "SystemSave0"), fixture(kHd2, "SystemSaveR0"), L"0", "SystemSaveR0"));
}

// (b) the foreign system save is absent: the own one is converted whole with the reference.
void reconcile_foreign_system_absent(Case& c) {
    c.start();
    c.check(c.fake().game_exists("SystemSaveR0"), "trigger");
    const fs::path ref = fixture(kStd1, "ManualSave0");
    c.check_file("ManualSave0", c.transfer(ref, fixture(kHd3, "ManualSaveR0"), "ManualSave0"));
    c.check_file("AutoSave0", c.transfer(ref, fixture(kHd3, "AutoSaveR0"), "AutoSave0"));
    c.check_file("SystemSave0", c.convert_system(ref, fixture(kHd3, "SystemSaveR0"), "SystemSave0"));
    c.check_log("convert whole to SystemSave0");
}

// (b) no reference: the only standard-named file carries an HD header, so it is rejected and
// outputs keep the source header and stamp (design fallback).
void reconcile_no_reference(Case& c) {
    c.start();
    c.check(c.fake().game_exists("SystemSaveR0"), "trigger");
    c.check_bytes("ManualSave0", read_file(fixture(kHd3, "ManualSaveR0")), "== source (header and stamp kept)");
    c.check_bytes("AutoSave0", read_file(fixture(kHd3, "AutoSaveR0")), "== source (header and stamp kept)");
    const Bytes system = read_file(fixture(kHd3, "SystemSaveR0"));
    Bytes expected;
    c.check(kakarot::ops::convert_system_save(system, system, g_key, false, expected), "core convert to standard");
    c.check_bytes("SystemSave0", expected, "== convert_system_save(own, own as reference, to standard)");
    c.check(c.fake().writes("ManualSave1") == 0, "the foreign file without a Headers entry is left alone");
    c.check_log("ManualSave1 (time");
    c.check_log("has a HD header, skipped");
    c.check_log("reference for standard: none found");
    c.check_log("slot 1: SystemSave0 has no Headers[1] entry");
}

// (c) the plugin's own writes call back through the hooked vtable; the hooks must forward them
// without any sync logic (no save-time production from the plugin's own ManualSaveR0 write).
void reentrancy(Case& c) {
    c.start();
    FakeStorage& f = c.fake();
    f.reenter = true;
    f.reenter_exists = "SystemSaveR0";
    const int64_t writes_before = hook_count(plugin::hooks::kFileWrite);
    const int64_t exists_before = hook_count(plugin::hooks::kFileExists);
    c.check(f.game_exists("SystemSaveR0"), "trigger");
    c.check(hook_count(plugin::hooks::kFileWrite) == writes_before, "no plugin write counted as a game call");
    c.check(hook_count(plugin::hooks::kFileExists) - exists_before == 1, "only the game's FileExists counted");
    c.check(f.writes("ManualSaveR0") == 2 && f.writes("SystemSaveR0") == 2, "re-entrant writes forwarded once each");
    c.check(f.writes("ManualSave0") == 0 && f.writes("SystemSave0") == 0, "no save-time logic ran on them");
    c.check_file("ManualSaveR0", c.transfer(fixture(kHd2, "SystemSaveR0"), fixture(kBackport, "ManualSave0"), "ManualSaveR0"));
    c.check(c.log_count("reconcile: start") == 1, "the pass ran once");
}

// (d) Steam refuses the plugin's slot write: logged, nothing pending, nothing recorded; the
// next save works normally. A refused game write gets no plugin action at all.
void refused_slot_write(Case& c) {
    c.start();
    FakeStorage& f = c.fake();
    c.check(f.game_exists("SystemSaveR0"), "trigger");
    f.refused = {"ManualSave0"};
    c.check(f.game_write("ManualSaveR0", read_file(fixture(kHd3, "ManualSaveR0"))), "the game's write still succeeds");
    c.check(f.game_write("SystemSaveR0", read_file(fixture(kHd3, "SystemSaveR0"))), "the game's system write succeeds");
    c.check_log("plugin write \"ManualSave0\"");
    c.check_log("-> REFUSED");
    c.check(f.writes("ManualSave0") == 1, "the refused write was not retried");
    c.check_bytes("ManualSave0", read_file(fixture(kStd1, "ManualSave0")), "unchanged");
    c.check(f.writes("SystemSave0") == 0 && !f.has(plugin::manifest::kCloudName), "no merge, no manifest");

    f.refused = {"ManualSaveR0"};
    c.check(!f.game_write("ManualSaveR0", read_file(fixture(kHd3, "ManualSaveR0"))), "the game sees its refusal");
    c.check(f.writes("ManualSave0") == 1, "no plugin action after a refused game write");

    f.refused.clear();
    c.check(f.game_write("ManualSaveR0", read_file(fixture(kHd3, "ManualSaveR0"))), "next save");
    c.check(f.game_write("SystemSaveR0", read_file(fixture(kHd3, "SystemSaveR0"))), "next system save");
    c.check_file("ManualSave0", c.transfer(fixture(kStd1, "SystemSave0"), fixture(kHd3, "ManualSaveR0"), "ManualSave0"));
    c.check_file("SystemSave0", c.merge(fixture(kHd3, "SystemSaveR0"), fixture(kStd1, "SystemSave0"), L"0", "SystemSave0"));
}

// (d) Steam refuses the plugin's system-save write: the slot link is recorded, and the next
// launch's reconcile pass sees "own newer" and completes the merge.
void refused_system_write(Case& c) {
    c.start();
    FakeStorage& f = c.fake();
    c.check(f.game_exists("SystemSaveR0"), "trigger");
    f.refused = {"SystemSave0"};
    c.check(f.game_write("ManualSaveR0", read_file(fixture(kHd3, "ManualSaveR0"))), "game slot write");
    c.check(f.game_write("SystemSaveR0", read_file(fixture(kHd3, "SystemSaveR0"))), "game system write");
    const fs::path slot = c.transfer(fixture(kStd1, "SystemSave0"), fixture(kHd3, "ManualSaveR0"), "ManualSave0");
    c.check_file("ManualSave0", slot);
    c.check_bytes("SystemSave0", read_file(fixture(kStd1, "SystemSave0")), "unchanged");
    c.check_log("FAILED (the next launch's reconcile pass retries)");
    c.check(c.cloud_manifest().links(0, hash_of(slot)), "the produced slot is recorded");

    f.refused.clear();
    c.new_session();
    c.check(f.game_exists("SystemSaveR0"), "next launch trigger");
    c.check_file("SystemSave0", c.merge(fixture(kHd3, "SystemSaveR0"), fixture(kStd1, "SystemSave0"), L"0", "SystemSave0"));
    c.check_file("ManualSave0", slot);
}

// FileDelete of an own slot records a deletion and keeps the foreign copy; the next launch
// finds the survivor linked (through the cloud manifest) and leaves it alone.
void delete_then_reconcile(Case& c) {
    c.start();
    FakeStorage& f = c.fake();
    c.check(f.game_exists("SystemSaveR0"), "trigger (own newer: ManualSave0 produced)");
    const Bytes survivor = f.get("ManualSave0");
    c.check(f.game_delete("ManualSaveR0"), "game deletes its slot");
    c.check(f.has("ManualSave0") && f.get("ManualSave0") == survivor, "the foreign copy is kept");
    auto m = c.cloud_manifest();
    c.check(!m.entries().empty() && m.entries().back().direction == "deleted:HD" &&
                m.entries().back().source == hash_of(fixture(kHd3, "ManualSaveR0")) &&
                m.entries().back().output == plugin::manifest::fnv1a64(survivor),
            "deletion entry with both hashes");

    c.new_session();
    c.check(f.game_exists("SystemSaveR0"), "next launch trigger");
    c.check(!f.has("ManualSaveR0"), "the deleted slot is not produced again");
    c.check_log("from the cloud copy");
    c.check_log("deleted on purpose, left alone");
}

// (f) The expected outputs when the standard slot 0 is newer (the b_foreign_newer fixtures).
void check_foreign_newer_outputs(Case& c) {
    c.check_file("ManualSaveR0", c.transfer(fixture(kHd2, "SystemSaveR0"), fixture(kBackport, "ManualSave0"), "ManualSaveR0"));
    c.check_file("SystemSaveR0",
                 c.merge(fixture(kBackport, "SystemSave0"), fixture(kHd2, "SystemSaveR0"), L"0", "SystemSaveR0"));
    c.check(c.log_count("reconcile: start") == 1, "the pass ran once");
}

// (f) The pass starts on the plugin's thread when the hooks go active and is over before the
// game's first call, which does not wait. The pass's own calls that come back through the hooks
// on its thread are forwarded, not held by the guard (the fake re-enters on every write).
void early_pass_no_wait(Case& c) {
    c.start();
    FakeStorage& f = c.fake();
    f.reenter = true;
    f.reenter_exists = "SystemSaveR0";
    const int64_t writes_before = hook_count(plugin::hooks::kFileWrite);
    const int64_t exists_before = hook_count(plugin::hooks::kFileExists);
    std::thread wait_thread(plugin::sync::reconcile_early);
    wait_thread.join();
    f.reenter = false;
    c.check(hook_count(plugin::hooks::kFileWrite) == writes_before &&
                hook_count(plugin::hooks::kFileExists) == exists_before,
            "the pass's re-entrant calls were forwarded, not counted as game calls");
    c.check(f.writes("ManualSaveR0") == 2 && f.writes("SystemSaveR0") == 2, "re-entrant writes forwarded once each");
    c.check(f.game_exists("SystemSaveR0"), "the game's first call");
    c.check(hook_count(plugin::hooks::kFileExists) - exists_before == 1, "one game call counted");
    c.check_log("reconcile: early start, on the plugin's thread");
    c.check_log("reconcile: early pass finished, total");
    c.check(c.log_count("waited") == 0, "the game's call did not wait");
    c.check(c.log_count("inline start") == 0, "no inline pass");
    const size_t done = c.log_pos("early pass finished"), call = c.log_pos("FileExists(\"SystemSaveR0\")");
    c.check(done != std::string::npos && call != std::string::npos && done < call,
            "the pass finished before the game's call");
    check_foreign_newer_outputs(c);
}

// (f) While the early pass runs, a game call naming another file passes straight through, and
// one naming a save file (here a foreign slot) waits for the pass, then proceeds.
void game_call_waits(Case& c) {
    constexpr DWORD kHoldMs = 500;
    c.start();
    FakeStorage& f = c.fake();
    HANDLE in_pass = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool held = false;
    f.on_read = [&](const std::string&) {
        if (held) return;
        held = true;
        SetEvent(in_pass);
        Sleep(kHoldMs);  // the pass is inside its first read
    };
    std::thread wait_thread(plugin::sync::reconcile_early);
    c.check(WaitForSingleObject(in_pass, 10000) == WAIT_OBJECT_0, "the pass reached its first read");

    plugin::log::Timer other;
    c.check(!f.game_exists("SomeOtherFile"), "a call naming another file is forwarded");
    c.check(other.ms() < kHoldMs / 2, "and does not wait");
    plugin::log::Timer slot;
    c.check(f.game_exists("ManualSave0"), "a call naming the foreign slot is forwarded");
    c.check(slot.ms() > kHoldMs / 4, "after waiting");
    wait_thread.join();
    f.on_read = nullptr;
    CloseHandle(in_pass);

    c.check(c.log_count("waited") == 1, "one call waited");
    c.check_log("the game's call naming ManualSave0 waited");
    const size_t other_call = c.log_pos("FileExists(\"SomeOtherFile\")");
    const size_t done = c.log_pos("early pass finished");
    const size_t slot_call = c.log_pos("FileExists(\"ManualSave0\")");
    c.check(other_call != std::string::npos && done != std::string::npos && other_call < done,
            "the other file's call went through during the pass");
    c.check(slot_call != std::string::npos && done < slot_call, "the slot call was forwarded after the pass finished");
    check_foreign_newer_outputs(c);
}

// (f) Fallback: if the early start has not happened, the first call naming the own system save
// runs the pass inline as before; a late early start then does nothing.
void inline_fallback(Case& c) {
    c.start();
    FakeStorage& f = c.fake();
    c.check(f.game_exists("ManualSave0"), "a foreign-slot call before any pass is forwarded");
    c.check(c.log_count("reconcile: start") == 0, "and does not start the pass");
    c.check(f.game_exists("SystemSaveR0"), "trigger");
    c.check_log("reconcile: inline start (fallback), by the game's first call naming SystemSaveR0");
    c.check_log("reconcile: inline pass finished, total");
    std::thread late(plugin::sync::reconcile_early);
    late.join();
    c.check(c.log_count("early start") == 0, "a late early start does nothing");
    check_foreign_newer_outputs(c);
}

// ------------------------------------------------------------------------- runner

struct TestCase {
    const char* name;
    Build own;
    std::vector<FixtureFile> files;
    void (*run)(Case&);
};

fs::path env_path(const wchar_t* name) {
    wchar_t value[MAX_PATH * 2] = {};
    DWORD n = GetEnvironmentVariableW(name, value, MAX_PATH * 2);
    return (n > 0 && n < MAX_PATH * 2) ? fs::path(value) : fs::path();
}

bool parse_options(int argc, wchar_t** argv) {
    for (int i = 1; i + 1 < argc; i += 2) {
        std::wstring a = argv[i];
        if (a == L"--exe") g_options.exe = argv[i + 1];
        else if (a == L"--fixtures") g_options.fixtures = argv[i + 1];
        else if (a == L"--scratch") g_options.scratch = argv[i + 1];
        else if (a == L"--cli") g_options.cli = argv[i + 1];
        else return false;
    }
    if (g_options.exe.empty()) g_options.exe = env_path(L"KAKAROT_EXE");
    if (g_options.fixtures.empty()) g_options.fixtures = env_path(L"KAKAROT_FIXTURES");
    if (g_options.exe.empty() || g_options.fixtures.empty()) return false;
    wchar_t self[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
    const fs::path bin = fs::path(self).parent_path();  // <repo>\x64\<Configuration>
    if (g_options.cli.empty()) g_options.cli = bin / "kakarot_cli.exe";
    if (g_options.scratch.empty()) g_options.scratch = bin.parent_path().parent_path() / "out" / "plugin_test";
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (!parse_options(argc, argv)) {
        std::fprintf(stderr, "usage: plugin_test --exe EXE --fixtures DIR [--scratch DIR] [--cli KAKAROT_CLI] (or set KAKAROT_EXE and KAKAROT_FIXTURES)\n");
        return 2;
    }
    std::string error;
    if (!kakarot::key::find_in_file(g_options.exe, g_key, &error)) {
        std::fprintf(stderr, "cannot locate the key in --exe: %s\n", error.c_str());
        return 2;
    }

    const auto none = std::vector<FixtureFile>();
    const auto foreign_newer =
        join(kHd2Files, {{kBackport, "AutoSave0"}, {kBackport, "ManualSave0"}, {kBackport, "SystemSave0"}});
    const std::vector<TestCase> cases = {
        {"manifest_roundtrip", Build::HD, none, manifest_roundtrip},
        {"aes_paths_equal", Build::HD, none, aes_paths_equal},
        {"a_save_time_hd", Build::HD, join(kHd2Files, kStd1Files), save_time_hd},
        {"a_save_time_standard", Build::Standard, join(kStd1Files, kHd2Files), save_time_standard},
        {"b_foreign_newer", Build::HD,
         join(kHd2Files, {{kBackport, "AutoSave0"}, {kBackport, "ManualSave0"}, {kBackport, "SystemSave0"}}),
         reconcile_foreign_newer},
        {"b_same", Build::HD, join(kHd2Files, kStd1Files), reconcile_same},
        {"b_own_newer", Build::HD, join(kHd3Files, kStd1Files), reconcile_own_newer},
        {"b_only_own_unlinked", Build::HD, join(kHd3Files, {{kStd1, "AutoSave0"}, {kStd1, "SystemSave0"}}),
         reconcile_only_own_unlinked},
        {"b_only_own_linked", Build::HD, join(kHd3Files, {{kStd1, "AutoSave0"}, {kStd1, "SystemSave0"}}),
         reconcile_only_own_linked},
        {"b_only_foreign_unlinked", Build::HD, join({{kHd2, "AutoSaveR0"}, {kHd2, "SystemSaveR0"}}, kStd1Files),
         reconcile_only_foreign_unlinked},
        {"b_foreign_system_absent", Build::HD, join(kHd3Files, {{kStd1, "ManualSave0"}}),
         reconcile_foreign_system_absent},
        {"b_no_reference", Build::HD, join(kHd3Files, {{kBackport, "ManualSave1"}}), reconcile_no_reference},
        {"c_reentrancy", Build::HD,
         join(kHd2Files, {{kBackport, "AutoSave0"}, {kBackport, "ManualSave0"}, {kBackport, "SystemSave0"}}),
         reentrancy},
        {"d_refused_slot_write", Build::HD, join(kHd2Files, kStd1Files), refused_slot_write},
        {"d_refused_system_write", Build::HD, join(kHd2Files, kStd1Files), refused_system_write},
        {"e_delete_then_reconcile", Build::HD, join(kHd3Files, kStd1Files), delete_then_reconcile},
        {"f_early_pass_no_wait", Build::HD, foreign_newer, early_pass_no_wait},
        {"f_game_call_waits", Build::HD, foreign_newer, game_call_waits},
        {"f_inline_fallback", Build::HD, foreign_newer, inline_fallback},
    };

    int passed = 0;
    for (const TestCase& t : cases) {
        plugin::log::Timer timer;
        std::vector<std::string> failures;
        try {
            Case c(t.name, t.own, t.files);
            t.run(c);
            c.stop();
            failures = c.failures();
        } catch (const std::exception& e) {
            failures.push_back(std::string("exception: ") + e.what());
        }
        std::printf("%s  %-28s %8.0f ms\n", failures.empty() ? "PASS" : "FAIL", t.name, timer.ms());
        for (const std::string& f : failures) std::printf("        - %s\n", f.c_str());
        passed += failures.empty();
    }
    std::printf("\n%d/%zu cases passed (logs and outputs under %s)\n", passed, cases.size(),
                g_options.scratch.u8string().c_str());
    SecureZeroMemory(g_key.data(), g_key.size());
    return passed == static_cast<int>(cases.size()) ? 0 : 1;
}
