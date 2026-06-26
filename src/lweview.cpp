#include "lweview.h"
#include "lwescenepatch.h"
#include "lwelibrary.h"

#include <QGuiApplication>
#include <QMouseEvent>
#include <QImage>
#include <QSGSimpleTextureNode>
#include <QQuickWindow>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QDebug>
#include <QPainter>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonArray>
#include <QDateTime>
#include <QPainterPath>
#include <QHash>

#include <sys/types.h>
#include <signal.h>

// X11 / XComposite — we use the simplest pipeline first: XGetImage (CPU
// readback) into a QImage and paint via QSGSimpleTextureNode. Not the fastest
// possible path but proven and far simpler than GLX_EXT_texture_from_pixmap.
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/Xatom.h>

// One shared X display for all instances. Reusing a connection avoids the
// per-frame XOpenDisplay/XCloseDisplay cost which dominated CPU on the v1.
static Display *xdisplay()
{
    static Display *d = XOpenDisplay(nullptr);
    return d;
}

static bool isDescendantOf(qint64 childPid, qint64 parentPid)
{
    if (childPid <= 0 || parentPid <= 0) return false;
    if (childPid == parentPid) return true;
    qint64 currentPid = childPid;
    for (int depth = 0; depth < 10; ++depth) {
        QFile file(QStringLiteral("/proc/%1/stat").arg(currentPid));
        if (!file.open(QIODevice::ReadOnly)) return false;
        QByteArray content = file.readAll();
        file.close();
        
        int lastParen = content.lastIndexOf(')');
        if (lastParen < 0) return false;
        
        QString rest = QString::fromUtf8(content.mid(lastParen + 2));
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        QStringList tokens = rest.split(u' ', Qt::SkipEmptyParts);
#else
        QStringList tokens = rest.split(' ', QString::SkipEmptyParts);
#endif
        if (tokens.size() < 2) return false;
        
        bool ok = false;
        qint64 ppid = tokens.at(1).toLongLong(&ok);
        if (!ok) return false;
        
        if (ppid == parentPid) return true;
        if (ppid <= 1) return false;
        currentPid = ppid;
    }
    return false;
}

static unsigned long getWindowPid(Display *d, Window w)
{
    Atom actualType;
    int actualFormat;
    unsigned long nItems, bytesAfter;
    unsigned char *prop = nullptr;
    unsigned long pid = 0;
    
    Atom atom = XInternAtom(d, "_NET_WM_PID", False);
    if (atom != None) {
        if (XGetWindowProperty(d, w, atom, 0, 1, False, XA_CARDINAL,
                               &actualType, &actualFormat, &nItems, &bytesAfter, &prop) == Success) {
            if (prop && nItems > 0) {
                pid = *reinterpret_cast<unsigned long *>(prop);
            }
            if (prop) XFree(prop);
        }
    }
    return pid;
}

// We need to find LWE's window by WM_CLASS and PID relationship to prevent race conditions
static unsigned long findWindowByClass(Display *d, Window root, const char *needle, qint64 targetPid = 0)
{
    Window rootRet, parent;
    Window *children = nullptr;
    unsigned int nchildren = 0;
    if (!XQueryTree(d, root, &rootRet, &parent, &children, &nchildren))
        return 0;
    unsigned long found = 0;
    for (unsigned int i = 0; i < nchildren && !found; ++i) {
        XClassHint h{};
        if (XGetClassHint(d, children[i], &h)) {
            if (h.res_class && std::string(h.res_class).find(needle) != std::string::npos) {
                if (targetPid > 0) {
                    unsigned long wpid = getWindowPid(d, children[i]);
                    if (isDescendantOf(wpid, targetPid)) {
                        found = children[i];
                    }
                } else {
                    found = children[i];
                }
            }
            if (h.res_name) XFree(h.res_name);
            if (h.res_class) XFree(h.res_class);
        }
        if (!found) found = findWindowByClass(d, children[i], needle, targetPid);
    }
    if (children) XFree(children);
    return found;
}

static QList<qint64> getAllDescendants(qint64 parentPid)
{
    QList<qint64> descendants;
    if (parentPid <= 0) return descendants;
    
    QHash<qint64, QList<qint64>> parentToChildren;
    QDir procDir(QStringLiteral("/proc"));
    QStringList entries = procDir.entryList({QStringLiteral("[0-9]*")}, QDir::Dirs);
    for (const QString &entry : entries) {
        bool ok = false;
        qint64 pid = entry.toLongLong(&ok);
        if (!ok) continue;
        
        QFile file(QStringLiteral("/proc/%1/stat").arg(pid));
        if (file.open(QIODevice::ReadOnly)) {
            QByteArray content = file.readAll();
            file.close();
            int lastParen = content.lastIndexOf(')');
            if (lastParen >= 0) {
                QString rest = QString::fromUtf8(content.mid(lastParen + 2));
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                QStringList tokens = rest.split(u' ', Qt::SkipEmptyParts);
#else
                QStringList tokens = rest.split(' ', QString::SkipEmptyParts);
#endif
                if (tokens.size() >= 2) {
                    bool okPpid = false;
                    qint64 ppid = tokens.at(1).toLongLong(&okPpid);
                    if (okPpid) {
                        parentToChildren[ppid].append(pid);
                    }
                }
            }
        }
    }
    
    QList<qint64> queue;
    queue.append(parentPid);
    int head = 0;
    while (head < queue.size()) {
        qint64 current = queue.at(head++);
        if (parentToChildren.contains(current)) {
            for (qint64 child : parentToChildren.value(current)) {
                queue.append(child);
                descendants.append(child);
            }
        }
    }
    return descendants;
}

