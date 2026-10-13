# GTASA DE Zygisk

Zygisk module for Grand Theft Auto: San Andreas — Definitive Edition (`com.rockstargames.gtasa.de`, arm64, 1.87.0).

[中文](README.zh.md)

## Target

The module remains loaded only for `com.rockstargames.gtasa.de`. It waits until `libUE4.so` is mapped and passes that library's load base to each plugin. Hook sites are ELF virtual addresses for the arm64 build whose versionCode is `54538169` (1.87.0). A plugin that publishes `gtasa_plugin_game_abi` is not loaded on any other versionCode.

## Runtime

When the game process starts, the module reads every `plugins/*.so` from its own directory and writes those files into the game's `code_cache`. Once `libUE4.so` is present, each plugin is opened and `gtasa_plugin_enter` receives the load base and the hook function.

A plugin pauses while an interior transition, a cutscene, or a respawn hold is set, and resumes when the game is playing and every hold is clear. Pause restores patched game instructions. Resume installs them again.

The NPatch module loads the same plugins inside the game process. They ship in the module's native-library directory and are written to the game's `code_cache` after startup.

## Plugin contract

```c
struct GtasaHost {
    uint32_t size;
    uintptr_t base; // load base of libUE4.so
    int (*hook)(void *target, void *detour, void **original);
};

extern const int64_t gtasa_plugin_game_abi[]; // [0] = count, then versionCodes
void gtasa_plugin_enter(const GtasaHost *host);
void gtasa_plugin_pause(void);
void gtasa_plugin_resume(void);
```

`hook` installs a detour and stores the original. Inline patches belong to the plugin and are restored in `pause`.

## gtasa_cop_react

Police respond when an NPC fires, and wanted-level police behavior is changed.

### NPC gunfire

When an NPC fires, police chase that shooter if they are closer than the player, and chase the player otherwise.

### Wanted level

From two stars, a slot the game was going to fill with a police car is kept by probability, up to a limit. Police boats use that same chance on water. A crime report is recorded only while police are already chasing, or a wanted roadblock or a police helicopter is present. Only the heaviest crime in that period is kept. After all three are gone, later reports do not change it. A later police slot is then kept only if it lies inside the circle around the player's last position while any of the three was still present, using that crime. The radius is the distance at which the game removes an ordinary car. A slot outside the circle is not kept. There is no circle until one of the three has been present. The record and the circle clear when the wanted level ends, and a new period starts when any of the three returns. While any of the three is present, the chance follows the wanted level and the wanted points instead, and is higher. If a crime report is also waiting, that chance is raised further by the wanted level and the wanted points. The circle is not used then.

From four stars, special vehicles are chosen from that crime by probability, limited to one. From five stars, the number of special vehicles is not limited.

## gtasa_fixes

Fixes ambulance spawning. An ambulance's removal distance was 0, so one created farther away was deleted immediately. It is now set to the fire truck's removal distance, 170 × scale × 1.5, and the ambulance stays after it is created.

## gtasa_probe

Writes a log line when the loader starts a plugin, pauses it, and resumes it.
