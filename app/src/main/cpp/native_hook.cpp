#include <android/log.h>
#include <jni.h>

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "dobby.h"

#define LOG_TAG "TextExtractTool"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

constexpr const char* kLibIl2cpp = "libil2cpp.so";
constexpr const char* kTargetText = "Hook Test";

struct MethodSpec {
    const char* name;
    uintptr_t rva;
};

const MethodSpec kTextSetters[] = {
        {"FairyGUI.TextField.set_text", 0x1d236e8},
        {"FairyGUI.GTextField.set_text", 0x1eb7bdc},
        {"UnityEngine.TextMesh.set_text", 0x3b08694},
};

struct Il2CppString {
    void* klass;
    void* monitor;
    int32_t length;
    char16_t chars[1];
};

using Il2CppStringNewFn = Il2CppString* (*)(const char*);

std::atomic_bool g_initialized{false};
std::unordered_map<void*, const MethodSpec*> g_hookMap;
Il2CppString* g_managedText{nullptr};
Il2CppStringNewFn g_stringNew{nullptr};

uintptr_t find_module_base(const char* name) {
    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp) {
        return 0;
    }

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, name) == nullptr) {
            continue;
        }

        uintptr_t start = 0;
        if (sscanf(line, "%" SCNxPTR "-%*lx", &start) == 1) {
            fclose(fp);
            return start;
        }
    }

    fclose(fp);
    return 0;
}

uintptr_t wait_for_module(const char* name, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto base = find_module_base(name);
        if (base != 0) {
            return base;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return 0;
}

std::string narrow_from_utf16(const char16_t* data, int32_t len) {
    std::string out;
    out.reserve(len);
    for (int32_t i = 0; i < len; ++i) {
        char16_t c = data[i];
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('?');
        }
    }
    return out;
}

std::string describe_il2cpp_string(Il2CppString* str) {
    if (str == nullptr) {
        return "<null>";
    }

    const int32_t len = str->length;
    if (len <= 0 || len > 0x1000) {
        char buf[64];
        snprintf(buf, sizeof(buf), "<length=%d>", len);
        return std::string(buf);
    }

    return narrow_from_utf16(str->chars, len);
}

void* get_second_arg(DobbyRegisterContext* ctx) {
#if defined(__aarch64__)
    return reinterpret_cast<void*>(ctx->general.regs.x1);
#elif defined(__arm__)
    return reinterpret_cast<void*>(ctx->general.regs.r1);
#elif defined(__x86_64__)
    return reinterpret_cast<void*>(ctx->general.regs.rsi);
#elif defined(__i386__)
    // esp + 4 holds the second argument on i386 cdecl/stdcall
    return nullptr;
#else
    return nullptr;
#endif
}

void set_second_arg(DobbyRegisterContext* ctx, void* value) {
#if defined(__aarch64__)
    ctx->general.regs.x1 = reinterpret_cast<uint64_t>(value);
#elif defined(__arm__)
    ctx->general.regs.r1 = reinterpret_cast<uint32_t>(value);
#elif defined(__x86_64__)
    ctx->general.regs.rsi = reinterpret_cast<uint64_t>(value);
#elif defined(__i386__)
    // Not implemented for i386; extend if needed.
    (void) value;
#else
    (void) value;
#endif
}

void setter_pre_handler(void* address, DobbyRegisterContext* ctx) {
    const MethodSpec* spec = nullptr;
    auto it = g_hookMap.find(address);
    if (it != g_hookMap.end()) {
        spec = it->second;
    }

    if (g_managedText == nullptr) {
        return;
    }

    void* arg = get_second_arg(ctx);
    const auto original = describe_il2cpp_string(reinterpret_cast<Il2CppString*>(arg));

    if (!original.empty() && original != kTargetText) {
        LOGI("[Setter] %s 原始内容: %s", spec ? spec->name : "<unknown>", original.c_str());
    }

    set_second_arg(ctx, g_managedText);
}

bool prepare_il2cpp_factory() {
    g_stringNew = reinterpret_cast<Il2CppStringNewFn>(
            DobbySymbolResolver(kLibIl2cpp, "il2cpp_string_new"));
    if (!g_stringNew) {
        g_stringNew = reinterpret_cast<Il2CppStringNewFn>(
                DobbySymbolResolver(nullptr, "il2cpp_string_new"));
    }

    if (!g_stringNew) {
        LOGE("无法找到 il2cpp_string_new");
        return false;
    }

    g_managedText = g_stringNew(kTargetText);
    if (!g_managedText) {
        LOGE("创建托管字符串失败");
        return false;
    }

    return true;
}

void install_hooks() {
#if !defined(__arm__) && !defined(__aarch64__) && !defined(__x86_64__) && !defined(__i386__)
    LOGE("当前架构不支持文本拦截");
    return;
#endif

    const auto base = wait_for_module(kLibIl2cpp, std::chrono::seconds(10));
    if (base == 0) {
        LOGE("等待 %s 载入超时", kLibIl2cpp);
        return;
    }

    LOGI("%s loaded @ 0x%" PRIxPTR, kLibIl2cpp, base);

    if (!prepare_il2cpp_factory()) {
        return;
    }

    std::vector<void*> targets;
    targets.reserve(sizeof(kTextSetters) / sizeof(kTextSetters[0]));
    for (const auto& spec : kTextSetters) {
        void* target = reinterpret_cast<void*>(base + spec.rva);
        g_hookMap[target] = &spec;
        targets.push_back(target);
    }

    for (void* target : targets) {
        const auto* spec = g_hookMap[target];
        const int ret = DobbyInstrument(target, setter_pre_handler);
        if (ret == 0) {
            LOGI("Hooked %s @ %p", spec->name, target);
        } else {
            LOGE("Hook %s 失败, ret=%d", spec->name, ret);
        }
    }
}

void init_worker() {
    install_hooks();
}

}  // namespace

extern "C" JNIEXPORT void JNICALL
Java_com_tools_module_NativeBridge_init(JNIEnv* env, jclass /*clazz*/) {
    if (g_initialized.exchange(true)) {
        LOGI("Native init already completed");
        return;
    }

    LOGI("Native init start");
    std::thread(init_worker).detach();

    (void) env;
}

jint JNI_OnLoad(JavaVM* vm, void*) {
    LOGI("JNI_OnLoad");
    (void) vm;
    return JNI_VERSION_1_6;
}
