#include "host.h"

#include <android/log.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

constexpr const char *kTag = "GTASADE";
constexpr int kSlots = 16;
constexpr size_t kPatch = 16;

struct Slot {
    int32_t depth;
    int32_t pad;
    void *ctx[4];
    void *detour;
    void (*after)();
    uint64_t saved_lr;
    void *target;
    uint8_t *page;
    void *cont;
};

static_assert(offsetof(Slot, ctx) == 8, "ctx offset");
static_assert(offsetof(Slot, detour) == 40, "detour offset");
static_assert(offsetof(Slot, after) == 48, "after offset");
static_assert(offsetof(Slot, saved_lr) == 56, "saved_lr offset");

constexpr size_t kLit = 0xF00;

Slot g_slots[kSlots];

uint32_t stp64(uint32_t rt, uint32_t rt2, uint32_t rn, int offset) {
    const int imm7 = offset / 8;
    return 0xa9000000u | (static_cast<uint32_t>(imm7) & 0x7fu) << 15 | rt2 << 10 | rn << 5 | rt;
}

uint32_t ldp64(uint32_t rt, uint32_t rt2, uint32_t rn, int offset) {
    const int imm7 = offset / 8;
    return 0xa9400000u | (static_cast<uint32_t>(imm7) & 0x7fu) << 15 | rt2 << 10 | rn << 5 | rt;
}

uint32_t stpq(uint32_t rt, uint32_t rt2, uint32_t rn, int offset) {
    const int imm7 = offset / 16;
    return 0xad000000u | (static_cast<uint32_t>(imm7) & 0x7fu) << 15 | rt2 << 10 | rn << 5 | rt;
}

uint32_t ldpq(uint32_t rt, uint32_t rt2, uint32_t rn, int offset) {
    const int imm7 = offset / 16;
    return 0xad400000u | (static_cast<uint32_t>(imm7) & 0x7fu) << 15 | rt2 << 10 | rn << 5 | rt;
}

uint32_t str64(uint32_t rt, uint32_t rn, int offset) {
    return 0xf9000000u | static_cast<uint32_t>(offset / 8) << 10 | rn << 5 | rt;
}

uint32_t ldr64(uint32_t rt, uint32_t rn, int offset) {
    return 0xf9400000u | static_cast<uint32_t>(offset / 8) << 10 | rn << 5 | rt;
}

bool emit_u32(uint8_t **cursor, uint8_t *end, uint32_t insn);

bool emit_ldr_lit(uint8_t **cursor, uint8_t *end, uint8_t *lit, uint32_t rt) {
    const int64_t words = (lit - *cursor) >> 2;
    if ((words << 2) != (lit - *cursor) || words < -(1 << 18) || words >= (1 << 18)) {
        return false;
    }
    const uint32_t insn = 0x58000000u | (static_cast<uint32_t>(words) & 0x7ffffu) << 5 | rt;
    return emit_u32(cursor, end, insn);
}

bool emit_save(uint8_t **cursor, uint8_t *end) {
    if (!emit_u32(cursor, end, 0xd10c83ffu)) {
        return false;
    }
    for (uint32_t i = 0; i < 30; i += 2) {
        if (!emit_u32(cursor, end, stp64(i, i + 1, 31, static_cast<int>(i * 8)))) {
            return false;
        }
    }
    if (!emit_u32(cursor, end, str64(30, 31, 0xf0)) || !emit_u32(cursor, end, 0x910c83f0u) ||
        !emit_u32(cursor, end, str64(16, 31, 0xf8))) {
        return false;
    }
    for (uint32_t i = 0; i < 32; i += 2) {
        if (!emit_u32(cursor, end, stpq(i, i + 1, 31, 0x100 + static_cast<int>(i * 16)))) {
            return false;
        }
    }
    return emit_u32(cursor, end, 0xd53b4210u) && emit_u32(cursor, end, str64(16, 31, 0x300));
}

bool emit_restore(uint8_t **cursor, uint8_t *end) {
    for (uint32_t i = 0; i < 32; i += 2) {
        if (!emit_u32(cursor, end, ldpq(i, i + 1, 16, 0x100 + static_cast<int>(i * 16)))) {
            return false;
        }
    }
    if (!emit_u32(cursor, end, ldr64(17, 16, 0x300)) || !emit_u32(cursor, end, 0xd51b4211u)) {
        return false;
    }
    for (uint32_t i = 0; i < 16; i += 2) {
        if (!emit_u32(cursor, end, ldp64(i, i + 1, 16, static_cast<int>(i * 8)))) {
            return false;
        }
    }
    for (uint32_t i = 18; i < 30; i += 2) {
        if (!emit_u32(cursor, end, ldp64(i, i + 1, 16, static_cast<int>(i * 8)))) {
            return false;
        }
    }
    return emit_u32(cursor, end, ldr64(30, 16, 0xf0)) && emit_u32(cursor, end, ldr64(17, 16, 0xf8)) &&
           emit_u32(cursor, end, 0x9100023fu) && emit_u32(cursor, end, ldr64(17, 16, 0x88));
}

