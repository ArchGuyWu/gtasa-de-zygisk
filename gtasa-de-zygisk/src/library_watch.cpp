#include "library_watch.h"
#include "plugins.h"

#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <pthread.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>

namespace {

constexpr const char *kTag = "GTASADE";
constexpr const char *kLibrary = "libUE4.so";
constexpr int kPollMs = 500;
constexpr int kHeartbeatMs = 15000;
constexpr int kTimeoutMs = 30000;
constexpr size_t kNameCap = 512;

using IterateFn = int (*)(int (*)(struct dl_phdr_info *, size_t, void *), void *);

struct WatchHit {
    bool matched;
    char label[kNameCap];
    uintptr_t addr;
    uintptr_t min_pvaddr;
    GtasaSymbols symbols;
};

void log_info(const char *message) {
    __android_log_print(ANDROID_LOG_INFO, kTag, "%s", message);
}

const char *library_file_name(const char *path) {
    if (path == nullptr || path[0] == '\0') {
        return nullptr;
    }
    const char *start = path;
    const char *bang = std::strrchr(path, '!');
    if (bang != nullptr && bang[1] == '/') {
        start = bang + 2;
    }
    const char *slash = std::strrchr(start, '/');
    return slash != nullptr ? slash + 1 : start;
}

bool name_is_target(const char *path) {
    const char *file_name = library_file_name(path);
    return file_name != nullptr && std::strcmp(file_name, kLibrary) == 0;
}

void copy_text(char *dest, size_t cap, const char *src) {
    if (cap == 0) {
        return;
    }
    if (src == nullptr) {
        dest[0] = '\0';
        return;
    }
    std::strncpy(dest, src, cap - 1);
    dest[cap - 1] = '\0';
}

uintptr_t min_load_vaddr(const dl_phdr_info *info) {
    bool found = false;
    uintptr_t min_vaddr = 0;
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) &phdr = info->dlpi_phdr[i];
        if (phdr.p_type != PT_LOAD) {
            continue;
        }
        if (!found || phdr.p_vaddr < min_vaddr) {
            min_vaddr = static_cast<uintptr_t>(phdr.p_vaddr);
            found = true;
        }
    }
    return min_vaddr;
}

bool range_in_one_load(const dl_phdr_info *info, uintptr_t address, size_t length) {
    if (length == 0) {
        return false;
    }
    const uintptr_t last = address + length - 1;
    if (last < address) {
        return false;
    }
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) &phdr = info->dlpi_phdr[i];
        if (phdr.p_type != PT_LOAD || phdr.p_memsz == 0) {
            continue;
        }
        const uintptr_t start = static_cast<uintptr_t>(info->dlpi_addr + phdr.p_vaddr);
        const uintptr_t end = start + static_cast<uintptr_t>(phdr.p_memsz);
        if (address >= start && last < end) {
            return true;
        }
    }
    return false;
}

bool address_in_load(const dl_phdr_info *info, uintptr_t address) {
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) &phdr = info->dlpi_phdr[i];
        if (phdr.p_type != PT_LOAD || phdr.p_memsz == 0) {
            continue;
        }
        const uintptr_t start = static_cast<uintptr_t>(info->dlpi_addr + phdr.p_vaddr);
        const uintptr_t end = start + static_cast<uintptr_t>(phdr.p_memsz);
        if (address >= start && address < end) {
            return true;
        }
    }
    return false;
}

bool copy_mapped_string(const dl_phdr_info *info, uintptr_t address, char *dest, size_t cap) {
    if (cap < 2 || !address_in_load(info, address)) {
        return false;
    }
    size_t i = 0;
    while (i + 1 < cap && address_in_load(info, address + i)) {
        const char ch = *reinterpret_cast<const char *>(address + i);
        if (ch == '\0') {
            dest[i] = '\0';
            return i > 0;
        }
        const auto byte = static_cast<unsigned char>(ch);
        if (byte < 0x20 || byte > 0x7e) {
            return false;
        }
        dest[i] = ch;
        ++i;
    }
    return false;
}

