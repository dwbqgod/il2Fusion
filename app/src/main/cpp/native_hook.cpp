#include <jni.h>

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdlib>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "dobby.h"
#include "il2CppDumper/hack.h"
#include "utils/db.h"
#include "utils/log.h"
#include "utils/utils.h"

namespace {

constexpr const char* kLibIl2cpp = "libil2cpp.so";
//constexpr const char* kTargetText = "Hook Test";

using Il2CppStringNewFn = textutils::Il2CppString* (*)(const char*);

struct HookRegistry {
    std::unordered_map<void*, uintptr_t> targets;  // address -> rva
};

std::atomic_bool g_initialized{false};
std::mutex g_hookMutex;
std::vector<uintptr_t> g_rvas;
std::vector<void*> g_installedTargets;
std::atomic<HookRegistry*> g_registry{nullptr};
uintptr_t g_il2cpp_base = 0;
Il2CppStringNewFn g_stringNew = nullptr;
std::string g_process_name = "unknown";
std::atomic_bool g_dump_enabled{false};
std::atomic_bool g_dump_started{false};
std::string g_dump_dir;
JavaVM* g_vm = nullptr;
jclass g_bridge_class = nullptr;
jmethodID g_on_dump_finished = nullptr;

std::string GetBasePackage(const std::string& process_name) {
    if (process_name.empty()) return "unknown";
    const auto pos = process_name.find(':');
    if (pos != std::string::npos) {
        return process_name.substr(0, pos);
    }
    return process_name;
}

void NotifyDumpResult(bool success, const char* msg) {
    if (g_vm == nullptr || g_bridge_class == nullptr || g_on_dump_finished == nullptr) {
        return;
    }
    JNIEnv* env = nullptr;
    bool attached = false;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            return;
        }
        attached = true;
    }
    jstring jmsg = env->NewStringUTF(msg ? msg : "");
    env->CallStaticVoidMethod(g_bridge_class, g_on_dump_finished,
                              success ? JNI_TRUE : JNI_FALSE, jmsg);
    env->DeleteLocalRef(jmsg);
    if (attached) {
        g_vm->DetachCurrentThread();
    }
}

bool CopyDumpToSdcard(const std::string& pkg, std::string& outPath) {
    const std::string src = g_dump_dir + "/files/dump.cs";
    outPath = "/sdcard/Download/" + pkg + ".cs";
    std::string cmd = "su -c 'cp " + src + " " + outPath + "'";
    const int ret = system(cmd.c_str());
    if (ret == 0) {
        LOGI("copied dump to %s", outPath.c_str());
        return true;
    } else {
        LOGE("copy dump failed ret=%d cmd=%s", ret, cmd.c_str());
        return false;
    }
}

void trigger_dump_async() {
    if (!g_dump_enabled.load()) {
        return;
    }
    if (g_dump_started.exchange(true)) {
        return;
    }
    if (g_dump_dir.empty()) {
        LOGE("trigger_dump skipped: dump dir empty");
        g_dump_started.store(false);
        return;
    }
    std::thread([] {
        const uintptr_t base = hookutils::WaitForModule(kLibIl2cpp, std::chrono::seconds(30));
        if (base == 0) {
            LOGE("dump wait libil2cpp timeout");
            g_dump_started.store(false);
            NotifyDumpResult(false, "等待 libil2cpp 超时");
            return;
        }
        LOGI("dump begin, libil2cpp @ 0x%" PRIxPTR, base);
        const bool ok = hack_prepare(g_dump_dir.c_str(), nullptr, 0);
        g_dump_started.store(false);
        if (ok) {
            std::string dst;
            const std::string pkg = GetBasePackage(g_process_name);
            const bool copyOk = CopyDumpToSdcard(pkg, dst);
            if (copyOk) {
                std::string msg = "Dump 完成，已复制到 " + dst;
                NotifyDumpResult(true, msg.c_str());
            } else {
                NotifyDumpResult(true, "Dump 完成，复制到 Download 失败");
            }
        } else {
            NotifyDumpResult(false, "Dump 失败，未找到 libil2cpp");
        }
    }).detach();
}

