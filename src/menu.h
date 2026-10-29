#pragma once

namespace vm {

// Called on the render thread, right before the game presents a frame (OpenGL context is current).
// Draws the eye icon, the panel and the FPS counter with Dear ImGui.
void menuFrame(int width, int height);

}  // namespace vm
