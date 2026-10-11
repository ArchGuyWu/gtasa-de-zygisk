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
// CEventGunShot::AffectsPed(CPed*): when the witness is a cop and the shooter
// is not the player, this b.ne returns false before the distance and line-of-sight checks.
constexpr uintptr_t kRejectNpcShot = 0x54c65cc;
constexpr uint32_t kRejectBranch = 0x540009e1u;
constexpr uint32_t kNop = 0xd503201fu;
// CEventHandler::IsKillTaskAppropriate(CPed*, CPed*, CEvent const&)
// An armed ped returns true because its weapon-info word is non-zero.
// An unarmed ped returns false when the other ped is armed.
constexpr uintptr_t kKillTask = 0x54e7340;
constexpr uint32_t kKillTaskFirst = 0x3957e008u;
constexpr uintptr_t kWeaponInfo = 0xae71578;
constexpr int kPedType = 0x7e0;
constexpr int kCopType = 6;
// Occupant of a vehicle with a driver: the first instruction loads the driver.
// Other branches land on the ped-type check 8 bytes later, so only the first
// instruction can be replaced.
constexpr uintptr_t kInCarGate = 0x54c64ec;
constexpr uintptr_t kInCarTypeCheck = 0x54c64f4;
constexpr uintptr_t kInCarFalse = 0x54c6708;
constexpr uint32_t kInCarFirst = 0xf9435508u;
// After the original hearing-distance test. Cops in a vehicle skip the facing and line-of-sight test.
constexpr uintptr_t kSightGate = 0x54c6694;
constexpr uintptr_t kSightContinue = 0x54c6698;
constexpr uintptr_t kSightAccept = 0x54c6700;
constexpr uint32_t kSightFirst = 0x3940e269u;
// CTaskComplexKillCriminal::CreateFirstSubTask.
// The original compare treats every cop in this task as the driver and then
// sets the driver mark. Only the ped in the driver seat enters that block.
// A passenger takes the sit exit, task 709. An on-foot cop takes the original
// on-foot path, including its driver mark.
constexpr uintptr_t kFootGate = 0x57c2e20;
constexpr uintptr_t kFootOnFoot = 0x57c2eb4;
constexpr uintptr_t kFootDriver = 0x57c30b4;
// Only drivers reach this point. A criminal on foot takes the game's own
// task-724 creation. A criminal already in a vehicle keeps the original path.
constexpr uintptr_t kOnFootCriminal = 0x57c3210;
constexpr uint32_t kOnFootCriminalTbz = 0x3607e609u;
constexpr uintptr_t kCriminalInCar = 0x57c3214;
constexpr uintptr_t kPassengerSit = 0x57c32c0;
constexpr uintptr_t kOriginalDrive = 0x57c3228;
constexpr uintptr_t kTaskAlloc = 0x56d0b88;
constexpr uintptr_t kRegisterRef = 0x5477b60;
constexpr uintptr_t kRefFree = 0xa8ba620;
constexpr uint32_t kFootFirst = 0x360004a8u;
constexpr int kDriveMission = 724;
// FireInstantHit scales spread by the target's speed only when the target is
// the player. A cop uses that same scale when the target is an NPC.
constexpr uintptr_t kNpcSpread = 0x585e564;
constexpr uint32_t kNpcSpreadBranch = 0x540002a8u;
constexpr uintptr_t kNpcSpreadApply = 0x585e568;
constexpr uintptr_t kNpcSpreadSkip = 0x585e5b8;
constexpr uintptr_t kVehicleDamage = 0x54e4bf4;
constexpr int kKillCriminalType = 1105;
constexpr int kPolicePursuit = 1103;
// Damage response stores a flee task when the cop has no weapon. The fight task
// gives the pistol itself once the response is a fight.
constexpr uintptr_t kPersonality = 0x54e73a4;
constexpr int kKillOnFoot = 1000;
// Task 1000 refuses an unarmed ped and sends them to flee when the attacker is armed.
constexpr uintptr_t kUnarmedGate = 0x54e74f4;
constexpr uint32_t kUnarmedCbz = 0x3400780au;
constexpr uintptr_t kUnarmedFlee = 0x54e83f4;
constexpr uintptr_t kUnarmedFight = 0x54e74f8;
constexpr uintptr_t kKillCriminalCtor = 0x57c25d4;
constexpr unsigned kKillCriminalBytes = 56;
constexpr uintptr_t kSetTask = 0x57aa824;
constexpr int kIntel = 1512;
// Persistent event responses use this slot. The car's own drive task stays in slot 4.
constexpr int kChaseSlot = 2;
// CEventGroup::Add has already accepted the ped. The original hearing-distance
// test inside AffectsPed has already rejected anyone too far away.
// w20 is zero when the decision task stayed 200, which means ignore, and the
// gunshot is then not stored. w22 is the event type. x19 is the group, x21 the event.
constexpr uintptr_t kIgnoreDrop = 0x54c31c8;
constexpr uint32_t kIgnoreCbz = 0x34000314u;
constexpr uintptr_t kIgnoreSkip = 0x54c3228;
constexpr uintptr_t kIgnoreKeep = 0x54c31cc;
// Shot response has just loaded the shooter. A cop is aimed at the nearest
// gunman this unit has heard. Keeping the current chase jumps to the epilogue.
constexpr uintptr_t kShotSource = 0x54dd57c;
constexpr uint32_t kShotSourceLdr = 0xf9400c34u;
constexpr uintptr_t kShotContinue = 0x54dd580;
constexpr uintptr_t kShotKeep = 0x54ddd1c;
// Wanted level 2 and above hands the next traffic slot to a police car.
// That slot stays a normal car. CreatePoliceChase still calls ChoosePoliceCarModel itself.
constexpr uintptr_t kSlotHandoff = 0x531dfbc;
constexpr uint32_t kSlotHandoffMov = 0x2a1f03e0u;
constexpr uint32_t kSlotHandoffBranch = 0x14000025u;
// CreatePoliceChase is skipped while stars are not zero. Drop that skip so a wanted
// player still gets the game's own chase cars.
constexpr uintptr_t kChaseStars = 0x531f92c;
constexpr uint32_t kChaseStarsCbnz = 0x35000728u;
// On foot, stars >= 3, and the cop count still under the cap: these branches
// respawn a cop about 42.5-50.5 m away. They join the normal ped path instead.
// The same generator ORs a second bit when its on-foot cop count is 0 and then
// stores ped type 6. Clearing that combined flag keeps the ped a civilian.
constexpr uintptr_t kCopPedFlag = 0x55f5d90;
constexpr uint32_t kCopPedOrr = 0x2a0902c8u;
constexpr uint32_t kCopPedClear = 0x2a1f03e8u;
// The type picker writes ped type 6 when its switch is set. Skip that roll.
constexpr uintptr_t kCopPicker = 0x55f38bc;
constexpr uint32_t kCopPickerCbz = 0x34000248u;
constexpr uint32_t kCopPickerSkip = 0x14000012u;
constexpr uintptr_t kRefillFar = 0x55f5bd4;
constexpr uint32_t kRefillFarGe = 0x54002e2au;
constexpr uint32_t kRefillFarNormal = 0x5400056au;
constexpr uintptr_t kRefillNear = 0x55f5c0c;
constexpr uint32_t kRefillNearLs = 0x54002c69u;
constexpr uint32_t kRefillNearNormal = 0x540003a9u;
constexpr uintptr_t kRefillFull = 0x55f5c7c;
constexpr uint32_t kRefillFullGe = 0x540028eau;
constexpr uintptr_t kFindPlayer = 0x5482ce4;
constexpr uintptr_t kClearPursuit = 0x57c13e4;
constexpr uintptr_t kCanJoinPursuit = 0x54aad34;
constexpr uintptr_t kPursueTail = 0x5784d00;
constexpr uint32_t kPursueTailBranch = 0x17f4980du;
constexpr uintptr_t kPersistCall = 0x57c1ee0;
constexpr uint32_t kPersistCallBl = 0x940000ecu;
constexpr uintptr_t kPersistPursuit = 0x57c2290;
constexpr uintptr_t kPersistBack = 0x57c1ee4;
constexpr uintptr_t kPersistAbort = 0x57c1f88;
// A wanted-level event gives the cop a player-pursuit task. Skip that when
// the person they are already attacking is nearer than the player.
constexpr uintptr_t kWantedResponse = 0x54e3e30;
constexpr uintptr_t kReportCrime = 0x5433eb0;
constexpr uint32_t kReportCrimeSub = 0xd101c3ffu;
constexpr uintptr_t kReportCrimeNext = 0x5433eb4;
// Extra cars are rolled when a normal traffic car is about to be created.
constexpr uintptr_t kChooseModelSite = 0x531e050;
constexpr uint32_t kChooseModelSub = 0xd10183a0u;
constexpr uintptr_t kChooseModelNext = 0x531e054;
// A cop-class traffic pick is thrown away while stars are above 0.
// Above 3 stars that pick is the usual one, so the slot has to become an ordinary car.
constexpr uintptr_t kCopClassAbort = 0x531e090;
constexpr uint32_t kCopClassAbortGt = 0x54009f2cu;
constexpr uintptr_t kCopClassKeep = 0x531e094;
constexpr uintptr_t kCopClassDrop = 0x531f474;
constexpr uintptr_t kPickRandomCar = 0x55fe0d8;
constexpr uintptr_t kCivilianCars = 0xa8b8768;
// After the second ChoosePoliceCarModel. Joining earlier lets that call replace
// the chosen model, so every 4-star extra becomes an enforcer.
constexpr uintptr_t kPoliceModelJoin = 0x531e0d0;
constexpr uintptr_t kRandCarSpawn = 0x531e5b0;
constexpr uint32_t kRandCarSpawnBl = 0x97fff7feu;
constexpr uintptr_t kRandCarSpawnNext = 0x531e5b4;
constexpr uintptr_t kChaseModel = 0x532198c;
constexpr uint32_t kChaseModelBl = 0x97fffa38u;
constexpr uintptr_t kChaseModelNext = 0x5321990;
constexpr uintptr_t kChaseSpawn = 0x53219b0;
constexpr uint32_t kChaseSpawnBl = 0x97ffeafeu;
constexpr uintptr_t kChaseSpawnNext = 0x53219b4;
// A water node rolls on its own when model selection did not already roll this slot.
// A hit becomes the police boat. A miss there does not roll again.
constexpr uintptr_t kWaterTest = 0x531e2c8;
constexpr uint32_t kWaterTbnz = 0x373800b3u;
constexpr uintptr_t kWaterLand = 0x531e2cc;
constexpr uintptr_t kWaterCop = 0x531e2dc;
constexpr uintptr_t kWaterCiv = 0x531e30c;
// Placing a wanted roadblock is the nearby-dispatch mark. The game bool stays
// set while that dispatch is running, and is cleared when it stops.
constexpr uintptr_t kRoadEntry = 0x53a49d0;
constexpr uint32_t kRoadEntryStp = 0x6db63befu;
constexpr uintptr_t kRoadEntryNext = 0x53a49d4;
constexpr uintptr_t kRoadDispatch = 0xa8b9170;
constexpr uintptr_t kGetNewVehicle = 0x531c5a8;
constexpr uintptr_t kChoosePoliceModel = 0x532026c;
constexpr uintptr_t kDefaultCopModel = 0x5641f6c;
// Pool slot still occupied. IsVehiclePointerValid also requires status
// +0x894/+0x890 or a pointer at +0xe8, which a car just returned from
// GetNewVehicle does not have yet, so that check never keeps the cap.
constexpr uintptr_t kVehicleSlot = 0x583c73c;
// Helicopter pool. Bit 7 of a slot flag means the slot is free. Model 497 is the police heli.
constexpr uintptr_t kHeliPool = 0xa8b8610;
constexpr int kHeliBytes = 0xdb8;
constexpr int kModelIndex = 0x3a;
constexpr int kPoliceHeliModel = 497;
constexpr uintptr_t kFindWanted = 0x548ac30;
constexpr uintptr_t kGameTimer = 0xaae2404;
constexpr uintptr_t kWantedUpdate = 0x54a9580;
constexpr uintptr_t kGenerateDamage = 0x5856268;
constexpr uintptr_t kKillPedWithCar = 0x55b21ac;
constexpr int kWantedStars = 0x2c;
constexpr int kPursuitCount = 0x18;
constexpr int kMaxPursuit = 0x19;
constexpr int kMaxCopCars = 0x1a;
constexpr uintptr_t kLawCars = 0xa8b86d8;
// CWanted keeps up to ten pursuit cops at this stride. ClearPursuit nulls a slot
// without packing the array, so the count byte is not a dense prefix.
constexpr int kPursuitSlots = 0x2b8;
constexpr int kPursuitSlotCount = 10;
// Sight fills at this many units. The same many killed units stretch the fall.
constexpr int kUnitCap = 4;

