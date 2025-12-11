#ifndef IL2FUSION_UTILS_H
#define IL2FUSION_UTILS_H

#include <chrono>
#include <cstdint>
#include <string>

#include "dobby.h"

// Forward declarations for il2cpp types
struct Il2CppDomain;
struct Il2CppAssembly;
struct Il2CppImage;
struct Il2CppClass;
struct MethodInfo;
struct Il2CppThread;

namespace textutils {

struct Il2CppString {
    void* klass;
    void* monitor;
    int32_t length;
    char16_t chars[1];
};

bool ShouldFilter(const std::string& text);
std::string Utf16ToUtf8(const char16_t* data, int32_t len);
std::string DescribeIl2CppString(const Il2CppString* str);
}

namespace hookutils {
uintptr_t FindModuleBase(const char* name);
uintptr_t WaitForModule(const char* name, std::chrono::milliseconds timeout);
std::string FindModulePath(const char* name);
uintptr_t FindExportInElf(const char* path, const char* symbol, uintptr_t base);
void* GetSecondArg(DobbyRegisterContext* ctx);
void SetSecondArg(DobbyRegisterContext* ctx, void* value);
}

namespace il2cpputils {

struct Il2CppApi {
    Il2CppDomain* (*domain_get)();
    const Il2CppAssembly** (*domain_get_assemblies)(Il2CppDomain*, size_t*);
    const Il2CppImage* (*assembly_get_image)(const Il2CppAssembly*);
    Il2CppClass* (*class_from_name)(const Il2CppImage*, const char*, const char*);
    const MethodInfo* (*class_get_methods)(Il2CppClass*, void**);
    const MethodInfo* (*class_get_method_from_name)(Il2CppClass*, const char*, int);
    const char* (*method_get_name)(const MethodInfo*);
    bool (*is_vm_thread)(Il2CppThread*);
    void* (*thread_attach)(Il2CppDomain*);
};

struct TargetSpec {
    std::string full;
    std::string namespaze;
    std::string klass;
    std::string method;
};

// Parses "Namespace.Class.method" into TargetSpec; returns false if format invalid.
bool ParseTarget(const std::string& full, TargetSpec& out);

// Resolve required il2cpp symbols from a handle (dlopen/xdl_open result).
bool ResolveIl2cppApi(void* handle, Il2CppApi& api);

// Waits for VM ready (if is_vm_thread is available) and attaches current thread when possible.
bool EnsureVmReadyAndAttach(const Il2CppApi& api, int max_retry, std::chrono::milliseconds interval);

// Finds the MethodInfo by namespace/class/method using the provided API.
const MethodInfo* FindMethodInAssemblies(const Il2CppApi& api, const TargetSpec& spec);

}  // namespace il2cpputils

extern "C" {
int find_handle(const char* handle_name);
void* lookup_symbol(const char* libraryname, const char* symbolname);
void* lookup_symbol2(const char* libraryname, const char* symbolname);
const char* find_full_path(const char* libraryname);
}

#endif  // IL2FUSION_UTILS_H