bool relocate_insn(uint32_t insn, uintptr_t old_pc, uintptr_t new_pc, uint32_t *out) {
    if ((insn & 0x9f000000u) == 0x90000000u) {
        int64_t imm = static_cast<int64_t>(((insn >> 5) & 0x7ffffu) << 2 | ((insn >> 29) & 3u));
        if (imm & (1ll << 20)) {
            imm -= 1ll << 21;
        }
        const int64_t target = static_cast<int64_t>(old_pc & ~0xfffull) + (imm << 12);
        const int64_t newer = target - static_cast<int64_t>(new_pc & ~0xfffull);
        if ((newer & 0xfff) != 0) {
            return false;
        }
        const int64_t new_imm = newer >> 12;
        if (new_imm < -(1ll << 20) || new_imm >= (1ll << 20)) {
            return false;
        }
        const auto encoded = static_cast<uint32_t>(new_imm);
        *out = (insn & 0x9f00001fu) | ((encoded & 3u) << 29) | (((encoded >> 2) & 0x7ffffu) << 5);
        return true;
    }
    if ((insn & 0x7c000000u) == 0x14000000u) {
        int32_t imm26 = static_cast<int32_t>(insn & 0x03ffffffu);
        if (imm26 & 0x02000000) {
            imm26 |= ~0x03ffffff;
        }
        const int64_t dest = static_cast<int64_t>(old_pc) + (static_cast<int64_t>(imm26) << 2);
        const int64_t new_imm = (dest - static_cast<int64_t>(new_pc)) >> 2;
        if (new_imm < -(1ll << 25) || new_imm >= (1ll << 25)) {
            return false;
        }
        *out = (insn & 0xfc000000u) | (static_cast<uint32_t>(new_imm) & 0x03ffffffu);
        return true;
    }
    const bool cond = (insn & 0xff000010u) == 0x54000000u;
    const bool cbz = (insn & 0x7e000000u) == 0x34000000u;
    const bool literal = (insn & 0x3b000000u) == 0x18000000u;
    if (cond || cbz || literal) {
        int32_t imm19 = static_cast<int32_t>((insn >> 5) & 0x7ffffu);
        if (imm19 & 0x40000) {
            imm19 |= ~0x7ffff;
        }
        const int64_t dest = static_cast<int64_t>(old_pc) + (static_cast<int64_t>(imm19) << 2);
        const int64_t new_imm = (dest - static_cast<int64_t>(new_pc)) >> 2;
        if (new_imm < -(1ll << 18) || new_imm >= (1ll << 18)) {
            return false;
        }
        *out = (insn & 0xff00001fu) | ((static_cast<uint32_t>(new_imm) & 0x7ffffu) << 5);
        return true;
    }
    *out = insn;
    return true;
}

void write_abs_jump(uint8_t *site, void *dest) {
    const uint32_t ldr = 0x58000050u;
    const uint32_t br = 0xd61f0200u;
    std::memcpy(site, &ldr, 4);
    std::memcpy(site + 4, &br, 4);
    std::memcpy(site + 8, &dest, 8);
}