const GtasaHost *g_host = nullptr;
bool g_patched = false;
bool g_active = false;
bool g_kill_patched = false;
bool g_car_patched = false;
uint32_t g_car_saved = 0;
uint8_t *g_car_stub = nullptr;
bool g_sight_patched = false;
uint32_t g_sight_saved = 0;
uint8_t *g_sight_stub = nullptr;
bool g_foot_patched = false;
uint32_t g_foot_saved = 0;
uint8_t *g_foot_stub = nullptr;
bool g_far_patched = false;
uint32_t g_far_saved = 0;
uint8_t *g_far_stub = nullptr;
bool g_aim_patched = false;
uint32_t g_aim_saved = 0;
uint8_t *g_aim_stub = nullptr;
bool g_ignore_patched = false;
uint32_t g_ignore_saved = 0;
uint8_t *g_ignore_stub = nullptr;
using VehicleFn = void (*)(uint8_t *, uint8_t *);
VehicleFn g_vehicle_orig = nullptr;
using PersonalityFn = void (*)(uint8_t *, uint8_t *, uint8_t *);
PersonalityFn g_personality_orig = nullptr;
using WantedFn = void (*)(uint8_t *, uint8_t *, uint8_t *, uint8_t *);
WantedFn g_wanted_orig = nullptr;
using WantedUpdateFn = void (*)(uint8_t *);
WantedUpdateFn g_wanted_update_orig = nullptr;
using DamageFn = void (*)(uint8_t *, uint8_t *, uint8_t *, int, int, int, int);
DamageFn g_damage_orig = nullptr;
using KillCarFn = void (*)(uint8_t *, uint8_t *, float, int);
KillCarFn g_kill_car_orig = nullptr;
float g_clue = 0.f;
float g_clue_speed = 0.f;
float g_clue_goal = 0.f;
int g_cop_kills = 0;
uint8_t *g_killed_units[kUnitCap] = {};
int g_fall_kills = -1;
int g_fall_stars = -1;
uint32_t g_clue_ms = 0;
uint32_t g_report_ms = 0;
uint32_t g_next_extra_ms = 0;
int g_pending_model = -1;
bool g_slot_rolled = false;
uint8_t *g_natural[8] = {};
int g_natural_count = 0;
bool g_road_placed = false;
bool g_road_entry_patched = false;
uint8_t *g_road_page = nullptr;
bool g_crime_patched = false;
uint8_t *g_crime_page = nullptr;
uint8_t *g_extra[8] = {};
int g_extra_model[8] = {};
int g_extra_count = 0;
bool g_choose_patched = false;
bool g_rand_spawn_patched = false;
bool g_chase_model_patched = false;
bool g_chase_spawn_patched = false;
uint8_t *g_extra_page = nullptr;
bool g_unarmed_patched = false;
uint32_t g_unarmed_saved = 0;
uint8_t *g_unarmed_stub = nullptr;
bool g_dispatch_patched = false;
uint32_t g_dispatch_saved = 0;
uint8_t *g_dispatch_stub = nullptr;
bool g_slot_patched = false;
bool g_chase_patched = false;
bool g_refill_far_patched = false;
bool g_refill_near_patched = false;
bool g_refill_full_patched = false;
bool g_cop_ped_patched = false;
bool g_cop_picker_patched = false;
bool g_water_patched = false;
bool g_abort_patched = false;
bool g_pursue_patched = false;
bool g_control_patched = false;
uint8_t *g_pursuit_page = nullptr;

int weapon_flag(uint8_t *ped) {
    const int slot = *reinterpret_cast<int8_t *>(ped + 0xa10);
    if (slot < 0 || slot > 12) {
        return 0;
    }
    const int weapon = *reinterpret_cast<int16_t *>(ped + slot * 40 + 0x7f0);
    if (weapon < 0 || weapon > 60 || g_host == nullptr) {
        return 0;
    }
    auto *table = reinterpret_cast<uint8_t *>(g_host->base + kWeaponInfo);
    return *reinterpret_cast<int *>(table + static_cast<unsigned>(weapon) * 0x70u);
}

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

bool set_react(bool enable) {
    if (g_host == nullptr) {
        return false;
    }
    const uintptr_t address = g_host->base + kRejectNpcShot;
    const uint32_t have = *reinterpret_cast<uint32_t *>(address);
    const uint32_t need = enable ? kRejectBranch : kNop;
    const uint32_t next = enable ? kNop : kRejectBranch;
    if (have == next) {
        g_patched = enable;
        return true;
    }
    if (have != need || !write_word(address, next)) {
        return false;
    }
    g_patched = enable;
    return true;
}

bool write_jump(uintptr_t address, void *dest);

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

