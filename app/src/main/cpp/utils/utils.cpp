#include "utils.h"

#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <thread>
#include <unistd.h>
#include <thread>
#include <vector>

#include "../il2CppDumper/il2cpp-class.h"
#include "../il2CppDumper/xdl/include/xdl.h"
#include "dobby.h"
#include "log.h"

namespace textutils {

bool ShouldFilter(const std::string& text) {
    if (text.empty()) {
        return true;
    }

    bool hasDigit = false;
    bool hasLetter = false;
    bool hasPunct = false;
    bool hasSpace = false;

    for (unsigned char ch : text) {
        if (std::isdigit(ch)) {
            hasDigit = true;
        } else if (std::isalpha(ch)) {
            hasLetter = true;
        } else if (std::ispunct(ch)) {
            hasPunct = true;
        } else if (std::isspace(ch)) {
            hasSpace = true;
        } else {
            // 包含非 ASCII 可见字符，不过滤
            return false;
        }
    }

    // 只包含数字/英文/标点/空格的组合（含任意排列）均过滤
    return hasDigit || hasLetter || hasPunct || hasSpace;
}

namespace {

void AppendUtf8(std::string& out, uint32_t cp) {
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

}  // namespace

std::string Utf16ToUtf8(const char16_t* data, int32_t len) {
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

        AppendUtf8(out, cp);
    }

    return out;
}

std::string DescribeIl2CppString(const Il2CppString* str) {
    if (str == nullptr) {
        return "<null>";
    }

    const int32_t len = str->length;
    if (len <= 0 || len > 0x1000) {
        char buf[64];
        snprintf(buf, sizeof(buf), "<length=%d>", len);
        return std::string(buf);
    }

    return Utf16ToUtf8(str->chars, len);
}

}  // namespace textutils

