#pragma once
#include "shared.h"

namespace vm {
namespace cheat {

using HookInstaller = bool (*)(void* target, void* replacement, void** original, const char* what);

// Resolves the classes, fields and methods of the game by name (nothing is a fixed address) and installs the hooks the
// Cheat tab needs. Called once from the boot thread, after il2cpp is ready. Hooks that cannot be installed are logged
// and shown in the menu; the features that do not need them keep working.
void install(HookInstaller hook);

// Runs on the game's main thread, once per frame, with the newest settings from the menu.
void tick(const CheatSettings& s);

}  // namespace cheat
}  // namespace vm