uint8_t *build_car_stub(uintptr_t site, uintptr_t cont, uintptr_t fail) {
    uint8_t *page = map_near(site);
    if (page == nullptr) {
        return nullptr;
    }
    const uint32_t code[] = {
        0xf9435510u,  // ldr x16, [x8, #0x6a8]
        0xb40000d0u,  // cbz x16, cont_abs
        0xb947e291u,  // ldr w17, [x20, #0x7e0]
        0x71001a3fu,  // cmp w17, #6
        0x54000060u,  // b.eq cont_abs
        0x580000b0u,  // ldr x16, fail_lit
        0xd61f0200u,  // br x16
        0x580000b0u,  // ldr x16, cont_lit
        0xd61f0200u,  // br x16
        0xd503201fu,  // nop
    };
    std::memcpy(page, code, sizeof(code));
    std::memcpy(page + 40, &fail, sizeof(fail));
    std::memcpy(page + 48, &cont, sizeof(cont));
    __builtin___clear_cache(reinterpret_cast<char *>(page), reinterpret_cast<char *>(page) + 64);
    return page;
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

bool set_car(bool enable) {
    if (g_host == nullptr) {
        return false;
    }
    const uintptr_t address = g_host->base + kInCarGate;
    if (enable) {
        if (g_car_stub == nullptr) {
            if (*reinterpret_cast<uint32_t *>(address) != kInCarFirst) {
                return false;
            }
            g_car_saved = *reinterpret_cast<uint32_t *>(address);
            g_car_stub = build_car_stub(address, g_host->base + kInCarTypeCheck, g_host->base + kInCarFalse);
            if (g_car_stub == nullptr) {
                return false;
            }
        }
        if (!write_branch(address, g_car_stub)) {
            return false;
        }
        g_car_patched = true;
        return true;
    }
    if (!g_car_patched) {
        return true;
    }
    if (!write_word(address, g_car_saved)) {
        return false;
    }
    g_car_patched = false;
    return true;
}

uint8_t *build_sight_stub(uintptr_t site, uintptr_t accept, uintptr_t cont) {
    uint8_t *page = map_near(site);
    if (page == nullptr) {
        return nullptr;
    }
    const uint32_t code[] = {
        0x3958d690u,  // ldrb w16, [x20, #0x635]
        0x360000d0u,  // tbz w16, #0, replay
        0xb947e290u,  // ldr w16, [x20, #0x7e0]
        0x71001a1fu,  // cmp w16, #6
        0x54000061u,  // b.ne replay
        0x580000f0u,  // ldr x16, accept_lit
        0xd61f0200u,  // br x16
        0x3940e269u,  // ldrb w9, [x19, #56]
        0x580000d0u,  // ldr x16, cont_lit
        0xd61f0200u,  // br x16
        0xd503201fu,
        0xd503201fu,
    };
    std::memcpy(page, code, sizeof(code));
    std::memcpy(page + 48, &accept, sizeof(accept));
    std::memcpy(page + 56, &cont, sizeof(cont));
    __builtin___clear_cache(reinterpret_cast<char *>(page), reinterpret_cast<char *>(page) + 64);
    return page;
}

uint8_t *build_foot_stub(uintptr_t site, uintptr_t on_foot, uintptr_t sit) {
    uint8_t *page = map_near(site);
    if (page == nullptr) {
        return nullptr;
    }
    const uintptr_t driver = g_host->base + kFootDriver;
    const uint32_t code[] = {
        0x36000108u,  // tbz w8, #0, to_foot
        0xf943e410u,  // ldr x16, [x0, #0x7c8]
        0xb40000d0u,  // cbz x16, to_foot
        0xf9435611u,  // ldr x17, [x16, #0x6a8]
        0xeb00023fu,  // cmp x17, x0
        0x540000a1u,  // b.ne to_sit
        0x58000110u,  // ldr x16, driver
        0xd61f0200u,  // br x16
        0x58000110u,  // ldr x16, foot
        0xd61f0200u,  // br x16
        0x58000110u,  // ldr x16, sit
        0xd61f0200u,  // br x16
        0xd503201fu,
        0xd503201fu,
    };
    std::memcpy(page, code, sizeof(code));
    std::memcpy(page + 0x38, &driver, sizeof(driver));
    std::memcpy(page + 0x40, &on_foot, sizeof(on_foot));
    std::memcpy(page + 0x48, &sit, sizeof(sit));
    __builtin___clear_cache(reinterpret_cast<char *>(page), reinterpret_cast<char *>(page) + 0x50);
    return page;
}

uint8_t *build_far_stub(uintptr_t) {
    uint8_t *page = map_near(g_host->base + kOnFootCriminal);
    if (page == nullptr) {
        return nullptr;
    }
    const uintptr_t cont = g_host->base + kCriminalInCar;
    const uintptr_t drive = g_host->base + kOriginalDrive;
    const uint32_t code[] = {
        0x37000049u,  // tbnz w9, #0, to_cont
        0x14000003u,  // b to_drive
        0x58000090u,  // ldr x16, cont
        0xd61f0200u,  // br x16
        0x58000090u,  // ldr x16, drive
        0xd61f0200u,  // br x16
    };
    std::memcpy(page, code, sizeof(code));
    std::memcpy(page + 0x18, &cont, sizeof(cont));
    std::memcpy(page + 0x20, &drive, sizeof(drive));
    __builtin___clear_cache(reinterpret_cast<char *>(page), reinterpret_cast<char *>(page) + 0x28);
    return page;
}

uint8_t *build_aim_stub(uintptr_t) {
    uint8_t *page = map_near(g_host->base + kNpcSpread);
    if (page == nullptr) {
        return nullptr;
    }
    const uintptr_t apply = g_host->base + kNpcSpreadApply;
    const uintptr_t skip = g_host->base + kNpcSpreadSkip;
    const uint32_t code[] = {
        0xd53b4210u,  // mrs x16, nzcv
        0xb4000096u,  // cbz x22, restore
        0xb947e2d1u,  // ldr w17, [x22, #0x7e0]
        0x71001a3fu,  // cmp w17, #6
        0x54000060u,  // b.eq apply
        0xd51b4210u,  // msr nzcv, x16
        0x54000068u,  // b.hi skip
        0x580000b0u,  // ldr x16, apply_lit
        0xd61f0200u,  // br x16
        0x580000b0u,  // ldr x16, skip_lit
        0xd61f0200u,  // br x16
    };
    std::memcpy(page, code, sizeof(code));
    std::memcpy(page + 48, &apply, sizeof(apply));
    std::memcpy(page + 56, &skip, sizeof(skip));
    __builtin___clear_cache(reinterpret_cast<char *>(page), reinterpret_cast<char *>(page) + 64);
    return page;
}

int task_type(uint8_t *task) {
    if (task == nullptr) {
        return -1;
    }
    auto *vt = *reinterpret_cast<uint8_t **>(task);
    auto get = *reinterpret_cast<int (**)(uint8_t *)>(vt + 40);
    return get(task);
}

uint8_t *kill_criminal(uint8_t *ped);
uint8_t *find_pursuit(uint8_t *cop);
bool retarget_kill(uint8_t *cop, uint8_t *attacker, bool report = true);

bool install_chase(uint8_t *ped, uint8_t *shooter) {
    auto *intel = *reinterpret_cast<uint8_t **>(ped + kIntel);
    if (intel == nullptr) {
        return false;
    }
    auto *manager = intel + 8;
    using AllocFn = uint8_t *(*)(unsigned);
    using CtorFn = void (*)(uint8_t *, uint8_t *, int);
    using SetFn = void (*)(uint8_t *, uint8_t *, int, int);
    auto *task = reinterpret_cast<AllocFn>(g_host->base + kTaskAlloc)(kKillCriminalBytes);
    if (task == nullptr) {
        return false;
    }
    reinterpret_cast<CtorFn>(g_host->base + kKillCriminalCtor)(task, shooter, 0);
    if (kill_criminal(ped) != nullptr) {
        auto *vt = *reinterpret_cast<uint8_t **>(task);
        auto dtor = *reinterpret_cast<void (**)(uint8_t *)>(vt + 8);
        dtor(task);
        retarget_kill(ped, shooter);
        return false;
    }
    reinterpret_cast<SetFn>(g_host->base + kSetTask)(manager, task, kChaseSlot, 0);
    return true;
}

uint8_t *kill_criminal(uint8_t *ped) {
    auto *intel = *reinterpret_cast<uint8_t **>(ped + kIntel);
    if (intel == nullptr) {
        return nullptr;
    }
    auto *manager = intel + 8;
    for (int i = 0; i < 5; ++i) {
        auto *task = *reinterpret_cast<uint8_t **>(manager + i * 8);
        if (task != nullptr && task_type(task) == kKillCriminalType) {
            return task;
        }
    }
    return nullptr;
}

void unlink_ref(uint8_t *entity, uint8_t **slot) {
    if (entity == nullptr || g_host == nullptr) {
        return;
    }
    auto **prev = reinterpret_cast<uint8_t **>(entity + 64);
    for (int guard = 0; *prev != nullptr && guard < 64; ++guard) {
        uint8_t *node = *prev;
        uint8_t *next = *reinterpret_cast<uint8_t **>(node);
        if (*reinterpret_cast<uint8_t **>(node + 8) != reinterpret_cast<uint8_t *>(slot)) {
            prev = reinterpret_cast<uint8_t **>(node);
            continue;
        }
        *prev = next;
        auto *header = *reinterpret_cast<uint8_t **>(g_host->base + kRefFree);
        if (header != nullptr) {
            *reinterpret_cast<uint8_t **>(node) = *reinterpret_cast<uint8_t **>(header);
            *reinterpret_cast<uint8_t **>(node + 8) = nullptr;
            *reinterpret_cast<uint8_t **>(header) = node;
        }
        return;
    }
}

void aim_at(uint8_t **slot, uint8_t *target) {
    if (*slot == target) {
        return;
    }
    unlink_ref(*slot, slot);
    *slot = target;
    if (target == nullptr || g_host == nullptr) {
        return;
    }
    using RegFn = void (*)(uint8_t *, uint8_t **, int);
    reinterpret_cast<RegFn>(g_host->base + kRegisterRef)(target, slot, 0);
}

// Keep the chase that is already running and aim it at the attacker.
// Rebuilding the fight here destroys the task the manager is still using.
bool retarget_kill(uint8_t *cop, uint8_t *attacker, bool report) {
    if (cop == nullptr || attacker == nullptr || g_host == nullptr) {
        return false;
    }
    if (*reinterpret_cast<int *>(cop + kPedType) != kCopType) {
        return false;
    }
    if ((attacker[0x6a] & 7) != 3) {
        return false;
    }
    if (*reinterpret_cast<int *>(attacker + kPedType) == kCopType) {
        return false;
    }
    auto *task = kill_criminal(cop);
    if (task == nullptr) {
        return false;
    }
    auto *sub = *reinterpret_cast<uint8_t **>(task + 16);
    const int kind = task_type(sub);
    auto **chase = reinterpret_cast<uint8_t **>(task + 24);
    uint8_t **driven = nullptr;
    uint8_t **foot = nullptr;
    if (kind == kDriveMission) {
        auto *vehicle = *reinterpret_cast<uint8_t **>(sub + 24);
        if (vehicle != nullptr) {
            driven = reinterpret_cast<uint8_t **>(vehicle + 1624);
        }
    } else if (kind == kKillOnFoot) {
        foot = reinterpret_cast<uint8_t **>(sub + 72);
    }
    const bool same = *chase == attacker && (driven == nullptr || *driven == attacker) &&
                      (foot == nullptr || *foot == attacker);
    if (same) {
        return false;
    }
    aim_at(chase, attacker);
    if (kind == kDriveMission) {
        *reinterpret_cast<uint8_t **>(sub + 56) = attacker;
        if (driven != nullptr) {
            aim_at(driven, attacker);
        }
    } else if (foot != nullptr) {
        aim_at(foot, attacker);
    }
    if (report) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react retarget");
    }
    return true;
}

void switch_target(uint8_t *cop, uint8_t *event) {
    if (cop == nullptr || event == nullptr) {
        return;
    }
    if ((cop[0x635] & 1) == 0) {
        return;
    }
    auto *damaged = *reinterpret_cast<uint8_t **>(event + 24);
    auto *mine = *reinterpret_cast<uint8_t **>(cop + 0x7c8);
    auto *attacker = *reinterpret_cast<uint8_t **>(event + 32);
    if (damaged == nullptr || mine != damaged) {
        return;
    }
    retarget_kill(cop, attacker);
}

void on_personality(uint8_t *handler, uint8_t *event, uint8_t *attacker) {
    if (g_active && handler != nullptr && event != nullptr) {
        auto *ped = *reinterpret_cast<uint8_t **>(handler);
        if (ped != nullptr && *reinterpret_cast<int *>(ped + kPedType) == kCopType) {
            auto *decision = reinterpret_cast<int16_t *>(event + 14);
            if (retarget_kill(ped, attacker) || kill_criminal(ped) != nullptr) {
                *decision = 200;
            }
        }
    }
    if (g_personality_orig != nullptr) {
        g_personality_orig(handler, event, attacker);
    }
}

void on_vehicle_damage(uint8_t *handler, uint8_t *event) {
    if (g_active && handler != nullptr) {
        switch_target(*reinterpret_cast<uint8_t **>(handler), event);
    }
    if (g_vehicle_orig != nullptr) {
        g_vehicle_orig(handler, event);
    }
}

const float *entity_pos(uint8_t *entity) {
    auto *matrix = *reinterpret_cast<uint8_t **>(entity + 0x18);
    if (matrix != nullptr) {
        return reinterpret_cast<const float *>(matrix + 0x30);
    }
    return reinterpret_cast<const float *>(entity + 8);
}