bool copy_soname(const dl_phdr_info *info, char *dest, size_t cap) {
    const ElfW(Dyn) *dyn = nullptr;
    uintptr_t dyn_addr = 0;
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
            dyn_addr = static_cast<uintptr_t>(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
            dyn = reinterpret_cast<const ElfW(Dyn) *>(dyn_addr);
            break;
        }
    }
    if (dyn == nullptr || !address_in_load(info, dyn_addr)) {
        return false;
    }

    uintptr_t strtab_addr = 0;
    uintptr_t soname_off = 0;
    bool have_strtab = false;
    bool have_soname = false;
    for (int n = 0; n < 256; ++n) {
        const uintptr_t entry = dyn_addr + static_cast<uintptr_t>(n) * sizeof(ElfW(Dyn));
        if (!address_in_load(info, entry)) {
            return false;
        }
        const ElfW(Dyn) item = *reinterpret_cast<const ElfW(Dyn) *>(entry);
        if (item.d_tag == DT_NULL) {
            break;
        }
        if (item.d_tag == DT_STRTAB) {
            strtab_addr = static_cast<uintptr_t>(item.d_un.d_ptr);
            have_strtab = true;
        } else if (item.d_tag == DT_SONAME) {
            soname_off = static_cast<uintptr_t>(item.d_un.d_val);
            have_soname = true;
        }
    }
    if (!have_strtab || !have_soname) {
        return false;
    }
    if (strtab_addr < info->dlpi_addr) {
        strtab_addr += info->dlpi_addr;
    }
    return copy_mapped_string(info, strtab_addr + soname_off, dest, cap);
}

uint32_t gnu_hash(const char *name) {
    uint32_t hash = 5381;
    for (const auto *cursor = reinterpret_cast<const unsigned char *>(name); *cursor != 0; ++cursor) {
        hash = hash * 33u + static_cast<uint32_t>(*cursor);
    }
    return hash;
}

bool read_bytes(const dl_phdr_info *info, uintptr_t address, void *dest, size_t length) {
    if (!range_in_one_load(info, address, length)) {
        return false;
    }
    std::memcpy(dest, reinterpret_cast<const void *>(address), length);
    return true;
}

bool read_u32(const dl_phdr_info *info, uintptr_t address, uint32_t *out) {
    return read_bytes(info, address, out, sizeof(*out));
}

bool read_u64(const dl_phdr_info *info, uintptr_t address, uint64_t *out) {
    return read_bytes(info, address, out, sizeof(*out));
}

uintptr_t dyn_pointer(const dl_phdr_info *info, uintptr_t value) {
    if (value < static_cast<uintptr_t>(info->dlpi_addr)) {
        return static_cast<uintptr_t>(info->dlpi_addr) + value;
    }
    return value;
}

bool executable_address(const dl_phdr_info *info, uintptr_t address) {
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr) &phdr = info->dlpi_phdr[i];
        if (phdr.p_type != PT_LOAD || (phdr.p_flags & PF_X) == 0 || phdr.p_memsz == 0) {
            continue;
        }
        const uintptr_t start = static_cast<uintptr_t>(info->dlpi_addr + phdr.p_vaddr);
        const uintptr_t end = start + static_cast<uintptr_t>(phdr.p_memsz);
        if (address >= start && address < end) {
            return true;
        }
    }
    return false;
}

bool mapped_streq(const dl_phdr_info *info, uintptr_t address, const char *name) {
    for (size_t i = 0;; ++i) {
        if (!address_in_load(info, address + i)) {
            return false;
        }
        const char ch = *reinterpret_cast<const char *>(address + i);
        if (ch != name[i]) {
            return false;
        }
        if (ch == '\0') {
            return true;
        }
    }
}

struct DynTables {
    uintptr_t symtab;
    uintptr_t strtab;
    uintptr_t gnu_hash;
};

bool read_dyn_tables(const dl_phdr_info *info, DynTables *tables) {
    uintptr_t dyn_addr = 0;
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
            dyn_addr = static_cast<uintptr_t>(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
            break;
        }
    }
    if (dyn_addr == 0 || !address_in_load(info, dyn_addr)) {
        return false;
    }
    uintptr_t symtab = 0;
    uintptr_t strtab = 0;
    uintptr_t gnu_hash = 0;
    for (int n = 0; n < 4096; ++n) {
        const uintptr_t entry = dyn_addr + static_cast<uintptr_t>(n) * sizeof(ElfW(Dyn));
        ElfW(Dyn) item {};
        if (!read_bytes(info, entry, &item, sizeof(item))) {
            return false;
        }
        if (item.d_tag == DT_NULL) {
            break;
        }
        if (item.d_tag == DT_SYMTAB) {
            symtab = static_cast<uintptr_t>(item.d_un.d_ptr);
        } else if (item.d_tag == DT_STRTAB) {
            strtab = static_cast<uintptr_t>(item.d_un.d_ptr);
        } else if (item.d_tag == DT_GNU_HASH) {
            gnu_hash = static_cast<uintptr_t>(item.d_un.d_ptr);
        }
    }
    if (symtab == 0 || strtab == 0 || gnu_hash == 0) {
        return false;
    }
    tables->symtab = dyn_pointer(info, symtab);
    tables->strtab = dyn_pointer(info, strtab);
    tables->gnu_hash = dyn_pointer(info, gnu_hash);
    return address_in_load(info, tables->symtab) && address_in_load(info, tables->strtab) &&
           address_in_load(info, tables->gnu_hash);
}

