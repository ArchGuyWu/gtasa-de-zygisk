#include "plugins.h"

#include "host.h"

#include <android/dlext.h>
#include <android/log.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr const char *kTag = "GTASADE";
constexpr int kMaxPlugins = 8;
constexpr size_t kMaxBytes = 8u * 1024u * 1024u;
constexpr int kPollMs = 500;
constexpr int kHeartbeatMs = 15000;
constexpr uint32_t kHoldInterior = 1u;
constexpr uint32_t kHoldCutscene = 2u;
constexpr uint32_t kHoldRespawn = 4u;

struct LoadedPlugin {
    char name[64];
    uint8_t *bytes;
    size_t size;
    char path[512];
    void *handle;
    void (*enter)(const GtasaHost *);
    void (*pause)();
    void (*resume)();
};

LoadedPlugin g_plugins[kMaxPlugins];
int g_count = 0;
char g_data_dir[512];
pthread_mutex_t g_state_lock = PTHREAD_MUTEX_INITIALIZER;
uintptr_t g_base = 0;
void *g_vm = nullptr;
bool g_game_abi_known = false;
int64_t g_game_abi = 0;
bool g_loaded = false;
bool g_active = false;
uint32_t g_holds = 0;
bool g_hooks_installed = false;

using VoidFn = void (*)();
VoidFn g_transition_started = nullptr;
VoidFn g_transition_finished = nullptr;
VoidFn g_start_cutscene = nullptr;
VoidFn g_delete_cutscene = nullptr;
VoidFn g_kill_player = nullptr;
VoidFn g_arrest_player = nullptr;
VoidFn g_respawn_hospital = nullptr;
VoidFn g_respawn_police = nullptr;
VoidFn g_shutdown_restart = nullptr;

extern "C" void gtasa_hook_on_return(void *target, void (*after)());

void log_info(const char *message) {
    __android_log_print(ANDROID_LOG_INFO, kTag, "%s", message);
}

bool ends_with_so(const char *name) {
    const size_t len = std::strlen(name);
    return len > 3 && std::strcmp(name + len - 3, ".so") == 0;
}

void capture_one(int dirfd, const char *name) {
    if (g_count >= kMaxPlugins || !ends_with_so(name) || std::strlen(name) >= sizeof(g_plugins[0].name)) {
        return;
    }
    const int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return;
    }
    struct stat st {};
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || static_cast<size_t>(st.st_size) > kMaxBytes) {
        close(fd);
        return;
    }
    auto *bytes = static_cast<uint8_t *>(std::malloc(static_cast<size_t>(st.st_size)));
    if (bytes == nullptr) {
        close(fd);
        return;
    }
    size_t got = 0;
    while (got < static_cast<size_t>(st.st_size)) {
        const ssize_t n = read(fd, bytes + got, static_cast<size_t>(st.st_size) - got);
        if (n <= 0) {
            break;
        }
        got += static_cast<size_t>(n);
    }
    close(fd);
    if (got != static_cast<size_t>(st.st_size)) {
        std::free(bytes);
        return;
    }
    LoadedPlugin &plugin = g_plugins[g_count];
    std::memset(&plugin, 0, sizeof(plugin));
    std::snprintf(plugin.name, sizeof(plugin.name), "%s", name);
    plugin.bytes = bytes;
    plugin.size = got;
    ++g_count;
}

void clear_staged_dir(const char *dir) {
    DIR *handle = opendir(dir);
    if (handle == nullptr) {
        return;
    }
    const int dirfd_value = dirfd(handle);
    while (dirent *entry = readdir(handle)) {
        if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        unlinkat(dirfd_value, entry->d_name, 0);
    }
    closedir(handle);
}

void *open_plugin(const char *path) {
    void *handle = dlopen(path, RTLD_NOW);
    if (handle != nullptr) {
        return handle;
    }
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return nullptr;
    }
    android_dlextinfo info {};
    info.flags = ANDROID_DLEXT_USE_LIBRARY_FD;
    info.library_fd = fd;
    handle = android_dlopen_ext(path, RTLD_NOW, &info);
    close(fd);
    return handle;
}

using PlayingFn = uint32_t (*)();
using FindPedFn = void *(*)(int);

PlayingFn g_is_playing = nullptr;
FindPedFn g_find_player_ped = nullptr;

bool is_playing() {
    return g_is_playing != nullptr && g_is_playing() != 0;
}

bool player_ped_present() {
    return g_find_player_ped != nullptr && g_find_player_ped(0) != nullptr;
}

