# GTASA DE Zygisk

适用于《侠盗猎车手：圣安地列斯 最终版》的 Zygisk 模块（`com.rockstargames.gtasa.de`，arm64，1.87.0）。

[English](README.md)

## 目标

模块只留在 `com.rockstargames.gtasa.de` 中。它等到 `libUE4.so` 映射完成，再把该库的加载基址交给每个插件。挂钩位置是 arm64、versionCode `54538169`（1.87.0）的 ELF 虚拟地址。插件若发布 `gtasa_plugin_game_abi`，其他 versionCode 不会加载该插件。

## 运行

游戏进程启动时，模块读取自身目录下的全部 `plugins/*.so`，并把这些文件写到游戏的 `code_cache`。`libUE4.so` 出现后，逐个打开插件，并由 `gtasa_plugin_enter` 收到加载基址和挂钩函数。

室内切换、过场或重生占住暂停位时，插件暂停；游戏处于可玩状态且暂停位全部清空后恢复。暂停会还原被改写的游戏指令，恢复时再次装上。

NPatch 模块在游戏进程里加载同一批插件。插件放在模块的原生库目录，启动后同样写到游戏的 `code_cache`。

## 插件约定

```c
struct GtasaHost {
    uint32_t size;
    uintptr_t base; // libUE4.so 的加载基址
    int (*hook)(void *target, void *detour, void **original);
};

extern const int64_t gtasa_plugin_game_abi[]; // [0] 为个数，其后为 versionCode
void gtasa_plugin_enter(const GtasaHost *host);
void gtasa_plugin_pause(void);
void gtasa_plugin_resume(void);
```

`hook` 安装跳转并保存原函数。指令级补丁由插件自己负责，并在 `pause` 中还原。

## gtasa_cop_react

警察会响应 NPC 开枪，通缉期间的警察行为也有改动。

### NPC 开枪

NPC 开枪时，开枪的人比玩家近，警察就追开枪的人；否则追玩家。

### 通缉

二星起，游戏本来要放成警车的位置按概率留下，并有上限。水面上的警用船用同一次概率。犯罪报告只在已经有警察在追，或者通缉路障、警用直升机还在时记下，只留这一段里最重的一次。这三样都没了以后，后来的报告不改它。这时后来的警车位置只有落在圆里才按这个罪行留下。圆心是玩家在这三样里至少还有一个时的最后位置，之后不再跟着人走。半径是游戏卸掉普通车的距离。圆外这次不留。这三样从来没出现过，就没有圆心，这次也不留。星级回到零会清掉圆心和这次罪行。三样里又有一个出现，才重新记。已经有警察在追，或者路障、直升机还在时，概率改按星级和通缉点数，并且更高。这时如果还有犯罪报告，星级和通缉点数会再提高这次概率，不用这个圆。

四星起按这次罪行的概率生成特殊车，并仅限一辆。五星起不限制特殊车数量。

## gtasa_fixes

修复救护车生成。救护车的移除距离原来是 0，较远处刚生成的会被马上删掉。现在改成消防车使用的移除距离，即 170×比例×1.5，生成后就能留下。

## gtasa_probe

在日志里记下加载器启动插件、暂停插件和恢复插件。