void setter_pre_handler(void* address, DobbyRegisterContext* ctx) {
    HookRegistry* registry = g_registry.load();
    uintptr_t rva = 0;
    if (registry) {
        auto it = registry->targets.find(address);
        if (it != registry->targets.end()) {
            rva = it->second;
        }
    }

    void* arg = hookutils::GetSecondArg(ctx);
    const auto original = textutils::DescribeIl2CppString(
            reinterpret_cast<textutils::Il2CppString*>(arg));

    if (!original.empty()) {
        const bool filtered = textutils::ShouldFilter(original);
        if (filtered) {
            LOGI("[Setter] RVA 0x%" PRIxPTR " 过滤：#%s#", rva, original.c_str());
        } else {
            LOGI("[Setter] RVA 0x%" PRIxPTR " %s", rva, original.c_str());
            textdb::InsertIfNeeded(original);
        }
    }

    // 之前会调用 il2cpp_string_new 构造新字符串并替换第二参数，现仅保留原文日志。
    // if (!g_stringNew) {
    //     return;
    // }
    // Il2CppString* newStr = nullptr;
    // try {
    //     newStr = g_stringNew(kTargetText);
    // } catch (...) {
    //     LOGE("g_stringNew call failed");
    // }
    // if (newStr == nullptr) {
    //     return;
    // }
    // hookutils::SetSecondArg(ctx, newStr);
}

void* open_il2cpp_handle() {
    // 优先把符号引入全局命名空间，避免重复映射
    void* handle = dlopen(kLibIl2cpp, RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD);
    if (handle) {
        return handle;
    }

    const std::string path = hookutils::FindModulePath(kLibIl2cpp);
    if (!path.empty()) {
        handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD);
        if (handle) {
            return handle;
        }
    }

    return dlopen(nullptr, RTLD_NOW);
}

bool prepare_il2cpp_factory() {
    void* handle = open_il2cpp_handle();
    if (!handle) {
        LOGE("dlopen failed when resolving il2cpp_string_new");
        return false;
    }

    g_stringNew = reinterpret_cast<Il2CppStringNewFn>(
            dlsym(handle, "il2cpp_string_new"));

    if (!g_stringNew) {
        g_stringNew = reinterpret_cast<Il2CppStringNewFn>(
                dlsym(RTLD_DEFAULT, "il2cpp_string_new"));
    }

    if (!g_stringNew) {
        const std::string path = hookutils::FindModulePath(kLibIl2cpp);
        const uintptr_t addr = hookutils::FindExportInElf(path.c_str(), "il2cpp_string_new", g_il2cpp_base);
        if (addr != 0) {
            g_stringNew = reinterpret_cast<Il2CppStringNewFn>(addr);
        }
    }

    if (!g_stringNew) {
        LOGE("无法找到 il2cpp_string_new");
        return false;
    }

    return true;
}

void clear_hooks_locked() {
    for (void* target : g_installedTargets) {
        const int ret = DobbyDestroy(target);
        if (ret != 0) {
            LOGE("销毁 hook @%p 失败, ret=%d", target, ret);
        }
    }
    g_installedTargets.clear();

    HookRegistry* old = g_registry.exchange(nullptr);
    delete old;
}

void install_hooks_locked() {
#if !defined(__arm__) && !defined(__aarch64__) && !defined(__x86_64__) && !defined(__i386__)
    LOGE("当前架构不支持文本拦截");
    return;
#endif

    if (g_il2cpp_base == 0) {
        LOGE("il2cpp 未准备好，跳过安装 hook");
        return;
    }

    if (g_rvas.empty()) {
        LOGI("未配置 RVA，跳过安装 hook");
        return;
    }

    clear_hooks_locked();

    HookRegistry* registry = new HookRegistry();

    for (uintptr_t rva : g_rvas) {
        void* target = reinterpret_cast<void*>(g_il2cpp_base + rva);
        const int ret = DobbyInstrument(target, setter_pre_handler);
        if (ret == 0) {
            g_installedTargets.push_back(target);
            registry->targets[target] = rva;
            LOGI("Hooked RVA 0x%" PRIxPTR " @ %p", rva, target);
        } else {
            LOGE("Hook RVA 0x%" PRIxPTR " 失败, ret=%d", rva, ret);
        }
    }

    HookRegistry* old = g_registry.exchange(registry);
    delete old;
}

