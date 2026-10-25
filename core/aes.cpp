// Straightforward AES-256 implementation following FIPS-197.
//
// The S-box and the GF(2^8) multiplication tables are computed at first use
// from their mathematical definitions (multiplicative inverse plus affine
// transform), so no constant tables are copied in. The state is kept as 16
// bytes in FIPS-197 column-major order: state[row + 4 * column].
//
// When the CPU has AES-NI (CPUID leaf 1, ECX bit 25, checked once), encrypt_ecb and
// decrypt_ecb use the hardware path instead; it gives the same bytes (see aes_paths.h).
#include "aes.h"

#include "aes_paths.h"

#if defined(_M_X64)
#include <immintrin.h>
#include <intrin.h>
#include <wmmintrin.h>
#define KAKAROT_AES_NI 1
#else
#define KAKAROT_AES_NI 0
#endif

namespace kakarot {
namespace aes {
namespace {

constexpr int kRounds = 14;           // Nr for a 256-bit key
constexpr int kKeyWords = 8;          // Nk for a 256-bit key
constexpr int kScheduleWords = 4 * (kRounds + 1);

// Multiplication by x (i.e. 2) in GF(2^8) modulo x^8 + x^4 + x^3 + x + 1.
uint8_t xtime(uint8_t a) {
    return static_cast<uint8_t>((a << 1) ^ ((a & 0x80) ? 0x1b : 0x00));
}

// General multiplication in GF(2^8) (shift-and-add).
uint8_t gf_mul(uint8_t a, uint8_t b) {
    uint8_t result = 0;
    while (b) {
        if (b & 1) result ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return result;
}

uint8_t rotl8(uint8_t v, int n) {
    return static_cast<uint8_t>((v << n) | (v >> (8 - n)));
}

struct Tables {
    uint8_t sbox[256];
    uint8_t inv_sbox[256];
    uint8_t mul2[256], mul3[256], mul9[256], mul11[256], mul13[256], mul14[256];

    Tables() {
        for (int x = 0; x < 256; ++x) {
            // Multiplicative inverse (0 maps to 0), found by search.
            uint8_t inv = 0;
            for (int y = 1; y < 256 && x != 0; ++y) {
                if (gf_mul(static_cast<uint8_t>(x), static_cast<uint8_t>(y)) == 1) {
                    inv = static_cast<uint8_t>(y);
                    break;
                }
            }
            // Affine transform (FIPS-197 eq. 5.1).
            uint8_t s = static_cast<uint8_t>(inv ^ rotl8(inv, 1) ^ rotl8(inv, 2) ^
                                             rotl8(inv, 3) ^ rotl8(inv, 4) ^ 0x63);
            sbox[x] = s;
            inv_sbox[s] = static_cast<uint8_t>(x);
        }
        for (int x = 0; x < 256; ++x) {
            uint8_t a = static_cast<uint8_t>(x);
            mul2[x] = gf_mul(a, 2);
            mul3[x] = gf_mul(a, 3);
            mul9[x] = gf_mul(a, 9);
            mul11[x] = gf_mul(a, 11);
            mul13[x] = gf_mul(a, 13);
            mul14[x] = gf_mul(a, 14);
        }
    }
};

const Tables& tables() {
    static const Tables t;
    return t;
}

// Expanded key: (Nr + 1) round keys of 16 bytes each, laid out like the state.
struct Schedule {
    uint8_t bytes[4 * kScheduleWords];
};

Schedule expand_key(const Key& key) {
    const Tables& t = tables();
    Schedule ks{};
    for (int i = 0; i < 4 * kKeyWords; ++i) ks.bytes[i] = key[i];
    uint8_t rcon = 0x01;
    for (int i = kKeyWords; i < kScheduleWords; ++i) {
        uint8_t temp[4];
        for (int j = 0; j < 4; ++j) temp[j] = ks.bytes[4 * (i - 1) + j];
        if (i % kKeyWords == 0) {
            // RotWord, SubWord, Rcon
            uint8_t first = temp[0];
            temp[0] = static_cast<uint8_t>(t.sbox[temp[1]] ^ rcon);
            temp[1] = t.sbox[temp[2]];
            temp[2] = t.sbox[temp[3]];
            temp[3] = t.sbox[first];
            rcon = xtime(rcon);
        } else if (i % kKeyWords == 4) {
            for (int j = 0; j < 4; ++j) temp[j] = t.sbox[temp[j]];
        }
        for (int j = 0; j < 4; ++j)
            ks.bytes[4 * i + j] = static_cast<uint8_t>(ks.bytes[4 * (i - kKeyWords) + j] ^ temp[j]);
    }
    return ks;
}

void add_round_key(uint8_t* s, const Schedule& ks, int round) {
    for (int i = 0; i < 16; ++i) s[i] ^= ks.bytes[16 * round + i];
}

void sub_bytes(uint8_t* s, const uint8_t* box) {
    for (int i = 0; i < 16; ++i) s[i] = box[s[i]];
}

// Row r is rotated left by r columns.
void shift_rows(uint8_t* s) {
    uint8_t out[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r + 4 * c] = s[r + 4 * ((c + r) % 4)];
    for (int i = 0; i < 16; ++i) s[i] = out[i];
}

void inv_shift_rows(uint8_t* s) {
    uint8_t out[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r + 4 * ((c + r) % 4)] = s[r + 4 * c];
    for (int i = 0; i < 16; ++i) s[i] = out[i];
}

void mix_columns(uint8_t* s, const Tables& t) {
    for (int c = 0; c < 4; ++c) {
        uint8_t* a = s + 4 * c;
        uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
        a[0] = static_cast<uint8_t>(t.mul2[a0] ^ t.mul3[a1] ^ a2 ^ a3);
        a[1] = static_cast<uint8_t>(a0 ^ t.mul2[a1] ^ t.mul3[a2] ^ a3);
        a[2] = static_cast<uint8_t>(a0 ^ a1 ^ t.mul2[a2] ^ t.mul3[a3]);
        a[3] = static_cast<uint8_t>(t.mul3[a0] ^ a1 ^ a2 ^ t.mul2[a3]);
    }
}

void inv_mix_columns(uint8_t* s, const Tables& t) {
    for (int c = 0; c < 4; ++c) {
        uint8_t* a = s + 4 * c;
        uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
        a[0] = static_cast<uint8_t>(t.mul14[a0] ^ t.mul11[a1] ^ t.mul13[a2] ^ t.mul9[a3]);
        a[1] = static_cast<uint8_t>(t.mul9[a0] ^ t.mul14[a1] ^ t.mul11[a2] ^ t.mul13[a3]);
        a[2] = static_cast<uint8_t>(t.mul13[a0] ^ t.mul9[a1] ^ t.mul14[a2] ^ t.mul11[a3]);
        a[3] = static_cast<uint8_t>(t.mul11[a0] ^ t.mul13[a1] ^ t.mul9[a2] ^ t.mul14[a3]);
    }
}

void encrypt_block(uint8_t* s, const Schedule& ks, const Tables& t) {
    add_round_key(s, ks, 0);
    for (int round = 1; round < kRounds; ++round) {
        sub_bytes(s, t.sbox);
        shift_rows(s);
        mix_columns(s, t);
        add_round_key(s, ks, round);
    }
    sub_bytes(s, t.sbox);
    shift_rows(s);
    add_round_key(s, ks, kRounds);
}

void decrypt_block(uint8_t* s, const Schedule& ks, const Tables& t) {
    add_round_key(s, ks, kRounds);
    for (int round = kRounds - 1; round > 0; --round) {
        inv_shift_rows(s);
        sub_bytes(s, t.inv_sbox);
        add_round_key(s, ks, round);
        inv_mix_columns(s, t);
    }
    inv_shift_rows(s);
    sub_bytes(s, t.inv_sbox);
    add_round_key(s, ks, 0);
}

#if KAKAROT_AES_NI
// ------------------------------------------------------------------- AES-NI path
//
// A block and a round key are their 16 bytes in memory order, exactly as loaded, which is the
// FIPS-197 state layout above.

struct HwSchedule {
    __m128i k[kRounds + 1];
};

// a ^ (a << 32) ^ (a << 64) ^ (a << 96): each word XORed with all the words before it.
__m128i prefix_xor(__m128i a) {
    __m128i t = _mm_slli_si128(a, 4);
    a = _mm_xor_si128(a, t);
    t = _mm_slli_si128(t, 4);
    a = _mm_xor_si128(a, t);
    t = _mm_slli_si128(t, 4);
    return _mm_xor_si128(a, t);
}

// Even round key (i % Nk == 0): RotWord, SubWord and Rcon of the previous key's last word.
template <int Rcon>
__m128i next_even(__m128i prev_even, __m128i prev_odd) {
    const __m128i assist = _mm_shuffle_epi32(_mm_aeskeygenassist_si128(prev_odd, Rcon), 0xff);
    return _mm_xor_si128(prefix_xor(prev_even), assist);
}

// Odd round key (i % Nk == 4): SubWord only, of the new even key's last word.
__m128i next_odd(__m128i prev_odd, __m128i even) {
    const __m128i assist = _mm_shuffle_epi32(_mm_aeskeygenassist_si128(even, 0), 0xaa);
    return _mm_xor_si128(prefix_xor(prev_odd), assist);
}

// The 15 round keys of AES-256.
HwSchedule hw_expand_key(const Key& key) {
    HwSchedule ks;
    __m128i* k = ks.k;
    k[0] = _mm_loadu_si128(reinterpret_cast<const __m128i*>(key.data()));
    k[1] = _mm_loadu_si128(reinterpret_cast<const __m128i*>(key.data() + 16));
    k[2] = next_even<0x01>(k[0], k[1]);
    k[3] = next_odd(k[1], k[2]);
    k[4] = next_even<0x02>(k[2], k[3]);
    k[5] = next_odd(k[3], k[4]);
    k[6] = next_even<0x04>(k[4], k[5]);
    k[7] = next_odd(k[5], k[6]);
    k[8] = next_even<0x08>(k[6], k[7]);
    k[9] = next_odd(k[7], k[8]);
    k[10] = next_even<0x10>(k[8], k[9]);
    k[11] = next_odd(k[9], k[10]);
    k[12] = next_even<0x20>(k[10], k[11]);
    k[13] = next_odd(k[11], k[12]);
    k[14] = next_even<0x40>(k[12], k[13]);
    return ks;
}

// Equivalent inverse cipher keys: reversed, with InvMixColumns applied to the inner ones.
HwSchedule hw_decrypt_schedule(const HwSchedule& enc) {
    HwSchedule dk;
    dk.k[0] = enc.k[kRounds];
    for (int r = 1; r < kRounds; ++r) dk.k[r] = _mm_aesimc_si128(enc.k[kRounds - r]);
    dk.k[kRounds] = enc.k[0];
    return dk;
}

// N independent blocks per call, interleaved round by round to keep the AES unit busy.
template <int N>
void hw_encrypt_blocks(const HwSchedule& ks, uint8_t* p) {
    __m128i b[N];
    for (int i = 0; i < N; ++i)
        b[i] = _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16 * i)), ks.k[0]);
    for (int r = 1; r < kRounds; ++r)
        for (int i = 0; i < N; ++i) b[i] = _mm_aesenc_si128(b[i], ks.k[r]);
    for (int i = 0; i < N; ++i)
        _mm_storeu_si128(reinterpret_cast<__m128i*>(p + 16 * i), _mm_aesenclast_si128(b[i], ks.k[kRounds]));
}

template <int N>
void hw_decrypt_blocks(const HwSchedule& dk, uint8_t* p) {
    __m128i b[N];
    for (int i = 0; i < N; ++i)
        b[i] = _mm_xor_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16 * i)), dk.k[0]);
    for (int r = 1; r < kRounds; ++r)
        for (int i = 0; i < N; ++i) b[i] = _mm_aesdec_si128(b[i], dk.k[r]);
    for (int i = 0; i < N; ++i)
        _mm_storeu_si128(reinterpret_cast<__m128i*>(p + 16 * i), _mm_aesdeclast_si128(b[i], dk.k[kRounds]));
}

