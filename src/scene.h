#pragma once
#include "shared.h"

namespace vm {
namespace scene {

// Dark-scene helpers: flat ambient light, a multiplier for every light, the camera's field of view.
// Runs on the game's main thread.
void tick(const FxSettings& s);
void onSceneLoaded();

}  // namespace scene
}  // namespace vm