void update_rvas(const std::vector<uintptr_t>& newRvas) {
    std::lock_guard<std::mutex> _lk(g_hookMutex);
    g_rvas = newRvas;
    LOGI("更新 RVA 列表，共 %zu 个", g_rvas.size());

    if (g_il2cpp_base != 0) {
        install_hooks_locked();
    }
}

void init_worker() {
    textdb::Init(g_process_name, true);

    g_il2cpp_base = hookutils::WaitForModule(kLibIl2cpp, std::chrono::seconds(10));
    if (g_il2cpp_base == 0) {
        LOGI("[%s] 等待 %s 载入超时，当前进程可能未使用 Unity/il2cpp", g_process_name.c_str(), kLibIl2cpp);
        return;
    }

    LOGI("[%s] %s loaded @ 0x%" PRIxPTR, g_process_name.c_str(), kLibIl2cpp, g_il2cpp_base);

    if (!prepare_il2cpp_factory()) {
        return;
    }

    std::lock_guard<std::mutex> _lk(g_hookMutex);
    install_hooks_locked();
}

}  // namespace

extern "C" JNIEXPORT void JNICALL
Java_com_tools_module_NativeBridge_init(JNIEnv* env, jclass /*clazz*/, jstring processName) {
    if (processName != nullptr) {
        const char* utf = env->GetStringUTFChars(processName, nullptr);
        if (utf != nullptr) {
            g_process_name.assign(utf);
            env->ReleaseStringUTFChars(processName, utf);
        }
    }

    if (g_initialized.exchange(true)) {
        LOGI("[%s] Native init already completed", g_process_name.c_str());
        return;
    }

    LOGI("[%s] Native init start", g_process_name.c_str());
    std::thread(init_worker).detach();

    (void) env;
}

extern "C" JNIEXPORT void JNICALL
Java_com_tools_module_NativeBridge_setConfig(JNIEnv* env, jclass /*clazz*/, jlongArray rvas) {
    if (rvas == nullptr) {
        return;
    }

    const jsize len = env->GetArrayLength(rvas);
    std::vector<uintptr_t> values;
    values.reserve(static_cast<size_t>(len));

    jlong* elems = env->GetLongArrayElements(rvas, nullptr);
    for (jsize i = 0; i < len; ++i) {
        values.push_back(static_cast<uintptr_t>(elems[i]));
    }
    env->ReleaseLongArrayElements(rvas, elems, JNI_ABORT);

    update_rvas(values);
}

extern "C" JNIEXPORT void JNICALL
Java_com_tools_module_NativeBridge_startDump(JNIEnv* env, jclass /*clazz*/, jstring dataDir) {
    const char* path = dataDir != nullptr ? env->GetStringUTFChars(dataDir, nullptr) : nullptr;
    if (path != nullptr && path[0] != '\0') {
        g_dump_dir.assign(path);
        env->ReleaseStringUTFChars(dataDir, path);
        g_dump_enabled.store(true);
        g_dump_started.store(false);
    } else {
        LOGE("startDump skipped: dataDir is null or empty");
        g_dump_dir.clear();
        g_dump_enabled.store(false);
        return;
    }
    trigger_dump_async();
}

jint JNI_OnLoad(JavaVM* vm, void*) {
    LOGI("JNI_OnLoad");
    g_vm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_VERSION_1_6;
    }
    jclass cls = env->FindClass("com/tools/module/NativeBridge");
    if (cls != nullptr) {
        g_bridge_class = static_cast<jclass>(env->NewGlobalRef(cls));
        env->DeleteLocalRef(cls);
        g_on_dump_finished = env->GetStaticMethodID(
                g_bridge_class,
                "onDumpFinished",
                "(ZLjava/lang/String;)V");
    }
    return JNI_VERSION_1_6;
}