namespace hookutils {

uintptr_t FindModuleBase(const char* name) {
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

uintptr_t WaitForModule(const char* name, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto base = FindModuleBase(name);
        if (base != 0) {
            return base;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return 0;
}

std::string FindModulePath(const char* name) {
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

uintptr_t FindExportInElf(const char* path, const char* symbol, uintptr_t base) {
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

void* GetSecondArg(DobbyRegisterContext* ctx) {
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

void SetSecondArg(DobbyRegisterContext* ctx, void* value) {
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

}  // namespace hookutils

namespace il2cpputils {

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

bool ResolveIl2cppApi(void* handle, Il2CppApi& api) {
    auto resolve = [&](auto& fn, const char* name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(xdl_sym(handle, name, nullptr));
        if (fn == nullptr) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(handle, name));
        }
        return fn != nullptr;
    };

    bool ok = true;
    ok &= resolve(api.domain_get, "il2cpp_domain_get");
    ok &= resolve(api.domain_get_assemblies, "il2cpp_domain_get_assemblies");
    ok &= resolve(api.assembly_get_image, "il2cpp_assembly_get_image");
    ok &= resolve(api.class_from_name, "il2cpp_class_from_name");
    ok &= resolve(api.class_get_methods, "il2cpp_class_get_methods");
    ok &= resolve(api.class_get_method_from_name, "il2cpp_class_get_method_from_name");
    ok &= resolve(api.method_get_name, "il2cpp_method_get_name");
    resolve(api.thread_attach, "il2cpp_thread_attach");
    resolve(api.is_vm_thread, "il2cpp_is_vm_thread");

    if (!ok) {
        LOGE("初始化 il2cpp API 失败，部分符号缺失");
    }
    return ok;
}

bool EnsureVmReadyAndAttach(const Il2CppApi& api, int max_retry, std::chrono::milliseconds interval) {
    if (api.is_vm_thread) {
        int retry = 0;
        while (!api.is_vm_thread(nullptr) && retry < max_retry) {
            std::this_thread::sleep_for(interval);
            ++retry;
        }
        if (!api.is_vm_thread(nullptr)) {
            LOGE("il2cpp VM 未就绪");
            return false;
        }
    }
    if (api.thread_attach && api.domain_get) {
        Il2CppDomain* domain = api.domain_get();
        if (domain != nullptr) {
            api.thread_attach(domain);
        }
    }
    return true;
}

const MethodInfo* FindMethodInAssemblies(const Il2CppApi& api, const TargetSpec& spec) {
    if (!api.domain_get || !api.domain_get_assemblies || !api.assembly_get_image || !api.class_from_name) {
        LOGE("Il2Cpp API 未准备好，跳过解析 %s", spec.full.c_str());
        return nullptr;
    }

    Il2CppDomain* domain = api.domain_get();
    if (domain == nullptr) {
        return nullptr;
    }

    size_t count = 0;
    const Il2CppAssembly** assemblies = api.domain_get_assemblies(domain, &count);
    for (size_t i = 0; i < count; ++i) {
        const Il2CppImage* image = api.assembly_get_image(assemblies[i]);
        if (image == nullptr) {
            continue;
        }
        Il2CppClass* klass = api.class_from_name(image, spec.namespaze.c_str(), spec.klass.c_str());
        if (klass == nullptr) {
            continue;
        }

        const MethodInfo* method = nullptr;
        if (api.class_get_method_from_name) {
            method = api.class_get_method_from_name(klass, spec.method.c_str(), 1);
            if (method == nullptr) {
                method = api.class_get_method_from_name(klass, spec.method.c_str(), 0);
            }
        }
        if (method == nullptr && api.class_get_methods && api.method_get_name) {
            void* iter = nullptr;
            while ((method = api.class_get_methods(klass, &iter)) != nullptr) {
                const char* name = api.method_get_name(method);
                if (name != nullptr && spec.method == name) {
                    break;
                }
            }
        }
        if (method != nullptr && method->methodPointer != nullptr) {
            return method;
        }
    }
    return nullptr;
}

}  // namespace il2cpputils

extern "C" {

int find_handle(const char* handle_name) {
    FILE* fp;
    char filename[32];
    char line[1024];
    const int pid = getpid();
    if (pid < 0) {
        strcpy(filename, "/proc/self/maps");
    } else {
        snprintf(filename, sizeof(filename), "/proc/%d/maps", pid);
    }
    fp = fopen(filename, "r");
    if (fp != nullptr) {
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, handle_name)) {
                LOGI("find_handle -> %s", line);
                fclose(fp);
                return 1;
            }
        }
        fclose(fp);
    }
    return 0;
}

void* lookup_symbol(const char* libraryname, const char* symbolname) {
    void* handle = dlopen(libraryname, RTLD_GLOBAL | RTLD_NOW);
    if (handle != nullptr) {
        void* symbol = dlsym(handle, symbolname);
        if (symbol != nullptr) {
            return symbol;
        }
        LOGE("lookup_symbol: %s not found in %s", symbolname, libraryname);
    }
    return nullptr;
}

void* lookup_symbol2(const char* libraryname, const char* symbolname) {
    FILE* fp;
    char* pch;
    void* path = malloc(1024);
    char filename[32];
    char line[1024];
    const int pid = getpid();
    void* handle = nullptr;

    if (pid < 0) {
        strcpy(filename, "/proc/self/maps");
    } else {
        snprintf(filename, sizeof(filename), "/proc/%d/maps", pid);
    }
    fp = fopen(filename, "r");
    if (fp != nullptr) {
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, libraryname)) {
                pch = strtok(line, " ");
                while (pch != nullptr && !strstr(pch, libraryname)) {
                    pch = strtok(nullptr, " ");
                }
                memset(path, 0, 1024);
                memcpy(path, pch, strlen(pch) - 1);

                handle = lookup_symbol(static_cast<const char*>(path), symbolname);
                if (handle != nullptr) {
                    LOGI("lookup_symbol2 success: %s %s %p", static_cast<char*>(path), symbolname, handle);
                    break;
                }
            }
        }
        fclose(fp);
    }
    free(path);
    return handle;
}

const char* find_full_path(const char* libraryname) {
    FILE* fp;
    char* pch;
    void* path = malloc(1024);
    memset(path, 0, 1024);
    char filename[32];
    char line[1024];
    const int pid = getpid();

    if (pid < 0) {
        strcpy(filename, "/proc/self/maps");
    } else {
        snprintf(filename, sizeof(filename), "/proc/%d/maps", pid);
    }
    fp = fopen(filename, "r");
    if (fp != nullptr) {
        while (fgets(line, sizeof(line), fp)) {
            if (strstr(line, libraryname)) {
                pch = strtok(line, " ");
                while (pch != nullptr && !strstr(pch, libraryname)) {
                    pch = strtok(nullptr, " ");
                }
                memcpy(path, pch, strlen(pch) - 1);
                fclose(fp);
                return static_cast<const char*>(path);
            }
        }
        fclose(fp);
    }
    free(path);
    return nullptr;
}

}  // extern "C"