LWEView::LWEView(QQuickItem *parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    m_findTimer.setInterval(30); // Poll faster (30ms) for near-instant window discovery
    connect(&m_findTimer, &QTimer::timeout, this, &LWEView::pollForWindow);

    // Coalesce option-change relaunches.
    m_relaunchTimer.setSingleShot(true);
    m_relaunchTimer.setInterval(250);
    connect(&m_relaunchTimer, &QTimer::timeout, this, [this] {
        if (!m_wid.isEmpty() && width() > 0 && height() > 0) launchLwe();
    });

    // Delay becoming "ready" so all initial QML property bindings
    // (workshopId, volume, fps, ...) finish before we actually launch
    // LWE. Reduced to 50ms for near-instant boot.
    QTimer::singleShot(50, this, [this] {
        m_ready = true;
        if (!m_wid.isEmpty() && width() > 0 && height() > 0)
            scheduleRelaunch();
    });

    // Repaint timer — 30fps.
    auto *repaint = new QTimer(this);
    connect(repaint, &QTimer::timeout, this, [this] { if (window()) update(); });
    repaint->start(33);

    // Event filter to capture mouse coordinates and button presses globally/passively
    connect(this, &QQuickItem::windowChanged, this, [this](QQuickWindow *win) {
        if (win) {
            win->installEventFilter(this);
        }
    });
}

void LWEView::scheduleRelaunch()
{
    emit optionsChanged();
    if (m_ready && !m_wid.isEmpty()) {
        if (!m_proc) {
            // First time load: launch instantly instead of waiting for the 250ms debounce timer!
            m_relaunchTimer.stop();
            launchLwe();
        } else {
            // Options changed while running: debounce to prevent rapid thrashing
            m_relaunchTimer.start();
        }
    }
}

void LWEView::setVolume(int v)       { if (v == m_volume) return; m_volume = v; scheduleRelaunch(); }
void LWEView::setMuted(bool b)       { if (b == m_muted)  return; m_muted = b;  scheduleRelaunch(); }
void LWEView::setFps(int n)          { if (n == m_fps)    return; m_fps = n;    scheduleRelaunch(); }
void LWEView::setScaling(const QString &s) { if (s == m_scaling) return; m_scaling = s; scheduleRelaunch(); }
void LWEView::setClamp(const QString &s)   { if (s == m_clamp)   return; m_clamp = s;   scheduleRelaunch(); }
void LWEView::setMouseInput(bool b)  { if (b == m_mouseInput) return; m_mouseInput = b; scheduleRelaunch(); }
void LWEView::setParallax(bool b)    { if (b == m_parallax)   return; m_parallax = b;   scheduleRelaunch(); }
void LWEView::setParticles(bool b)   { if (b == m_particles)  return; m_particles = b;  scheduleRelaunch(); }
void LWEView::setFullscreenPause(bool b) { if (b == m_fullscreenPause) return; m_fullscreenPause = b; scheduleRelaunch(); }
void LWEView::setShowDebug(bool b)   { if (b == m_showDebug) return; m_showDebug = b; emit showDebugChanged(); update(); }

void LWEView::setRelaunchTrigger(int t)
{
    if (t == m_relaunchTrigger) return;
    m_relaunchTrigger = t;
    emit relaunchTriggerChanged();
    scheduleRelaunch();
}

bool LWEView::eventFilter(QObject *watched, QEvent *event)
{
    if (m_mouseInput && watched == window()) {
        if (event->type() == QEvent::MouseMove) {
            QMouseEvent *me = static_cast<QMouseEvent *>(event);
            QPointF localPos = mapFromScene(me->scenePosition());
            forwardMouseMove(localPos.x(), localPos.y());
        } else if (event->type() == QEvent::MouseButtonPress ||
                   event->type() == QEvent::MouseButtonRelease) {
            QMouseEvent *me = static_cast<QMouseEvent *>(event);
            QPointF localPos = mapFromScene(me->scenePosition());
            
            int btn = 0;
            if (me->button() == Qt::LeftButton) btn = 1;
            else if (me->button() == Qt::MiddleButton) btn = 2;
            else if (me->button() == Qt::RightButton) btn = 3;
            
            if (btn > 0) {
                forwardMouseButton(localPos.x(), localPos.y(), btn, event->type() == QEvent::MouseButtonPress);
            }
        }
    }
    return QQuickItem::eventFilter(watched, event);
}

LWEView::~LWEView()
{
    if (window()) {
        window()->removeEventFilter(this);
    }
    stopLwe();
    releasePixmap();
}

void LWEView::setWorkshopId(const QString &id)
{
    if (id == m_wid) return;
    m_wid = id;
    emit workshopIdChanged();
    if (!m_wid.isEmpty()) {
        if (!m_ready) {
            // During init — the startup timer will launch once ready.
            setStatus(QStringLiteral("waiting for init..."));
            return;
        }
        // Debounce via the same timer so rapid id changes don't thrash.
        if (width() > 0 && height() > 0)
            m_relaunchTimer.start();
        else
            setStatus(QStringLiteral("waiting for size to be set..."));
    } else {
        stopLwe();
    }
}

