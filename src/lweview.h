#pragma once
// LWEView — a QML item that hosts linux-wallpaperengine's output as a texture
// inside Plasma's wallpaper QML scene graph. Uses XComposite to redirect LWE's
// X11 window to an off-screen pixmap, glXBindTexImageEXT to bind that pixmap
// as an OpenGL texture, and exposes it as a QSGSimpleTextureNode so Plasma's
// icons and widgets render naturally on top.
//
// LWE is spawned as a child process; one workshop ID at a time. When the QML
// `workshopId` property changes (i.e. user picked a different wallpaper), the
// previous LWE is killed and a new one launched.
//
// All X11/GLX interaction happens on the QtQuick render thread to avoid
// cross-thread GL context issues.
#include <QQuickItem>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QTimer>

class LWEView : public QQuickItem
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString workshopId READ workshopId WRITE setWorkshopId NOTIFY workshopIdChanged)
    Q_PROPERTY(QString assetsDir  READ assetsDir  WRITE setAssetsDir  NOTIFY assetsDirChanged)
    Q_PROPERTY(QString status     READ status     NOTIFY statusChanged)
    Q_PROPERTY(int     volume     READ volume     WRITE setVolume     NOTIFY optionsChanged)
    Q_PROPERTY(bool    muted      READ muted      WRITE setMuted      NOTIFY optionsChanged)
    Q_PROPERTY(int     fps        READ fps        WRITE setFps        NOTIFY optionsChanged)
    Q_PROPERTY(QString scaling    READ scaling    WRITE setScaling    NOTIFY optionsChanged)
    Q_PROPERTY(QString clamp      READ clamp      WRITE setClamp      NOTIFY optionsChanged)
    Q_PROPERTY(bool    mouseInput READ mouseInput WRITE setMouseInput NOTIFY optionsChanged)
    Q_PROPERTY(bool    parallax   READ parallax   WRITE setParallax   NOTIFY optionsChanged)
    Q_PROPERTY(bool    particles  READ particles  WRITE setParticles  NOTIFY optionsChanged)
    Q_PROPERTY(bool    fullscreenPause READ fullscreenPause WRITE setFullscreenPause NOTIFY optionsChanged)
    Q_PROPERTY(bool    showDebug  READ showDebug  WRITE setShowDebug  NOTIFY showDebugChanged)
    Q_PROPERTY(int     relaunchTrigger READ relaunchTrigger WRITE setRelaunchTrigger NOTIFY relaunchTriggerChanged)
public:
    LWEView(QQuickItem *parent = nullptr);
    ~LWEView() override;

    bool showDebug() const { return m_showDebug; }
    void setShowDebug(bool b);

    int relaunchTrigger() const { return m_relaunchTrigger; }
    void setRelaunchTrigger(int t);

    QString workshopId() const { return m_wid; }
    void setWorkshopId(const QString &id);

    QString assetsDir() const { return m_assetsDir; }
    void setAssetsDir(const QString &dir);

    QString status() const { return m_status; }

    // Per-wallpaper options. Setters debounce relaunch — many at once trigger
    // a single relaunch via a 250ms single-shot timer.
    int volume() const { return m_volume; }
    void setVolume(int v);
    bool muted() const { return m_muted; }
    void setMuted(bool b);
    int fps() const { return m_fps; }
    void setFps(int n);
    QString scaling() const { return m_scaling; }
    void setScaling(const QString &s);
    QString clamp() const { return m_clamp; }
    void setClamp(const QString &s);
    bool mouseInput() const { return m_mouseInput; }
    void setMouseInput(bool b);
    bool parallax() const { return m_parallax; }
    void setParallax(bool b);
    bool particles() const { return m_particles; }
    void setParticles(bool b);
    bool fullscreenPause() const { return m_fullscreenPause; }
    void setFullscreenPause(bool b);

    // Mouse forwarding — called from main.qml's MouseArea so that user
    // interaction with the wallpaper reaches the off-screen LWE window via
    // synthetic X11 events (XSendEvent). Coordinates are in QML local space.
    Q_INVOKABLE void forwardMouseMove(qreal x, qreal y);
    Q_INVOKABLE void forwardMouseButton(qreal x, qreal y, int button, bool press);

signals:
    void workshopIdChanged();
    void assetsDirChanged();
    void statusChanged();
    void optionsChanged();
    void showDebugChanged();
    void relaunchTriggerChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void pollForWindow();
    void onLweFinished(int code, QProcess::ExitStatus status);
    void onLweError(QProcess::ProcessError err);
    void onLweOutput();

private:
    void launchLwe();
    void stopLwe();
    void setStatus(const QString &s);

    QString m_wid;            // workshop id
    QString m_runningWid;     // workshop id of currently running LWE process
    QString m_assetsDir;
    QString m_status = QStringLiteral("idle");
    int     m_volume = 30;
    bool    m_muted = true;
    int     m_fps = 30;
    QString m_scaling = QStringLiteral("default");
    QString m_clamp = QStringLiteral("clamp");
    bool    m_mouseInput = false;
    bool    m_parallax = true;
    bool    m_particles = true;
    bool    m_fullscreenPause = true;
    bool    m_showDebug = false;
    int     m_relaunchTrigger = 0;
    QTimer  m_relaunchTimer;
    void scheduleRelaunch();
    bool m_ready = false;        // set once initial QML bindings settle

    QString m_logPath;        // unique per-instance LWE stdout log path —
                              // a single shared path across multi-screen
                              // instances let one screen's Truncate wipe
                              // another's in-progress log and let unbounded
                              // per-frame script output balloon to 90+ MB.

    QPointer<QProcess> m_proc;
    QTimer m_findTimer;
    unsigned long m_lweWindow = 0;       // X11 Window id
    unsigned long m_lwePixmap = 0;       // off-screen pixmap from XComposite
    int  m_pixmapWidth = 0;
    int  m_pixmapHeight = 0;
    bool m_redirected = false;
    bool m_textureReady = false;
    int m_pendingFindAttempts = 0;
    qint64 m_lwePid = 0;                 // PID of OUR LWE process — used to
                                          // distinguish from other LWE instances
                                          // running for other screens
    bool m_firstFrameLogged = false;     // per-instance debug — was the
                                          // shared static bool hiding bugs.
    int m_lastLweMouseX = -1;
    int m_lastLweMouseY = -1;
    Qt::MouseButtons m_lastButtons = Qt::NoButton;

    void releasePixmap();
    void refreshPixmap();
};