void *lookup_symbol(const dl_phdr_info *info, const DynTables &tables, const char *name) {
    uint32_t nbuckets = 0;
    uint32_t symoffset = 0;
    uint32_t bloom_size = 0;
    uint32_t bloom_shift = 0;
    if (!read_u32(info, tables.gnu_hash, &nbuckets) || !read_u32(info, tables.gnu_hash + 4, &symoffset) ||
        !read_u32(info, tables.gnu_hash + 8, &bloom_size) ||
        !read_u32(info, tables.gnu_hash + 12, &bloom_shift)) {
        return nullptr;
    }
    if (nbuckets == 0 || bloom_size == 0 || bloom_size > (1u << 20) || nbuckets > (1u << 24) ||
        bloom_shift >= 32) {
        return nullptr;
    }
    const uint32_t hash = gnu_hash(name);
    constexpr uint32_t kBloomBits = 64;
    const uintptr_t bloom = tables.gnu_hash + 16;
    uint64_t word = 0;
    if (!read_u64(info, bloom + static_cast<uintptr_t>((hash / kBloomBits) % bloom_size) * 8, &word)) {
        return nullptr;
    }
    const uint32_t bit1 = hash % kBloomBits;
    const uint32_t bit2 = (hash >> bloom_shift) % kBloomBits;
    if (((word >> bit1) & 1ull) == 0 || ((word >> bit2) & 1ull) == 0) {
        return nullptr;
    }
    const uintptr_t buckets = bloom + static_cast<uintptr_t>(bloom_size) * 8;
    uint32_t index = 0;
    if (!read_u32(info, buckets + static_cast<uintptr_t>(hash % nbuckets) * 4, &index) || index == 0 ||
        index < symoffset) {
        return nullptr;
    }
    const uintptr_t chains = buckets + static_cast<uintptr_t>(nbuckets) * 4;
    for (uint32_t steps = 0; steps < nbuckets; ++steps) {
        uint32_t chain = 0;
        ElfW(Sym) sym {};
        if (!read_u32(info, chains + static_cast<uintptr_t>(index - symoffset) * 4, &chain) ||
            !read_bytes(info, tables.symtab + static_cast<uintptr_t>(index) * sizeof(sym), &sym, sizeof(sym))) {
            return nullptr;
        }
        const unsigned type = sym.st_info & 0xf;
        if (((chain ^ hash) >> 1) == 0 && sym.st_shndx != SHN_UNDEF && type == STT_FUNC &&
            mapped_streq(info, tables.strtab + static_cast<uintptr_t>(sym.st_name), name)) {
            const uintptr_t address =
                static_cast<uintptr_t>(info->dlpi_addr) + static_cast<uintptr_t>(sym.st_value);
            if (!executable_address(info, address)) {
                return nullptr;
            }
            return reinterpret_cast<void *>(address);
        }
        if ((chain & 1u) != 0 || index == 0xffffffffu) {
            break;
        }
        ++index;
    }
    return nullptr;
}

void resolve_symbols(const dl_phdr_info *info, GtasaSymbols *symbols) {
    std::memset(symbols, 0, sizeof(*symbols));
    DynTables tables {};
    if (!read_dyn_tables(info, &tables)) {
        return;
    }
    struct Item {
        const char *name;
        void **slot;
    };
    Item items[] = {
        {"_Z13IsPlayingGamev", &symbols->is_playing},
        {"_Z13FindPlayerPedi", &symbols->find_player_ped},
        {"_ZN10CEntryExit17TransitionStartedEP4CPed", &symbols->transition_started},
        {"_ZN10CEntryExit18TransitionFinishedEP4CPed", &symbols->transition_finished},
        {"_ZN12CCutsceneMgr13StartCutsceneEv", &symbols->start_cutscene},
        {"_ZN12CCutsceneMgr18DeleteCutsceneDataEv", &symbols->delete_cutscene},
        {"_ZN11CPlayerInfo10KillPlayerEv", &symbols->kill_player},
        {"_ZN11CPlayerInfo12ArrestPlayerEv", &symbols->arrest_player},
        {"_ZN10CGameLogic23HandleRespawnAtHospitalEv", &symbols->respawn_hospital},
        {"_ZN10CGameLogic28HandleRespawnAtPoliceStationEv", &symbols->respawn_police},
        {"_ZN5CGame18ShutDownForRestartEv", &symbols->shutdown_restart},
    };
    for (const Item &item : items) {
        *item.slot = lookup_symbol(info, tables, item.name);
    }
}

