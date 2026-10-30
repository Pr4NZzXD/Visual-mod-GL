#include <EGL/egl.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstring>

#include "And64InlineHook.hpp"
#include "effects.h"
#include "fog.h"
#include "il2cpp.h"
#include "log.h"
#include "menu.h"
#include "perf.h"
#include "shared.h"

using namespace vm;

namespace {

// ---------------------------------------------------------------- hooking helper (And64InlineHook, arm64)
bool installHook(void* target, void* replacement, void** original, const char* what) {
    if (!target) {
        logf("hook %s: address not found", what);
        return false;
    }
    if (original) *original = nullptr;
    A64HookFunction(target, replacement, original);
    // A64HookFunction returns nothing; when it fails the trampoline pointer stays null
    bool ok = !original || *original != nullptr;
    logf("hook %s: %s", what, ok ? "ok" : "FAILED");
    return ok;
}

// ---------------------------------------------------------------- the game's main thread
std::atomic<pid_t> gMainTid{0};
std::atomic<int> gCalls{0};
std::atomic<int64_t> gLastTickMs{0};

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool onMainThread() {
    pid_t tid = (pid_t)syscall(SYS_gettid);
    pid_t m = gMainTid.load();
    if (m) return tid == m;
    char name[32] = {0};
    prctl(PR_GET_NAME, name, 0, 0, 0);
    if (strcmp(name, "UnityMain") == 0) {
        gMainTid.store(tid);
        logf("main thread: %d (UnityMain)", (int)tid);
        return true;
    }
    // fallback if the thread has another name: after many calls take the one that is calling
    if (++gCalls > 4000) {
        gMainTid.store(tid);
        logf("main thread: %d (assumed, thread name was \"%s\")", (int)tid, name);
        return true;
    }
    return false;
}

// UnityEngine.Time.get_deltaTime is called many times per frame from the main thread: a safe place to run
// code that may call into Unity.
float (*gOrigDt)(const void*) = nullptr;
thread_local bool gInTick = false;

float hookDeltaTime(const void* mi) {
    float r = gOrigDt(mi);
    if (!gInTick && onMainThread()) {
        int64_t now = nowMs();
        if (now - gLastTickMs.load() >= 4) {
            gLastTickMs.store(now);
            gInTick = true;
            fx::pollTouch();
            fx::tick();
            static FxSettings cur;
            static unsigned curVersion = 0;
            takeFx(cur, curVersion);
            fog::tick(cur);
            perf::tick(cur);
            gInTick = false;
        }
    }
    return r;
}

// ---------------------------------------------------------------- the render thread
EGLBoolean (*gOrigSwap)(EGLDisplay, EGLSurface) = nullptr;

EGLBoolean hookSwap(EGLDisplay dpy, EGLSurface surf) {
    if (eglGetCurrentContext() != EGL_NO_CONTEXT) {
        EGLint w = 0, h = 0;
        eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
        eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
        menuFrame(w, h);
    }
    return gOrigSwap(dpy, surf);
}

// ---------------------------------------------------------------- do not turn the camera while the menu is touched
void (*gOrigTouch1)(void*) = nullptr;
void (*gOrigTouch2)(void*) = nullptr;

void hookTouch1(void* self) {
    if (gBlockTouch.load()) return;
    gOrigTouch1(self);
}

void hookTouch2(void* self) {
    if (gBlockTouch.load()) return;
    gOrigTouch2(self);
}

void blockTouchOf(const char* name, void* replacement, void** original) {
    Il2CppClass* k = il2::findClass("", "MobileFPS");
    if (!k) {
        logf("touch block: class MobileFPS not found (not Granny Legacy?), skipped");
        return;
    }
    const MethodInfo* m = il2::findMethod(k, name, 0);
    if (!m) {
        logf("touch block: MobileFPS.%s() not found, skipped", name);
        return;
    }
    if (il2::A.type_get_type(il2::A.method_get_return_type(m)) != 1) {  // 1 = void
        logf("touch block: MobileFPS.%s returns a value, skipped", name);
        return;
    }
    installHook(il2::fnPtr(m), replacement, original, name);
}

// ---------------------------------------------------------------- loader thread
void* bootstrap(void*) {
    logInit();

    void* h = nullptr;
    for (int i = 0; i < 1800 && !h; i++) {
        h = dlopen("libil2cpp.so", RTLD_NOW | RTLD_NOLOAD);
        if (!h) usleep(100 * 1000);
    }
    if (!h) {
        logf("libil2cpp.so never showed up, giving up");
        return nullptr;
    }
    logf("libil2cpp.so found");
    if (!il2::load(h)) {
        logf("some il2cpp exports are missing, giving up");
        return nullptr;
    }

    // libil2cpp.so is mapped long before the game calls il2cpp_init(). il2cpp_domain_get() builds the domain
    // object from il2cpp_defaults, which are still empty until then, so calling it too early crashes the game.
    // il2cpp_get_corlib() only reads a global, so it is safe to poll: it stays null until the runtime is up.
    using GetCorlibFn = void* (*)();
    GetCorlibFn getCorlib = reinterpret_cast<GetCorlibFn>(dlsym(h, "il2cpp_get_corlib"));
    if (getCorlib) {
        for (int i = 0; i < 6000 && !getCorlib(); i++) usleep(50 * 1000);
        if (!getCorlib()) {
            logf("il2cpp runtime never initialised, giving up");
            return nullptr;
        }
        logf("il2cpp runtime initialised");
        usleep(1500 * 1000);  // let il2cpp_init() finish setting up before touching the domain
    } else {
        logf("il2cpp_get_corlib not found, waiting 15 s before touching il2cpp");
        sleep(15);
    }

    bool attached = false;
    bool ready = false;
    for (int i = 0; i < 3000 && !ready; i++) {
        Il2CppDomain* d = il2::A.domain_get();
        if (d) {
            if (!attached) {
                il2::A.thread_attach(d);
                attached = true;
                logf("attached to the il2cpp domain");
            }
            ready = il2::ready();
        }
        if (!ready) usleep(100 * 1000);
    }
    if (!ready) {
        logf("il2cpp did not get ready, giving up");
        return nullptr;
    }
    logf("il2cpp ready");

    // 1. the per-frame hook on the main thread
    Il2CppClass* kTime = il2::findClass("UnityEngine", "Time");
    const MethodInfo* mDt = il2::findMethod(kTime, "get_deltaTime", 0);
    if (!installHook(il2::fnPtr(mDt), (void*)hookDeltaTime, (void**)&gOrigDt, "Time.get_deltaTime")) {
        logf("without this hook nothing can run, giving up");
        return nullptr;
    }

    // 2. the draw hook on the render thread
    void* egl = dlopen("libEGL.so", RTLD_NOW);
    void* swap = egl ? dlsym(egl, "eglSwapBuffers") : nullptr;
    installHook(swap, (void*)hookSwap, (void**)&gOrigSwap, "eglSwapBuffers");

    // 3. optional: do not rotate the camera while the menu is touched
    blockTouchOf("GetTouchInput", (void*)hookTouch1, (void**)&gOrigTouch1);
    blockTouchOf("OldTouchInput", (void*)hookTouch2, (void**)&gOrigTouch2);

    logf("all set");
    return nullptr;
}

}  // namespace

// Runs when the game loads the library (System.loadLibrary from the patched activity).
__attribute__((constructor)) static void onLoad() {
    pthread_t t;
    if (pthread_create(&t, nullptr, bootstrap, nullptr) == 0) pthread_detach(t);
}
