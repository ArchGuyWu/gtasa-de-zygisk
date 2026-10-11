#include <sys/types.h>

#include "library_watch.h"
#include "plugins.h"
#include "zygisk.hpp"

#include <cstdio>
#include <cstring>

#include <jni.h>

namespace {

constexpr const char *kTarget = "com.rockstargames.gtasa.de";

bool nice_name_matches(JNIEnv *env, jstring name) {
    if (env == nullptr || name == nullptr) {
        return false;
    }
    const char *utf = env->GetStringUTFChars(name, nullptr);
    if (utf == nullptr) {
        return false;
    }
    const bool match = std::strcmp(utf, kTarget) == 0;
    env->ReleaseStringUTFChars(name, utf);
    return match;
}

class GtasaModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        api_ = api;
        env_ = env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        is_target_ = args != nullptr && nice_name_matches(env_, args->nice_name);
        if (!is_target_) {
            request_unload();
            return;
        }
        char data_dir[512];
        data_dir[0] = '\0';
        if (env_ != nullptr && args->app_data_dir != nullptr) {
            const char *utf = env_->GetStringUTFChars(args->app_data_dir, nullptr);
            if (utf != nullptr) {
                std::snprintf(data_dir, sizeof(data_dir), "%s", utf);
                env_->ReleaseStringUTFChars(args->app_data_dir, utf);
            }
        }
        if (env_ != nullptr) {
            JavaVM *vm = nullptr;
            if (env_->GetJavaVM(&vm) == JNI_OK) {
                plugins_set_jvm(vm);
            }
        }
        const int module_dir = api_ != nullptr ? api_->getModuleDir() : -1;
        plugins_capture(module_dir, data_dir);
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        if (is_target_) {
            plugins_stage();
            start_library_watch();
        }
    }

    void preServerSpecialize(zygisk::ServerSpecializeArgs *) override {
        is_target_ = false;
        request_unload();
    }

private:
    void request_unload() {
        if (api_ != nullptr) {
            api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
        }
    }

    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    bool is_target_ = false;
};

}  // namespace

REGISTER_ZYGISK_MODULE(GtasaModule)