float dist2(uint8_t *from, uint8_t *to) {
    const float *a = entity_pos(from);
    const float *b = entity_pos(to);
    const float dx = a[0] - b[0];
    const float dy = a[1] - b[1];
    const float dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

bool heard_criminal(uint8_t *ped, uint8_t *self) {
    if (ped == nullptr || ped == self) {
        return false;
    }
    if ((ped[0x6a] & 7) != 3) {
        return false;
    }
    if (*reinterpret_cast<int *>(ped + kPedType) == kCopType) {
        return false;
    }
    return *reinterpret_cast<float *>(ped + 0x76c) > 0.f;
}

void add_case(uint8_t **list, int *count, uint8_t *ped, uint8_t *self) {
    if (!heard_criminal(ped, self) || *count >= 16) {
        return;
    }
    for (int i = 0; i < *count; ++i) {
        if (list[i] == ped) {
            return;
        }
    }
    list[(*count)++] = ped;
}

void add_heard(uint8_t **list, int *count, uint8_t *ped) {
    if (ped == nullptr) {
        return;
    }
    auto *intel = *reinterpret_cast<uint8_t **>(ped + kIntel);
    if (intel == nullptr) {
        return;
    }
    auto *group = intel + 200;
    const int n = *reinterpret_cast<int *>(group + 16);
    if (n <= 0 || n > 16) {
        return;
    }
    for (int i = 0; i < n; ++i) {
        auto *event = *reinterpret_cast<uint8_t **>(group + 24 + i * 8);
        if (event == nullptr) {
            continue;
        }
        auto *vt = *reinterpret_cast<uintptr_t **>(event);
        if (vt == nullptr) {
            continue;
        }
        const int type = reinterpret_cast<int (*)(uint8_t *)>(vt[2])(event);
        if (type != 15) {
            continue;
        }
        add_case(list, count, *reinterpret_cast<uint8_t **>(event + 24), ped);
    }
}

void add_unit_heard(uint8_t **list, int *count, uint8_t *cop) {
    if ((cop[0x635] & 1) == 0) {
        add_heard(list, count, cop);
        return;
    }
    auto *vehicle = *reinterpret_cast<uint8_t **>(cop + 0x7c8);
    if (vehicle == nullptr) {
        return;
    }
    add_heard(list, count, *reinterpret_cast<uint8_t **>(vehicle + 0x6a8));
    for (int seat = 0; seat < 8; ++seat) {
        add_heard(list, count, *reinterpret_cast<uint8_t **>(vehicle + 0x6b0 + seat * 8));
    }
}

uint8_t *find_player() {
    if (g_host == nullptr) {
        return nullptr;
    }
    using FindFn = uint8_t *(*)(int);
    return reinterpret_cast<FindFn>(g_host->base + kFindPlayer)(0);
}

// The car's driver is the unit. A passenger is not. On foot the cop is the unit.
// Null when the cop is marked in a car but the vehicle or its driver is missing.
uint8_t *unit_leader(uint8_t *cop, bool *leader = nullptr) {
    if ((cop[0x635] & 1) == 0) {
        if (leader != nullptr) {
            *leader = true;
        }
        return cop;
    }
    auto *vehicle = *reinterpret_cast<uint8_t **>(cop + 0x7c8);
    if (vehicle == nullptr) {
        return nullptr;
    }
    auto *driver = *reinterpret_cast<uint8_t **>(vehicle + 0x6a8);
    if (driver == nullptr) {
        return nullptr;
    }
    if (leader != nullptr) {
        *leader = driver == cop;
    }
    return driver;
}

void take_npc(uint8_t *cop, uint8_t *npc, uint8_t *current) {
    if (current != nullptr) {
        retarget_kill(cop, npc, false);
    } else {
        install_chase(cop, npc);
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react pursuit npc");
}

void drop_pursuit(uint8_t *task, uint8_t *cop) {
    if (task == nullptr || cop == nullptr || g_host == nullptr) {
        return;
    }
    using ClearFn = void (*)(uint8_t *, uint8_t *);
    reinterpret_cast<ClearFn>(g_host->base + kClearPursuit)(task, cop);
}

// Null means the player is nearer, tied, or the only case, so the vanilla pursuit stays.
// A returned ped is a heard non-player strictly closer than the player.
uint8_t *nearer_than_player(uint8_t *cop) {
    if (!g_active || cop == nullptr || g_host == nullptr) {
        return nullptr;
    }
    if (*reinterpret_cast<int *>(cop + kPedType) != kCopType) {
        return nullptr;
    }
    bool leader = false;
    uint8_t *unit = unit_leader(cop, &leader);
    if (unit == nullptr) {
        return nullptr;
    }
    uint8_t *player = find_player();
    if (player == nullptr) {
        return nullptr;
    }
    uint8_t *list[16];
    int count = 0;
    add_unit_heard(list, &count, unit);
    uint8_t *current = nullptr;
    bool saw_criminal = false;
    uint8_t *foot[5];
    int foot_count = 0;
    auto *intel = *reinterpret_cast<uint8_t **>(unit + kIntel);
    if (intel != nullptr) {
        auto *manager = intel + 8;
        for (int i = 0; i < 5; ++i) {
            auto *task = *reinterpret_cast<uint8_t **>(manager + i * 8);
            if (task == nullptr) {
                continue;
            }
            const int kind = task_type(task);
            if (kind == kKillCriminalType && !saw_criminal) {
                saw_criminal = true;
                current = *reinterpret_cast<uint8_t **>(task + 24);
                add_case(list, &count, current, unit);
            } else if (kind == kKillOnFoot) {
                auto *target = *reinterpret_cast<uint8_t **>(task + 0x20);
                add_case(list, &count, target, unit);
                foot[foot_count++] = target;
            }
        }
    }
    uint8_t *best = nullptr;
    float best_d = 0.f;
    for (int i = 0; i < count; ++i) {
        if (list[i] == player) {
            continue;
        }
        const float d = dist2(unit, list[i]);
        if (best == nullptr || d < best_d) {
            best = list[i];
            best_d = d;
        }
    }
    if (best == nullptr || !(best_d < dist2(unit, player))) {
        return nullptr;
    }
    if (leader) {
        bool fighting = current == best;
        for (int i = 0; i < foot_count; ++i) {
            if (foot[i] == best) {
                fighting = true;
            }
        }
        if (!fighting) {
            take_npc(unit, best, current);
        }
    }
    return best;
}

void on_wanted_response(uint8_t *handler, uint8_t *event, uint8_t *task_a, uint8_t *task_b) {
    if (g_active && handler != nullptr) {
        auto *ped = *reinterpret_cast<uint8_t **>(handler);
        if (nearer_than_player(ped) != nullptr) {
            return;
        }
    }
    if (g_wanted_orig != nullptr) {
        g_wanted_orig(handler, event, task_a, task_b);
    }
}

// One means this pursuit task should end. PersistPursuit is not called.
int leave_pursuit(uint8_t *task, uint8_t *cop) {
    if (nearer_than_player(cop) == nullptr || task == nullptr) {
        return 0;
    }
    drop_pursuit(task, cop);
    return 1;
}

// Null keeps the chase already aimed at the nearest person, or means this cop
// just left the player pursuit for a nearer NPC. Any other pointer is who to chase.
uint8_t *dispatch_target(uint8_t *cop, uint8_t *event_shooter) {
    if (!g_active || cop == nullptr) {
        return event_shooter;
    }
    bool leader = false;
    if (unit_leader(cop, &leader) == nullptr || !leader) {
        return nullptr;
    }
    uint8_t *list[16];
    int count = 0;
    add_unit_heard(list, &count, cop);
    add_case(list, &count, event_shooter, cop);
    uint8_t *current = nullptr;
    if (auto *task = kill_criminal(cop)) {
        current = *reinterpret_cast<uint8_t **>(task + 24);
        add_case(list, &count, current, cop);
    }
    if (count == 0) {
        return event_shooter;
    }
    uint8_t *best = nullptr;
    float best_d = 0.f;
    for (int i = 0; i < count; ++i) {
        const float d = dist2(cop, list[i]);
        const bool tie = list[i] == current && d == best_d;
        if (best == nullptr || d < best_d || tie) {
            best = list[i];
            best_d = d;
        }
    }
    const bool keep = best == current;
    if (!keep && best != event_shooter) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react assign nearest");
    }
    uint8_t *player = find_player();
    if (player != nullptr && best != player && dist2(cop, best) < dist2(cop, player)) {
        if (auto *pursuit = find_pursuit(cop)) {
            if (!keep) {
                take_npc(cop, best, current);
            }
            drop_pursuit(pursuit, cop);
            return nullptr;
        }
    }
    if (keep) {
        return nullptr;
    }
    return best;
}

bool looks_like_task(uint8_t *task) {
    if (task == nullptr || g_host == nullptr) {
        return false;
    }
    const auto vt = *reinterpret_cast<uintptr_t *>(task);
    return vt >= g_host->base && vt < g_host->base + g_host->size;
}

uint8_t *find_pursuit(uint8_t *cop) {
    auto *intel = *reinterpret_cast<uint8_t **>(cop + kIntel);
    if (intel == nullptr) {
        return nullptr;
    }
    auto *manager = intel + 8;
    for (int i = 0; i < 5; ++i) {
        auto *task = *reinterpret_cast<uint8_t **>(manager + i * 8);
        if (!looks_like_task(task)) {
            continue;
        }
        if (task_type(task) == kPolicePursuit) {
            return task;
        }
        auto *sub = *reinterpret_cast<uint8_t **>(task + 0x10);
        if (looks_like_task(sub) && task_type(sub) == kPolicePursuit) {
            return sub;
        }
    }
    return nullptr;
}

void chase_ignored(uint8_t *cop, uint8_t *shooter) {
    if (!g_active || cop == nullptr || shooter == nullptr) {
        return;
    }
    bool leader = false;
    uint8_t *driver = unit_leader(cop, &leader);
    if (driver == nullptr) {
        return;
    }
    if (!leader) {
        cop = driver;
    }
    uint8_t *best = dispatch_target(cop, shooter);
    if (best == nullptr) {
        return;
    }
    if (install_chase(cop, best)) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react in-car heard");
    }
}

uint8_t *build_ignore_stub(uintptr_t) {
    uint8_t *page = map_near(g_host->base + kIgnoreDrop);
    if (page == nullptr) {
        return nullptr;
    }
    const uintptr_t chase = reinterpret_cast<uintptr_t>(chase_ignored);
    const uintptr_t skip = g_host->base + kIgnoreSkip;
    const uintptr_t keep = g_host->base + kIgnoreKeep;
    const uint32_t code[] = {
        0x35000374u,  // cbnz w20, store
        0x71003edfu,  // cmp w22, #15
        0x540002e1u,  // b.ne drop
        0xf9400670u,  // ldr x16, [x19, #8]
        0xb40002b0u,  // cbz x16, drop
        0x3958d611u,  // ldrb w17, [x16, #0x635]
        0x36000271u,  // tbz w17, #0, drop
        0xb947e211u,  // ldr w17, [x16, #0x7e0]
        0x71001a3fu,  // cmp w17, #6
        0x54000201u,  // b.ne drop
        0xf9400eb1u,  // ldr x17, [x21, #24]
        0xb40001d1u,  // cbz x17, drop
        0x3941aa20u,  // ldrb w0, [x17, #0x6a]
        0x12000800u,  // and w0, w0, #7
        0x71000c1fu,  // cmp w0, #3
        0x54000141u,  // b.ne drop
        0xb947e220u,  // ldr w0, [x17, #0x7e0]
        0x7100041fu,  // cmp w0, #1
        0x540000edu,  // b.le drop
        0x7100181fu,  // cmp w0, #6
        0x540000a0u,  // b.eq drop
        0xaa1003e0u,  // mov x0, x16
        0xaa1103e1u,  // mov x1, x17
        0x580000f0u,  // ldr x16, fn
        0xd63f0200u,  // blr x16
        0x580000f0u,  // ldr x16, skip
        0xd61f0200u,  // br x16
        0x580000f0u,  // ldr x16, cont
        0xd61f0200u,  // br x16
        0xd503201fu,
    };
    std::memcpy(page, code, sizeof(code));
    std::memcpy(page + 0x78, &chase, sizeof(chase));
    std::memcpy(page + 0x80, &skip, sizeof(skip));
    std::memcpy(page + 0x88, &keep, sizeof(keep));
    __builtin___clear_cache(reinterpret_cast<char *>(page), reinterpret_cast<char *>(page) + 0x90);
    return page;
}

bool set_ignore(bool enable) {
    if (g_host == nullptr) {
        return false;
    }
    const uintptr_t address = g_host->base + kIgnoreDrop;
    if (enable) {
        if (g_ignore_stub == nullptr) {
            if (*reinterpret_cast<uint32_t *>(address) != kIgnoreCbz) {
                return false;
            }
            g_ignore_saved = kIgnoreCbz;
            g_ignore_stub = build_ignore_stub(address);
            if (g_ignore_stub == nullptr) {
                return false;
            }
        }
        if (!write_branch(address, g_ignore_stub)) {
            return false;
        }
        g_ignore_patched = true;
        return true;
    }
    if (!g_ignore_patched) {
        return true;
    }
    if (!write_word(address, g_ignore_saved)) {
        return false;
    }
    g_ignore_patched = false;
    return true;
}

uint8_t *build_unarmed_stub(uintptr_t) {
    uint8_t *page = map_near(g_host->base + kUnarmedGate);
    if (page == nullptr) {
        return nullptr;
    }
    const uintptr_t flee = g_host->base + kUnarmedFlee;
    const uintptr_t next = g_host->base + kUnarmedFight;
    const uint32_t code[] = {
        0x350000cau,  // cbnz w10, cont
        0xb947e110u,  // ldr w16, [x8, #0x7e0]
        0x71001a1fu,  // cmp w16, #6
        0x54000060u,  // b.eq cont
        0x58000090u,  // ldr x16, flee
        0xd61f0200u,  // br x16
        0x58000090u,  // ldr x16, next
        0xd61f0200u,  // br x16
    };
    std::memcpy(page, code, sizeof(code));
    std::memcpy(page + 0x20, &flee, sizeof(flee));
    std::memcpy(page + 0x28, &next, sizeof(next));
    __builtin___clear_cache(reinterpret_cast<char *>(page), reinterpret_cast<char *>(page) + 0x30);
    return page;
}

bool set_unarmed(bool enable) {
    if (g_host == nullptr) {
        return false;
    }
    const uintptr_t address = g_host->base + kUnarmedGate;
    if (enable) {
        if (g_unarmed_stub == nullptr) {
            if (*reinterpret_cast<uint32_t *>(address) != kUnarmedCbz) {
                return false;
            }
            g_unarmed_saved = kUnarmedCbz;
            g_unarmed_stub = build_unarmed_stub(address);
            if (g_unarmed_stub == nullptr) {
                return false;
            }
        }
        if (!write_branch(address, g_unarmed_stub)) {
            return false;
        }
        g_unarmed_patched = true;
        return true;
    }
    if (!g_unarmed_patched) {
        return true;
    }
    if (!write_word(address, g_unarmed_saved)) {
        return false;
    }
    g_unarmed_patched = false;
    return true;
}