bool jni_failed(JNIEnv *env) {
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return true;
    }
    return false;
}

bool query_package(JNIEnv *env, int64_t *code, char *version, size_t version_cap) {
    jclass thread_class = env->FindClass("android/app/ActivityThread");
    if (thread_class == nullptr || jni_failed(env)) {
        return false;
    }
    jmethodID current_app =
        env->GetStaticMethodID(thread_class, "currentApplication", "()Landroid/app/Application;");
    if (current_app == nullptr || jni_failed(env)) {
        env->DeleteLocalRef(thread_class);
        return false;
    }
    jobject app = nullptr;
    for (int attempt = 0; attempt < 10; ++attempt) {
        app = env->CallStaticObjectMethod(thread_class, current_app);
        if (jni_failed(env)) {
            app = nullptr;
        }
        if (app != nullptr) {
            break;
        }
        usleep(200000);
    }
    env->DeleteLocalRef(thread_class);
    if (app == nullptr) {
        return false;
    }
    jclass app_class = env->GetObjectClass(app);
    if (app_class == nullptr || jni_failed(env)) {
        env->DeleteLocalRef(app);
        return false;
    }
    jmethodID get_pm =
        env->GetMethodID(app_class, "getPackageManager", "()Landroid/content/pm/PackageManager;");
    if (get_pm == nullptr || jni_failed(env)) {
        env->DeleteLocalRef(app);
        env->DeleteLocalRef(app_class);
        return false;
    }
    jmethodID get_pkg = env->GetMethodID(app_class, "getPackageName", "()Ljava/lang/String;");
    if (get_pkg == nullptr || jni_failed(env)) {
        env->DeleteLocalRef(app);
        env->DeleteLocalRef(app_class);
        return false;
    }
    jobject pm = env->CallObjectMethod(app, get_pm);
    if (pm == nullptr || jni_failed(env)) {
        env->DeleteLocalRef(app_class);
        env->DeleteLocalRef(app);
        if (pm != nullptr) {
            env->DeleteLocalRef(pm);
        }
        return false;
    }
    jstring pkg = static_cast<jstring>(env->CallObjectMethod(app, get_pkg));
    env->DeleteLocalRef(app_class);
    env->DeleteLocalRef(app);
    if (pm == nullptr || pkg == nullptr || jni_failed(env)) {
        if (pm != nullptr) {
            env->DeleteLocalRef(pm);
        }
        if (pkg != nullptr) {
            env->DeleteLocalRef(pkg);
        }
        return false;
    }
    jclass pm_class = env->GetObjectClass(pm);
    if (pm_class == nullptr || jni_failed(env)) {
        env->DeleteLocalRef(pm);
        env->DeleteLocalRef(pkg);
        return false;
    }
    jmethodID get_info =
        env->GetMethodID(pm_class, "getPackageInfo", "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;");
    if (get_info == nullptr || jni_failed(env)) {
        env->DeleteLocalRef(pm);
        env->DeleteLocalRef(pkg);
        if (pm_class != nullptr) {
            env->DeleteLocalRef(pm_class);
        }
        return false;
    }
    jobject info = env->CallObjectMethod(pm, get_info, pkg, 0);
    env->DeleteLocalRef(pm_class);
    env->DeleteLocalRef(pm);
    env->DeleteLocalRef(pkg);
    if (info == nullptr || jni_failed(env)) {
        if (info != nullptr) {
            env->DeleteLocalRef(info);
        }
        return false;
    }
    jclass info_class = env->GetObjectClass(info);
    if (info_class == nullptr || jni_failed(env)) {
        env->DeleteLocalRef(info);
        return false;
    }
    jmethodID long_code = env->GetMethodID(info_class, "getLongVersionCode", "()J");
    if (long_code == nullptr || jni_failed(env)) {
        env->DeleteLocalRef(info);
        if (info_class != nullptr) {
            env->DeleteLocalRef(info_class);
        }
        return false;
    }
    const jlong value = env->CallLongMethod(info, long_code);
    if (jni_failed(env) || value <= 0) {
        env->DeleteLocalRef(info_class);
        env->DeleteLocalRef(info);
        return false;
    }
    jfieldID name_field = env->GetFieldID(info_class, "versionName", "Ljava/lang/String;");
    if (jni_failed(env)) {
        name_field = nullptr;
    }
    if (name_field != nullptr && version_cap > 1) {
        auto name = static_cast<jstring>(env->GetObjectField(info, name_field));
        if (!jni_failed(env) && name != nullptr) {
            const char *utf = env->GetStringUTFChars(name, nullptr);
            if (utf != nullptr) {
                std::snprintf(version, version_cap, "%s", utf);
                env->ReleaseStringUTFChars(name, utf);
            }
            env->DeleteLocalRef(name);
        }
    }
    env->DeleteLocalRef(info_class);
    env->DeleteLocalRef(info);
    *code = static_cast<int64_t>(value);
    return true;
}

