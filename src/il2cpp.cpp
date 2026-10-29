#include "il2cpp.h"

#include <dlfcn.h>

#include <cstring>
#include <map>
#include <string>

#include "log.h"

namespace il2 {

Api A = {};

static bool sym(void* h, const char* name, void** out, bool optional = false) {
    *out = dlsym(h, name);
    if (!*out && !optional) {
        vm::logf("il2cpp: missing export %s", name);
        return false;
    }
    return true;
}

#define NEED(field, name) ok &= sym(handle, name, reinterpret_cast<void**>(&A.field))

bool load(void* handle) {
    bool ok = true;
    NEED(domain_get, "il2cpp_domain_get");
    NEED(domain_get_assemblies, "il2cpp_domain_get_assemblies");
    NEED(assembly_get_image, "il2cpp_assembly_get_image");
    NEED(class_from_name, "il2cpp_class_from_name");
    NEED(class_get_method_from_name, "il2cpp_class_get_method_from_name");
    NEED(class_get_methods, "il2cpp_class_get_methods");
    NEED(method_get_name, "il2cpp_method_get_name");
    NEED(method_get_param_count, "il2cpp_method_get_param_count");
    NEED(method_get_param, "il2cpp_method_get_param");
    NEED(method_get_return_type, "il2cpp_method_get_return_type");
    NEED(type_get_name, "il2cpp_type_get_name");
    NEED(type_get_type, "il2cpp_type_get_type");
    NEED(class_get_field_from_name, "il2cpp_class_get_field_from_name");
    NEED(field_get_offset, "il2cpp_field_get_offset");
    NEED(runtime_invoke, "il2cpp_runtime_invoke");
    NEED(object_new, "il2cpp_object_new");
    NEED(object_unbox, "il2cpp_object_unbox");
    NEED(object_get_class, "il2cpp_object_get_class");
    NEED(string_new, "il2cpp_string_new");
    NEED(class_get_type, "il2cpp_class_get_type");
    NEED(type_get_object, "il2cpp_type_get_object");
    NEED(thread_attach, "il2cpp_thread_attach");
    NEED(gchandle_new, "il2cpp_gchandle_new");
    NEED(array_length, "il2cpp_array_length");
    sym(handle, "il2cpp_gc_wbarrier_set_field", reinterpret_cast<void**>(&A.gc_wbarrier_set_field), true);
    return ok;
}

bool ready() {
    Il2CppDomain* d = A.domain_get();
    if (!d) return false;
    size_t n = 0;
    Il2CppAssembly** asms = A.domain_get_assemblies(d, &n);
    if (!asms || n < 4) return false;
    return findClass("UnityEngine", "Time") != nullptr;
}

static std::map<std::string, Il2CppClass*> gClassCache;

Il2CppClass* findClass(const char* ns, const char* name) {
    std::string key = std::string(ns) + "|" + name;
    auto it = gClassCache.find(key);
    if (it != gClassCache.end()) return it->second;

    Il2CppDomain* d = A.domain_get();
    if (!d) return nullptr;
    size_t n = 0;
    Il2CppAssembly** asms = A.domain_get_assemblies(d, &n);
    if (!asms) return nullptr;
    for (size_t i = 0; i < n; i++) {
        const Il2CppImage* img = A.assembly_get_image(asms[i]);
        if (!img) continue;
        Il2CppClass* k = A.class_from_name(img, ns, name);
        if (k) {
            gClassCache[key] = k;
            return k;
        }
    }
    return nullptr;  // not cached: the assembly may still be loading
}

const MethodInfo* findMethod(Il2CppClass* k, const char* name, int argc, const char* param0) {
    if (!k) return nullptr;
    if (!param0) return A.class_get_method_from_name(k, name, argc);

    void* iter = nullptr;
    while (const MethodInfo* m = A.class_get_methods(k, &iter)) {
        if (strcmp(A.method_get_name(m), name) != 0) continue;
        if ((int)A.method_get_param_count(m) != argc) continue;
        if (argc > 0) {
            const char* tn = A.type_get_name(A.method_get_param(m, 0));
            if (!tn || !strstr(tn, param0)) continue;
        }
        return m;
    }
    return nullptr;
}

void* fnPtr(const MethodInfo* m) {
    // MethodInfo starts with the pointer to the native code
    return m ? *reinterpret_cast<void* const*>(m) : nullptr;
}

Il2CppObject* invoke(const MethodInfo* m, void* self, void** args, bool* ok) {
    Il2CppException* exc = nullptr;
    Il2CppObject* r = A.runtime_invoke(m, self, args, &exc);
    if (ok) *ok = (exc == nullptr);
    if (exc) vm::logf("il2cpp: exception inside %s", A.method_get_name(m));
    return r;
}

Il2CppObject* typeObject(Il2CppClass* k) { return A.type_get_object(A.class_get_type(k)); }
Il2CppClass* classOf(Il2CppObject* o) { return A.object_get_class(o); }
void* unbox(Il2CppObject* o) { return o ? A.object_unbox(o) : nullptr; }

size_t fieldOffset(Il2CppClass* k, const char* name) {
    FieldInfo* f = A.class_get_field_from_name(k, name);
    return f ? A.field_get_offset(f) : 0;
}

}  // namespace il2
