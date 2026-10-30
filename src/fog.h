#pragma once
#include "shared.h"

namespace vm {
namespace fog {

// Both run on the game's main thread.
void tick(const FxSettings& s);
void onSceneLoaded();  // the scene reset RenderSettings: look at them again

}  // namespace fog
}  // namespace vm