void LWEView::setAssetsDir(const QString &dir)
{
    if (dir == m_assetsDir) return;
    m_assetsDir = dir;
    emit assetsDirChanged();
}

void LWEView::setStatus(const QString &s)
{
    if (m_status == s) return;
    m_status = s;
    emit statusChanged();
}

void LWEView::launchLwe()
{
    stopLwe();
    if (m_wid.isEmpty()) return;

    const QString home = QDir::homePath();

    // Apply known-bad-effect patches BEFORE LWE reads the scene.pkg.
    // This is the "shader-patch built into the plugin" path — it strips
    // image-effects we know crash LWE's GLSL compiler (e.g. waterflow on
    // Valtiel) so the rest of the scene renders.
    {
        const QString wpPath = home + QStringLiteral("/.local/share/Steam/steamapps/workshop/content/431960/") + m_wid;
        if (LWEScenePatch::patchIfNeeded(wpPath))
            qWarning() << "[lwepaper] scene.pkg patched for" << m_wid;
    }

    QStringList presetArgs;

    // Auto-patch missing type and file in project.json before spawning LWE.
    // Since LWELibrary (where the primary scanner runs) is only loaded inside the
    // settings UI (config.qml), we must also run this patching check at launch
    // time to ensure wallpapers function correctly on desktop startup.
    {
        const QString wpPath = home + QStringLiteral("/.local/share/Steam/steamapps/workshop/content/431960/") + m_wid;
        const QString pjPath = wpPath + QStringLiteral("/project.json");
        QFile f(pjPath);
        if (f.open(QIODevice::ReadOnly)) {
            QByteArray bytes = f.readAll();
            f.close();
            QJsonParseError err{};
            QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
            if (err.error == QJsonParseError::NoError && doc.isObject()) {
                QJsonObject obj = doc.object();

                // Extract preset properties from project.json to pass to LWE
                                // Merge wallpaper default properties from "properties" and user choices from "preset"
                QJsonObject propertiesObj = obj.value(QStringLiteral("properties")).toObject();
                QJsonObject presetObj = obj.value(QStringLiteral("preset")).toObject();
                
                QJsonObject activeProperties;
                for (auto it = propertiesObj.begin(); it != propertiesObj.end(); ++it) {
                    QString key = it.key();
                    QJsonObject prop = it.value().toObject();
                    if (prop.contains(QStringLiteral("value"))) {
                        activeProperties.insert(key, prop.value(QStringLiteral("value")));
                    }
                }
                for (auto it = presetObj.begin(); it != presetObj.end(); ++it) {
                    activeProperties.insert(it.key(), it.value());
                }

                // Format into presetArgs
                for (auto it = activeProperties.begin(); it != activeProperties.end(); ++it) {
                    QString key = it.key();
                    QJsonValue val = it.value();
                    if (val.isNull()) continue;

                    QString valStr;
                    if (val.isBool()) {
                        valStr = val.toBool() ? QStringLiteral("true") : QStringLiteral("false");
                    } else if (val.isDouble()) {
                        valStr = QString::number(val.toDouble());
                    } else if (val.isString()) {
                        valStr = val.toString();
                    } else {
                        valStr = val.toVariant().toString();
                    }

                    if (!valStr.isEmpty()) {
                        presetArgs << QStringLiteral("--set-property") << QStringLiteral("%1=%2").arg(key, valStr);
                    }
                }

                QString type = obj.value(QStringLiteral("type")).toString().toLower();
                QString file = obj.value(QStringLiteral("file")).toString();

                bool hasScenePkg = QFileInfo::exists(wpPath + QStringLiteral("/scene.pkg"));
                bool hasGifPkg = QFileInfo::exists(wpPath + QStringLiteral("/gifscene.pkg"));
                bool hasPkg = hasScenePkg || hasGifPkg;

                QString targetVirtualFile = QStringLiteral("scene.json");
                if (hasGifPkg && !hasScenePkg) {
                    targetVirtualFile = QStringLiteral("gifscene.json");
                    QFile::remove(wpPath + QStringLiteral("/scene.pkg"));
                    QFile::link(wpPath + QStringLiteral("/gifscene.pkg"), wpPath + QStringLiteral("/scene.pkg"));
                }

                bool hasMainFile = false;

                if (!file.isEmpty() && QFileInfo::exists(wpPath + QLatin1Char('/') + file)) {
                    if (file == QStringLiteral("scene.pkg") || file == QStringLiteral("gifscene.pkg")) {
                        file = targetVirtualFile;
                    }
                    hasMainFile = true;
                } else if (hasPkg && (file == QStringLiteral("scene.json") || file == QStringLiteral("gifscene.json") || file.isEmpty())) {
                    file = targetVirtualFile;
                    hasMainFile = true;
                }

                if (!hasMainFile) {
                    for (const auto &name : {QStringLiteral("scene.json"), QStringLiteral("index.html")}) {
                        if (QFileInfo::exists(wpPath + QLatin1Char('/') + name)) {
                            hasMainFile = true;
                            file = name;
                            break;
                        }
                    }
                    if (!hasMainFile && hasPkg) {
                        hasMainFile = true;
                        file = QStringLiteral("scene.json");
                    }
                }

                // Collect and link dependencies recursively
                {
                    QStringList deps;
                    QString workshopRoot = QFileInfo(wpPath).absolutePath();
                    LWELibrary::collectDependenciesRecursively(workshopRoot, m_wid, deps, true);

                    // Missing-dep guard: if any required dep is NOT downloaded,
                    // tell the user via status instead of letting LWE crash.
                    {
                        QStringList missing;
                        for (const QString &depId : deps) {
                            const QString dpj = workshopRoot + QLatin1Char('/') + depId + QStringLiteral("/project.json");
                            if (!QFileInfo::exists(dpj)) missing << depId;
                        }
                        if (!missing.isEmpty()) {
                            setStatus(QStringLiteral(
                                "Subscribe in Steam Workshop to the required wallpaper(s): %1")
                                .arg(missing.join(QStringLiteral(", "))));
                            qWarning() << "[lwepaper] missing deps for" << m_wid << ":" << missing;
                            return;   // don't even try launching LWE
                        }
                    }

                    // Link all dependencies recursively
                    for (const QString &depId : deps) {
                        QString depPath = workshopRoot + QLatin1Char('/') + depId;
                        
                        QString depFileVal;
                        QString depPj = depPath + QStringLiteral("/project.json");
                        QFile depF(depPj);
                        if (depF.open(QIODevice::ReadOnly)) {
                            QByteArray depBytes = depF.readAll();
                            depF.close();
                            QJsonDocument depDoc = QJsonDocument::fromJson(depBytes);
                            if (depDoc.isObject()) {
                                depFileVal = depDoc.object().value(QStringLiteral("file")).toString();
                            }
                        }

                        if (depFileVal == QStringLiteral("scene.pkg") || depFileVal == QStringLiteral("gifscene.pkg")) {
                            depFileVal = QStringLiteral("scene.json");
                        }

                        bool linkedAny = false;
                        QDir depDir(depPath);
                        if (depDir.exists()) {
                            QStringList depEntries = depDir.entryList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
                            for (const QString &name : depEntries) {
                                if (name == QStringLiteral("project.json")) continue;
                                QString src = depPath + QLatin1Char('/') + name;
                                QString dst = wpPath + QLatin1Char('/') + name;
                                if (LWELibrary::linkFilesRecursively(src, dst)) {
                                    linkedAny = true;
                                }
                            }
                        }

                        if (linkedAny && !hasMainFile && !depFileVal.isEmpty()) {
                            file = depFileVal;
                            hasMainFile = true;
                        }
                    }
                }

                if (type.isEmpty()) {
                    if (file == QStringLiteral("index.html")) {
                        type = QStringLiteral("web");
                    } else if (file == QStringLiteral("scene.json") ||
                               file == QStringLiteral("scene.pkg") ||
                               file == QStringLiteral("gifscene.pkg")) {
                        type = QStringLiteral("scene");
                    } else {
                        QStringList filters = { QStringLiteral("*.mp4"), QStringLiteral("*.webm"), QStringLiteral("*.avi"),
                                                QStringLiteral("*.mkv"), QStringLiteral("*.mov") };
                        QDir dir(wpPath);
                        QStringList videos = dir.entryList(filters, QDir::Files);
                        if (videos.isEmpty() && dir.exists(QStringLiteral("files"))) {
                            QDir filesDir(wpPath + QStringLiteral("/files"));
                            videos = filesDir.entryList(filters, QDir::Files);
                        }
                        if (!videos.isEmpty()) {
                            type = QStringLiteral("video");
                            if (file.isEmpty()) file = videos.first();
                        } else {
                            type = QStringLiteral("scene");
                            if (file.isEmpty()) file = QStringLiteral("scene.json");
                        }
                    }
                }

                bool changed = false;
                if (obj.value(QStringLiteral("type")).toString() != type) {
                    obj.insert(QStringLiteral("type"), type);
                    changed = true;
                }
                if (obj.value(QStringLiteral("file")).toString() != file && !file.isEmpty()) {
                    obj.insert(QStringLiteral("file"), file);
                    changed = true;
                }

                if (changed) {
                    if (f.open(QIODevice::WriteOnly)) {
                        QJsonDocument newDoc(obj);
                        f.write(newDoc.toJson());
                        f.close();
                        qWarning() << "[lwepaper] Auto-patched project.json type=" << type << "file=" << file << "for launch:" << pjPath;
                    }
                }
            }
        }
    }
    const QString nixgl = home + QStringLiteral("/.nix-profile/bin/nixGL");
    const QString lwe   = home + QStringLiteral("/.nix-profile/bin/linux-wallpaperengine");
    if (!QFileInfo::exists(nixgl) || !QFileInfo::exists(lwe)) {
        setStatus(QStringLiteral("missing nixGL or linux-wallpaperengine"));
        return;
    }
    const QString assets = m_assetsDir.isEmpty()
        ? home + QStringLiteral("/.local/share/Steam/steamapps/common/wallpaper_engine/assets")
        : m_assetsDir;

    // LWE has to create an X11 window (no headless mode). KWin on Plasma 6
    // already keeps a composite redirect on every window, so we can sample
    // the off-screen pixmap regardless of whether the source is visible.
    // To prevent KWin from also drawing the source on screen (which would
    // appear as a fullscreen overlay above Plasma's wallpaper layer), we
    // place LWE at large positive coordinates that are outside any monitor.
    // The user has two monitors max-extending to roughly (1920,1080) and
    // (244+1280, 1080+800) = (1524, 1880). 4000,4000 is safely off both.
    // Avoid negative coordinates — LWE parses "-N" as a flag.
    const int w = qMax(64, int(width()));
    const int h = qMax(64, int(height()));
    QStringList args;
    args << lwe
         << QStringLiteral("--window") << QStringLiteral("4000x4000x%1x%2").arg(w).arg(h)
         << QStringLiteral("--bg") << m_wid
         << QStringLiteral("--assets-dir") << assets
         << QStringLiteral("--fps") << QString::number(qBound(5, m_fps, 240));
    if (m_muted)        args << QStringLiteral("--silent");
    else                args << QStringLiteral("--volume") << QString::number(qBound(0, m_volume, 100));
    if (!m_mouseInput)  args << QStringLiteral("--disable-mouse");
    if (!m_parallax)    args << QStringLiteral("--disable-parallax");
    if (!m_particles)   args << QStringLiteral("--disable-particles");
    if (!m_fullscreenPause) args << QStringLiteral("--no-fullscreen-pause");
    if (!m_scaling.isEmpty() && m_scaling != QLatin1String("default"))
        args << QStringLiteral("--scaling") << m_scaling;
    if (!m_clamp.isEmpty() && m_clamp != QLatin1String("clamp"))
        args << QStringLiteral("--clamp") << m_clamp;
    if (!presetArgs.isEmpty())
        args << presetArgs;

    m_proc = new QProcess(this);
    auto env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("LD_LIBRARY_PATH"));
    m_proc->setProcessEnvironment(env);
    m_proc->setProgram(nixgl);
    m_proc->setArguments(args);
    m_proc->setProcessChannelMode(QProcess::MergedChannels);
    m_proc->setStandardOutputFile(QStringLiteral("/tmp/lwepaper-lwe.log"), QIODevice::Truncate);

    connect(m_proc, &QProcess::finished, this, &LWEView::onLweFinished);
    connect(m_proc, &QProcess::errorOccurred, this, &LWEView::onLweError);
    m_runningWid = m_wid;
    m_proc->start();
    setStatus(QStringLiteral("starting LWE for %1 (args 0=%2)").arg(m_wid).arg(nixgl));

    m_pendingFindAttempts = 0;
    m_findTimer.start();
}

