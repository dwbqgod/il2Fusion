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

struct TargetSpec {
    std::string full;
    std::string namespaze;
    std::string klass;
    std::string method;
};

struct HookedMethod {
    std::string full;
};

struct HookRegistry {
    std::unordered_map<void*, HookedMethod> targets;  // method entry -> meta
};

struct Il2CppApi {
    Il2CppDomain* (*domain_get)();
    const Il2CppAssembly** (*domain_get_assemblies)(Il2CppDomain*, size_t*);
    const Il2CppImage* (*assembly_get_image)(const Il2CppAssembly*);
    Il2CppClass* (*class_from_name)(const Il2CppImage*, const char*, const char*);
    const MethodInfo* (*class_get_methods)(Il2CppClass*, void**);
    const MethodInfo* (*class_get_method_from_name)(Il2CppClass*, const char*, int);
    const char* (*method_get_name)(const MethodInfo*);
    void* (*thread_attach)(Il2CppDomain*);
    bool (*is_vm_thread)(Il2CppThread*);
};

std::mutex g_hook_mutex;
std::vector<TargetSpec> g_targets;
std::vector<void*> g_installed_targets;
std::atomic<HookRegistry*> g_registry{nullptr};
std::string g_process_name = "unknown";
std::atomic_bool g_worker_started{false};
std::atomic_bool g_il2cpp_ready{false};
std::atomic_bool g_api_initialized{false};
Il2CppApi g_api{};

bool ParseTarget(const std::string& full, TargetSpec& out) {
    const auto start = full.find_first_not_of(" \t\r\n");
    const auto end = full.find_last_not_of(" \t\r\n");
    if (start == std::string::npos || end == std::string::npos) {
        return false;
    }
    const std::string cleaned = full.substr(start, end - start + 1);

    auto last_dot = cleaned.rfind('.');
    if (last_dot == std::string::npos || last_dot + 1 >= cleaned.size()) {
        return false;
    }
    const std::string method = cleaned.substr(last_dot + 1);
    const std::string class_and_ns = cleaned.substr(0, last_dot);
    auto second_last = class_and_ns.rfind('.');
    std::string ns;
    std::string klass;
    if (second_last == std::string::npos) {
        klass = class_and_ns;
    } else {
        ns = class_and_ns.substr(0, second_last);
        klass = class_and_ns.substr(second_last + 1);
    }
    if (klass.empty() || method.empty()) {
        return false;
    }
    out.full = cleaned;
    out.namespaze = ns;
    out.klass = klass;
    out.method = method;
    return true;
}

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

    auto resolve = [&](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(xdl_sym(handle, name, nullptr));
        if (fn == nullptr) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(handle, name));
        }
        return fn != nullptr;
    };

    bool ok = true;
    ok &= resolve(g_api.domain_get, "il2cpp_domain_get");
    ok &= resolve(g_api.domain_get_assemblies, "il2cpp_domain_get_assemblies");
    ok &= resolve(g_api.assembly_get_image, "il2cpp_assembly_get_image");
    ok &= resolve(g_api.class_from_name, "il2cpp_class_from_name");
    ok &= resolve(g_api.class_get_methods, "il2cpp_class_get_methods");
    ok &= resolve(g_api.class_get_method_from_name, "il2cpp_class_get_method_from_name");
    ok &= resolve(g_api.method_get_name, "il2cpp_method_get_name");
    resolve(g_api.thread_attach, "il2cpp_thread_attach");
    resolve(g_api.is_vm_thread, "il2cpp_is_vm_thread");

    if (!ok) {
        LOGE("初始化 il2cpp API 失败，部分符号缺失");
        return false;
    }

    g_api_initialized.store(true);
    return true;
}

const MethodInfo* FindMethodInClass(Il2CppClass* klass, const std::string& method_name) {
    if (klass == nullptr) {
        return nullptr;
    }

    // set_text 一般有 1 个参数，优先尝试 1 / 0，再遍历名称匹配
    const MethodInfo* method = g_api.class_get_method_from_name
            ? g_api.class_get_method_from_name(klass, method_name.c_str(), 1)
            : nullptr;
    if (method == nullptr) {
        method = g_api.class_get_method_from_name
                ? g_api.class_get_method_from_name(klass, method_name.c_str(), 0)
                : nullptr;
    }
    if (method != nullptr) {
        return method;
    }

    void* iter = nullptr;
    const MethodInfo* candidate = nullptr;
    while (g_api.class_get_methods && g_api.method_get_name &&
           (candidate = g_api.class_get_methods(klass, &iter)) != nullptr) {
        const char* name = g_api.method_get_name(candidate);
        if (name != nullptr && method_name == name) {
            return candidate;
        }
    }
    return nullptr;
}

const MethodInfo* ResolveMethod(const TargetSpec& spec) {
    if (!EnsureIl2cppApi()) {
        return nullptr;
    }

    if (g_api.domain_get == nullptr || g_api.domain_get_assemblies == nullptr ||
        g_api.assembly_get_image == nullptr || g_api.class_from_name == nullptr) {
        LOGE("Il2Cpp API 未准备好，跳过解析 %s", spec.full.c_str());
        return nullptr;
    }

    Il2CppDomain* domain = g_api.domain_get();
    if (domain == nullptr) {
        return nullptr;
    }
    if (g_api.is_vm_thread) {
        int retry = 0;
        while (!g_api.is_vm_thread(nullptr) && retry < 50) {  // wait up to ~5s
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            ++retry;
        }
        if (!g_api.is_vm_thread(nullptr)) {
            LOGE("il2cpp VM 未就绪，跳过解析 %s", spec.full.c_str());
            return nullptr;
        }
    }
    if (g_api.thread_attach) {
        g_api.thread_attach(domain);
    }

    size_t count = 0;
    const Il2CppAssembly** assemblies = g_api.domain_get_assemblies(domain, &count);
    for (size_t i = 0; i < count; ++i) {
        const Il2CppImage* image = g_api.assembly_get_image(assemblies[i]);
        if (image == nullptr) {
            continue;
        }
        Il2CppClass* klass = g_api.class_from_name(
                image, spec.namespaze.c_str(), spec.klass.c_str());
        if (klass == nullptr) {
            continue;
        }
        const MethodInfo* method = FindMethodInClass(klass, spec.method);
        if (method != nullptr && method->methodPointer != nullptr) {
            return method;
        }
    }
    return nullptr;
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
        TargetSpec spec;
        if (ParseTarget(full, spec)) {
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
