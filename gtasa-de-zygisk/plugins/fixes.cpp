#include <android/log.h>

#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

struct GtasaHost {
    uint32_t size;
    uintptr_t base;
    int (*hook)(void *target, void *detour, void **original);
};

namespace {

constexpr const char *kTag = "GTASADE";
// Removal distance is at least 170 * scale. A stored cruise of 0 falls back to
// that. Writing 170 * scale * 1.5 at +1922 is what keeps a distant ambulance,
// the same value a fire truck gets.
constexpr uintptr_t kAmbulanceCruise = 0x532012c;
constexpr uint32_t kAmbulanceCruiseFirst = 0xb9400328u;
constexpr uintptr_t kAmbulanceCruiseNext = 0x5320140;

const GtasaHost *g_host = nullptr;
bool g_ambulance_patched = false;
uint32_t g_ambulance_saved = 0;
uint8_t *g_ambulance_stub = nullptr;

bool write_word(uintptr_t address, uint32_t word) {
    const uintptr_t page_size = static_cast<uintptr_t>(getpagesize());
    const uintptr_t page = address & ~(page_size - 1);
    if (mprotect(reinterpret_cast<void *>(page), page_size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        return false;
    }
    std::memcpy(reinterpret_cast<void *>(address), &word, sizeof(word));
    __builtin___clear_cache(reinterpret_cast<char *>(address), reinterpret_cast<char *>(address) + sizeof(word));
    mprotect(reinterpret_cast<void *>(page), page_size, PROT_READ | PROT_EXEC);
    return true;
}

uint8_t *map_near(uintptr_t site) {
    const size_t page_size = static_cast<size_t>(getpagesize());
    constexpr uintptr_t kReach = 0x7f00000;
    const uintptr_t lo = site > kReach ? site - kReach : page_size;
    const uintptr_t hi = UINTPTR_MAX - site < kReach ? UINTPTR_MAX : site + kReach;
    FILE *maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) {
        return nullptr;
    }
    uintptr_t prev_end = 0;
    uint8_t *mapped = nullptr;
    char line[384];
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long start = 0;
        unsigned long end = 0;
        if (std::sscanf(line, "%lx-%lx", &start, &end) != 2 || end <= start) {
            continue;
        }
        const uintptr_t gap_lo = prev_end;
        const uintptr_t gap_hi = static_cast<uintptr_t>(start);
        prev_end = static_cast<uintptr_t>(end);
        if (gap_hi < gap_lo + page_size) {
            continue;
        }
        uintptr_t from = gap_lo < lo ? lo : gap_lo;
        const uintptr_t to = gap_hi > hi ? hi : gap_hi;
        from = (from + page_size - 1) & ~(static_cast<uintptr_t>(page_size) - 1);
        if (from < page_size || from + page_size > to) {
            continue;
        }
        void *mem = mmap(reinterpret_cast<void *>(from), page_size, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (mem == MAP_FAILED) {
            continue;
        }
        mapped = static_cast<uint8_t *>(mem);
        break;
    }
    std::fclose(maps);
    return mapped;
}

bool write_branch(uintptr_t address, void *dest) {
    const auto target = reinterpret_cast<uintptr_t>(dest);
    const int64_t delta = static_cast<int64_t>(target) - static_cast<int64_t>(address);
    if ((delta & 3) != 0) {
        return false;
    }
    const int64_t imm = delta / 4;
    if (imm < -0x2000000 || imm > 0x1ffffff) {
        return false;
    }
    const uint32_t word = 0x14000000u | (static_cast<uint32_t>(imm) & 0x03ffffffu);
    return write_word(address, word);
}

void build_ambulance_stub(uint8_t *code) {
    const uint32_t words[] = {
        0xf94002c8u,  // ldr x8, [x22]
        0x52a86549u,  // mov w9, #170.0
        0xf940b908u,  // ldr x8, [x8, #368]
        0x1e270120u,  // fmov s0, w9
        0xbd43c501u,  // ldr s1, [x8, #964]
        0x1e200820u,  // fmul s0, s1, s0
        0x1e2f1001u,  // fmov s1, #1.5
        0x1e210800u,  // fmul s0, s0, s1
        0x1e380008u,  // fcvtzs w8, s0
        0x790f0408u,  // strh w8, [x0, #1922]
        0xb9400328u,  // ldr w8, [x25]
        0xb9000308u,  // str w8, [x24]
        0x58000050u,  // ldr x16, next
        0xd61f0200u,  // br x16
    };
    std::memcpy(code, words, sizeof(words));
    const uintptr_t next = g_host->base + kAmbulanceCruiseNext;
    std::memcpy(code + 0x38, &next, sizeof(next));
    __builtin___clear_cache(reinterpret_cast<char *>(code), reinterpret_cast<char *>(code) + 0x40);
}

bool set_ambulance(bool enable) {
    if (g_host == nullptr) {
        return false;
    }
    const uintptr_t address = g_host->base + kAmbulanceCruise;
    if (enable) {
        if (g_ambulance_patched) {
            return true;
        }
        if (*reinterpret_cast<uint32_t *>(address) != kAmbulanceCruiseFirst) {
            return false;
        }
        g_ambulance_stub = map_near(address);
        if (g_ambulance_stub == nullptr) {
            return false;
        }
        build_ambulance_stub(g_ambulance_stub);
        g_ambulance_saved = kAmbulanceCruiseFirst;
        if (!write_branch(address, g_ambulance_stub)) {
            return false;
        }
        g_ambulance_patched = true;
        return true;
    }
    if (!g_ambulance_patched) {
        return true;
    }
    if (!write_word(address, g_ambulance_saved)) {
        return false;
    }
    g_ambulance_patched = false;
    return true;
}

}  // namespace

extern "C" __attribute__((visibility("default"))) const int64_t gtasa_plugin_game_abi[] = {1, 54538169};

extern "C" __attribute__((visibility("default"))) void gtasa_plugin_enter(const GtasaHost *host) {
    g_host = host;
    if (!set_ambulance(true)) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "fixes ambulance cruise patch failed");
    } else {
        __android_log_print(ANDROID_LOG_INFO, kTag, "fixes keep distant ambulance");
    }
}

extern "C" __attribute__((visibility("default"))) void gtasa_plugin_pause() {
    set_ambulance(false);
}

extern "C" __attribute__((visibility("default"))) void gtasa_plugin_resume() {
    set_ambulance(true);
}