constexpr int kHwBatch = 8;
constexpr size_t kHwBatchBytes = kHwBatch * kBlockSize;

void hw_encrypt(const Key& key, uint8_t* data, size_t size) {
    const HwSchedule ks = hw_expand_key(key);
    size_t off = 0;
    for (; off + kHwBatchBytes <= size; off += kHwBatchBytes) hw_encrypt_blocks<kHwBatch>(ks, data + off);
    for (; off < size; off += kBlockSize) hw_encrypt_blocks<1>(ks, data + off);
}

void hw_decrypt(const Key& key, uint8_t* data, size_t size) {
    const HwSchedule dk = hw_decrypt_schedule(hw_expand_key(key));
    size_t off = 0;
    for (; off + kHwBatchBytes <= size; off += kHwBatchBytes) hw_decrypt_blocks<kHwBatch>(dk, data + off);
    for (; off < size; off += kBlockSize) hw_decrypt_blocks<1>(dk, data + off);
}

bool detect_hardware() {
    int regs[4] = {};
    __cpuid(regs, 0);
    if (regs[0] < 1) return false;
    __cpuid(regs, 1);
    return (regs[2] & (1 << 25)) != 0;
}
#else
bool detect_hardware() { return false; }
void hw_encrypt(const Key&, uint8_t*, size_t) {}
void hw_decrypt(const Key&, uint8_t*, size_t) {}
#endif

}  // namespace