void LWEView::stopLwe()
{
    m_findTimer.stop();
    releasePixmap();
    m_lweWindow = 0;
    m_redirected = false;

    if (m_proc) {
        qint64 pid = m_proc->processId();
        m_proc->disconnect(this);

        if (pid > 0) {
            // Find all descendants of the nixGL/LWE parent process
            QList<qint64> descendants = getAllDescendants(pid);
            
            // Kill all descendant processes first
            for (qint64 dpid : descendants) {
                qWarning() << "[lwepaper] Killing descendant process" << dpid;
                ::kill(static_cast<pid_t>(dpid), SIGKILL);
            }
            
            // Kill the parent process
            ::kill(static_cast<pid_t>(pid), SIGKILL);
        }

        m_proc->kill();
        m_proc->waitForFinished(500);
        m_proc->deleteLater();
        m_proc = nullptr;
    }
    m_runningWid.clear();
}

void LWEView::onLweFinished(int code, QProcess::ExitStatus st)
{
    // Pull out a human-readable reason by inspecting the tail of LWE's stdout.
    // Generic "see log" is unhelpful — the user can rarely see that file.
    auto extractReason = []() -> QString {
        QFile f(QStringLiteral("/tmp/lwepaper-lwe.log"));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
        // read last ~8KB
        const auto size = f.size();
        if (size > 8192) f.seek(size - 8192);
        const QByteArray tail = f.readAll();
        f.close();
        const QString text = QString::fromUtf8(tail);
        // Common LWE failure modes we can summarize
        if (text.contains(QLatin1String("Cannot find requested file")))
            return QStringLiteral("LWE couldn't find scene assets — workshop folder may be missing files.");
        if (text.contains(QLatin1String("GLSL fragment unit parsing Failed")) ||
            text.contains(QLatin1String("compilation terminated")))
            return QStringLiteral("LWE's shader compiler rejected this scene — known LWE bug with newer effects.");
        if (text.contains(QLatin1String("Cannot find a valid assets folder")))
            return QStringLiteral("Wallpaper Engine assets folder missing — reinstall WE in Steam.");
        if (text.contains(QLatin1String("VK_ERROR")))
            return QStringLiteral("Vulkan failed to initialize (only a warning if video still decoded).");
        if (text.contains(QLatin1String("Text objects are not supported")))
            return QStringLiteral("Scene loaded; text overlays not shown (LWE limitation).");
        return {};
    };

    QString reason;
    if (st == QProcess::CrashExit)
        reason = QStringLiteral("LWE crashed (signal)");
    else if (code != 0)
        reason = QStringLiteral("LWE exited code %1").arg(code);
    else
        reason = QStringLiteral("LWE exited normally");

    const QString human = extractReason();
    if (!human.isEmpty())
        setStatus(QStringLiteral("%1: %2").arg(reason, human));
    else
        setStatus(QStringLiteral("%1 (workshop %2). See /tmp/lwepaper-lwe.log for details.")
                  .arg(reason, m_wid));
    m_lweWindow = 0;
    m_redirected = false;
    update();
}