bool write_target(void *target, const uint8_t *bytes) {
    const uintptr_t page = reinterpret_cast<uintptr_t>(target) & ~static_cast<uintptr_t>(getpagesize() - 1);
    if (mprotect(reinterpret_cast<void *>(page), static_cast<size_t>(getpagesize()),
                 PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
        std::memcpy(target, bytes, kPatch);
        __builtin___clear_cache(reinterpret_cast<char *>(target),
                                reinterpret_cast<char *>(target) + kPatch);
        mprotect(reinterpret_cast<void *>(page), static_cast<size_t>(getpagesize()), PROT_READ | PROT_EXEC);
        return true;
    }
    return false;
}

bool emit_u32(uint8_t **cursor, uint8_t *end, uint32_t insn) {
    if (*cursor + 4 > end) {
        return false;
    }
    std::memcpy(*cursor, &insn, 4);
    *cursor += 4;
    return true;
}

bool emit_abs_branch(uint8_t **cursor, uint8_t *end, void *dest) {
    if (*cursor + 16 > end) {
        return false;
    }
    write_abs_jump(*cursor, dest);
    *cursor += 16;
    return true;
}

bool emit_insn(uint32_t insn, uintptr_t old_pc, uint8_t **cursor, uint8_t *end) {
    uint32_t relocated = 0;
    if (relocate_insn(insn, old_pc, reinterpret_cast<uintptr_t>(*cursor), &relocated)) {
        return emit_u32(cursor, end, relocated);
    }
    if ((insn & 0x9f000000u) == 0x90000000u) {
        int64_t imm = static_cast<int64_t>(((insn >> 5) & 0x7ffffu) << 2 | ((insn >> 29) & 3u));
        if (imm & (1ll << 20)) {
            imm -= 1ll << 21;
        }
        const auto page = static_cast<uint64_t>(static_cast<uintptr_t>(old_pc & ~0xfffull) +
                                                 static_cast<uintptr_t>(imm << 12));
        const uint32_t rd = insn & 31u;
        const uint32_t ldr = 0x58000040u | rd;
        const uint32_t branch = 0x14000003u;
        if (!emit_u32(cursor, end, ldr) || !emit_u32(cursor, end, branch) || *cursor + 8 > end) {
            return false;
        }
        std::memcpy(*cursor, &page, 8);
        *cursor += 8;
        return true;
    }
    if ((insn & 0xff000010u) == 0x54000000u) {
        int32_t imm19 = static_cast<int32_t>((insn >> 5) & 0x7ffffu);
        if (imm19 & 0x40000) {
            imm19 |= ~0x7ffff;
        }
        const auto dest = reinterpret_cast<void *>(static_cast<uintptr_t>(
            static_cast<int64_t>(old_pc) + (static_cast<int64_t>(imm19) << 2)));
        const uint32_t cond = insn & 0xfu;
        const uint32_t taken = 0x54000040u | cond;
        const uint32_t skip = 0x14000005u;
        if (!emit_u32(cursor, end, taken) || !emit_u32(cursor, end, skip)) {
            return false;
        }
        return emit_abs_branch(cursor, end, dest);
    }
    if ((insn & 0xfc000000u) == 0x94000000u) {
        int32_t imm26 = static_cast<int32_t>(insn & 0x03ffffffu);
        if (imm26 & 0x02000000) {
            imm26 |= ~0x03ffffff;
        }
        const auto dest = reinterpret_cast<void *>(static_cast<uintptr_t>(
            static_cast<int64_t>(old_pc) + (static_cast<int64_t>(imm26) << 2)));
        const uint32_t ldr = 0x58000070u;
        const uint32_t blr = 0xd63f0200u;
        const uint32_t skip = 0x14000003u;
        if (!emit_u32(cursor, end, ldr) || !emit_u32(cursor, end, blr) || !emit_u32(cursor, end, skip) ||
            *cursor + 8 > end) {
            return false;
        }
        std::memcpy(*cursor, &dest, 8);
        *cursor += 8;
        return true;
    }
    return false;
}

bool emit_push(uint8_t **cursor, uint8_t *end, uint8_t *lit_slot) {
    if (!emit_ldr_lit(cursor, end, lit_slot, 16) || !emit_u32(cursor, end, 0xb9400211u) ||
        !emit_u32(cursor, end, 0x7100123fu) || !emit_u32(cursor, end, 0x54000043u) ||
        !emit_u32(cursor, end, 0x52800071u)) {
        return false;
    }
    return emit_u32(cursor, end, 0x8b314e02u) && emit_u32(cursor, end, 0x910003e3u) &&
           emit_u32(cursor, end, 0xf9000443u) && emit_u32(cursor, end, 0x11000631u) &&
           emit_u32(cursor, end, 0xb9000211u);
}

bool emit_pop(uint8_t **cursor, uint8_t *end, uint8_t *lit_slot, uint8_t *lit_gate) {
    if (!emit_ldr_lit(cursor, end, lit_slot, 16) || !emit_u32(cursor, end, 0xb9400211u) ||
        !emit_u32(cursor, end, 0x51000631u) || !emit_u32(cursor, end, 0xb9000211u) ||
        !emit_u32(cursor, end, 0x8b314e02u) || !emit_u32(cursor, end, 0xf9400450u)) {
        return false;
    }
    if (!emit_u32(cursor, end, ldr64(17, 16, 0xf0)) || !emit_ldr_lit(cursor, end, lit_slot, 2) ||
        !emit_u32(cursor, end, 0xf9001c51u) || !emit_ldr_lit(cursor, end, lit_gate, 17) ||
        !emit_u32(cursor, end, str64(17, 16, 0xf0))) {
        return false;
    }
    return emit_restore(cursor, end);
}

void *make_trampoline(Slot &slot, void *resume) {
    const size_t page_size = static_cast<size_t>(getpagesize());
    void *mem = mmap(nullptr, page_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "hook: no executable page");
        return nullptr;
    }
    auto *page = static_cast<uint8_t *>(mem);
    auto *cursor = page;
    uint8_t *end = page + kLit;
    uint8_t *lit_slot = page + kLit;
    uint8_t *lit_gate = page + kLit + 8;
    uint8_t *cont = nullptr;
    uint8_t *gate = nullptr;
    bool ok = emit_save(&cursor, end);
    ok = ok && emit_push(&cursor, end, lit_slot);
    ok = ok && emit_ldr_lit(&cursor, end, lit_slot, 16);
    ok = ok && emit_u32(&cursor, end, ldr64(16, 16, static_cast<int>(offsetof(Slot, detour))));
    ok = ok && emit_u32(&cursor, end, 0xd63f0200u);
    cont = cursor;
    ok = ok && emit_pop(&cursor, end, lit_slot, lit_gate);
    for (size_t i = 0; i < 4 && ok; ++i) {
        uint32_t insn = 0;
        std::memcpy(&insn, static_cast<uint8_t *>(slot.target) + i * 4, 4);
        ok = emit_insn(insn, reinterpret_cast<uintptr_t>(slot.target) + i * 4, &cursor, end);
    }
    ok = ok && emit_abs_branch(&cursor, end, resume);
    gate = cursor;
    ok = ok && emit_u32(&cursor, end, 0xd10043ffu) && emit_u32(&cursor, end, str64(0, 31, 0));
    ok = ok && emit_ldr_lit(&cursor, end, lit_slot, 16);
    ok = ok && emit_u32(&cursor, end, ldr64(17, 16, static_cast<int>(offsetof(Slot, after))));
    uint8_t *cbz_at = cursor;
    ok = ok && emit_u32(&cursor, end, 0xb4000051u);
    ok = ok && emit_u32(&cursor, end, 0xd63f0220u);
    const int64_t skip = (cursor - cbz_at) >> 2;
    if (ok && skip != 2) {
        ok = false;
    }
    ok = ok && emit_ldr_lit(&cursor, end, lit_slot, 16);
    ok = ok && emit_u32(&cursor, end, ldr64(30, 16, static_cast<int>(offsetof(Slot, saved_lr))));
    ok = ok && emit_u32(&cursor, end, ldr64(0, 31, 0)) && emit_u32(&cursor, end, 0x910043ffu) &&
        emit_u32(&cursor, end, 0xd65f03c0u);
    if (!ok || cont == nullptr || gate == nullptr) {
        munmap(mem, page_size);
        __android_log_print(ANDROID_LOG_INFO, kTag, "hook: trampoline relocation failed");
        return nullptr;
    }
    const auto slot_addr = reinterpret_cast<uint64_t>(&slot);
    const auto gate_addr = reinterpret_cast<uint64_t>(gate);
    std::memcpy(lit_slot, &slot_addr, 8);
    std::memcpy(lit_gate, &gate_addr, 8);
    slot.page = page;
    slot.cont = cont;
    __builtin___clear_cache(reinterpret_cast<char *>(page), reinterpret_cast<char *>(cursor));
    return page;
}

}  // namespace

