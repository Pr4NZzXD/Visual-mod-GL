#pragma once
#include <cstddef>
#include <cstdint>

// Only the il2cpp API that is needed. Everything is looked up by name at run time (dlsym), so the mod
// does not depend on addresses and survives game updates as long as the class and method names stay.

struct Il2CppDomain;
struct Il2CppAssembly;
struct Il2CppImage;
struct Il2CppClass;
struct Il2CppObject;
struct Il2CppString;
struct Il2CppException;
struct Il2CppType;
struct MethodInfo;
struct FieldInfo;

namespace il2 {

struct Api {
    Il2CppDomain* (*domain_get)();
    Il2CppAssembly** (*domain_get_assemblies)(const Il2CppDomain*, size_t*);
    const Il2CppImage* (*assembly_get_image)(const Il2CppAssembly*);
    Il2CppClass* (*class_from_name)(const Il2CppImage*, const char*, const char*);
    const MethodInfo* (*class_get_method_from_name)(Il2CppClass*, const char*, int);
    const MethodInfo* (*class_get_methods)(Il2CppClass*, void**);
    const char* (*method_get_name)(const MethodInfo*);
    uint32_t (*method_get_param_count)(const MethodInfo*);
    const Il2CppType* (*method_get_param)(const MethodInfo*, uint32_t);
    const Il2CppType* (*method_get_return_type)(const MethodInfo*);
    char* (*type_get_name)(const Il2CppType*);
    int (*type_get_type)(const Il2CppType*);
    FieldInfo* (*class_get_field_from_name)(Il2CppClass*, const char*);
    size_t (*field_get_offset)(FieldInfo*);
    Il2CppObject* (*runtime_invoke)(const MethodInfo*, void*, void**, Il2CppException**);
    Il2CppObject* (*object_new)(Il2CppClass*);
    void* (*object_unbox)(Il2CppObject*);
    Il2CppClass* (*object_get_class)(Il2CppObject*);
    Il2CppString* (*string_new)(const char*);
    const Il2CppType* (*class_get_type)(Il2CppClass*);
    Il2CppObject* (*type_get_object)(const Il2CppType*);
    void* (*thread_attach)(Il2CppDomain*);
    uint32_t (*gchandle_new)(Il2CppObject*, int);
    size_t (*array_length)(void*);
    void (*gc_wbarrier_set_field)(Il2CppObject*, void**, void*);  // optional
};

extern Api A;

// dlsym every function from libil2cpp.so. Returns false (and logs the names) if one is missing.
bool load(void* handle);

// Is the runtime up and are the game's assemblies loaded?
bool ready();

Il2CppClass* findClass(const char* ns, const char* name);
// param0: part of the name of the first parameter's type, to pick one overload ("System.Type").
const MethodInfo* findMethod(Il2CppClass* k, const char* name, int argc, const char* param0 = nullptr);
void* fnPtr(const MethodInfo* m);  // address of the native code of a method
Il2CppObject* invoke(const MethodInfo* m, void* self, void** args, bool* ok = nullptr);
Il2CppObject* typeObject(Il2CppClass* k);  // System.Type of a class
Il2CppClass* classOf(Il2CppObject* o);
void* unbox(Il2CppObject* o);
size_t fieldOffset(Il2CppClass* k, const char* name);  // 0 when missing

template <typename T>
inline T* fieldPtr(Il2CppObject* o, size_t off) {
    return reinterpret_cast<T*>(reinterpret_cast<char*>(o) + off);
}

}  // namespace il2
