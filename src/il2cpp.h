#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

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
    const Il2CppType* (*field_get_type)(FieldInfo*);  // optional: used to check a field before it is written
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
    // optional: only used by the performance / fog features
    const char* (*class_get_name)(Il2CppClass*);
    Il2CppClass* (*class_get_parent)(Il2CppClass*);
    const uint16_t* (*string_chars)(Il2CppString*);
    int32_t (*string_length)(Il2CppString*);
    void (*gchandle_free)(uint32_t);
    void (*gc_collect)(int);
    void* (*resolve_icall)(const char*);
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

// Offset of a field, but only when the field has the type the code expects (the il2cpp type numbers below, or kTypeRef for
// any reference). A wrong guess would write garbage into the game, so on a mismatch it returns 0 and logs why.
constexpr int kTypeBool = 0x02, kTypeI4 = 0x08, kTypeR4 = 0x0c, kTypeValue = 0x11, kTypeRef = -1;
size_t fieldChecked(Il2CppClass* k, const char* owner, const char* name, int want);

// ---- helpers used by the fog / performance code
std::string str(Il2CppString* s);                  // managed string -> UTF-8 ("" for null)
const char* className(Il2CppObject* o);            // "" when unknown
bool isA(Il2CppObject* o, const char* name);       // the class, or one of its parents, has this name
// The engine function behind a managed method (UnityEngine.Class::method). The game strips many managed wrappers,
// the engine side is still there. nullptr when missing (logged once).
void* icall(const char* name);
bool callBool(const MethodInfo* m, void* self, void** args, bool def = false);
int callInt(const MethodInfo* m, void* self, void** args, int def = 0);
float callFloat(const MethodInfo* m, void* self, void** args, float def = 0.f);
uint32_t retain(Il2CppObject* o);                  // strong GC handle: the object stays alive
void release(uint32_t handle);
bool alive(Il2CppObject* o);                       // a UnityEngine.Object whose native side still exists
// UnityEngine.Object.FindObjectsOfType(type): the managed array (nullptr if nothing). Keep it referenced while you use it.
Il2CppObject* findObjectsOfType(Il2CppClass* k);
inline Il2CppObject** arrayData(Il2CppObject* arr) { return reinterpret_cast<Il2CppObject**>(reinterpret_cast<char*>(arr) + 0x20); }

template <typename T>
inline T* fieldPtr(Il2CppObject* o, size_t off) {
    return reinterpret_cast<T*>(reinterpret_cast<char*>(o) + off);
}

}  // namespace il2