bool set_aim(bool enable) {
    if (g_host == nullptr) {
        return false;
    }
    const uintptr_t address = g_host->base + kNpcSpread;
    if (enable) {
        if (g_aim_stub == nullptr) {
            if (*reinterpret_cast<uint32_t *>(address) != kNpcSpreadBranch) {
                return false;
            }
            g_aim_saved = kNpcSpreadBranch;
            g_aim_stub = build_aim_stub(address);
            if (g_aim_stub == nullptr) {
                return false;
            }
        }
        if (!write_branch(address, g_aim_stub)) {
            return false;
        }
        g_aim_patched = true;
        return true;
    }
    if (!g_aim_patched) {
        return true;
    }
    if (!write_word(address, g_aim_saved)) {
        return false;
    }
    g_aim_patched = false;
    return true;
}

bool set_far(bool enable) {
    if (g_host == nullptr) {
        return false;
    }
    const uintptr_t address = g_host->base + kOnFootCriminal;
    if (enable) {
        if (g_far_stub == nullptr) {
            if (*reinterpret_cast<uint32_t *>(address) != kOnFootCriminalTbz) {
                return false;
            }
            g_far_saved = kOnFootCriminalTbz;
            g_far_stub = build_far_stub(address);
            if (g_far_stub == nullptr) {
                return false;
            }
        }
        if (!write_branch(address, g_far_stub)) {
            return false;
        }
        g_far_patched = true;
        return true;
    }
    if (!g_far_patched) {
        return true;
    }
    if (!write_word(address, g_far_saved)) {
        return false;
    }
    g_far_patched = false;
    return true;
}

bool set_branch_patch(bool enable, uintptr_t offset, uint32_t expect, uint32_t *saved, uint8_t **stub,
                      bool *patched, uint8_t *(*build)(uintptr_t, uintptr_t, uintptr_t), uintptr_t primary,
                      uintptr_t secondary) {
    if (g_host == nullptr) {
        return false;
    }
    const uintptr_t address = g_host->base + offset;
    if (enable) {
        if (*stub == nullptr) {
            if (*reinterpret_cast<uint32_t *>(address) != expect) {
                return false;
            }
            *saved = expect;
            *stub = build(address, g_host->base + primary, g_host->base + secondary);
            if (*stub == nullptr) {
                return false;
            }
        }
        if (!write_branch(address, *stub)) {
            return false;
        }
        *patched = true;
        return true;
    }
    if (!*patched) {
        return true;
    }
    if (!write_word(address, *saved)) {
        return false;
    }
    *patched = false;
    return true;
}

