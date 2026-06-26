import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import local.lwepaper 1.0

ColumnLayout {
    id: root
    spacing: 6

    // kcfg bindings
    property string cfg_WorkshopId: ""
    property string cfg_AssetsDir: ""
    property int    cfg_Volume: 30
    property bool   cfg_Muted: true
    property int    cfg_Fps: 30
    property string cfg_Scaling: "default"
    property string cfg_Clamp: "clamp"
    property bool   cfg_MouseInput: false
    property bool   cfg_Parallax: true
    property bool   cfg_Particles: true
    property bool   cfg_FullscreenPause: true
    property string cfg_SortMode: "title"
    property string cfg_FilterType: "scene"
    property bool   cfg_ShowDebug: false
    property int    cfg_RelaunchTrigger: 0

    onCfg_WorkshopIdChanged: loadWpProperties()

    ListModel {
        id: wpPropertiesModel
    }

    function loadWpProperties() {
        wpPropertiesModel.clear()
        if (!root.cfg_WorkshopId) return
        var props = library.getWallpaperProperties(root.cfg_WorkshopId)
        for (var i = 0; i < props.length; ++i) {
            wpPropertiesModel.append(props[i])
        }
    }

    Component.onCompleted: {
        loadWpProperties()
    }

    LWELibrary {
        id: library
        filterType: root.cfg_FilterType
        sortMode:   root.cfg_SortMode
        search:     searchField.text
    }

    // ─── Filter / sort / search row ───────────────────────────────────
    RowLayout {
        Layout.fillWidth: true
        spacing: 6

        Label { text: "Type:" }
        ComboBox {
            Layout.preferredWidth: 110
            model: [ "all", "scene", "video", "web" ]
            currentIndex: model.indexOf(root.cfg_FilterType)
            onCurrentTextChanged: root.cfg_FilterType = (currentText === "all" ? "" : currentText)
        }

        Label { text: "Sort:"; Layout.leftMargin: 12 }
        ComboBox {
            Layout.preferredWidth: 150
            textRole: "label"; valueRole: "value"
            model: [
                { label: "Title (A-Z)",      value: "title" },
                { label: "Newest modified",  value: "modified" },
                { label: "Workshop ID",      value: "id" },
                { label: "Type",             value: "type" },
                { label: "PKG version",      value: "pkg" },
            ]
            currentIndex: { for (var i = 0; i < model.length; ++i) if (model[i].value === root.cfg_SortMode) return i; return 0 }
            onActivated: root.cfg_SortMode = currentValue
        }

        Label { text: "Search:"; Layout.leftMargin: 12 }
        TextField {
            id: searchField
            Layout.fillWidth: true
            placeholderText: "title or workshop id"
        }

        Label {
            text: library.total + "/" + library.allTotal
            opacity: 0.7
            Layout.leftMargin: 8
        }

        Button { text: "Rescan"; onClicked: library.reload() }
    }

    // ─── Wallpaper grid ──────────────────────────────────────────────
    Rectangle {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumHeight: 280
        color: "transparent"
        border.color: "#444"
        border.width: 1
        clip: true

        GridView {
            id: grid
            anchors.fill: parent
            anchors.margins: 4
            cellWidth: 192
            cellHeight: 144
            model: library
            clip: true
            ScrollBar.vertical: ScrollBar {}

            delegate: Item {
                width: grid.cellWidth - 4
                height: grid.cellHeight - 4

                Rectangle {
                    anchors.fill: parent
                    color: model.wid === root.cfg_WorkshopId ? "#3daee9" : "#1a1a1a"
                    border.color: model.wid === root.cfg_WorkshopId ? "#3daee9" : "#333"
                    border.width: 2

                    Image {
                        anchors.fill: parent
                        anchors.margins: 4
                        source: model.preview || ""
                        fillMode: Image.PreserveAspectCrop
                        smooth: true
                        asynchronous: true
                    }

                    // Dependency badge (top-right) if this wallpaper requires others
                    Rectangle {
                        visible: (model.dependencies || []).length > 0
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 6
                        width: depLabel.implicitWidth + 8
                        height: 18
                        color: "#cc8a2be2"
                        radius: 3
                        Label {
                            id: depLabel
                            anchors.centerIn: parent
                            text: "+" + (model.dependencies || []).length + " req"
                            color: "white"; font.pixelSize: 10
                        }
                    }

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.margins: 4
                        height: titleLabel.implicitHeight + 6
                        color: "#cc000000"
                        Label {
                            id: titleLabel
                            anchors.fill: parent
                            anchors.margins: 3
                            text: model.title +
                                  (model.type === "scene" && model.pkgVersion ? "  [" + model.pkgVersion + "]" : "")
                            color: "white"
                            font.pixelSize: 8
                            elide: Text.ElideRight
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: root.cfg_WorkshopId = model.wid
                    }
                }
            }
        }
    }

    // ─── Selected info + manual id ───────────────────────────────────
    RowLayout {
        Layout.fillWidth: true
        Label { text: "Selected:" }
        Label { text: root.cfg_WorkshopId || "(none)"; font.bold: true; Layout.fillWidth: true }
        TextField {
            Layout.preferredWidth: 200
            placeholderText: "or type id…"
            text: root.cfg_WorkshopId
            onEditingFinished: if (text !== root.cfg_WorkshopId) root.cfg_WorkshopId = text
        }
    }

    // ─── Wallpaper properties ────────────────────────────────────────
    GroupBox {
        Layout.fillWidth: true
        title: "Wallpaper properties"
        visible: wpPropertiesModel.count > 0

        ColumnLayout {
            width: parent.width
            spacing: 6

            Repeater {
                model: wpPropertiesModel
                delegate: RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    Label {
                        text: model.text
                        Layout.preferredWidth: 180
                        elide: Text.ElideRight
                    }

                    Loader {
                        Layout.fillWidth: true
                        sourceComponent: {
                            if (model.type === "bool") return boolControl
                            if (model.type === "slider") return sliderControl
                            if (model.type === "combo") return comboControl
                            if (model.type === "color") return colorControl
                            return textControl
                        }
                    }
                }
            }
        }
    }

    Component {
        id: boolControl
        CheckBox {
            checked: model.value === true || model.value === "true"
            onToggled: {
                library.saveWallpaperProperty(root.cfg_WorkshopId, model.key, checked)
                root.cfg_RelaunchTrigger = root.cfg_RelaunchTrigger + 1
            }
        }
    }

    Component {
        id: sliderControl
        RowLayout {
            Slider {
                from: model.min !== undefined ? model.min : 0
                to: model.max !== undefined ? model.max : 100
                stepSize: model.step !== undefined ? model.step : 1
                value: model.value !== undefined ? model.value : from
                onMoved: {
                    library.saveWallpaperProperty(root.cfg_WorkshopId, model.key, value)
                }
                onReleased: {
                    root.cfg_RelaunchTrigger = root.cfg_RelaunchTrigger + 1
                }
                Layout.fillWidth: true
            }
            Label {
                text: Math.round(model.value * 100) / 100
            }
        }
    }

    Component {
        id: comboControl
        ComboBox {
            id: combo
            textRole: "label"
            valueRole: "value"
            model: {
                var opts = model.options || []
                var result = []
                for (var i = 0; i < opts.length; ++i) {
                    if (typeof opts[i] === "object") {
                        result.push({ label: opts[i].label || opts[i].value || "", value: opts[i].value })
                    } else {
                        result.push({ label: opts[i], value: opts[i] })
                    }
                }
                return result
            }
            currentIndex: {
                var val = model.value
                var opts = combo.model
                if (opts && opts.length) {
                    for (var i = 0; i < opts.length; ++i) {
                        if (opts[i].value === val) return i
                    }
                }
                return 0
            }
            onActivated: {
                library.saveWallpaperProperty(root.cfg_WorkshopId, model.key, currentValue)
                root.cfg_RelaunchTrigger = root.cfg_RelaunchTrigger + 1
            }
        }
    }

    Component {
        id: colorControl
        RowLayout {
            Rectangle {
                width: 24; height: 24
                color: {
                    var val = model.value || "1 1 1"
                    var parts = val.toString().split(" ")
                    if (parts.length >= 3) {
                        var r = Math.min(1.0, Math.max(0.0, parseFloat(parts[0])))
                        var g = Math.min(1.0, Math.max(0.0, parseFloat(parts[1])))
                        var b = Math.min(1.0, Math.max(0.0, parseFloat(parts[2])))
                        return Qt.rgba(r, g, b, 1)
                    }
                    return val
                }
                border.color: "#888"; border.width: 1
            }
            TextField {
                text: model.value !== undefined ? model.value.toString() : ""
                Layout.fillWidth: true
                placeholderText: "e.g. 1.0 0.0 0.0 or #ff0000"
                onEditingFinished: {
                    library.saveWallpaperProperty(root.cfg_WorkshopId, model.key, text)
                    root.cfg_RelaunchTrigger = root.cfg_RelaunchTrigger + 1
                }
            }
        }
    }

    Component {
        id: textControl
        TextField {
            text: model.value !== undefined ? model.value.toString() : ""
            Layout.fillWidth: true
            onEditingFinished: {
                library.saveWallpaperProperty(root.cfg_WorkshopId, model.key, text)
                root.cfg_RelaunchTrigger = root.cfg_RelaunchTrigger + 1
            }
        }
    }

    // ─── Playback options ────────────────────────────────────────────
    GroupBox {
        Layout.fillWidth: true
        title: "Playback options"

        GridLayout {
            columns: 2
            columnSpacing: 12
            rowSpacing: 6
            anchors.fill: parent

            Label { text: "Mute audio" }
            CheckBox { checked: root.cfg_Muted; onToggled: root.cfg_Muted = checked }

            Label { text: "Volume (when not muted)" }
            RowLayout {
                Slider {
                    from: 0; to: 100; stepSize: 1
                    value: root.cfg_Volume
                    onMoved: root.cfg_Volume = Math.round(value)
                    enabled: !root.cfg_Muted
                    Layout.preferredWidth: 200
                }
                Label { text: root.cfg_Volume + "%" }
            }

            Label { text: "Frame-rate cap" }
            RowLayout {
                SpinBox {
                    from: 5; to: 240; stepSize: 5
                    value: root.cfg_Fps
                    onValueModified: root.cfg_Fps = value
                }
                Label { text: "fps" }
            }

            Label { text: "Scaling" }
            ComboBox {
                Layout.preferredWidth: 160
                model: [ "default", "stretch", "fit", "fill" ]
                currentIndex: Math.max(0, model.indexOf(root.cfg_Scaling))
                onCurrentTextChanged: root.cfg_Scaling = currentText
            }

            Label { text: "Clamp" }
            ComboBox {
                Layout.preferredWidth: 160
                model: [ "clamp", "border", "repeat" ]
                currentIndex: Math.max(0, model.indexOf(root.cfg_Clamp))
                onCurrentTextChanged: root.cfg_Clamp = currentText
            }

            Label { text: "Allow mouse input" }
            CheckBox { checked: root.cfg_MouseInput; onToggled: root.cfg_MouseInput = checked }

            Label { text: "Parallax effect" }
            CheckBox { checked: root.cfg_Parallax;   onToggled: root.cfg_Parallax = checked }

            Label { text: "Particle effects" }
            CheckBox { checked: root.cfg_Particles;  onToggled: root.cfg_Particles = checked }

            Label { text: "Pause when fullscreen app active" }
            CheckBox { checked: root.cfg_FullscreenPause; onToggled: root.cfg_FullscreenPause = checked }

            Label { text: "Show bright green debug text" }
            CheckBox { checked: root.cfg_ShowDebug; onToggled: root.cfg_ShowDebug = checked }
        }
    }

    // ─── Assets directory ────────────────────────────────────────────
    RowLayout {
        Layout.fillWidth: true
        Label { text: "Assets dir:" }
        TextField {
            Layout.fillWidth: true
            placeholderText: "(default: Steam wallpaper_engine/assets)"
            text: root.cfg_AssetsDir
            onEditingFinished: root.cfg_AssetsDir = text
        }
    }
}
