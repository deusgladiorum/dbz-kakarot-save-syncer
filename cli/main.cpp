// Command-line front end for kakarot_core, mirroring the Python tools in tools/.
//
//   kakarot_cli --exe EXE decrypt --out DIR FILE...
//   kakarot_cli --exe EXE encrypt --out DIR FILE...
//   kakarot_cli --exe EXE header FILE...
//   kakarot_cli --exe EXE transfer --ref REF --out OUT SRC
//   kakarot_cli --exe EXE system-convert --ref REF --out OUT SRC
//   kakarot_cli --exe EXE system-merge --src SRC --dst DST --slots 0,10|all --out OUT
//   kakarot_cli keyscan EXE...
//   kakarot_cli bench
//
// The key is always located in --exe at runtime and is never printed.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "core/aes_paths.h"
#include "core/build.h"
#include "core/gvas.h"
#include "core/key.h"
#include "core/ops.h"

namespace fs = std::filesystem;
using namespace kakarot;

namespace {

struct Args {
    std::string command;
    std::map<std::string, std::wstring> options;  // "--out" -> value
    std::vector<std::wstring> positional;
};

std::string narrow(const std::wstring& w) {
    return fs::path(w).u8string();
}

Args parse_args(int argc, wchar_t** argv) {
    static const char* kValued[] = {"--exe", "--out", "--ref", "--src", "--dst", "--slots"};
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = narrow(argv[i]);
        bool valued = false;
        for (const char* v : kValued) valued |= (arg == v);
        if (valued && i + 1 < argc) {
            a.options[arg] = argv[++i];
        } else if (a.command.empty()) {
            a.command = arg;
        } else {
            a.positional.push_back(argv[i]);
        }
    }
    return a;
}

// Prints an error and returns false (for bool helpers).
bool report(const std::string& message) {
    std::fprintf(stderr, "error: %s\n", message.c_str());
    return false;
}

// Prints an error and returns exit code 1.
int fail(const std::string& message) {
    report(message);
    return 1;
}

