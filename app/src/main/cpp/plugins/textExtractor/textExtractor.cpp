#include "textExtractor.h"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdlib>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <type_traits>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../../il2CppDumper/il2cpp-class.h"
#include "../../il2CppDumper/il2cpp-tabledefs.h"
#include "../../il2CppDumper/xdl/include/xdl.h"
#include "../../utils/db.h"
#include "../../utils/log.h"
#include "../../utils/utils.h"
#include "dobby.h"

namespace text_extractor {
namespace {

constexpr const char* kLibIl2cpp = "libil2cpp.so";

struct HookedMethod {
    std::string full;
};

struct HookRegistry {
    std::unordered_map<void*, HookedMethod> targets;  // method entry -> meta
};

std::mutex g_hook_mutex;
std::vector<il2cpputils::TargetSpec> g_targets;
std::vector<void*> g_installed_targets;
std::atomic<HookRegistry*> g_registry{nullptr};
std::string g_process_name = "unknown";
std::atomic_bool g_worker_started{false};
std::atomic_bool g_il2cpp_ready{false};
std::atomic_bool g_api_initialized{false};
il2cpputils::Il2CppApi g_api{};

bool EnsureIl2cppApi() {
    if (g_api_initialized.load()) {
        return true;
    }

    void* handle = dlopen(kLibIl2cpp, RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD);
    if (handle == nullptr) {
        const char* path = find_full_path("libil2cpp");
        if (path != nullptr) {
            handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD);
            free(const_cast<char*>(path));
        }
    }
    if (handle == nullptr) {
        const std::string mapped = hookutils::FindModulePath(kLibIl2cpp);
        if (!mapped.empty()) {
            handle = dlopen(mapped.c_str(), RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD);
        }
    }
    if (handle == nullptr) {
        handle = xdl_open(kLibIl2cpp, 0);
    }
    if (handle == nullptr) {
        const std::string mapped = hookutils::FindModulePath(kLibIl2cpp);
        if (!mapped.empty()) {
            handle = xdl_open(mapped.c_str(), 0);
        }
    }
    if (handle == nullptr) {
        // 最后尝试直接加载一次
        handle = dlopen(kLibIl2cpp, RTLD_NOW | RTLD_GLOBAL);
    }
    if (handle == nullptr) {
        LOGE("il2cpp dlopen 失败，无法初始化 API");
        return false;
    }

    if (!il2cpputils::ResolveIl2cppApi(handle, g_api)) {
        return false;
    }

    g_api_initialized.store(true);
    return true;
}

const MethodInfo* ResolveMethod(const il2cpputils::TargetSpec& spec) {
    if (!EnsureIl2cppApi()) {
        return nullptr;
    }

    if (!il2cpputils::EnsureVmReadyAndAttach(g_api, 50, std::chrono::milliseconds(100))) {
        return nullptr;
    }

    return il2cpputils::FindMethodInAssemblies(g_api, spec);
}

void setter_pre_handler(void* address, DobbyRegisterContext* ctx) {
    HookRegistry* registry = g_registry.load();
    const char* target_name = "<unknown>";
    if (registry) {
        auto it = registry->targets.find(address);
        if (it != registry->targets.end()) {
            target_name = it->second.full.c_str();
        }
    }

    void* arg = hookutils::GetSecondArg(ctx);
    const auto original = textutils::DescribeIl2CppString(
            reinterpret_cast<textutils::Il2CppString*>(arg));

    if (!original.empty()) {
        const bool filtered = textutils::ShouldFilter(original);
        if (filtered) {
            LOGI("[Setter] %s 过滤：#%s#", target_name, original.c_str());
        } else {
            LOGI("[Setter] %s %s", target_name, original.c_str());
            textdb::InsertIfNeeded(original);
        }
    }
}

void clear_hooks_locked() {
    for (void* target : g_installed_targets) {
        const int ret = DobbyDestroy(target);
        if (ret != 0) {
            LOGE("销毁 hook @%p 失败, ret=%d", target, ret);
        }
    }
    g_installed_targets.clear();

    HookRegistry* old = g_registry.exchange(nullptr);
    delete old;
}

void install_hooks_locked() {
#if !defined(__arm__) && !defined(__aarch64__) && !defined(__x86_64__) && !defined(__i386__)
    LOGE("当前架构不支持文本拦截");
    return;
#endif

    if (!g_il2cpp_ready.load()) {
        LOGE("il2cpp 未准备好，跳过安装 hook");
        return;
    }

    if (g_targets.empty()) {
        LOGI("未配置方法，跳过安装 hook");
        return;
    }

    if (!EnsureIl2cppApi()) {
        return;
    }

    clear_hooks_locked();
    HookRegistry* registry = new HookRegistry();

    for (const auto& spec : g_targets) {
        const MethodInfo* method = ResolveMethod(spec);
        if (method == nullptr || method->methodPointer == nullptr) {
            LOGE("解析方法失败：%s", spec.full.c_str());
            continue;
        }
        void* target = reinterpret_cast<void*>(method->methodPointer);
        const int ret = DobbyInstrument(target, setter_pre_handler);
        if (ret == 0) {
            g_installed_targets.push_back(target);
            registry->targets[target] = HookedMethod{spec.full};
            LOGI("Hook 成功: %s @ %p", spec.full.c_str(), target);
        } else {
            LOGE("Hook 失败 %s, ret=%d", spec.full.c_str(), ret);
        }
    }

    HookRegistry* old = g_registry.exchange(registry);
    delete old;
}

void update_targets_internal(const std::vector<std::string>& new_targets) {
    std::lock_guard<std::mutex> _lk(g_hook_mutex);
    g_targets.clear();
    for (const auto& full : new_targets) {
        il2cpputils::TargetSpec spec;
        if (il2cpputils::ParseTarget(full, spec)) {
            g_targets.push_back(std::move(spec));
        } else {
            LOGE("非法方法名：%s", full.c_str());
        }
    }
    LOGI("更新方法列表，共 %zu 个", g_targets.size());

    if (g_il2cpp_ready.load()) {
        install_hooks_locked();
    }
}

void init_worker() {
    textdb::Init(g_process_name, true);

    const uintptr_t base = hookutils::WaitForModule(kLibIl2cpp, std::chrono::seconds(10));
    if (base == 0) {
        LOGI("[%s] 等待 %s 载入超时，当前进程可能未使用 Unity/il2cpp", g_process_name.c_str(), kLibIl2cpp);
        return;
    }

    LOGI("[%s] %s loaded @ 0x%" PRIxPTR, g_process_name.c_str(), kLibIl2cpp, base);
    if (!EnsureIl2cppApi()) {
        return;
    }

    g_il2cpp_ready.store(true);
    std::lock_guard<std::mutex> _lk(g_hook_mutex);
    install_hooks_locked();
}

}  // namespace

void Init(const std::string& process_name) {
    g_process_name = process_name.empty() ? "unknown" : process_name;
    if (g_worker_started.exchange(true)) {
        return;
    }
    LOGI("[%s] Text extractor init", g_process_name.c_str());
    std::thread(init_worker).detach();
}

void UpdateTargets(const std::vector<std::string>& new_targets) {
    update_targets_internal(new_targets);
}

}  // namespace text_extractor
