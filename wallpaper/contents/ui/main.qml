import QtQuick
import org.kde.plasma.plasmoid
import local.lwepaper 1.0

WallpaperItem {
    id: root

    // Hidden library — triggers scan() + prepareAllDependencies() so every
    // dep-bearing wallpaper has its files symlinked into place before LWE
    // tries to render. Without this, wallpapers that depend on others fail
    // with "Cannot find requested file [/scene.json]" unless the user
    // opens the picker dialog first.
    LWELibrary { id: depPrepLib }
    Component.onCompleted: {
        const prepared = depPrepLib.prepareAllDependencies()
        console.warn("[lwepaper] startup auto-prep linked deps for", prepared, "wallpapers")
    }

    Rectangle {
        anchors.fill: parent
        color: "black"

        LWEView {
            id: view
            anchors.fill: parent
            workshopId:   root.configuration.WorkshopId || ""
            assetsDir:    root.configuration.AssetsDir  || ""
            volume:       root.configuration.Volume
            fps:          root.configuration.Fps
            scaling:      root.configuration.Scaling
            clamp:        root.configuration.Clamp
            mouseInput:   root.configuration.MouseInput
            parallax:     root.configuration.Parallax
            particles:    root.configuration.Particles
            fullscreenPause: root.configuration.FullscreenPause
            muted:        root.configuration.Muted
            relaunchTrigger: root.configuration.RelaunchTrigger
        }

        // Debug overlay (optional — user-controlled via ShowDebug kcfg).
        Rectangle {
            visible: root.configuration.ShowDebug || false
            anchors { top: parent.top; left: parent.left; margins: 20 }
            width: 720; height: 60
            color: "#aa000000"
            border.color: "#3daee9"; border.width: 1

            Text {
                anchors.fill: parent
                anchors.margins: 8
                color: "#aaff66"; font.pixelSize: 16
                text: "LWE: " + view.status
                      + "  wid=" + view.workshopId
                      + "  size=" + view.width.toFixed(0) + "x" + view.height.toFixed(0)
            }
        }
    }
}
