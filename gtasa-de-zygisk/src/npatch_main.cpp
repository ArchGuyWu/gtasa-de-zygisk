#include "library_watch.h"
#include "plugins.h"

#include <dlfcn.h>
#include <jni.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using HookFunType = int (*)(void *, void *, void **);
using UnhookFunType = int (*)(void *);
using NativeOnModuleLoaded = void (*)(const char *, void *);

struct NativeAPIEntries {
    uint32_t version;
    HookFunType hook_func;
    UnhookFunType unhook_func;
};

constexpr const char *kPackage = "com.rockstargames.gtasa.de";

void on_library_loaded(const char *, void *) {}

void copy_dirname(char *out, size_t cap, const char *path) {
    if (cap == 0) {
        return;
    }
    out[0] = '\0';
    if (path == nullptr) {
        return;
    }
    std::snprintf(out, cap, "%s", path);
    char *slash = std::strrchr(out, '/');
    if (slash == nullptr) {
        out[0] = '\0';
        return;
    }
    *slash = '\0';
}

bool data_dir_from_vm(char *out, size_t cap) {
    using GetVms = jint (*)(JavaVM **, jsize, jsize *);
    auto *get_vms = reinterpret_cast<GetVms>(dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs"));
    if (get_vms == nullptr) {
        return false;
    }
    JavaVM *vm = nullptr;
    jsize count = 0;
    if (get_vms(&vm, 1, &count) != JNI_OK || vm == nullptr || count < 1) {
        return false;
    }
    plugins_set_jvm(vm);
    JNIEnv *env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK || env == nullptr) {
        return false;
    }
    jclass thread = env->FindClass("android/app/ActivityThread");
    if (thread == nullptr || env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    jmethodID current = env->GetStaticMethodID(thread, "currentApplication", "()Landroid/app/Application;");
    if (current == nullptr || env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    jobject app = env->CallStaticObjectMethod(thread, current);
    if (env->ExceptionCheck() || app == nullptr) {
        env->ExceptionClear();
        return false;
    }
    jclass app_class = env->GetObjectClass(app);
    jmethodID get_info = env->GetMethodID(app_class, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
    if (get_info == nullptr || env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    jobject info = env->CallObjectMethod(app, get_info);
    if (env->ExceptionCheck() || info == nullptr) {
        env->ExceptionClear();
        return false;
    }
    jclass info_class = env->GetObjectClass(info);
    jfieldID field = env->GetFieldID(info_class, "dataDir", "Ljava/lang/String;");
    if (field == nullptr || env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    auto dir = static_cast<jstring>(env->GetObjectField(info, field));
    if (dir == nullptr) {
        return false;
    }
    const char *utf = env->GetStringUTFChars(dir, nullptr);
    if (utf == nullptr) {
        return false;
    }
    std::snprintf(out, cap, "%s", utf);
    env->ReleaseStringUTFChars(dir, utf);
    return out[0] != '\0';
}

void start_from_here() {
    Dl_info info {};
    if (dladdr(reinterpret_cast<void *>(start_from_here), &info) == 0 || info.dli_fname == nullptr) {
        return;
    }
    char lib_dir[512];
    copy_dirname(lib_dir, sizeof(lib_dir), info.dli_fname);
    char data_dir[512];
    if (!data_dir_from_vm(data_dir, sizeof(data_dir))) {
        std::snprintf(data_dir, sizeof(data_dir), "/data/data/%s", kPackage);
    }
    plugins_capture_libs(lib_dir, data_dir);
    plugins_stage();
    start_library_watch();
}

extern "C" __attribute__((visibility("default"), used)) NativeOnModuleLoaded
native_init(const NativeAPIEntries *) {
    start_from_here();
    return on_library_loaded;
}

}  // namespace