extern "C" void gtasa_hook_on_return(void *target, void (*after)()) {
    for (Slot &slot : g_slots) {
        if (slot.target == target) {
            slot.after = after;
            return;
        }
    }
}

extern "C" int gtasa_hook(void *target, void *detour, void **original) {
    if (target == nullptr || detour == nullptr || original == nullptr) {
        return -1;
    }
    if ((reinterpret_cast<uintptr_t>(target) & 3u) != 0) {
        return -1;
    }
    for (Slot &slot : g_slots) {
        if (slot.target != target) {
            continue;
        }
        *original = slot.detour;
        slot.detour = detour;
        return 0;
    }
    for (Slot &slot : g_slots) {
        if (slot.target != nullptr) {
            continue;
        }
        slot.target = target;
        slot.detour = detour;
        slot.after = nullptr;
        slot.depth = 0;
        slot.saved_lr = 0;
        if (make_trampoline(slot, static_cast<uint8_t *>(target) + kPatch) == nullptr) {
            slot.target = nullptr;
            slot.detour = nullptr;
            return -1;
        }
        uint8_t patched[kPatch];
        write_abs_jump(patched, slot.page);
        if (!write_target(target, patched)) {
            munmap(slot.page, static_cast<size_t>(getpagesize()));
            slot.page = nullptr;
            slot.target = nullptr;
            slot.detour = nullptr;
            __android_log_print(ANDROID_LOG_INFO, kTag, "hook: write failed");
            return -1;
        }
        *original = slot.cont;
        return 0;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "hook: table full");
    return -1;
}
