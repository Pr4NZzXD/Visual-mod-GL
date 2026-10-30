#pragma once
#include "shared.h"

namespace vm {
namespace perf {

// Runs on the game's main thread (a few times per frame at most).
void tick(const FxSettings& s);

}  // namespace perf
}  // namespace vm
