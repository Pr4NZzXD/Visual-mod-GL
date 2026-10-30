# VisualMobile (native)

The same idea as VisualMobile, but as a plain `.so` library instead of a MelonLoader mod. It is for **Granny: Legacy** on Android (64-bit, arm64). It changes how the game looks and runs. Single player only.

Tap the little eye on the screen to open the menu. Drag the eye, or the top bar of the menu, to move it. Drag on an empty part of a page to scroll it.

Everything the mod changes is remembered and put back when you switch the option off.

## Features

**General**
- Effects on / off, with a line that says what is going on
- 6 presets with a small picture each: Default, Warm, Cold, Vivid, Cinematic, Noir
- Fog: the game's own, none, or your own colour and thickness
- FPS counter and a frame-time graph (both can be moved around)

**Color**: hue, saturation, contrast, warmth (cool to warm), tint (green to magenta), colour filter with RGB sliders and swatches

**Effects**: exposure, gamma, bloom, vignette, film grain

**Dark scenes** (on the Effects tab)
- Brightness: flat ambient light, so shadowed corners are less black
- Light boost: makes the game's real-time lights stronger (flashlight, lamps). Light that is baked into the map does not change. For that, use exposure or gamma
- Field of view

**Advanced**: motion blur (and its quality), chromatic aberration, lens distortion, bloom threshold, vignette softness, grain size

**Performance**
- Graphics level (low / ultra low), no textures, hide particles, no post-processing, no shadow
- Draw distance
- Render resolution (100% to 35%), a custom size, or **adaptive resolution** that follows your target FPS (30 / 45 / 60, never below 50%)
- Squash view, FPS cap
- Clean memory now, or automatically after a scene change

**ESP** (outlines)
- Items and entities (Granny, Grandpa, the spider, the robot, Slendrina and the others) get an outline that follows their shape
- 8 colours and a thickness slider for each; visible through walls or only where you can see them
- The game's own outline component draws it; the mod only turns it on, so it looks like the game's own item outlines

**Settings**: menu size, menu opacity and icon opacity (both down to 0%), icon size, accent and background colours, rainbow accent, rounded corners, lock the position

Text in the menu never gets smaller than a readable size, even when the menu is made small, and it gets a soft shadow when the menu is see-through.

## Get the library

1. Make a new repo on GitHub and upload everything in this folder.
2. Open the Actions tab and wait for the build to finish.
3. Download `libvisualmobile` from the artifacts of the run. Inside is `libvisualmobile.so`.

The build downloads Dear ImGui and And64InlineHook by itself. It also takes the menu font (Roboto) from ImGui's `misc/fonts` folder and compiles it into the library; if that folder has no usable font, ImGui's built-in font is used.

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

## Your files

All of them are in `Android/data/<game package>/files/`:

- `visualmobile.cfg`: your settings. Delete it to reset everything (for example when you made the icon invisible and cannot find it)
- `visualmobile.log`: what happened in this run
- `visualmobile.prev.log`: the run before. The log is moved here every time the game starts, so if `visualmobile.log` is empty because you just restarted the game, look here

## If something goes wrong

Send `visualmobile.log` (and `visualmobile.prev.log` if the game was restarted) when you report a problem. It says how far the loading got.

- **No eye on the screen:** the log tells whether the library was loaded and whether the hooks worked. The mod needs the game to draw with OpenGL ES; if the game uses Vulkan, the menu will not show.
- **Menu does not react on a screen without gameplay (main menu):** the mod does its work from three engine calls the game makes every frame. The log line `tick: first call from ...` says which one ran.
- **Eye shows but no effects:** the log tells whether the post-processing parts of the game were found.
- **An option does nothing:** the log lines `perf: ready, not found: ...`, `scene: ...` and `esp: ...` say what this game build does not have. Many Unity functions are stripped from the game; the mod calls the engine directly where it can.
- **Light boost does nothing:** the line under it says how many lights were found. Baked light cannot be changed.
- Only 64-bit (arm64) phones are supported.
- The render resolution and the light boost call the engine directly. If the game crashes right after you use one of them, switch it off in `visualmobile.cfg` (`perfCustom=0`, `perfRes=0`, `perfAdaptive=0`, `lightOn=0`) and send the log.

## Credits

Menu by [Dear ImGui](https://github.com/ocornut/imgui) (MIT). Hooks by [And64InlineHook](https://github.com/Rprop/And64InlineHook). Menu font: Roboto (Apache 2.0), as shipped with Dear ImGui. The outlines are drawn by the game's own outline component. The libraries are downloaded by the build, not included here.
