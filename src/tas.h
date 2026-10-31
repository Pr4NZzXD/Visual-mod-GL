#pragma once
#include "shared.h"

namespace vm {
namespace tas {

using HookInstaller = bool (*)(void* target, void* replacement, void** original, const char* what);

// Finds the game's classes by name and hooks what the TAS tab needs (item pick-up, the four escapes, the bed catches).
// Called once from the boot thread, after il2cpp is ready. Whatever cannot be hooked is logged and shown in the tab.
void install(HookInstaller hook);

// Runs on the game's main thread, once per frame, with the newest settings from the menu.
void tick(const TasSettings& s);

}  // namespace tas
}  // namespace vm
