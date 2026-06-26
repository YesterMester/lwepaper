# lwepaper

An improved KDE Plasma 6 wallpaper plugin that hosts **linux-wallpaperengine** natively inside the Plasma desktop scene graph, allowing interactive scene, web, and video-based wallpapers to render beautifully under your desktop widgets and icons.

## Features

- 🌟 **Full Interactive Support**: Supports Scene, Web, and Video-based wallpapers.
- 🖱️ **Zero-Interception Mouse Passthrough**: Implements a passive C++ window event filter. Hover effects and clicks are forwarded to the wallpaper while keeping desktop icons, drags, and right-click menus 100% functional.
- ⚙️ **Wallpaper-Specific settings**: Dynamically detects custom wallpaper settings defined in `project.json` (such as custom colors, speeds, text, sliders, combos, and checkboxes) and exposes them right inside the Plasma Wallpaper Configuration UI.
- 🔗 **Recursive Dependency Merging**: Traverses and merges assets from nested and overlapping dependencies recursively, ensuring complex assets packs (like shared materials, shaders, or presets) load correctly.
- 🛡️ **Stack-Safe Patcher**: Statically patches and cleans up GLSL-compiler-breaking shader effects (e.g. waterflow bugs on Valtiel) inside a safe, non-recursive environment to prevent crash loops.
- 📈 **Memory Leak Free**: Cleans up SceneGraph GPU texture data (`QSGTexture`) dynamically on every frame swap.

---

## Build & Installation

### Requirements
- **Qt 6** (6.5+ with Core, Gui, Qml, Quick modules)
- **X11** & **XComposite** development libraries
- **CMake** & a C++17 compliant compiler
- **linux-wallpaperengine** and **nixGL** installed via Nix or compile/AUR.

### Compilation
1. Configure and build the CMake project:
   ```bash
   cmake -B build -DCMAKE_INSTALL_PREFIX=~/.local
   cmake --build build --parallel
   ```

2. Install the plugin under your user-local KDE directory:
   ```bash
   cmake --install build
   ```

3. Restart the Plasma desktop shell to load the new QML module and wallpaper type:
   ```bash
   kquitapp6 plasmashell && kstart plasmashell
   ```

---

## Wallpaper Patcher Utility

Some Steam Workshop wallpapers have incorrect file paths or types in their `project.json`. A helper script is included to batch-inspect and repair them:

```bash
./patch_all.py
```
This automatically corrects common header issues (like `.pkg` files set as the primary scene file instead of virtual `.json` files) so they run flawlessly inside the engine.