bool write_jump(uintptr_t address, void *dest) {
    const uintptr_t page_size = static_cast<uintptr_t>(getpagesize());
    const uintptr_t page = address & ~(page_size - 1);
    if (mprotect(reinterpret_cast<void *>(page), page_size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        return false;
    }
    const uint32_t ldr = 0x58000050u;
    const uint32_t br = 0xd61f0200u;
    std::memcpy(reinterpret_cast<void *>(address), &ldr, 4);
    std::memcpy(reinterpret_cast<void *>(address + 4), &br, 4);
    std::memcpy(reinterpret_cast<void *>(address + 8), &dest, 8);
    __builtin___clear_cache(reinterpret_cast<char *>(address), reinterpret_cast<char *>(address) + 16);
    mprotect(reinterpret_cast<void *>(page), page_size, PROT_READ | PROT_EXEC);
    return true;
}

uint8_t *build_dispatch_stub(uintptr_t) {
    uint8_t *page = map_near(g_host->base + kShotSource);
    if (page == nullptr) {
        return nullptr;
    }
    const uintptr_t fn = reinterpret_cast<uintptr_t>(dispatch_target);
    const uintptr_t next = g_host->base + kShotContinue;
    const uintptr_t keep = g_host->base + kShotKeep;
    const uint32_t code[] = {
        0xf9400c34u, 0xb4000234u, 0xf9400010u, 0xb40001f0u, 0xb947e211u, 0x71001a3fu, 0x54000181u, 0xd10083ffu,
        0xa90007e0u, 0xaa1003e0u, 0xaa1403e1u, 0x580001b0u, 0xd63f0200u, 0xaa0003f1u, 0xa94007e0u, 0x910083ffu,
        0xb4000091u, 0xaa1103f4u, 0x58000110u, 0xd61f0200u, 0xaa0003f3u, 0xf900227fu, 0x580000d0u, 0xd61f0200u,
    };
    std::memcpy(page, code, sizeof(code));
    std::memcpy(page + 0x60, &fn, sizeof(fn));
    std::memcpy(page + 0x68, &next, sizeof(next));
    std::memcpy(page + 0x70, &keep, sizeof(keep));
    __builtin___clear_cache(reinterpret_cast<char *>(page), reinterpret_cast<char *>(page) + 0x78);
    return page;
}

bool set_dispatch(bool enable) {
    if (g_host == nullptr) {
        return false;
    }
    const uintptr_t address = g_host->base + kShotSource;
    if (enable) {
        if (g_dispatch_stub == nullptr) {
            if (*reinterpret_cast<uint32_t *>(address) != kShotSourceLdr) {
                return false;
            }
            g_dispatch_saved = kShotSourceLdr;
            g_dispatch_stub = build_dispatch_stub(address);
            if (g_dispatch_stub == nullptr) {
                return false;
            }
        }
        if (!write_branch(address, g_dispatch_stub)) {
            return false;
        }
        g_dispatch_patched = true;
        return true;
    }
    if (!g_dispatch_patched) {
        return true;
    }
    if (!write_word(address, g_dispatch_saved)) {
        return false;
    }
    g_dispatch_patched = false;
    return true;
}

bool exchange_word(bool enable, uintptr_t rva, uint32_t vanilla, uint32_t changed, bool *done) {
    if (g_host == nullptr) {
        return false;
    }
    const uintptr_t address = g_host->base + rva;
    const uint32_t have = *reinterpret_cast<uint32_t *>(address);
    const uint32_t from = enable ? vanilla : changed;
    const uint32_t to = enable ? changed : vanilla;
    if (have == to) {
        *done = enable;
        return true;
    }
    if (have != from || !write_word(address, to)) {
        return false;
    }
    *done = enable;
    return true;
}

bool vehicle_ok(uint8_t *veh) {
    if (veh == nullptr || g_host == nullptr) {
        return false;
    }
    using SlotFn = int (*)(uint8_t *);
    return reinterpret_cast<SlotFn>(g_host->base + kVehicleSlot)(veh) != 0;
}

void prune_cars(uint8_t **list, int *count, int *models) {
    int kept = 0;
    for (int i = 0; i < *count; ++i) {
        if (!vehicle_ok(list[i])) {
            continue;
        }
        list[kept] = list[i];
        if (models != nullptr) {
            models[kept] = models[i];
        }
        ++kept;
    }
    *count = kept;
}

uint32_t game_ms() {
    return *reinterpret_cast<uint32_t *>(g_host->base + kGameTimer);
}

uint32_t mix_rand() {
    static uint32_t state = 0x6d2b79f5u;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

int default_cop_model() {
    using ModelFn = int (*)(unsigned);
    return reinterpret_cast<ModelFn>(g_host->base + kDefaultCopModel)(1);
}

int regularize_chase_model(int model) {
    if (model == 427 || model == 490 || model == 432 || model == 433) {
        return default_cop_model();
    }
    return model;
}

bool player_caused(uint8_t *attacker) {
    uint8_t *player = find_player();
    if (player == nullptr || attacker == nullptr) {
        return false;
    }
    if (attacker == player) {
        return true;
    }
    if ((attacker[0x6a] & 7) != 2) {
        return false;
    }
    auto *driver = *reinterpret_cast<uint8_t **>(attacker + 0x6a8);
    return driver == player;
}

void note_cop_kill(uint8_t *cop) {
    bool leader = false;
    uint8_t *unit = unit_leader(cop, &leader);
    if (!leader) {
        return;
    }
    for (int i = 0; i < g_cop_kills; ++i) {
        if (g_killed_units[i] == unit) {
            return;
        }
    }
    if (g_cop_kills >= kUnitCap) {
        return;
    }
    g_killed_units[g_cop_kills++] = unit;
}

void note_if_cop_killed(uint8_t *ped, float before, uint8_t *cause) {
    if (!g_active || ped == nullptr) {
        return;
    }
    const float after = *reinterpret_cast<float *>(ped + 0x76c);
    if (before > 0.f && after <= 0.f && *reinterpret_cast<int *>(ped + kPedType) == kCopType &&
        player_caused(cause)) {
        note_cop_kill(ped);
    }
}

void on_damage(uint8_t *weapon, uint8_t *victim, uint8_t *attacker, int kind, int unknown, int piece,
               int direction) {
    const float before = victim == nullptr ? 1.f : *reinterpret_cast<float *>(victim + 0x76c);
    if (g_damage_orig != nullptr) {
        g_damage_orig(weapon, victim, attacker, kind, unknown, piece, direction);
    }
    note_if_cop_killed(victim, before, attacker);
}

void on_kill_car(uint8_t *ped, uint8_t *vehicle, float impulse, int unknown) {
    const float before = ped == nullptr ? 1.f : *reinterpret_cast<float *>(ped + 0x76c);
    if (g_kill_car_orig != nullptr) {
        g_kill_car_orig(ped, vehicle, impulse, unknown);
    }
    note_if_cop_killed(ped, before, vehicle);
}

// An on-foot cop is one unit. Everyone in the same car shares the driver's unit.
int pursuit_units(uint8_t *wanted) {
    uint8_t *seen[kPursuitSlotCount];
    int count = 0;
    for (int i = 0; i < kPursuitSlotCount; ++i) {
        auto *cop = *reinterpret_cast<uint8_t **>(wanted + kPursuitSlots + i * 8);
        if (cop == nullptr) {
            continue;
        }
        uint8_t *unit = unit_leader(cop);
        if (unit == nullptr) {
            continue;
        }
        bool found = false;
        for (int j = 0; j < count; ++j) {
            if (seen[j] == unit) {
                found = true;
            }
        }
        if (found) {
            continue;
        }
        seen[count++] = unit;
    }
    return count;
}

void refresh_road_placed();
float clue_target(int units);

void update_clue(uint8_t *wanted) {
    const uint32_t now = game_ms();
    if (g_clue_ms == 0) {
        g_clue_ms = now;
    }
    float dt = static_cast<float>(now - g_clue_ms) / 1000.f;
    g_clue_ms = now;
    if (dt > 1.f) {
        dt = 0.f;
    }
    const int stars = wanted == nullptr ? 0 : *reinterpret_cast<int *>(wanted + kWantedStars);
    if (stars <= 0) {
        g_cop_kills = 0;
        g_report_ms = 0;
        g_next_extra_ms = 0;
    }
    const int units = wanted == nullptr ? 0 : pursuit_units(wanted);
    const float target = clue_target(units);
    const bool rising = target > g_clue;
    if (target != g_clue_goal || (!rising && (g_fall_kills != g_cop_kills || g_fall_stars != stars))) {
        g_clue_goal = target;
        g_fall_kills = g_cop_kills;
        g_fall_stars = stars;
        const float heat = 1.f + static_cast<float>(g_cop_kills) + 0.5f * static_cast<float>(stars > 0 ? stars : 0);
        const float seconds = rising ? 3.f : 10.f * heat;
        g_clue_speed = (target - g_clue) / seconds;
    }
    g_clue += g_clue_speed * dt;
    if ((g_clue_speed > 0.f && g_clue > g_clue_goal) || (g_clue_speed < 0.f && g_clue < g_clue_goal)) {
        g_clue = g_clue_goal;
        g_clue_speed = 0.f;
    }
}

void on_wanted_update(uint8_t *wanted) {
    if (g_active) {
        update_clue(wanted);
    }
    if (g_wanted_update_orig != nullptr) {
        g_wanted_update_orig(wanted);
    }
}

// Vanilla car cap, plus one slot that only the probability roll can fill.
// A full pursuit list stops cars the same way the game does.
void cop_quota(uint8_t *wanted, int *have, int *cap, int *vanilla) {
    *have = 0;
    *cap = 0;
    *vanilla = 0;
    if (wanted == nullptr || g_host == nullptr) {
        return;
    }
    const int max_cars = wanted[kMaxCopCars];
    *vanilla = max_cars;
    auto *count = *reinterpret_cast<int **>(g_host->base + kLawCars);
    if (count != nullptr && *count > 0) {
        *have = *count;
    }
    const int max_cops = wanted[kMaxPursuit];
    const int limit = max_cars > 0 ? max_cars + 1 : 1;
    if (max_cars <= 0 || (max_cops > 0 && wanted[kPursuitCount] >= max_cops)) {
        *cap = limit;
        *have = *cap;
        return;
    }
    *cap = limit;
}

float star_ceiling(int stars) {
    if (stars < 2) {
        return 0.f;
    }
    if (stars == 2) {
        return 0.15f;
    }
    if (stars == 3) {
        return 0.20f;
    }
    if (stars == 4) {
        return 0.25f;
    }
    return 0.30f;
}

// Clue, stars, and kills each run from 0 to 1. Their sum may pass 1.
// Only 1 counts, and that maps the next roll's wait from 8s down to 5s.
uint32_t extra_gap_ms(int stars) {
    float clue = g_clue;
    if (clue < 0.f) {
        clue = 0.f;
    }
    if (clue > 1.f) {
        clue = 1.f;
    }
    float star_part = static_cast<float>(stars > 0 ? stars : 0) / 6.f;
    if (star_part > 1.f) {
        star_part = 1.f;
    }
    float kill_part = static_cast<float>(g_cop_kills) / static_cast<float>(kUnitCap);
    if (kill_part > 1.f) {
        kill_part = 1.f;
    }
    float heat = clue + star_part + kill_part;
    if (heat > 1.f) {
        heat = 1.f;
    }
    return static_cast<uint32_t>((8.f - 3.f * heat) * 1000.f);
}

bool extra_ready() {
    if (g_host == nullptr) {
        return false;
    }
    if (g_next_extra_ms == 0) {
        return true;
    }
    return static_cast<int32_t>(game_ms() - g_next_extra_ms) >= 0;
}

void arm_extra_gap() {
    if (g_host == nullptr) {
        return;
    }
    using WantedLookup = uint8_t *(*)(int);
    auto *wanted = reinterpret_cast<WantedLookup>(g_host->base + kFindWanted)(0);
    const int stars = wanted == nullptr ? 0 : *reinterpret_cast<int *>(wanted + kWantedStars);
    g_next_extra_ms = game_ms() + extra_gap_ms(stars);
}

float roll_chance(int stars, float clue, int have, int vanilla, int cap) {
    if (cap <= 0 || have >= cap) {
        return 0.f;
    }
    const float room = static_cast<float>(cap - have) / static_cast<float>(cap);
    const bool bonus = vanilla > 0 && have >= vanilla;
    const float ceiling = star_ceiling(stars);
    if (ceiling <= 0.f) {
        return 0.f;
    }
    return bonus ? ceiling * clue : ceiling * clue * room;
}

bool special_present(int model) {
    for (int i = 0; i < g_extra_count; ++i) {
        if (g_extra_model[i] == model) {
            return true;
        }
    }
    return false;
}

bool police_heli_up() {
    auto *pool = *reinterpret_cast<uint8_t **>(g_host->base + kHeliPool);
    if (pool == nullptr) {
        return false;
    }
    auto *objects = *reinterpret_cast<uint8_t **>(pool);
    auto *flags = *reinterpret_cast<uint8_t **>(pool + 8);
    const int size = *reinterpret_cast<int *>(pool + 0x10);
    if (objects == nullptr || flags == nullptr || size <= 0 || size > 32) {
        return false;
    }
    for (int i = 0; i < size; ++i) {
        if ((flags[i] & 0x80) != 0) {
            continue;
        }
        uint8_t *heli = objects + static_cast<unsigned>(i) * kHeliBytes;
        if (*reinterpret_cast<int16_t *>(heli + kModelIndex) == kPoliceHeliModel) {
            return true;
        }
    }
    return false;
}

// Sight fills the meter. A roadblock, a crime report, and a police helicopter
// only make up the share that witnesses have not already filled.
float clue_target(int units) {
    refresh_road_placed();
    const int seen = units > kUnitCap ? kUnitCap : units;
    float sum = 0.25f * static_cast<float>(seen);
    if (g_road_placed) {
        sum += 0.25f;
    }
    if (g_report_ms != 0 && game_ms() < g_report_ms) {
        sum += 0.25f;
    }
    if (g_host != nullptr && police_heli_up()) {
        sum += 0.50f;
    }
    if (sum > 1.f) {
        sum = 1.f;
    }
    return sum;
}

bool water_roll() {
    if (!g_active || g_host == nullptr || g_slot_rolled || !extra_ready()) {
        return false;
    }
    using WantedLookup = uint8_t *(*)(int);
    auto *wanted = reinterpret_cast<WantedLookup>(g_host->base + kFindWanted)(0);
    const int stars = wanted == nullptr ? 0 : *reinterpret_cast<int *>(wanted + kWantedStars);
    int cap = 0;
    int have = 0;
    int vanilla = 0;
    cop_quota(wanted, &have, &cap, &vanilla);
    const float chance = roll_chance(stars, g_clue, have, vanilla, cap);
    if (chance <= 0.f || static_cast<float>(mix_rand() % 10000) >= chance * 10000.f) {
        return false;
    }
    g_pending_model = 430;
    __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react water boat");
    return true;
}

int pick_extra_model(int stars) {
    int model = default_cop_model();
    int special = 0;
    if (stars == 4) {
        special = 427;
    } else if (stars == 5) {
        special = 490;
    } else if (stars >= 6) {
        special = (mix_rand() & 1u) != 0u ? 432 : 433;
    }
    if (special != 0 && (stars >= 5 || !special_present(special))) {
        float chance = 0.5f + 0.1f * static_cast<float>(g_cop_kills);
        if (stars > 4) {
            chance += 0.05f * static_cast<float>(stars - 4);
        }
        if (chance > 0.9f) {
            chance = 0.9f;
        }
        if (static_cast<float>(mix_rand() % 10000) < chance * 10000.f) {
            model = special;
        }
    }
    return model;
}

void on_report_crime(unsigned crime, uint8_t *ped) {
    if (!g_active || g_host == nullptr || ped == nullptr || crime == 0) {
        return;
    }
    if (*reinterpret_cast<int *>(ped + kPedType) > 1) {
        return;
    }
    g_report_ms = game_ms() + 3000;
}

int extra_model() {
    g_slot_rolled = false;
    if (!extra_ready()) {
        return -1;
    }
    g_pending_model = -1;
    if (!g_active || g_host == nullptr) {
        return -1;
    }
    prune_cars(g_natural, &g_natural_count, nullptr);
    prune_cars(g_extra, &g_extra_count, g_extra_model);
    if (g_clue <= 0.f || (g_clue_goal <= 0.f && g_natural_count <= 0)) {
        return -1;
    }
    using WantedLookup = uint8_t *(*)(int);
    auto *wanted = reinterpret_cast<WantedLookup>(g_host->base + kFindWanted)(0);
    const int stars = wanted == nullptr ? 0 : *reinterpret_cast<int *>(wanted + kWantedStars);
    int cap = 0;
    int have = 0;
    int vanilla = 0;
    cop_quota(wanted, &have, &cap, &vanilla);
    const float chance = roll_chance(stars, g_clue, have, vanilla, cap);
    g_slot_rolled = true;
    if (static_cast<float>(mix_rand() % 10000) >= chance * 10000.f) {
        return -1;
    }
    const int model = pick_extra_model(stars);
    g_pending_model = model;
    __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react extra car");
    return model;
}

void note_natural(uint8_t *veh) {
    if (!vehicle_ok(veh)) {
        return;
    }
    for (int i = 0; i < g_natural_count; ++i) {
        if (g_natural[i] == veh) {
            return;
        }
    }
    if (g_natural_count >= 8) {
        return;
    }
    g_natural[g_natural_count++] = veh;
}

bool road_dispatch_active() {
    if (g_host == nullptr) {
        return false;
    }
    auto *flag = *reinterpret_cast<uint8_t **>(g_host->base + kRoadDispatch);
    return flag != nullptr && *flag != 0;
}

void refresh_road_placed() {
    if (!road_dispatch_active()) {
        g_road_placed = false;
    }
}

void mark_road_kind(unsigned kind) {
    if (kind == 0) {
        g_road_placed = true;
    }
}

int civilian_model() {
    if (g_host == nullptr) {
        return -1;
    }
    auto *group = *reinterpret_cast<uint8_t **>(g_host->base + kCivilianCars);
    if (group == nullptr) {
        return -1;
    }
    using PickFn = int (*)(uint8_t *, unsigned, unsigned);
    return reinterpret_cast<PickFn>(g_host->base + kPickRandomCar)(group, 1, 0);
}

void note_extra(uint8_t *veh) {
    const int chosen = g_pending_model;
    g_pending_model = -1;
    if (chosen < 0 || !vehicle_ok(veh) || g_extra_count >= 8) {
        return;
    }
    const int actual = *reinterpret_cast<int16_t *>(veh + kModelIndex);
    g_extra[g_extra_count] = veh;
    g_extra_model[g_extra_count] = actual == 430 ? 430 : chosen;
    ++g_extra_count;
    arm_extra_gap();
}

uint8_t *build_extra_page() {
    uint8_t *page = map_near(g_host->base + kChooseModelSite);
    if (page == nullptr) {
        return nullptr;
    }
    const uint32_t decide[] = {
        0xa9bf7bfdu, 0x580001f0u, 0xd63f0200u, 0x7100001fu, 0x540000ebu, 0x2a0003f4u, 0x528001bbu, 0xb81a03bbu,
        0xa8c17bfdu, 0x58000130u, 0xd61f0200u, 0xa8c17bfdu, 0xd10183a0u, 0x580000f0u, 0xd61f0200u, 0xd503201fu,
    };
    const uint32_t note[] = {
        0xa9be7bfdu, 0x58000170u, 0xd63f0200u, 0xf9000be0u, 0xb4000080u, 0x58000130u, 0xd63f0200u, 0xf9400be0u,
        0xa8c27bfdu, 0x580000f0u, 0xd61f0200u, 0xd503201fu,
    };
    const uint32_t fix[] = {
        0xa9bf7bfdu, 0x580000f0u, 0xd63f0200u, 0x580000f0u, 0xd63f0200u, 0xa8c17bfdu, 0x580000d0u, 0xd61f0200u,
    };
    const uintptr_t decide_fn = reinterpret_cast<uintptr_t>(extra_model);
    const uintptr_t police = g_host->base + kPoliceModelJoin;
    const uintptr_t next = g_host->base + kChooseModelNext;
    const uintptr_t created = g_host->base + kGetNewVehicle;
    const uintptr_t extra_fn = reinterpret_cast<uintptr_t>(note_extra);
    const uintptr_t extra_back = g_host->base + kRandCarSpawnNext;
    const uintptr_t natural_fn = reinterpret_cast<uintptr_t>(note_natural);
    const uintptr_t natural_back = g_host->base + kChaseSpawnNext;
    const uintptr_t choose = g_host->base + kChoosePoliceModel;
    const uintptr_t fix_fn = reinterpret_cast<uintptr_t>(regularize_chase_model);
    const uintptr_t fix_back = g_host->base + kChaseModelNext;
    const uint32_t water[] = {
        0x363801d3u, 0x7100377fu, 0x540001c0u, 0xa9bf7bfdu, 0x580001d0u, 0xd63f0200u, 0x340000a0u, 0xa8c17bfdu,
        0x528001bbu, 0x58000170u, 0xd61f0200u, 0xa8c17bfdu, 0x58000150u, 0xd61f0200u, 0x58000150u, 0xd61f0200u,
        0x58000150u, 0xd61f0200u,
    };
    const uintptr_t water_fn = reinterpret_cast<uintptr_t>(water_roll);
    const uintptr_t water_cop = g_host->base + kWaterCop;
    const uintptr_t water_civ = g_host->base + kWaterCiv;
    const uintptr_t water_land = g_host->base + kWaterLand;
    std::memcpy(page, decide, sizeof(decide));
    std::memcpy(page + 0x40, &decide_fn, sizeof(decide_fn));
    std::memcpy(page + 0x48, &police, sizeof(police));
    std::memcpy(page + 0x50, &next, sizeof(next));
    std::memcpy(page + 0x60, note, sizeof(note));
    std::memcpy(page + 0x90, &created, sizeof(created));
    std::memcpy(page + 0x98, &extra_fn, sizeof(extra_fn));
    std::memcpy(page + 0xa0, &extra_back, sizeof(extra_back));
    std::memcpy(page + 0xb0, note, sizeof(note));
    std::memcpy(page + 0xe0, &created, sizeof(created));
    std::memcpy(page + 0xe8, &natural_fn, sizeof(natural_fn));
    std::memcpy(page + 0xf0, &natural_back, sizeof(natural_back));
    std::memcpy(page + 0x100, fix, sizeof(fix));
    std::memcpy(page + 0x120, &choose, sizeof(choose));
    std::memcpy(page + 0x128, &fix_fn, sizeof(fix_fn));
    std::memcpy(page + 0x130, &fix_back, sizeof(fix_back));
    std::memcpy(page + 0x140, water, sizeof(water));
    std::memcpy(page + 0x188, &water_fn, sizeof(water_fn));
    std::memcpy(page + 0x190, &water_cop, sizeof(water_cop));
    std::memcpy(page + 0x198, &water_civ, sizeof(water_civ));
    std::memcpy(page + 0x1a0, &water_land, sizeof(water_land));
    std::memcpy(page + 0x1a8, &water_cop, sizeof(water_cop));
    const uint32_t plain[] = {
        0x5400014du, 0xa9bf7bfdu, 0x58000190u, 0xd63f0200u, 0xa8c17bfdu, 0x7100001fu, 0x540000cdu, 0x2a0003f4u,
        0x2a1f03fbu, 0xb81a03bbu, 0x580000d0u, 0xd61f0200u, 0x580000d0u, 0xd61f0200u,
    };
    const uintptr_t plain_fn = reinterpret_cast<uintptr_t>(civilian_model);
    const uintptr_t plain_keep = g_host->base + kCopClassKeep;
    const uintptr_t plain_drop = g_host->base + kCopClassDrop;
    std::memcpy(page + 0x1c0, plain, sizeof(plain));
    std::memcpy(page + 0x1f8, &plain_fn, sizeof(plain_fn));
    std::memcpy(page + 0x200, &plain_keep, sizeof(plain_keep));
    std::memcpy(page + 0x208, &plain_drop, sizeof(plain_drop));
    __builtin___clear_cache(reinterpret_cast<char *>(page), reinterpret_cast<char *>(page) + 0x210);
    return page;
}

bool restore_extra_word(bool *done, uintptr_t address, uint32_t vanilla) {
    if (!*done) {
        return true;
    }
    if (!write_word(address, vanilla)) {
        return false;
    }
    *done = false;
    return true;
}

bool set_extra(bool enable) {
    if (g_host == nullptr) {
        return false;
    }
    const uintptr_t choose = g_host->base + kChooseModelSite;
    const uintptr_t rand_spawn = g_host->base + kRandCarSpawn;
    const uintptr_t chase_model = g_host->base + kChaseModel;
    const uintptr_t chase_spawn = g_host->base + kChaseSpawn;
    const uintptr_t water = g_host->base + kWaterTest;
    const uintptr_t abort_at = g_host->base + kCopClassAbort;
    if (enable) {
        if (g_extra_page == nullptr) {
            if (*reinterpret_cast<uint32_t *>(choose) != kChooseModelSub ||
                *reinterpret_cast<uint32_t *>(rand_spawn) != kRandCarSpawnBl ||
                *reinterpret_cast<uint32_t *>(chase_model) != kChaseModelBl ||
                *reinterpret_cast<uint32_t *>(chase_spawn) != kChaseSpawnBl ||
                *reinterpret_cast<uint32_t *>(water) != kWaterTbnz ||
                *reinterpret_cast<uint32_t *>(abort_at) != kCopClassAbortGt) {
                return false;
            }
            g_extra_page = build_extra_page();
            if (g_extra_page == nullptr) {
                return false;
            }
        }
        if (!write_branch(choose, g_extra_page)) {
            return false;
        }
        g_choose_patched = true;
        if (!write_branch(rand_spawn, g_extra_page + 0x60)) {
            restore_extra_word(&g_choose_patched, choose, kChooseModelSub);
            return false;
        }
        g_rand_spawn_patched = true;
        if (!write_branch(chase_spawn, g_extra_page + 0xb0)) {
            restore_extra_word(&g_rand_spawn_patched, rand_spawn, kRandCarSpawnBl);
            restore_extra_word(&g_choose_patched, choose, kChooseModelSub);
            return false;
        }
        g_chase_spawn_patched = true;
        if (!write_branch(chase_model, g_extra_page + 0x100)) {
            restore_extra_word(&g_chase_spawn_patched, chase_spawn, kChaseSpawnBl);
            restore_extra_word(&g_rand_spawn_patched, rand_spawn, kRandCarSpawnBl);
            restore_extra_word(&g_choose_patched, choose, kChooseModelSub);
            return false;
        }
        g_chase_model_patched = true;
        if (!write_branch(water, g_extra_page + 0x140)) {
            restore_extra_word(&g_chase_model_patched, chase_model, kChaseModelBl);
            restore_extra_word(&g_chase_spawn_patched, chase_spawn, kChaseSpawnBl);
            restore_extra_word(&g_rand_spawn_patched, rand_spawn, kRandCarSpawnBl);
            restore_extra_word(&g_choose_patched, choose, kChooseModelSub);
            return false;
        }
        g_water_patched = true;
        if (!write_branch(abort_at, g_extra_page + 0x1c0)) {
            restore_extra_word(&g_water_patched, water, kWaterTbnz);
            restore_extra_word(&g_chase_model_patched, chase_model, kChaseModelBl);
            restore_extra_word(&g_chase_spawn_patched, chase_spawn, kChaseSpawnBl);
            restore_extra_word(&g_rand_spawn_patched, rand_spawn, kRandCarSpawnBl);
            restore_extra_word(&g_choose_patched, choose, kChooseModelSub);
            return false;
        }
        g_abort_patched = true;
        return true;
    }
    const bool choose_ok = restore_extra_word(&g_choose_patched, choose, kChooseModelSub);
    const bool rand_ok = restore_extra_word(&g_rand_spawn_patched, rand_spawn, kRandCarSpawnBl);
    const bool spawn_ok = restore_extra_word(&g_chase_spawn_patched, chase_spawn, kChaseSpawnBl);
    const bool model_ok = restore_extra_word(&g_chase_model_patched, chase_model, kChaseModelBl);
    const bool water_ok = restore_extra_word(&g_water_patched, water, kWaterTbnz);
    const bool abort_ok = restore_extra_word(&g_abort_patched, abort_at, kCopClassAbortGt);
    return choose_ok && rand_ok && spawn_ok && model_ok && water_ok && abort_ok;
}

uint8_t *build_road_trigger() {
    uint8_t *page = map_near(g_host->base + kRoadEntry);
    if (page == nullptr) {
        return nullptr;
    }
    const uint32_t entry[] = {
        0xd102c3ffu, 0xa9007bfdu, 0xa90107e0u, 0xad0107e0u, 0xad020fe2u, 0xad0317e4u, 0x58000190u, 0xd63f0200u,
        0xad4317e4u, 0xad420fe2u, 0xad4107e0u, 0xa94107e0u, 0xa9407bfdu, 0x9102c3ffu, 0x6db63befu, 0x580000b0u,
        0xd61f0200u, 0xd503201fu,
    };
    const uintptr_t mark = reinterpret_cast<uintptr_t>(mark_road_kind);
    const uintptr_t cont = g_host->base + kRoadEntryNext;
    std::memcpy(page, entry, sizeof(entry));
    std::memcpy(page + 0x48, &mark, sizeof(mark));
    std::memcpy(page + 0x50, &cont, sizeof(cont));
    __builtin___clear_cache(reinterpret_cast<char *>(page), reinterpret_cast<char *>(page) + 0x58);
    return page;
}

bool set_road_trigger(bool enable) {
    if (g_host == nullptr) {
        return false;
    }
    const uintptr_t entry = g_host->base + kRoadEntry;
    if (enable) {
        if (g_road_page == nullptr) {
            if (*reinterpret_cast<uint32_t *>(entry) != kRoadEntryStp) {
                return false;
            }
            g_road_page = build_road_trigger();
            if (g_road_page == nullptr) {
                return false;
            }
        }
        if (!write_branch(entry, g_road_page)) {
            return false;
        }
        g_road_entry_patched = true;
        return true;
    }
    return restore_extra_word(&g_road_entry_patched, entry, kRoadEntryStp);
}

uint8_t *build_crime_page() {
    uint8_t *page = map_near(g_host->base + kReportCrime);
    if (page == nullptr) {
        return nullptr;
    }
    const uint32_t code[] = {
        0xa9bf7bfdu, 0xa9bf07e0u, 0xa9bf0fe2u, 0xaa0203e1u, 0x58000110u, 0xd63f0200u, 0xa8c10fe2u, 0xa8c107e0u,
        0xa8c17bfdu, 0xd101c3ffu, 0x58000090u, 0xd61f0200u,
    };
    const uintptr_t fn = reinterpret_cast<uintptr_t>(on_report_crime);
    const uintptr_t next = g_host->base + kReportCrimeNext;
    std::memcpy(page, code, sizeof(code));
    std::memcpy(page + 0x30, &fn, sizeof(fn));
    std::memcpy(page + 0x38, &next, sizeof(next));
    __builtin___clear_cache(reinterpret_cast<char *>(page), reinterpret_cast<char *>(page) + 0x40);
    return page;
}

bool set_crime(bool enable) {
    if (g_host == nullptr) {
        return false;
    }
    const uintptr_t site = g_host->base + kReportCrime;
    if (enable) {
        if (g_crime_page == nullptr) {
            if (*reinterpret_cast<uint32_t *>(site) != kReportCrimeSub) {
                return false;
            }
            g_crime_page = build_crime_page();
            if (g_crime_page == nullptr) {
                return false;
            }
        }
        if (!write_branch(site, g_crime_page)) {
            return false;
        }
        g_crime_patched = true;
        return true;
    }
    return restore_extra_word(&g_crime_patched, site, kReportCrimeSub);
}

bool set_spawn(bool enable) {
    const bool slot = exchange_word(enable, kSlotHandoff, kSlotHandoffMov, kSlotHandoffBranch, &g_slot_patched);
    const bool chase = exchange_word(enable, kChaseStars, kChaseStarsCbnz, kNop, &g_chase_patched);
    const bool far = exchange_word(enable, kRefillFar, kRefillFarGe, kRefillFarNormal, &g_refill_far_patched);
    const bool near = exchange_word(enable, kRefillNear, kRefillNearLs, kRefillNearNormal, &g_refill_near_patched);
    const bool full = exchange_word(enable, kRefillFull, kRefillFullGe, kNop, &g_refill_full_patched);
    const bool ped = exchange_word(enable, kCopPedFlag, kCopPedOrr, kCopPedClear, &g_cop_ped_patched);
    const bool picker = exchange_word(enable, kCopPicker, kCopPickerCbz, kCopPickerSkip, &g_cop_picker_patched);
    return slot && chase && far && near && full && ped && picker;
}

uint8_t *build_pursuit_page() {
    uint8_t *page = map_near(g_host->base + kPursueTail);
    if (page == nullptr) {
        return nullptr;
    }
    const uint32_t pursue[] = {
        0xa9be07e0u, 0xf9000bfeu, 0xaa0103e0u, 0x58000170u, 0xd63f0200u, 0xf9400bfeu, 0xb5000080u, 0xa8c207e0u,
        0x58000110u, 0xd61f0200u, 0x910083ffu, 0x2a1f03e0u, 0xd65f03c0u, 0xd503201fu,
    };
    const uint32_t control[] = {
        0xa9bf07e0u, 0x58000170u, 0xd63f0200u, 0x350000c0u, 0xa8c107e0u, 0x58000130u, 0xd63f0200u, 0x58000130u,
        0xd61f0200u, 0x910043ffu, 0x58000110u, 0xd61f0200u,
    };
    const uintptr_t nearer = reinterpret_cast<uintptr_t>(nearer_than_player);
    const uintptr_t join = g_host->base + kCanJoinPursuit;
    const uintptr_t leave = reinterpret_cast<uintptr_t>(leave_pursuit);
    const uintptr_t persist = g_host->base + kPersistPursuit;
    const uintptr_t back = g_host->base + kPersistBack;
    const uintptr_t abort_at = g_host->base + kPersistAbort;
    std::memcpy(page, pursue, sizeof(pursue));
    std::memcpy(page + 0x38, &nearer, sizeof(nearer));
    std::memcpy(page + 0x40, &join, sizeof(join));
    std::memcpy(page + 0x50, control, sizeof(control));
    std::memcpy(page + 0x80, &leave, sizeof(leave));
    std::memcpy(page + 0x88, &persist, sizeof(persist));
    std::memcpy(page + 0x90, &back, sizeof(back));
    std::memcpy(page + 0x98, &abort_at, sizeof(abort_at));
    __builtin___clear_cache(reinterpret_cast<char *>(page), reinterpret_cast<char *>(page) + 0xa0);
    return page;
}

bool set_pursuit(bool enable) {
    if (g_host == nullptr) {
        return false;
    }
    const uintptr_t pursue = g_host->base + kPursueTail;
    const uintptr_t control = g_host->base + kPersistCall;
    if (enable) {
        if (g_pursuit_page == nullptr) {
            if (*reinterpret_cast<uint32_t *>(pursue) != kPursueTailBranch ||
                *reinterpret_cast<uint32_t *>(control) != kPersistCallBl) {
                return false;
            }
            g_pursuit_page = build_pursuit_page();
            if (g_pursuit_page == nullptr) {
                return false;
            }
        }
        if (!write_branch(pursue, g_pursuit_page)) {
            return false;
        }
        g_pursue_patched = true;
        if (!write_branch(control, g_pursuit_page + 0x50)) {
            write_word(pursue, kPursueTailBranch);
            g_pursue_patched = false;
            return false;
        }
        g_control_patched = true;
        return true;
    }
    bool ok = true;
    if (g_pursue_patched) {
        if (!write_word(pursue, kPursueTailBranch)) {
            ok = false;
        } else {
            g_pursue_patched = false;
        }
    }
    if (g_control_patched) {
        if (!write_word(control, kPersistCallBl)) {
            ok = false;
        } else {
            g_control_patched = false;
        }
    }
    return ok;
}

int kill_task(uint8_t *responder, uint8_t *other, void *) {
    if (responder == nullptr || other == nullptr) {
        return 0;
    }
    if (responder[0x5f8] == 2 || weapon_flag(responder) != 0 || weapon_flag(other) == 0) {
        return 1;
    }
    if (g_active && *reinterpret_cast<int *>(responder + kPedType) == kCopType) {
        return 1;
    }
    return 0;
}

}  // namespace

// versionCode of GTASA DE 1.87.0. Offsets inside functions match that build only.
extern "C" __attribute__((visibility("default"))) const int64_t gtasa_plugin_game_abi[] = {1, 54538169};

extern "C" __attribute__((visibility("default"))) void gtasa_plugin_enter(const GtasaHost *host) {
    g_host = host;
    g_active = true;
    if (!set_react(true)) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react patch failed");
    } else {
        __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react gunshot AffectsPed");
    }
    if (!set_car(true)) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react car patch failed");
    } else {
        __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react in-car AffectsPed");
    }
    if (!set_branch_patch(true, kSightGate, kSightFirst, &g_sight_saved, &g_sight_stub, &g_sight_patched,
                          build_sight_stub, kSightAccept, kSightContinue)) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react sight patch failed");
    } else {
        __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react in-car line of sight");
    }
    if (!set_branch_patch(true, kFootGate, kFootFirst, &g_foot_saved, &g_foot_stub, &g_foot_patched, build_foot_stub,
                          kFootOnFoot, kPassengerSit)) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react foot patch failed");
    } else {
        __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react passenger sits");
    }
    if (!set_far(true)) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react far patch failed");
    } else {
        __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react on-foot uses game drive");
    }
    if (!set_aim(true)) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react accuracy patch failed");
    } else {
        __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react npc accuracy");
    }
    if (!set_ignore(true)) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react ignore patch failed");
    } else {
        __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react in-car ignore");
    }
    if (!set_dispatch(true)) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react dispatch patch failed");
    } else {
        __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react nearest case");
    }
    if (!set_spawn(true) || !set_extra(true) || !set_road_trigger(true) || !set_crime(true)) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react spawn patch failed");
    } else {
        __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react civilian slots, chase while wanted, no foot refill");
    }
    if (!set_pursuit(true)) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react pursuit patch failed");
    } else {
        __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react pursuit choice");
    }
    if (host != nullptr && host->hook != nullptr && g_vehicle_orig == nullptr) {
        auto *target = reinterpret_cast<void *>(host->base + kVehicleDamage);
        if (host->hook(target, reinterpret_cast<void *>(on_vehicle_damage),
                       reinterpret_cast<void **>(&g_vehicle_orig)) != 0) {
            __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react vehicle hook failed");
        }
    }
    if (host != nullptr && host->hook != nullptr && g_wanted_update_orig == nullptr) {
        auto *target = reinterpret_cast<void *>(host->base + kWantedUpdate);
        if (host->hook(target, reinterpret_cast<void *>(on_wanted_update),
                       reinterpret_cast<void **>(&g_wanted_update_orig)) != 0) {
            __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react clue hook failed");
        }
    }
    if (host != nullptr && host->hook != nullptr && g_damage_orig == nullptr) {
        auto *target = reinterpret_cast<void *>(host->base + kGenerateDamage);
        if (host->hook(target, reinterpret_cast<void *>(on_damage), reinterpret_cast<void **>(&g_damage_orig)) != 0) {
            __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react cop-kill hook failed");
        }
    }
    if (host != nullptr && host->hook != nullptr && g_kill_car_orig == nullptr) {
        auto *target = reinterpret_cast<void *>(host->base + kKillPedWithCar);
        if (host->hook(target, reinterpret_cast<void *>(on_kill_car), reinterpret_cast<void **>(&g_kill_car_orig)) !=
            0) {
            __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react cop-kill car hook failed");
        }
    }
    if (host != nullptr && host->hook != nullptr && g_wanted_orig == nullptr) {
        auto *target = reinterpret_cast<void *>(host->base + kWantedResponse);
        if (host->hook(target, reinterpret_cast<void *>(on_wanted_response),
                       reinterpret_cast<void **>(&g_wanted_orig)) != 0) {
            __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react wanted response hook failed");
        } else {
            __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react keep nearer fight");
        }
    }
    if (host != nullptr && host->hook != nullptr && g_personality_orig == nullptr) {
        auto *target = reinterpret_cast<void *>(host->base + kPersonality);
        if (host->hook(target, reinterpret_cast<void *>(on_personality),
                       reinterpret_cast<void **>(&g_personality_orig)) != 0) {
            __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react personality hook failed");
        } else {
            __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react unarmed no flee");
        }
    }
    if (!set_unarmed(true)) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react unarmed patch failed");
    }
    if (host == nullptr || g_kill_patched) {
        return;
    }
    const uintptr_t address = host->base + kKillTask;
    if (*reinterpret_cast<uint32_t *>(address) != kKillTaskFirst ||
        !write_jump(address, reinterpret_cast<void *>(kill_task))) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react kill-task patch failed");
        return;
    }
    g_kill_patched = true;
    __android_log_print(ANDROID_LOG_INFO, kTag, "cop-react unarmed IsKillTaskAppropriate");
}

