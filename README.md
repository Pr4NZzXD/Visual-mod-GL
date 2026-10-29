# VisualMobile (native)

The same idea as VisualMobile, but as a plain `.so` library instead of a MelonLoader mod. It is for **Granny: Legacy** on Android (64-bit). It only changes how the game looks. Single player only.

Tap the little eye on the screen to open the menu. Drag the eye, or the top bar of the menu, to move it.

**This is the first version.** It has less than the MelonLoader mod. More will come once this one is confirmed to run.

## Features

**Colour**
- Exposure
- Saturation
- Contrast
- Hue
- Warmth (cool to warm)
- Tint (green to magenta)

**Menu**
- Floating eye icon
- FPS counter you can move around
- Menu size, opacity and icon size
- Lock the position of the icon and the menu
- Settings are saved

**Not in this version yet:** presets, bloom, vignette, film grain, fog and the performance options.

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

Menu by [Dear ImGui](https://github.com/ocornut/imgui). Hooks by [Dobby](https://github.com/jmpews/Dobby). Both are downloaded by the build, not included here.