void read_game_abi() {
    if (g_game_abi_known) {
        return;
    }
    auto *vm = static_cast<JavaVM *>(g_vm);
    if (vm == nullptr) {
        log_info("game abi unknown");
        return;
    }
    JNIEnv *env = nullptr;
    bool attached = false;
    const jint existing = vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6);
    if (existing == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            log_info("game abi unknown");
            return;
        }
        attached = true;
    } else if (existing != JNI_OK || env == nullptr) {
        log_info("game abi unknown");
        return;
    }
    int64_t code = 0;
    char version[32];
    version[0] = '\0';
    const bool ok = query_package(env, &code, version, sizeof(version));
    if (attached) {
        vm->DetachCurrentThread();
    }
    if (!ok) {
        log_info("game abi unknown");
        return;
    }
    g_game_abi = code;
    g_game_abi_known = true;
    if (version[0] != '\0') {
        __android_log_print(ANDROID_LOG_INFO, kTag, "game abi %lld %s", static_cast<long long>(code), version);
    } else {
        __android_log_print(ANDROID_LOG_INFO, kTag, "game abi %lld", static_cast<long long>(code));
    }
}

bool accepts_game(void *handle, const char *name) {
    const auto *abi = reinterpret_cast<const int64_t *>(dlsym(handle, "gtasa_plugin_game_abi"));
    if (abi == nullptr) {
        return true;
    }
    const int64_t count = abi[0];
    if (count <= 0 || count > 16) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "abi malformed %s", name);
        return false;
    }
    if (g_game_abi_known) {
        for (int64_t i = 0; i < count; ++i) {
            if (abi[i + 1] == g_game_abi) {
                __android_log_print(ANDROID_LOG_INFO, kTag, "abi ok %s", name);
                return true;
            }
        }
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "abi skip %s", name);
    return false;
}

void call_enter(uintptr_t base) {
    GtasaHost host {};
    host.size = sizeof(host);
    host.base = base;
    host.hook = gtasa_hook;
    for (int i = 0; i < g_count; ++i) {
        LoadedPlugin &plugin = g_plugins[i];
        if (plugin.path[0] == '\0') {
            continue;
        }
        plugin.handle = open_plugin(plugin.path);
        if (plugin.handle == nullptr) {
            __android_log_print(ANDROID_LOG_INFO, kTag, "dlopen failed %s %s", plugin.name, dlerror());
            continue;
        }
        plugin.enter = reinterpret_cast<void (*)(const GtasaHost *)>(dlsym(plugin.handle, "gtasa_plugin_enter"));
        plugin.pause = reinterpret_cast<void (*)()>(dlsym(plugin.handle, "gtasa_plugin_pause"));
        plugin.resume = reinterpret_cast<void (*)()>(dlsym(plugin.handle, "gtasa_plugin_resume"));
        if (plugin.enter == nullptr) {
            __android_log_print(ANDROID_LOG_INFO, kTag, "%s missing gtasa_plugin_enter", plugin.name);
            continue;
        }
        if (!accepts_game(plugin.handle, plugin.name)) {
            plugin.enter = nullptr;
            plugin.pause = nullptr;
            plugin.resume = nullptr;
            continue;
        }
        plugin.enter(&host);
        __android_log_print(ANDROID_LOG_INFO, kTag, "status=enter %s", plugin.name);
    }
}

void call_pause() {
    for (int i = 0; i < g_count; ++i) {
        if (g_plugins[i].pause != nullptr) {
            g_plugins[i].pause();
            __android_log_print(ANDROID_LOG_INFO, kTag, "status=pause %s", g_plugins[i].name);
        }
    }
}

void call_resume() {
    for (int i = 0; i < g_count; ++i) {
        if (g_plugins[i].resume != nullptr) {
            g_plugins[i].resume();
            __android_log_print(ANDROID_LOG_INFO, kTag, "status=resume %s", g_plugins[i].name);
        }
    }
}

}  // namespace

void plugins_set_jvm(void *vm) {
    g_vm = vm;
}

