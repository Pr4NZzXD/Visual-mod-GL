#pragma once
#include "shared.h"

namespace vm {
namespace esp {

// Outlines that follow the shape of items and entities, visible through walls. Runs on the game's main thread.
void tick(const FxSettings& s);

}  // namespace esp
}  // namespace vm
