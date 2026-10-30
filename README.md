# VisualMobile (native)

The same idea as VisualMobile, but as a plain `.so` library instead of a MelonLoader mod. It is for **Granny: Legacy** on Android (64-bit). It only changes how the game looks. Single player only.

Tap the little eye on the screen to open the menu. Drag the eye, or the top bar of the menu, to move it.

## Features

**Color**
- Exposure, gamma
- Saturation, contrast, hue
- Warmth (cool to warm) and tint (green to magenta)
- Colour filter with your own colour

**Effects**
- Bloom
- Vignette
- Film grain
- Chromatic aberration
- Lens distortion
- Motion blur
- Ambient occlusion (new)
- Depth of field (new)

**Presets**
- Default, Warm, Cold, Vivid, Cinematic, Noir
- New: Night vision, VHS, Dreamy, Horror, Golden hour, Sepia

**Fog**
- Game default, no fog, or your own colour and thickness

**Dark scenes**
- Brightness, light boost, field of view

**Performance**
- Low and ultra low graphics
- No textures, no shadows, no post-processing, hide particles
- Anti-aliasing, anisotropic filtering and LOD bias (new)
- Draw distance
- Resolution scale, adaptive resolution, custom screen size
- FPS limit
- Clean memory

**Menu**
- Floating eye icon
- FPS counter and frame graph you can move around
- Menu size, opacity and icon size
- Your own colours, rainbow mode, rounded corners
- Lock the position of the icon and the menu
- Settings are saved

Ambient occlusion and depth of field cost some FPS, so use them with care on weak phones. They only work if the game has them. If it does not, the General page tells you.

## Get the library

1. Make a new repo on GitHub and upload everything in this folder.
2. Open the Actions tab and wait for the build to finish.
3. Download `libvisualmobile` from the artifacts of the run. Inside is `libvisualmobile.so`.

## Put it in the game (MT Manager)

1. Open the game's APK in MT Manager.
2. Go to `lib/arm64-v8a` and add `libvisualmobile.so` there.
3. Open `classes.dex` and find the main activity. In most Unity games it is `com/unity3d/player/UnityPlayerActivity`. If it is not there, look at the activity with the MAIN / LAUNCHER entry in `AndroidManifest.xml`.
4. Open its `onCreate` method. Right after the line that calls `onCreate` of the parent class (`invoke-super`), add these two lines:

```
const-string v0, "visualmobile"
invoke-static {v0}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
```

   If the method says `.locals 0`, change it to `.locals 1`.
5. Save, let MT Manager sign the APK, and install it.

## If something goes wrong

Look at `visualmobile.log` in `Android/data/<game package>/files/`. It says how far the loading got. Send it along when you report a problem.

- **No eye on the screen:** the log tells whether the library was loaded and whether the hooks worked.
- **Eye shows but no effects:** the log tells whether the post-processing parts of the game were found.
- Only 64-bit (arm64) phones are supported.
- It needs the game to draw with OpenGL ES. If the game uses Vulkan, the menu will not show.

## Credits

Menu by [Dear ImGui](https://github.com/ocornut/imgui). Hooks by [And64InlineHook](https://github.com/Rprop/And64InlineHook). Both are downloaded by the build, not included here.
