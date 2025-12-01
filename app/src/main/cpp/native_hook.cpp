#include <android/log.h>
#include <jni.h>

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "dobby.h"

#define LOG_TAG "[TextExtractTool]"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

constexpr const char* kLibIl2cpp = "libil2cpp.so";
constexpr const char* kTargetText = "Hook Test";

struct Il2CppString {
    void* klass;
    void* monitor;
    int32_t length;
    char16_t chars[1];
};

using Il2CppStringNewFn = Il2CppString* (*)(const char*);

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

std::string find_module_path(const char* name) {
    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp) {
        return {};
    }

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, name) == nullptr) {
            continue;
        }
        char* path = strchr(line, '/');
        if (path != nullptr) {
            const size_t len = strlen(path);
            if (len > 0 && path[len - 1] == '\n') {
                path[len - 1] = '\0';
            }
            fclose(fp);
            return std::string(path);
        }
    }

    fclose(fp);
    return {};
}

uintptr_t find_export_in_elf(const char* path, const char* symbol, uintptr_t base) {
    if (path == nullptr || symbol == nullptr || base == 0) {
        return 0;
    }

    FILE* fp = fopen(path, "rb");
    if (!fp) {
        return 0;
    }

    Elf64_Ehdr ehdr64{};
    if (fread(&ehdr64, 1, sizeof(ehdr64), fp) != sizeof(ehdr64)) {
        fclose(fp);
        return 0;
    }

    if (memcmp(ehdr64.e_ident, ELFMAG, SELFMAG) != 0) {
        fclose(fp);
        return 0;
    }

    const bool is64 = (ehdr64.e_ident[EI_CLASS] == ELFCLASS64);
    fseek(fp, 0, SEEK_SET);  // rewind for reuse

    if (is64) {
        if (ehdr64.e_shentsize != sizeof(Elf64_Shdr)) {
            fclose(fp);
            return 0;
        }
        std::vector<Elf64_Shdr> shdrs(ehdr64.e_shnum);
        fseek(fp, static_cast<long>(ehdr64.e_shoff), SEEK_SET);
        if (fread(shdrs.data(), sizeof(Elf64_Shdr), shdrs.size(), fp) != shdrs.size()) {
            fclose(fp);
            return 0;
        }

        for (const auto& shdr : shdrs) {
            if (shdr.sh_type != SHT_DYNSYM || shdr.sh_entsize != sizeof(Elf64_Sym)) {
                continue;
            }
            if (shdr.sh_link >= shdrs.size()) {
                continue;
            }
            const Elf64_Shdr& strhdr = shdrs[shdr.sh_link];
            std::vector<char> strtab(strhdr.sh_size);
            fseek(fp, static_cast<long>(strhdr.sh_offset), SEEK_SET);
            if (fread(strtab.data(), 1, strtab.size(), fp) != strtab.size()) {
                continue;
            }

            const size_t count = shdr.sh_size / sizeof(Elf64_Sym);
            std::vector<Elf64_Sym> syms(count);
            fseek(fp, static_cast<long>(shdr.sh_offset), SEEK_SET);
            if (fread(syms.data(), sizeof(Elf64_Sym), syms.size(), fp) != syms.size()) {
                continue;
            }

            for (const auto& sym : syms) {
                if (sym.st_name >= strtab.size()) {
                    continue;
                }
                const char* name = strtab.data() + sym.st_name;
                if (strcmp(name, symbol) == 0 && sym.st_value != 0) {
                    fclose(fp);
                    return base + static_cast<uintptr_t>(sym.st_value);
                }
            }
        }
    } else {
        Elf32_Ehdr ehdr32{};
        if (fread(&ehdr32, 1, sizeof(ehdr32), fp) != sizeof(ehdr32)) {
            fclose(fp);
            return 0;
        }
        if (ehdr32.e_shentsize != sizeof(Elf32_Shdr)) {
            fclose(fp);
            return 0;
        }
        std::vector<Elf32_Shdr> shdrs(ehdr32.e_shnum);
        fseek(fp, static_cast<long>(ehdr32.e_shoff), SEEK_SET);
        if (fread(shdrs.data(), sizeof(Elf32_Shdr), shdrs.size(), fp) != shdrs.size()) {
            fclose(fp);
            return 0;
        }

        for (const auto& shdr : shdrs) {
            if (shdr.sh_type != SHT_DYNSYM || shdr.sh_entsize != sizeof(Elf32_Sym)) {
                continue;
            }
            if (shdr.sh_link >= shdrs.size()) {
                continue;
            }
            const Elf32_Shdr& strhdr = shdrs[shdr.sh_link];
            std::vector<char> strtab(strhdr.sh_size);
            fseek(fp, static_cast<long>(strhdr.sh_offset), SEEK_SET);
            if (fread(strtab.data(), 1, strtab.size(), fp) != strtab.size()) {
                continue;
            }

            const size_t count = shdr.sh_size / sizeof(Elf32_Sym);
            std::vector<Elf32_Sym> syms(count);
            fseek(fp, static_cast<long>(shdr.sh_offset), SEEK_SET);
            if (fread(syms.data(), sizeof(Elf32_Sym), syms.size(), fp) != syms.size()) {
                continue;
            }

            for (const auto& sym : syms) {
                if (sym.st_name >= strtab.size()) {
                    continue;
                }
                const char* name = strtab.data() + sym.st_name;
                if (strcmp(name, symbol) == 0 && sym.st_value != 0) {
                    fclose(fp);
                    return base + static_cast<uintptr_t>(sym.st_value);
                }
            }
        }
    }

    fclose(fp);
    return 0;
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back('?');
    }
}