void LWEView::onLweError(QProcess::ProcessError err)
{
    QString s;
    switch (err) {
        case QProcess::FailedToStart: s = QStringLiteral("failed to start"); break;
        case QProcess::Crashed:       s = QStringLiteral("crashed"); break;
        case QProcess::Timedout:      s = QStringLiteral("timed out"); break;
        case QProcess::WriteError:    s = QStringLiteral("write error"); break;
        case QProcess::ReadError:     s = QStringLiteral("read error"); break;
        default: s = QStringLiteral("unknown error"); break;
    }
    setStatus(QStringLiteral("QProcess %1: %2")
              .arg(s, m_proc ? m_proc->errorString() : QString()));
}

void LWEView::onLweOutput() { /* unused for now */ }

void LWEView::forwardMouseMove(qreal x, qreal y)
{
    if (!m_lweWindow) return;
    Display *d = xdisplay();
    if (!d || m_pixmapWidth <= 0 || m_pixmapHeight <= 0) return;
    // Map QML local -> LWE window local (same scale; LWE window matches our size).
    const int lx = int(x / qMax<qreal>(1, width())  * m_pixmapWidth);
    const int ly = int(y / qMax<qreal>(1, height()) * m_pixmapHeight);
    XEvent ev{};
    auto &m = ev.xmotion;
    m.type = MotionNotify;
    m.window = m_lweWindow;
    m.root = DefaultRootWindow(d);
    m.subwindow = None;
    m.time = CurrentTime;
    m.x = lx; m.y = ly;
    // LWE's window is parked off-screen — root coords reflect that.
    m.x_root = 8000 + lx;
    m.y_root = 8000 + ly;
    m.same_screen = True;
    XSendEvent(d, m_lweWindow, True, PointerMotionMask, &ev);
    XFlush(d);
}

