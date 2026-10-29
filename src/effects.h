#pragma once

namespace vm {
namespace fx {

// Both run on the game's main thread (called from the Time.deltaTime hook).
void pollTouch();  // reads the first finger from UnityEngine.Input and hands it to the menu
void tick();       // builds / updates the post-processing volume from the menu settings

}  // namespace fx
}  // namespace vm