std::string utf16_to_utf8(const char16_t* data, int32_t len) {
    std::string out;
    out.reserve(static_cast<size_t>(len) * 3);

    for (int32_t i = 0; i < len; ++i) {
        uint32_t cp = data[i];

        // Handle surrogate pairs
        if (cp >= 0xD800 && cp <= 0xDBFF && (i + 1) < len) {
            const uint32_t low = data[i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                ++i;
            } else {
                cp = '?';
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = '?';
        }

        append_utf8(out, cp);
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

    return utf16_to_utf8(str->chars, len);
}

void* get_second_arg(DobbyRegisterContext* ctx) {
#if defined(__aarch64__)
    return reinterpret_cast<void*>(ctx->general.regs.x1);
#elif defined(__arm__)
    return reinterpret_cast<void*>(ctx->general.regs.r1);
#elif defined(__x86_64__)
    return reinterpret_cast<void*>(ctx->general.regs.rsi);
#elif defined(__i386__)
    return nullptr;  // extend if needed
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
    (void) value;  // extend if needed
#else
    (void) value;
#endif
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

    void* arg = get_second_arg(ctx);
    const auto original = describe_il2cpp_string(reinterpret_cast<Il2CppString*>(arg));

    if (!original.empty() && original != kTargetText) {
        LOGI("[Setter] RVA 0x%" PRIxPTR " 原始内容: %s", rva, original.c_str());
    }

    if (!g_stringNew) {
        return;
    }

    Il2CppString* newStr = nullptr;
    try {
        newStr = g_stringNew(kTargetText);
    } catch (...) {
        LOGE("g_stringNew call failed");
    }

    if (newStr == nullptr) {
        return;
    }

    set_second_arg(ctx, newStr);
}

void* open_il2cpp_handle() {
    // 优先把符号引入全局命名空间，避免重复映射
    void* handle = dlopen(kLibIl2cpp, RTLD_NOW | RTLD_GLOBAL | RTLD_NOLOAD);
    if (handle) {
        return handle;
    }

    const std::string path = find_module_path(kLibIl2cpp);
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
        const std::string path = find_module_path(kLibIl2cpp);
        const uintptr_t addr = find_export_in_elf(path.c_str(), "il2cpp_string_new", g_il2cpp_base);
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
    g_il2cpp_base = wait_for_module(kLibIl2cpp, std::chrono::seconds(10));
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

jint JNI_OnLoad(JavaVM* vm, void*) {
    LOGI("JNI_OnLoad");
    (void) vm;
    return JNI_VERSION_1_6;
}