extern "C" __attribute__((visibility("default"))) void gtasa_plugin_pause() {
    g_active = false;
    set_react(false);
    set_car(false);
    set_branch_patch(false, kSightGate, kSightFirst, &g_sight_saved, &g_sight_stub, &g_sight_patched, build_sight_stub,
                     kSightAccept, kSightContinue);
    set_branch_patch(false, kFootGate, kFootFirst, &g_foot_saved, &g_foot_stub, &g_foot_patched, build_foot_stub,
                     kFootOnFoot, kPassengerSit);
    set_far(false);
    set_aim(false);
    set_ignore(false);
    set_dispatch(false);
    set_spawn(false);
    set_extra(false);
    set_road_trigger(false);
    set_crime(false);
    set_pursuit(false);
    set_unarmed(false);
}

extern "C" __attribute__((visibility("default"))) void gtasa_plugin_resume() {
    g_active = true;
    set_react(true);
    set_car(true);
    set_branch_patch(true, kSightGate, kSightFirst, &g_sight_saved, &g_sight_stub, &g_sight_patched, build_sight_stub,
                     kSightAccept, kSightContinue);
    set_branch_patch(true, kFootGate, kFootFirst, &g_foot_saved, &g_foot_stub, &g_foot_patched, build_foot_stub,
                     kFootOnFoot, kPassengerSit);
    set_far(true);
    set_aim(true);
    set_ignore(true);
    set_dispatch(true);
    set_spawn(true);
    set_extra(true);
    set_road_trigger(true);
    set_crime(true);
    set_pursuit(true);
    set_unarmed(true);
}