bool read_file(const fs::path& path, Bytes& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool write_file(const fs::path& path, const Bytes& data) {
    if (path.has_parent_path()) fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

// A non-reversible 64-bit FNV-1a fingerprint, so keys can be compared without printing them.
unsigned long long fingerprint(const Key& key) {
    unsigned long long h = 1469598103934665603ULL;
    for (uint8_t b : key) h = (h ^ b) * 1099511628211ULL;
    return h;
}

const std::wstring* option(const Args& a, const char* name) {
    auto it = a.options.find(name);
    return it == a.options.end() ? nullptr : &it->second;
}

// Loads the key from --exe and checks it against an encrypted sample file.
bool load_key(const Args& a, const fs::path& sample, Key& key) {
    const std::wstring* exe = option(a, "--exe");
    if (!exe) return report("--exe is required");
    std::string error;
    if (!key::find_in_file(*exe, key, &error)) return report(narrow(*exe) + ": " + error);
    Bytes data;
    if (!read_file(sample, data)) return report("cannot read " + sample.u8string());
    if (data.size() >= 4 && std::equal(data.begin(), data.begin() + 4, "GVAS")) return true;  // plaintext
    if (!key::verify_key(key, data)) return report("key does not decrypt " + sample.u8string() + " to GVAS");
    return true;
}

bool require(const Args& a, std::initializer_list<const char*> names) {
    for (const char* n : names)
        if (!option(a, n)) return report(std::string(n) + " is required");
    return true;
}

int cmd_decrypt_encrypt(const Args& a, bool encrypt) {
    if (a.positional.empty() || !require(a, {"--out"})) return fail("usage: decrypt|encrypt --out DIR FILE...");
    Key key;
    if (!load_key(a, a.positional[0], key)) return 1;
    fs::path out_dir = *option(a, "--out");
    for (const std::wstring& file : a.positional) {
        fs::path src = file;
        Bytes data, result;
        if (!read_file(src, data)) return fail("cannot read " + src.u8string());
        fs::path dst;
        if (encrypt) {
            result = ops::encrypt(data, key);
            std::wstring name = src.filename().wstring();
            bool dec = name.size() > 4 && name.compare(name.size() - 4, 4, L".dec") == 0;
            dst = out_dir / (dec ? name.substr(0, name.size() - 4) : name + L".enc");
        } else {
            std::string error;
            if (!ops::decrypt(data, key, result, &error)) return fail(src.u8string() + ": " + error);
            dst = out_dir / (src.filename().wstring() + L".dec");
        }
        if (!write_file(dst, result)) return fail("cannot write " + dst.u8string());
        std::printf("%s: %zu bytes -> %s\n", src.u8string().c_str(), result.size(), dst.u8string().c_str());
    }
    return 0;
}

int cmd_header(const Args& a) {
    if (a.positional.empty()) return fail("usage: header FILE...");
    Key key;
    if (!load_key(a, a.positional[0], key)) return 1;
    for (const std::wstring& file : a.positional) {
        fs::path src = file;
        Bytes data, plain;
        if (!read_file(src, data)) return fail("cannot read " + src.u8string());
        std::string error;
        if (data.size() >= 4 && std::equal(data.begin(), data.begin() + 4, "GVAS")) {
            plain = data;
        } else if (!ops::decrypt(data, key, plain, &error)) {
            return fail(src.u8string() + ": " + error);
        }
        gvas::Layout layout;
        if (!gvas::parse_layout(plain, layout, &error)) return fail(src.u8string() + ": " + error);
        const gvas::Header& h = layout.header;
        uint32_t version = 0;
        bool has_version = gvas::read_leading_version(plain, version);
        std::printf("== %s  (%zu bytes)\n", src.u8string().c_str(), plain.size());
        std::printf("  build: %s\n", build::name(build::from_save_header(h)));
        std::printf("  SaveGameFileVersion: %d\n  PackageFileUE4Version: %d\n", h.save_game_version,
                    h.package_version);
        std::printf("  EngineVersion: %u.%u.%u changelist %u branch %s\n", h.engine.major, h.engine.minor,
                    h.engine.patch, h.engine.changelist, h.engine.branch.c_str());
        std::printf("  CustomVersionFormat: %d\n  CustomVersions: %zu entries\n", h.custom_version_format,
                    h.custom_versions.size());
        std::printf("  SaveGameClassName: %s\n  ClassNameOffset: %zu\n  BodyOffset: %zu\n  BodyEnd: %zu\n",
                    h.save_game_class.c_str(), h.class_name_offset, h.body_offset, layout.body_end);
        if (has_version) std::printf("  %s.Version: %u\n", layout.top.properties[0].name.c_str(), version);
        std::printf("  Top-level properties: %zu\n", layout.top.properties.size());
    }
    return 0;
}

int cmd_transfer(const Args& a, bool system) {
    if (a.positional.size() != 1 || !require(a, {"--ref", "--out"}))
        return fail("usage: transfer|system-convert --ref REF --out OUT SRC");
    Key key;
    if (!load_key(a, a.positional[0], key)) return 1;
    Bytes src, ref, out;
    if (!read_file(a.positional[0], src) || !read_file(*option(a, "--ref"), ref)) return fail("cannot read input");
    std::string error;
    bool ok;
    if (system) {
        Bytes ref_plain;
        gvas::Header ref_header;
        if (!ops::decrypt(ref, key, ref_plain, &error) || !gvas::parse_header(ref_plain, ref_header, &error))
            return fail("reference: " + error);
        Build target = build::from_save_header(ref_header);
        if (target == Build::Unknown) return fail("reference is from an unknown build");
        ok = ops::convert_system_save(src, ref, key, target == Build::HD, out, &error);
        std::printf("target build: %s\n", build::name(target));
    } else {
        ok = ops::convert_game_save(src, ref, key, out, &error);
    }
    if (!ok) return fail(error);
    fs::path dst = *option(a, "--out");
    if (!write_file(dst, out)) return fail("cannot write " + dst.u8string());
    std::printf("%s -> %s (%zu bytes)\n", narrow(a.positional[0]).c_str(), dst.u8string().c_str(), out.size());
    return 0;
}

int cmd_system_merge(const Args& a) {
    if (!require(a, {"--src", "--dst", "--slots", "--out"}))
        return fail("usage: system-merge --src S --dst D --slots 0,10|all --out O");
    Key key;
    if (!load_key(a, *option(a, "--src"), key)) return 1;
    Bytes src, dst, out;
    if (!read_file(*option(a, "--src"), src) || !read_file(*option(a, "--dst"), dst))
        return fail("cannot read input");
    std::string error;
    std::vector<int> slots;
    std::string spec = narrow(*option(a, "--slots"));
    if (spec == "all") {
        if (!ops::header_slots(src, key, slots, &error)) return fail(error);
    } else {
        size_t start = 0;
        while (start <= spec.size()) {
            size_t comma = spec.find(',', start);
            if (comma == std::string::npos) comma = spec.size();
            try {
                slots.push_back(std::stoi(spec.substr(start, comma - start)));
            } catch (...) {
                return fail("bad --slots value");
            }
            start = comma + 1;
        }
    }
    if (!ops::merge_headers(src, dst, key, slots, out, &error)) return fail(error);
    fs::path out_path = *option(a, "--out");
    if (!write_file(out_path, out)) return fail("cannot write " + out_path.u8string());
    std::printf("merged slots");
    for (int s : slots) std::printf(" %d", s);
    std::printf(" -> %s (%zu bytes)\n", out_path.u8string().c_str(), out.size());
    return 0;
}

int cmd_keyscan(const Args& a) {
    if (a.positional.empty()) return fail("usage: keyscan EXE...");
    bool all_ok = true;
    Key first_key{};
    for (size_t i = 0; i < a.positional.size(); ++i) {
        const std::wstring& exe = a.positional[i];
        std::string error;
        Key file_key{}, image_key{};
        bool file_ok = key::find_in_file(exe, file_key, &error);
        std::printf("%s\n  build (version resource): %s\n", narrow(exe).c_str(), build::name(build::from_exe_file(exe)));
        std::printf("  file scan: %s", file_ok ? "found" : error.c_str());
        if (file_ok) std::printf(", fingerprint %016llx", fingerprint(file_key));
        std::printf("\n");

        // Map the exe as an image without running it, to exercise the in-memory scan.
        HMODULE image = LoadLibraryExW(exe.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        bool image_ok = false;
        if (image) {
            image_ok = key::find_in_module(image, image_key, &error);
            std::printf("  mapped-image scan: %s", image_ok ? "found" : error.c_str());
            if (image_ok) std::printf(", fingerprint %016llx", fingerprint(image_key));
            std::printf("\n  build (mapped image): %s\n", build::name(build::from_module(image)));
            FreeLibrary(image);
        } else {
            std::printf("  mapped-image scan: cannot map image\n");
        }
        // Both scans must agree, and every exe must hold the same key as the first.
        if (i == 0) first_key = file_key;
        bool same = file_ok && image_ok && file_key == image_key && file_key == first_key;
        std::printf("  consistent: %s\n", same ? "yes" : "no");
        all_ok = all_ok && same;
    }
    std::printf("keyscan: %s\n", all_ok ? "OK" : "FAILED");
    return all_ok ? 0 : 1;
}

// Times AES-256 ECB encrypt and decrypt of a 16 MB random buffer (random key) with the
// software and the AES-NI path; best of 3 runs each. Also checks both give the same bytes.
int cmd_bench() {
    using Clock = std::chrono::steady_clock;
    using EcbFn = bool (*)(const Key&, uint8_t*, size_t);
    constexpr size_t kSize = 16u << 20;
    constexpr double kMb = static_cast<double>(kSize) / (1u << 20);
    constexpr int kRuns = 3;

    std::mt19937_64 rng(0x6b616b61726f74ULL);
    Key key;
    for (uint8_t& b : key) b = static_cast<uint8_t>(rng());
    Bytes plain(kSize);
    for (uint8_t& b : plain) b = static_cast<uint8_t>(rng());

    struct Path {
        const char* name;
        EcbFn encrypt;
        EcbFn decrypt;
    };
    const Path all[] = {{"software", aes::paths::encrypt_ecb_software, aes::paths::decrypt_ecb_software},
                        {"AES-NI", aes::paths::encrypt_ecb_hardware, aes::paths::decrypt_ecb_hardware}};
    const bool hardware = aes::paths::hardware_supported();
    std::printf("AES-NI supported: %s; buffer %.0f MB (1 MB = 2^20 bytes), best of %d runs\n",
                hardware ? "yes" : "no", kMb, kRuns);

    Bytes ciphertexts[2];
    bool ok = true;
    for (int p = 0; p < (hardware ? 2 : 1); ++p) {
        double best_enc = 1e30, best_dec = 1e30;
        for (int run = 0; run < kRuns; ++run) {
            Bytes buffer = plain;
            const Clock::time_point t0 = Clock::now();
            all[p].encrypt(key, buffer.data(), buffer.size());
            const Clock::time_point t1 = Clock::now();
            if (run == 0) ciphertexts[p] = buffer;
            all[p].decrypt(key, buffer.data(), buffer.size());
            const Clock::time_point t2 = Clock::now();
            best_enc = std::min(best_enc, std::chrono::duration<double>(t1 - t0).count());
            best_dec = std::min(best_dec, std::chrono::duration<double>(t2 - t1).count());
            ok = ok && buffer == plain;
        }
        std::printf("%-8s  encrypt %8.1f MB/s  decrypt %8.1f MB/s  encrypt+decrypt %8.1f MB/s\n", all[p].name,
                    kMb / best_enc, kMb / best_dec, 2 * kMb / (best_enc + best_dec));
    }
    if (hardware) ok = ok && ciphertexts[0] == ciphertexts[1];
    std::printf("round trip%s: %s\n", hardware ? " and software == AES-NI ciphertext" : "", ok ? "OK" : "MISMATCH");
    return ok ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    Args a = parse_args(argc, argv);
    if (a.command == "decrypt") return cmd_decrypt_encrypt(a, false);
    if (a.command == "encrypt") return cmd_decrypt_encrypt(a, true);
    if (a.command == "header") return cmd_header(a);
    if (a.command == "transfer") return cmd_transfer(a, false);
    if (a.command == "system-convert") return cmd_transfer(a, true);
    if (a.command == "system-merge") return cmd_system_merge(a);
    if (a.command == "keyscan") return cmd_keyscan(a);
    if (a.command == "bench") return cmd_bench();
    std::fprintf(stderr,
                 "usage: kakarot_cli --exe EXE decrypt|encrypt --out DIR FILE...\n"
                 "       kakarot_cli --exe EXE header FILE...\n"
                 "       kakarot_cli --exe EXE transfer --ref REF --out OUT SRC\n"
                 "       kakarot_cli --exe EXE system-convert --ref REF --out OUT SRC\n"
                 "       kakarot_cli --exe EXE system-merge --src S --dst D --slots 0,10|all --out OUT\n"
                 "       kakarot_cli keyscan EXE...\n"
                 "       kakarot_cli bench\n");
    return 2;
}