void LWEView::forwardMouseButton(qreal x, qreal y, int button, bool press)
{
    if (!m_lweWindow || button <= 0) return;
    Display *d = xdisplay();
    if (!d || m_pixmapWidth <= 0 || m_pixmapHeight <= 0) return;
    const int lx = int(x / qMax<qreal>(1, width())  * m_pixmapWidth);
    const int ly = int(y / qMax<qreal>(1, height()) * m_pixmapHeight);
    XEvent ev{};
    auto &b = ev.xbutton;
    b.type = press ? ButtonPress : ButtonRelease;
    b.window = m_lweWindow;
    b.root = DefaultRootWindow(d);
    b.subwindow = None;
    b.time = CurrentTime;
    b.x = lx; b.y = ly;
    b.x_root = 8000 + lx; b.y_root = 8000 + ly;
    b.button = static_cast<unsigned int>(button);
    b.same_screen = True;
    b.state = 0;
    XSendEvent(d, m_lweWindow, True,
               press ? ButtonPressMask : ButtonReleaseMask, &ev);
    XFlush(d);
}

void LWEView::pollForWindow()
{
    if (m_lweWindow) { m_findTimer.stop(); return; }
    if (++m_pendingFindAttempts > 100) {  // 100 * 100ms = 10s
        setStatus(QStringLiteral("timed out waiting for LWE window"));
        qWarning() << "[lwepaper] timed out waiting for LWE window after 10s";
        m_findTimer.stop();
        return;
    }
    Display *d = xdisplay();
    if (!d) return;
    Window root = DefaultRootWindow(d);
    qint64 targetPid = m_proc ? m_proc->processId() : 0;
    if (targetPid <= 0) {
        // Wait until the process starts and gets a valid PID to prevent matching stale/unrelated windows
        return;
    }
    unsigned long w = findWindowByClass(d, root, "linux-wallpaperengine", targetPid);
    if (w) {
        // Wait until the window is fully mapped and viewable in X11
        XWindowAttributes attrs{};
        if (!XGetWindowAttributes(d, w, &attrs) || attrs.map_state != IsViewable) {
            // Not ready yet, check again next poll
            return;
        }

        m_lweWindow = w;
        qWarning() << "[lwepaper] found LWE window" << w << "attempt" << m_pendingFindAttempts;

        // Set override_redirect = True to completely bypass KWin window management,
        // preventing focus, taskbar listing, and off-screen compositing suspension.
        XSetWindowAttributes attr{};
        attr.override_redirect = True;
        XChangeWindowAttributes(d, w, CWOverrideRedirect, &attr);

        // Move the window off-screen to 8000,8000 and redirect using CompositeRedirectAutomatic.
        XMoveWindow(d, w, 8000, 8000);
        XCompositeRedirectWindow(d, w, CompositeRedirectAutomatic);
        XSync(d, False);
        m_redirected = true;

        refreshPixmap();
        if (!m_lwePixmap) {
            // Pixmap creation failed or compositor is still initializing the backing store, poll again
            m_lweWindow = 0;
            m_redirected = false;
            return;
        }
        qWarning() << "[lwepaper] pixmap=" << m_lwePixmap << "size=" << m_pixmapWidth << "x" << m_pixmapHeight;
        setStatus(QStringLiteral("rendering"));

        // Send a fake mouse move to wake up the web rendering loop (CEF/Web wallpapers)
        XEvent ev{};
        auto &m = ev.xmotion;
        m.type = MotionNotify;
        m.window = w;
        m.root = DefaultRootWindow(d);
        m.subwindow = None;
        m.time = CurrentTime;
        m.x = 0; m.y = 0;
        m.x_root = 8000; m.y_root = 8000;
        m.same_screen = True;
        XSendEvent(d, w, True, PointerMotionMask, &ev);
        XFlush(d);

        m_findTimer.stop();
    }
}

