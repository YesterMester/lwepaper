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
#include <QByteArray>
#include <QString>
#include <QStringList>

class LWEScenePatch
{
public:
    // Known-broken effect names — anything we've seen LWE choke on. Add to
    // this list as new wallpapers reveal new LWE preprocessor bugs.
    static QStringList knownBrokenEffects();

    // Patch a wallpaper in-place by writing a `scene.pkg.patched` next to
    // the original and replacing the scene.json entry inside it with one
    // that has the listed image-effects removed. Returns true if anything
    // was changed. Idempotent: re-running with no broken effects is a no-op.
    static bool patchIfNeeded(const QString &wallpaperPath);

private:
    static QByteArray stripEffectsFromSceneJson(const QByteArray &json,
                                                const QStringList &effectNames);
};