int visit_object(dl_phdr_info *info, size_t, void *data) {
    auto *hit = static_cast<WatchHit *>(data);
    char soname[128];
    soname[0] = '\0';
    const bool by_path = name_is_target(info->dlpi_name);
    const bool by_soname = !by_path && copy_soname(info, soname, sizeof(soname)) &&
                           std::strcmp(soname, kLibrary) == 0;
    if (!by_path && !by_soname) {
        return 0;
    }
    hit->matched = true;
    hit->addr = static_cast<uintptr_t>(info->dlpi_addr);
    hit->min_pvaddr = min_load_vaddr(info);
    resolve_symbols(info, &hit->symbols);
    if (by_path) {
        copy_text(hit->label, sizeof(hit->label), info->dlpi_name);
    } else {
        copy_text(hit->label, sizeof(hit->label), soname);
    }
    return 1;
}

IterateFn system_iterate() {
    void *libc = dlopen("libc.so", RTLD_NOW | RTLD_NOLOAD);
    if (libc == nullptr) {
        libc = dlopen("libc.so", RTLD_NOW);
    }
    if (libc == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<IterateFn>(dlsym(libc, "dl_iterate_phdr"));
}

void *watch_main(void *) {
    log_info("status=watching");
    IterateFn iterate = system_iterate();
    if (iterate == nullptr) {
        log_info("dl_iterate_phdr unavailable");
        return nullptr;
    }

    int elapsed = 0;
    int since_beat = 0;
    while (elapsed <= kTimeoutMs) {
        WatchHit hit{};
        iterate(visit_object, &hit);
        if (hit.matched) {
            __android_log_print(ANDROID_LOG_INFO, kTag,
                                "libUE4.so label=%s dlpi_addr=0x%llx min_pvaddr=0x%llx",
                                hit.label, static_cast<unsigned long long>(hit.addr),
                                static_cast<unsigned long long>(hit.min_pvaddr));
            const struct {
                const char *label;
                void *address;
            } resolved[] = {
                {"IsPlayingGame", hit.symbols.is_playing},
                {"FindPlayerPed", hit.symbols.find_player_ped},
                {"TransitionStarted", hit.symbols.transition_started},
                {"TransitionFinished", hit.symbols.transition_finished},
                {"StartCutscene", hit.symbols.start_cutscene},
                {"DeleteCutsceneData", hit.symbols.delete_cutscene},
                {"KillPlayer", hit.symbols.kill_player},
                {"ArrestPlayer", hit.symbols.arrest_player},
                {"HandleRespawnAtHospital", hit.symbols.respawn_hospital},
                {"HandleRespawnAtPoliceStation", hit.symbols.respawn_police},
                {"ShutDownForRestart", hit.symbols.shutdown_restart},
            };
            for (const auto &item : resolved) {
                if (item.address == nullptr) {
                    __android_log_print(ANDROID_LOG_INFO, kTag, "symbol missing %s", item.label);
                } else {
                    __android_log_print(ANDROID_LOG_INFO, kTag, "symbol %s 0x%llx", item.label,
                                        static_cast<unsigned long long>(
                                            reinterpret_cast<uintptr_t>(item.address)));
                }
            }
            if (hit.symbols.is_playing == nullptr || hit.symbols.find_player_ped == nullptr) {
                log_info("symbols incomplete");
                return nullptr;
            }
            plugins_run(hit.addr, &hit.symbols);
            return nullptr;
        }
        if (elapsed >= kTimeoutMs) {
            break;
        }
        usleep(static_cast<useconds_t>(kPollMs) * 1000);
        elapsed += kPollMs;
        since_beat += kPollMs;
        if (since_beat >= kHeartbeatMs) {
            log_info("status=waiting");
            since_beat = 0;
        }
    }
    log_info("libUE4.so not seen in watch window");
    return nullptr;
}

}  // namespace

void start_library_watch() {
    pthread_t thread;
    if (pthread_create(&thread, nullptr, watch_main, nullptr) == 0) {
        pthread_detach(thread);
    } else {
        log_info("failed to start watch thread");
    }
}