void LWEView::releasePixmap()
{
    if (!m_lwePixmap) return;
    Display *d = xdisplay();
    if (d) XFreePixmap(d, m_lwePixmap);
    m_lwePixmap = 0;
    m_pixmapWidth = 0;
    m_pixmapHeight = 0;
}

void LWEView::refreshPixmap()
{
    if (!m_lweWindow) return;
    Display *d = xdisplay();
    if (!d) return;
    XWindowAttributes attrs{};
    if (!XGetWindowAttributes(d, m_lweWindow, &attrs)) {
        qWarning() << "[lwepaper] XGetWindowAttributes failed for window" << m_lweWindow;
        return;
    }

    // If the window is unmapped/unviewable, skip pixmap allocation
    if (attrs.map_state != IsViewable) {
        return;
    }

    // Force the window to stay off-screen at 8000,8000.
    // Managed windows can have their positions overridden by KWin upon mapping.
    // Enforcing this on every frame ensures it stays safely off-screen.
    if (attrs.x != 8000 || attrs.y != 8000) {
        XMoveWindow(d, m_lweWindow, 8000, 8000);
        XSync(d, False);
    }

    if (m_lwePixmap && attrs.width == m_pixmapWidth && attrs.height == m_pixmapHeight)
        return;
    releasePixmap();
    m_lwePixmap = XCompositeNameWindowPixmap(d, m_lweWindow);
    m_pixmapWidth = attrs.width;
    m_pixmapHeight = attrs.height;
    qWarning() << "[lwepaper] refreshPixmap: new pixmap" << m_lwePixmap
               << "window attrs" << attrs.width << "x" << attrs.height
               << "map_state=" << attrs.map_state;
}

void LWEView::geometryChange(const QRectF &newG, const QRectF &oldG)
{
    QQuickItem::geometryChange(newG, oldG);
    // First time the size becomes valid, launch LWE if we have a wallpaper.
    // If the size changes significantly later, relaunch (LWE bakes its
    // rendering size in at startup).
    const bool was_valid = oldG.width() > 0 && oldG.height() > 0;
    const bool now_valid = newG.width() > 0 && newG.height() > 0;
    const bool big_change =
        was_valid && now_valid &&
        (qAbs(int(newG.width())  - int(oldG.width()))  > 4 ||
         qAbs(int(newG.height()) - int(oldG.height())) > 4);
    if (m_ready && !m_wid.isEmpty() && now_valid && (!was_valid || big_change)) {
        scheduleRelaunch();
    }
    update();
}