namespace paths {

bool hardware_supported() {
    static const bool supported = detect_hardware();
    return supported;
}

bool encrypt_ecb_software(const Key& key, uint8_t* data, size_t size) {
    if (size % kBlockSize != 0) return false;
    const Tables& t = tables();
    const Schedule ks = expand_key(key);
    for (size_t off = 0; off < size; off += kBlockSize) encrypt_block(data + off, ks, t);
    return true;
}

bool decrypt_ecb_software(const Key& key, uint8_t* data, size_t size) {
    if (size % kBlockSize != 0) return false;
    const Tables& t = tables();
    const Schedule ks = expand_key(key);
    for (size_t off = 0; off < size; off += kBlockSize) decrypt_block(data + off, ks, t);
    return true;
}

bool encrypt_ecb_hardware(const Key& key, uint8_t* data, size_t size) {
    if (size % kBlockSize != 0 || !hardware_supported()) return false;
    hw_encrypt(key, data, size);
    return true;
}

bool decrypt_ecb_hardware(const Key& key, uint8_t* data, size_t size) {
    if (size % kBlockSize != 0 || !hardware_supported()) return false;
    hw_decrypt(key, data, size);
    return true;
}

}  // namespace paths

bool encrypt_ecb(const Key& key, uint8_t* data, size_t size) {
    return paths::hardware_supported() ? paths::encrypt_ecb_hardware(key, data, size)
                                       : paths::encrypt_ecb_software(key, data, size);
}

bool decrypt_ecb(const Key& key, uint8_t* data, size_t size) {
    return paths::hardware_supported() ? paths::decrypt_ecb_hardware(key, data, size)
                                       : paths::decrypt_ecb_software(key, data, size);
}

void pad16(Bytes& data) {
    while (data.size() % kBlockSize != 0) data.push_back(0);
}

}  // namespace aes
}  // namespace kakarot