void plugins_capture(int module_dirfd, const char *app_data_dir) {
    g_data_dir[0] = '\0';
    if (app_data_dir != nullptr) {
        std::snprintf(g_data_dir, sizeof(g_data_dir), "%s", app_data_dir);
    }
    if (module_dirfd < 0) {
        log_info("no module directory");
        return;
    }
    const int plugins = openat(module_dirfd, "plugins", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    close(module_dirfd);
    if (plugins < 0) {
        log_info("no plugins directory");
        return;
    }
    DIR *handle = fdopendir(plugins);
    if (handle == nullptr) {
        close(plugins);
        return;
    }
    while (dirent *entry = readdir(handle)) {
        capture_one(dirfd(handle), entry->d_name);
    }
    closedir(handle);
    __android_log_print(ANDROID_LOG_INFO, kTag, "plugins=%d", g_count);
}

void plugins_capture_libs(const char *lib_dir, const char *app_data_dir) {
    g_data_dir[0] = '\0';
    if (app_data_dir != nullptr) {
        std::snprintf(g_data_dir, sizeof(g_data_dir), "%s", app_data_dir);
    }
    if (lib_dir == nullptr) {
        log_info("no npatch library directory");
        return;
    }
    DIR *handle = opendir(lib_dir);
    if (handle == nullptr) {
        log_info("no npatch library directory");
        return;
    }
    while (dirent *entry = readdir(handle)) {
        if (std::strcmp(entry->d_name, "libgtasa_npatch.so") == 0) {
            continue;
        }
        capture_one(dirfd(handle), entry->d_name);
    }
    closedir(handle);
    __android_log_print(ANDROID_LOG_INFO, kTag, "plugins=%d", g_count);
}

void plugins_stage() {
    if (g_data_dir[0] == '\0') {
        log_info("no app data directory");
        return;
    }
    char code_cache[512];
    char dest_dir[512];
    std::snprintf(code_cache, sizeof(code_cache), "%s/code_cache", g_data_dir);
    std::snprintf(dest_dir, sizeof(dest_dir), "%s/code_cache/gtasa-de", g_data_dir);
    mkdir(code_cache, 0700);
    mkdir(dest_dir, 0700);
    clear_staged_dir(dest_dir);
    for (int i = 0; i < g_count; ++i) {
        LoadedPlugin &plugin = g_plugins[i];
        std::snprintf(plugin.path, sizeof(plugin.path), "%s/%s", dest_dir, plugin.name);
        const int fd = open(plugin.path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0755);
        if (fd < 0) {
            plugin.path[0] = '\0';
            __android_log_print(ANDROID_LOG_INFO, kTag, "write failed %s", plugin.name);
            continue;
        }
        size_t put = 0;
        while (put < plugin.size) {
            const ssize_t n = write(fd, plugin.bytes + put, plugin.size - put);
            if (n <= 0) {
                break;
            }
            put += static_cast<size_t>(n);
        }
        close(fd);
        if (put != plugin.size) {
            unlink(plugin.path);
            plugin.path[0] = '\0';
            __android_log_print(ANDROID_LOG_INFO, kTag, "short write %s", plugin.name);
        }
    }
}

void sync_hold(uint32_t bit, bool on, bool *pause_now, bool *resume_now) {
    pthread_mutex_lock(&g_state_lock);
    if (on) {
        g_holds |= bit;
    } else {
        g_holds &= ~bit;
    }
    const bool ready = g_base != 0 && g_holds == 0 && is_playing();
    *pause_now = false;
    *resume_now = false;
    if (g_loaded && g_active && !ready) {
        g_active = false;
        *pause_now = true;
    } else if (g_loaded && !g_active && ready) {
        g_active = true;
        *resume_now = true;
    }
    pthread_mutex_unlock(&g_state_lock);
}

void apply_hold(uint32_t bit, bool on) {
    bool pause_now = false;
    bool resume_now = false;
    sync_hold(bit, on, &pause_now, &resume_now);
    if (pause_now) {
        call_pause();
    } else if (resume_now) {
        call_resume();
    }
}

void detour_transition_started() {
    apply_hold(kHoldInterior, true);
    g_transition_started();
}

void after_transition_finished() {
    apply_hold(kHoldInterior, false);
}

void detour_transition_finished() {
    g_transition_finished();
}

void detour_start_cutscene() {
    apply_hold(kHoldCutscene, true);
    g_start_cutscene();
}

void detour_delete_cutscene() {
    apply_hold(kHoldCutscene, false);
    g_delete_cutscene();
}

void detour_kill_player() {
    apply_hold(kHoldRespawn, true);
    g_kill_player();
}

void detour_arrest_player() {
    apply_hold(kHoldRespawn, true);
    g_arrest_player();
}

void after_respawn() {
    apply_hold(kHoldRespawn, false);
}

void detour_respawn_hospital() {
    g_respawn_hospital();
}

void detour_respawn_police() {
    g_respawn_police();
}

void detour_shutdown_restart() {
    g_shutdown_restart();
}

void install_one(void *target, void *detour, void **original, const char *name, void (*after)()) {
    if (target == nullptr) {
        __android_log_print(ANDROID_LOG_INFO, kTag, "hook skipped %s", name);
        *original = nullptr;
        return;
    }
    if (gtasa_hook(target, detour, original) == 0) {
        if (after != nullptr) {
            gtasa_hook_on_return(target, after);
        }
        __android_log_print(ANDROID_LOG_INFO, kTag, "hook %s", name);
        return;
    }
    __android_log_print(ANDROID_LOG_INFO, kTag, "hook failed %s", name);
    *original = nullptr;
}

void install_hooks(const GtasaSymbols *symbols) {
    if (g_hooks_installed || symbols == nullptr) {
        return;
    }
    g_hooks_installed = true;
    install_one(symbols->transition_started, reinterpret_cast<void *>(detour_transition_started),
                reinterpret_cast<void **>(&g_transition_started), "TransitionStarted", nullptr);
    install_one(symbols->transition_finished, reinterpret_cast<void *>(detour_transition_finished),
                reinterpret_cast<void **>(&g_transition_finished), "TransitionFinished",
                after_transition_finished);
    install_one(symbols->start_cutscene, reinterpret_cast<void *>(detour_start_cutscene),
                reinterpret_cast<void **>(&g_start_cutscene), "StartCutscene", nullptr);
    install_one(symbols->delete_cutscene, reinterpret_cast<void *>(detour_delete_cutscene),
                reinterpret_cast<void **>(&g_delete_cutscene), "DeleteCutsceneData", nullptr);
    install_one(symbols->kill_player, reinterpret_cast<void *>(detour_kill_player),
                reinterpret_cast<void **>(&g_kill_player), "KillPlayer", nullptr);
    install_one(symbols->arrest_player, reinterpret_cast<void *>(detour_arrest_player),
                reinterpret_cast<void **>(&g_arrest_player), "ArrestPlayer", nullptr);
    install_one(symbols->respawn_hospital, reinterpret_cast<void *>(detour_respawn_hospital),
                reinterpret_cast<void **>(&g_respawn_hospital), "HandleRespawnAtHospital", after_respawn);
    install_one(symbols->respawn_police, reinterpret_cast<void *>(detour_respawn_police),
                reinterpret_cast<void **>(&g_respawn_police), "HandleRespawnAtPoliceStation", after_respawn);
    install_one(symbols->shutdown_restart, reinterpret_cast<void *>(detour_shutdown_restart),
                reinterpret_cast<void **>(&g_shutdown_restart), "ShutDownForRestart", after_respawn);
}

void plugins_run(uintptr_t base, const GtasaSymbols *symbols) {
    g_base = base;
    read_game_abi();
    if (symbols != nullptr) {
        g_is_playing = reinterpret_cast<PlayingFn>(symbols->is_playing);
        g_find_player_ped = reinterpret_cast<FindPedFn>(symbols->find_player_ped);
    }
    install_hooks(symbols);
    int since_beat = 0;
    while (true) {
        bool do_enter = false;
        bool do_pause = false;
        bool do_resume = false;
        pthread_mutex_lock(&g_state_lock);
        const bool ready = g_holds == 0 && is_playing() && (g_loaded || player_ped_present());
        if (!g_loaded && ready) {
            g_loaded = true;
            g_active = true;
            do_enter = true;
        } else if (g_loaded && g_active && !ready) {
            g_active = false;
            do_pause = true;
        } else if (g_loaded && !g_active && ready) {
            g_active = true;
            do_resume = true;
        }
        pthread_mutex_unlock(&g_state_lock);
        if (do_enter) {
            call_enter(base);
            if (g_count == 0) {
                log_info("status=enter plugins=0");
            }
        } else if (do_pause) {
            call_pause();
        } else if (do_resume) {
            call_resume();
        } else if (!g_loaded && since_beat >= kHeartbeatMs) {
            log_info("status=awaiting-world");
            since_beat = 0;
        }
        usleep(static_cast<useconds_t>(kPollMs) * 1000);
        if (!g_loaded) {
            since_beat += kPollMs;
        }
    }
}