QSGNode *LWEView::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    auto *node = static_cast<QSGSimpleTextureNode *>(oldNode);
    if (!node) {
        node = new QSGSimpleTextureNode();
        node->setOwnsTexture(true);
    }
    node->setRect(boundingRect());

    if (!window()) return node;

    // If LWE isn't rendering, show a premium glassmorphic loading screen.
    if (!m_lweWindow) {
        if (!m_status.isEmpty() && m_status != QLatin1String("idle") && width() > 0 && height() > 0) {
            const int w = qMax(64, int(width()));
            const int h = qMax(64, int(height()));
            QImage fallback(w, h, QImage::Format_RGB32);
            
            QPainter p(&fallback);
            p.setRenderHint(QPainter::Antialiasing);

            // 1. Draw a beautiful dark slate-indigo gradient background
            QLinearGradient bgGrad(0, 0, w, h);
            bgGrad.setColorAt(0.0, QColor(14, 15, 23));
            bgGrad.setColorAt(1.0, QColor(24, 26, 39));
            p.fillRect(0, 0, w, h, bgGrad);

            // 2. Draw glassmorphic card in the center of the screen
            const int cardW = qMin(460, w - 40);
            const int cardH = qMin(180, h - 40);
            const int cardX = (w - cardW) / 2;
            const int cardY = (h - cardH) / 2;

            // Card background (dark translucent)
            QPainterPath cardPath;
            cardPath.addRoundedRect(QRectF(cardX, cardY, cardW, cardH), 16, 16);
            p.fillPath(cardPath, QColor(30, 32, 48, 200));

            // Card border (subtle glowing gradient)
            QLinearGradient borderGrad(cardX, cardY, cardX + cardW, cardY + cardH);
            borderGrad.setColorAt(0.0, QColor(255, 255, 255, 60));
            borderGrad.setColorAt(0.5, QColor(255, 255, 255, 10));
            borderGrad.setColorAt(1.0, QColor(255, 255, 255, 40));
            QPen borderPen(QBrush(borderGrad), 1.5);
            p.setPen(borderPen);
            p.drawPath(cardPath);

            // 3. Draw spinner/micro-animation in the card
            const int spinnerSize = 44;
            const int spinnerX = cardX + 32;
            const int spinnerY = cardY + (cardH - spinnerSize) / 2;

            // Draw background track ring
            p.setPen(QPen(QColor(255, 255, 255, 15), 4, Qt::SolidLine, Qt::RoundCap));
            p.drawEllipse(spinnerX, spinnerY, spinnerSize, spinnerSize);

            // Draw glowing spinning arc
            QConicalGradient spinGrad(spinnerX + spinnerSize/2, spinnerY + spinnerSize/2, 0);
            spinGrad.setColorAt(0.0, QColor(0, 240, 255)); // cyan
            spinGrad.setColorAt(0.5, QColor(180, 0, 255)); // violet
            spinGrad.setColorAt(1.0, QColor(0, 240, 255));
            
            p.setPen(QPen(QBrush(spinGrad), 4, Qt::SolidLine, Qt::RoundCap));
            // Rotate based on time
            int angle = (QDateTime::currentMSecsSinceEpoch() / 3) % 360;
            p.drawArc(spinnerX, spinnerY, spinnerSize, spinnerSize, angle * 16, 120 * 16);

            // 4. Draw Typography
            // Title
            p.setPen(QColor(255, 255, 255));
            QFont titleFont(QStringLiteral("sans-serif"));
            titleFont.setPixelSize(18);
            titleFont.setBold(true);
            p.setFont(titleFont);
            p.drawText(QRect(spinnerX + spinnerSize + 24, cardY + 36, cardW - spinnerSize - 70, 30),
                       Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral("Loading Wallpaper..."));

            // Subtitle / Status
            p.setPen(QColor(160, 165, 192));
            QFont statusFont(QStringLiteral("sans-serif"));
            statusFont.setPixelSize(13);
            p.setFont(statusFont);
            
            // Clean up status text (e.g. remove long absolute paths)
            QString displayStatus = m_status;
            if (displayStatus.contains(QStringLiteral("starting LWE"))) {
                displayStatus = QStringLiteral("Initializing graphics engine...");
            } else if (displayStatus.contains(QStringLiteral("timed out"))) {
                displayStatus = QStringLiteral("Connection timed out. Retrying...");
            }
            p.drawText(QRect(spinnerX + spinnerSize + 24, cardY + 70, cardW - spinnerSize - 70, 60),
                       Qt::AlignTop | Qt::AlignLeft | Qt::TextWordWrap, displayStatus);

            p.end();
            QSGTexture *oldTex = node->texture();
            if (auto *tex = window()->createTextureFromImage(fallback)) {
                node->setTexture(tex);
                if (oldTex) delete oldTex;
            }
        }
        return node;
    }
    Display *d = xdisplay();
    if (!d) return node;

    refreshPixmap();  // ensure pixmap matches current window size
    if (!m_lwePixmap || m_pixmapWidth <= 0 || m_pixmapHeight <= 0) return node;

    // Read the off-screen window contents straight from the composite pixmap.
    // This works whether or not the source window is currently mapped/visible.
    XImage *img = XGetImage(d, m_lwePixmap, 0, 0,
                            m_pixmapWidth, m_pixmapHeight,
                            AllPlanes, ZPixmap);
    if (!img) {
        static bool once = false;
        if (!once) { qWarning() << "[lwepaper] XGetImage returned null for pixmap" << m_lwePixmap; once = true; }
        return node;
    }
    {
        // Per-instance "first frame" log — static-bool dedup hides whether
        // each LWEView is actually getting frames. Move it into the object.
        if (!m_firstFrameLogged) {
            qWarning() << "[lwepaper] first frame for" << this
                       << "wid" << m_wid
                       << "win" << m_lweWindow
                       << "pixmap" << m_lwePixmap
                       << img->width << "x" << img->height
                       << "depth" << img->depth << "bpp" << img->bits_per_pixel;
            // Sample a few pixels in the middle so we can tell if it's pure black.
            int midOff = (img->height / 2) * img->bytes_per_line + (img->width / 2) * (img->bits_per_pixel / 8);
            if (midOff + 4 <= img->bytes_per_line * img->height) {
                unsigned char *p = reinterpret_cast<unsigned char *>(img->data) + midOff;
                qWarning() << "[lwepaper]  center pixel bgrx:" << p[0] << p[1] << p[2] << p[3];
            }
            m_firstFrameLogged = true;
        }
    }

    QImage qimg(reinterpret_cast<uchar *>(img->data),
                img->width, img->height, img->bytes_per_line,
                QImage::Format_RGB32);
    QImage copy = qimg.copy();
    XDestroyImage(img);
    // Draw optional debug overlay in the top-left corner
    if (m_showDebug) {
        QPainter p(&copy);
        p.setPen(QColor(0, 255, 0)); // bright green
        QFont f;
        f.setPixelSize(24);
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRect(40, 40, copy.width() - 80, 200),
                   Qt::AlignTop | Qt::AlignLeft,
                   QStringLiteral("[LWE DEBUG] status: %1 | window: %2 | pixmap: %3 | size: %4x%5")
                   .arg(m_status)
                   .arg(m_lweWindow)
                   .arg(m_lwePixmap)
                   .arg(m_pixmapWidth)
                   .arg(m_pixmapHeight));
        p.end();
    }

    QSGTexture *oldTex = node->texture();
    if (auto *tex = window()->createTextureFromImage(copy)) {
        node->setTexture(tex);
        if (oldTex) delete oldTex;
    }
    return node;
}
