#pragma once
// LWEScenePatch — read a Wallpaper Engine `.pkg`, identify image-effects
// known to crash LWE (e.g. the v_TexCoord shader bug in `waterflow`), strip
// them, and write a patched copy of the .pkg into the wallpaper's directory
// so LWE picks the safe version over the original.
//
// This is the "shader-patch built into the plugin" the user asked for. We
// don't actually rewrite shaders (LWE's preprocessor injects the bad line),
// but we can keep LWE from ever loading the offending effect by editing the
// embedded scene.json — which IS something the plugin can do cleanly.
//
// Asset-recovery additions (for wallpapers that reference missing workshop
// dependencies, custom fonts, models, textures, etc.):
//   * detectImplicitDependencies()  — find `workshop/<id>/...` paths in
//                                     scene.json + project.json
//   * linkWorkshopDependencyPaths() — symlink the dep's files into the
//                                     wallpaper at the path scene.json
//                                     expects (e.g. fonts/workshop/<id>/X)
//   * stubMissingAssets()           — generate placeholder .tex / .json /
//                                     fallback fonts so LWE can keep going
//                                     even with un-subscribed deps
#include <QByteArray>
#include <QString>
#include <QStringList>

class LWEScenePatch
{
public:
    // Known-broken effect names — anything we've seen LWE choke on. Add to
    // this list as new wallpapers reveal new LWE preprocessor bugs.
    static QStringList knownBrokenEffects();

    // Full healing pass for a wallpaper. Runs every sub-step in the right
    // order: (1) implicit-dep discovery → (2) auto-link → (3) stub missing
    // assets → (4) repack scene.pkg with strips + script-injection patches.
    // Returns true if anything was changed on disk.
    static bool patchIfNeeded(const QString &wallpaperPath);

    // ── Asset-recovery pieces (also usable standalone) ──

    // Scan scene.json (and project.json) for workshop-id references inline
    // in paths, e.g. "fonts/workshop/2978738836/Foo.otf" → adds "2978738836".
    // Combined with project.json's explicit `dependency` field.
    static QStringList detectImplicitDependencies(const QString &wallpaperPath);

    // For each dep workshop id that IS downloaded locally, mirror the dep's
    // files into the wallpaper dir at the path scene.json expects
    // (e.g. `<wp>/fonts/workshop/<id>/X.otf` -> `<workshop>/<id>/X.otf`).
    // Returns the count of links created/refreshed.
    static int linkWorkshopDependencyPaths(const QString &wallpaperPath,
                                           const QStringList &depIds);

    // For each asset referenced by scene.json that ISN'T on disk anywhere,
    // create a safe stub:
    //   *.tex    -> a 1x1 transparent texture (minimal valid TEXV0005)
    //   *.ttf/*.otf -> a symlink to a system fallback font
    //   *.json (material/model) -> a minimal empty stub
    //   *.frag/.vert -> a passthrough shader
    // Returns the count of stubs created.
    static int stubMissingAssets(const QString &wallpaperPath);

    // Lists workshop ids referenced by the wallpaper that the user hasn't
    // subscribed to. Used by LWEView to render a clear "subscribe to ..."
    // status message instead of a silent black wallpaper.
    static QStringList unresolvedDependencies(const QString &wallpaperPath);

private:
    static QByteArray stripEffectsFromSceneJson(const QByteArray &json,
                                                const QStringList &effectNames);
    // Internal: collect every `string` value inside a parsed scene.json that
    // looks like a file reference (has `/`, ends in known asset extension).
    static QStringList collectAssetReferences(const QByteArray &sceneJson);
};
