#include <android/log.h>

#include <cstdint>

struct GtasaHost {
    uint32_t size;
    uintptr_t base;
    int (*hook)(void *target, void *detour, void **original);
};

namespace {

constexpr const char *kTag = "GTASADE";
const GtasaHost *g_host = nullptr;

}  // namespace

extern "C" __attribute__((visibility("default"))) void gtasa_plugin_enter(const GtasaHost *host) {
    g_host = host;
    const uintptr_t base = host != nullptr ? host->base : 0;
    __android_log_print(ANDROID_LOG_INFO, kTag, "probe enter base=0x%llx",
                        static_cast<unsigned long long>(base));
}

extern "C" __attribute__((visibility("default"))) void gtasa_plugin_pause() {
    __android_log_print(ANDROID_LOG_INFO, kTag, "probe pause");
}

extern "C" __attribute__((visibility("default"))) void gtasa_plugin_resume() {
    __android_log_print(ANDROID_LOG_INFO, kTag, "probe resume");
}
