#include "lwelibrary.h"
#include "lwescenepatch.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QDebug>
#include <algorithm>

static QString detectAndFixMissingTypeAndFile(const QString &dirPath, QJsonObject &obj, const QString &pjPath, bool linkDependencies = false)
{
    QString type = obj.value(QStringLiteral("type")).toString().toLower();
    QString file = obj.value(QStringLiteral("file")).toString();

    bool hasScenePkg = QFileInfo::exists(dirPath + QStringLiteral("/scene.pkg"));
    bool hasGifPkg = QFileInfo::exists(dirPath + QStringLiteral("/gifscene.pkg"));
    bool hasPkg = hasScenePkg || hasGifPkg;

    QString targetVirtualFile = QStringLiteral("scene.json");
    if (hasGifPkg && !hasScenePkg) {
        targetVirtualFile = QStringLiteral("gifscene.json");
        QFile::remove(dirPath + QStringLiteral("/scene.pkg"));
        QFile::link(dirPath + QStringLiteral("/gifscene.pkg"), dirPath + QStringLiteral("/scene.pkg"));
    }

    bool hasMainFile = false;

    // Check if the physical file is present
    if (!file.isEmpty() && QFileInfo::exists(dirPath + QLatin1Char('/') + file)) {
        if (file == QStringLiteral("scene.pkg") || file == QStringLiteral("gifscene.pkg")) {
            file = targetVirtualFile;
        }
        hasMainFile = true;
    } else if (hasPkg && (file == QStringLiteral("scene.json") || file == QStringLiteral("gifscene.json") || file.isEmpty())) {
        file = targetVirtualFile;
        hasMainFile = true;
    }

    if (!hasMainFile) {
        // Look for common physical files in the directory
        for (const auto &name : {QStringLiteral("scene.json"), QStringLiteral("index.html")}) {
            if (QFileInfo::exists(dirPath + QLatin1Char('/') + name)) {
                hasMainFile = true;
                file = name;
                break;
            }
        }
        // If not found, check if a package exists physically
        if (!hasMainFile && hasPkg) {
            hasMainFile = true;
            file = QStringLiteral("scene.json");
        }
    }

    // Collect and link dependencies if they exist
    {
        // Collect dependencies
        QStringList deps;
        auto collect = [&deps](const QJsonValue &v) {
            if (v.isArray()) {
                for (const auto &x : v.toArray()) {
                    const QString s = x.isString() ? x.toString()
                        : (x.isObject() ? x.toObject().value("id").toString() : QString());
                    if (!s.isEmpty()) deps << s;
                }
            } else if (v.isString()) {
                deps << v.toString();
            }
        };
        collect(obj.value(QStringLiteral("dependencies")));
        collect(obj.value(QStringLiteral("dependency")));
        collect(obj.value(QStringLiteral("required")));
        deps.removeDuplicates();

        for (const QString &depId : deps) {
            QString depPath = QFileInfo(dirPath).absolutePath() + QLatin1Char('/') + depId;
            
            // Read dependency's project.json to find its correct "file" key
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

            // Correct dependency's "file" value if it is scene.pkg/gifscene.pkg
            if (depFileVal == QStringLiteral("scene.pkg") || depFileVal == QStringLiteral("gifscene.pkg")) {
                depFileVal = QStringLiteral("scene.json");
            }

            // Link all potential physical files that support LWE rendering
            bool linkedAny = false;
            QDir depDir(depPath);
            if (linkDependencies && depDir.exists()) {
                QStringList depEntries = depDir.entryList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
                for (const QString &name : depEntries) {
                    if (name == QStringLiteral("project.json")) {
                        continue;
                    }
                    QString src = depPath + QLatin1Char('/') + name;
                    QString dst = dirPath + QLatin1Char('/') + name;
                    QFile::remove(dst); // remove stale links
                    if (QFile::link(src, dst)) {
                        qWarning() << "[lwepaper] Created link from" << src << "to" << dst;
                        linkedAny = true;
                    } else {
                        qWarning() << "[lwepaper] Failed to create link from" << src << "to" << dst;
                    }
                }
            } else if (!linkDependencies) {
                // If we are not linking, just assume linkedAny is true if the dependency exists
                // so we can resolve the file type properly.
                linkedAny = depDir.exists();
            }

            if (linkedAny && !hasMainFile && !depFileVal.isEmpty()) {
                file = depFileVal;
                hasMainFile = true;
            }
        }
    }

    // Now resolve the type based on files
    if (type.isEmpty()) {
        if (file == QStringLiteral("index.html")) {
            type = QStringLiteral("web");
        } else if (file == QStringLiteral("scene.json") ||
                   file == QStringLiteral("scene.pkg") ||
                   file == QStringLiteral("gifscene.pkg")) {
            type = QStringLiteral("scene");
        } else {
            // Check for videos
            QStringList filters = { QStringLiteral("*.mp4"), QStringLiteral("*.webm"), QStringLiteral("*.avi"),
                                    QStringLiteral("*.mkv"), QStringLiteral("*.mov") };
            QDir dir(dirPath);
            QStringList videos = dir.entryList(filters, QDir::Files);
            if (videos.isEmpty() && dir.exists(QStringLiteral("files"))) {
                QDir filesDir(dirPath + QStringLiteral("/files"));
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

    // Save back to project.json if anything changed
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
        QFile f(pjPath);
        if (f.open(QIODevice::WriteOnly)) {
            QJsonDocument doc(obj);
            f.write(doc.toJson());
            f.close();
            qWarning() << "[lwepaper] Auto-patched project.json type=" << type << "file=" << file << "for" << pjPath;
        }
    }

    return type;
}

LWELibrary::LWELibrary(QObject *parent) : QAbstractListModel(parent)
{
    // Default to the standard Steam workshop content path for Wallpaper Engine.
    m_workshopDir = QDir::homePath() +
        QStringLiteral("/.local/share/Steam/steamapps/workshop/content/431960");
    scan();
    // Pre-link dependencies on first load so any wallpaper with a `dependency`
    // field in project.json is rendering-ready before the user clicks it.
    prepareAllDependencies();
    applyFilterAndSort();
}

void LWELibrary::setWorkshopDir(const QString &dir)
{
    if (dir == m_workshopDir) return;
    m_workshopDir = dir;
    emit workshopDirChanged();
    reload();
}

void LWELibrary::setFilterType(const QString &type)
{
    if (type == m_filterType) return;
    m_filterType = type;
    emit filterTypeChanged();
    applyFilterAndSort();
}

void LWELibrary::setSearch(const QString &s)
{
    if (s == m_search) return;
    m_search = s;
    emit searchChanged();
    applyFilterAndSort();
}

void LWELibrary::setSortMode(const QString &mode)
{
    if (mode == m_sortMode) return;
    m_sortMode = mode;
    emit sortModeChanged();
    applyFilterAndSort();
}

void LWELibrary::reload()
{
    scan();
    // Always re-link dependencies after a scan so the picker shows a
    // consistent, ready-to-render library.
    prepareAllDependencies();
    applyFilterAndSort();
}

void LWELibrary::collectDependenciesRecursively(const QString &workshopDir, const QString &wpId, QStringList &collected, bool isRoot)
{
    if (!isRoot) {
        if (collected.contains(wpId)) return;
        collected.append(wpId);
    }
    
    QString pjPath = workshopDir + QLatin1Char('/') + wpId + QStringLiteral("/project.json");
    QFile f(pjPath);
    if (!f.open(QIODevice::ReadOnly)) return;
    
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject()) return;
    
    QJsonObject obj = doc.object();
    QStringList deps;
    auto collect = [&deps](const QJsonValue &v) {
        if (v.isArray()) {
            for (const auto &x : v.toArray()) {
                const QString s = x.isString() ? x.toString()
                    : (x.isObject() ? x.toObject().value("id").toString() : QString());
                if (!s.isEmpty()) deps << s;
            }
        } else if (v.isString()) {
            deps << v.toString();
        }
    };
    collect(obj.value(QStringLiteral("dependencies")));
    collect(obj.value(QStringLiteral("dependency")));
    collect(obj.value(QStringLiteral("required")));
    deps.removeDuplicates();
    
    for (const QString &depId : deps) {
        collectDependenciesRecursively(workshopDir, depId, collected, false);
    }
}

bool LWELibrary::linkFilesRecursively(const QString &srcPath, const QString &dstPath)
{
    QFileInfo srcInfo(srcPath);
    if (!srcInfo.exists()) return false;

    if (srcInfo.isDir()) {
        QDir srcDir(srcPath);
        QFileInfo dstInfo(dstPath);
        
        if ((dstInfo.exists() || dstInfo.isSymLink()) && dstInfo.isSymLink()) {
            QFile::remove(dstPath);
        }

        QDir dstDir(dstPath);
        if (!dstDir.exists()) {
            if (!dstDir.mkpath(QStringLiteral("."))) {
                qWarning() << "[lwepaper] Failed to create directory" << dstPath;
                return false;
            }
        }

        bool success = true;
        const QStringList entries = srcDir.entryList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
        for (const QString &entry : entries) {
            if (entry == QStringLiteral("project.json")) continue;
            QString childSrc = srcPath + QLatin1Char('/') + entry;
            QString childDst = dstPath + QLatin1Char('/') + entry;
            if (!linkFilesRecursively(childSrc, childDst)) {
                success = false;
            }
        }
        return success;
    } else {
        QFileInfo dstInfo(dstPath);
        if (dstInfo.exists() || dstInfo.isSymLink()) {
            if (dstInfo.isSymLink()) {
                if (QFile::symLinkTarget(dstPath) == srcPath) return true;
                QFile::remove(dstPath);
            } else {
                return true; // Keep local file
            }
        }
        return QFile::link(srcPath, dstPath);
    }
}

bool LWELibrary::linkDepFiles(const QString &wpPath, const QString &depId)
{
    QString workshopRoot = QFileInfo(wpPath).absolutePath();
    QString depPath = workshopRoot + QLatin1Char('/') + depId;
    QDir depDir(depPath);
    if (!depDir.exists()) return false;

    bool any = false;
    const QStringList entries = depDir.entryList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);
    for (const QString &name : entries) {
        if (name == QLatin1String("project.json")) continue;
        QString src = depPath + QLatin1Char('/') + name;
        QString dst = wpPath + QLatin1Char('/') + name;
        if (linkFilesRecursively(src, dst)) any = true;
    }
    return any;
}

bool LWELibrary::prepareWallpaper(const QString &workshopId)
{
    for (const auto &e : m_all) {
        if (e.id != workshopId) continue;
        QStringList allDeps;
        QString workshopRoot = QFileInfo(e.path).absolutePath();
        collectDependenciesRecursively(workshopRoot, workshopId, allDeps, true);
        if (allDeps.isEmpty()) return false;
        
        bool any = false;
        for (const QString &depId : allDeps) {
            if (linkDepFiles(e.path, depId)) any = true;
        }
        return any;
    }
    return false;
}

int LWELibrary::prepareAllDependencies()
{
    int prepared = 0;
    for (const auto &e : m_all) {
        QStringList allDeps;
        QString workshopRoot = QFileInfo(e.path).absolutePath();
        collectDependenciesRecursively(workshopRoot, e.id, allDeps, true);
        if (allDeps.isEmpty()) continue;
        
        bool any = false;
        for (const QString &depId : allDeps) {
            if (linkDepFiles(e.path, depId)) any = true;
        }
        if (any) ++prepared;
    }
    return prepared;
}

bool LWELibrary::patchWallpaper(const QString &workshopId)
{
    for (const auto &e : m_all) {
        if (e.id != workshopId) continue;
        const bool changed = LWEScenePatch::patchIfNeeded(e.path);
        qInfo() << "[lwepaper] manual patch of" << workshopId
                << (changed ? "applied changes" : "no changes needed");
        return changed;
    }
    qWarning() << "[lwepaper] manual patch: workshop id" << workshopId << "not found in library";
    return false;
}

int LWELibrary::patchAllWallpapers()
{
    int patched = 0;
    for (const auto &e : m_all) {
        // patchIfNeeded no-ops cleanly on non-scene wallpapers (no scene.pkg).
        if (LWEScenePatch::patchIfNeeded(e.path)) ++patched;
    }
    qInfo() << "[lwepaper] Patch all: changed" << patched << "of" << m_all.size() << "wallpaper(s)";
    return patched;
}

QStringList LWELibrary::missingDependencies(const QString &workshopId) const
{
    QStringList allDeps;
    QString workshopRoot = m_workshopDir;
    for (const auto &e : m_all) {
        if (e.id == workshopId) {
            workshopRoot = QFileInfo(e.path).absolutePath();
            break;
        }
    }
    collectDependenciesRecursively(workshopRoot, workshopId, allDeps, true);
    
    QStringList missing;
    for (const QString &dep : allDeps) {
        if (!QFileInfo::exists(workshopRoot + QLatin1Char('/') + dep + QStringLiteral("/project.json"))) {
            missing << dep;
        }
    }
    return missing;
}

QJsonArray LWELibrary::getWallpaperProperties(const QString &workshopId) const
{
    QJsonArray result;
    QString path = m_workshopDir + QLatin1Char('/') + workshopId;
    QString pjPath = path + QStringLiteral("/project.json");
    QFile f(pjPath);
    if (!f.open(QIODevice::ReadOnly)) return result;
    
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject()) return result;
    
    QJsonObject obj = doc.object();
    QJsonObject propertiesObj = obj.value(QStringLiteral("properties")).toObject();
    if (propertiesObj.isEmpty() && obj.contains(QStringLiteral("general"))) {
        propertiesObj = obj.value(QStringLiteral("general")).toObject().value(QStringLiteral("properties")).toObject();
    }
    QJsonObject presetObj = obj.value(QStringLiteral("preset")).toObject();
    
    for (auto it = propertiesObj.begin(); it != propertiesObj.end(); ++it) {
        QString key = it.key();
        QJsonObject prop = it.value().toObject();
        
        QJsonObject propInfo;
        propInfo.insert(QStringLiteral("key"), key);
        propInfo.insert(QStringLiteral("text"), prop.value(QStringLiteral("text")).toString(key));
        propInfo.insert(QStringLiteral("type"), prop.value(QStringLiteral("type")).toString());
        
        if (prop.contains(QStringLiteral("min"))) propInfo.insert(QStringLiteral("min"), prop.value(QStringLiteral("min")));
        if (prop.contains(QStringLiteral("max"))) propInfo.insert(QStringLiteral("max"), prop.value(QStringLiteral("max")));
        if (prop.contains(QStringLiteral("step"))) propInfo.insert(QStringLiteral("step"), prop.value(QStringLiteral("step")));
        if (prop.contains(QStringLiteral("options"))) propInfo.insert(QStringLiteral("options"), prop.value(QStringLiteral("options")));
        
        QJsonValue activeVal;
        if (presetObj.contains(key)) {
            activeVal = presetObj.value(key);
        } else if (prop.contains(QStringLiteral("value"))) {
            activeVal = prop.value(QStringLiteral("value"));
        }
        propInfo.insert(QStringLiteral("value"), activeVal);
        
        result.append(propInfo);
    }
    return result;
}

void LWELibrary::saveWallpaperProperty(const QString &workshopId, const QString &key, const QJsonValue &value)
{
    QString path = m_workshopDir + QLatin1Char('/') + workshopId;
    QString pjPath = path + QStringLiteral("/project.json");
    QFile f(pjPath);
    if (!f.open(QIODevice::ReadOnly)) return;
    
    QByteArray bytes = f.readAll();
    f.close();
    
    QJsonParseError err{};
    QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return;
    
    QJsonObject obj = doc.object();
    QJsonObject propertiesObj = obj.value(QStringLiteral("properties")).toObject();
    if (propertiesObj.isEmpty() && obj.contains(QStringLiteral("general"))) {
        propertiesObj = obj.value(QStringLiteral("general")).toObject().value(QStringLiteral("properties")).toObject();
    }
    QJsonObject prop = propertiesObj.value(key).toObject();
    QString propType = prop.value(QStringLiteral("type")).toString().toLower();

    QJsonValue typedVal = value;
    if (propType == QStringLiteral("bool")) {
        if (value.isString()) {
            typedVal = (value.toString() == QStringLiteral("true"));
        } else {
            typedVal = value.toBool();
        }
    } else if (propType == QStringLiteral("slider")) {
        if (value.isString()) {
            typedVal = value.toString().toDouble();
        } else {
            typedVal = value.toDouble();
        }
    }

    QJsonObject presetObj = obj.value(QStringLiteral("preset")).toObject();
    presetObj.insert(key, typedVal);
    obj.insert(QStringLiteral("preset"), presetObj);
    
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(obj).toJson());
        f.close();
        qWarning() << "[lwepaper] saved property" << key << "=" << typedVal << "for" << workshopId;
        emit changed();
    }
}

QString LWELibrary::getWallpaperType(const QString &workshopId) const
{
    for (const auto &e : m_all) {
        if (e.id == workshopId) return e.type;
    }
    return {};
}

void LWELibrary::scan()
{
    m_all.clear();
    QDir d(m_workshopDir);
    if (!d.exists()) return;

    const auto subdirs = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &sub : subdirs) {
        const QString path = d.absoluteFilePath(sub);
        const QString pj = path + QStringLiteral("/project.json");
        QFile f(pj);
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QByteArray bytes = f.readAll();
        f.close();
        QJsonParseError err{};
        QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) continue;
        QJsonObject obj = doc.object();

        Entry e;
        e.id = sub;
        e.path = path;
        e.title = obj.value(QStringLiteral("title")).toString(sub);
        e.type = detectAndFixMissingTypeAndFile(path, obj, pj);
        e.modified = QFileInfo(pj).lastModified().toSecsSinceEpoch();
        const QString preview = obj.value(QStringLiteral("preview")).toString();
        if (!preview.isEmpty()) {
            const QString absPreview = path + QLatin1Char('/') + preview;
            if (QFileInfo::exists(absPreview))
                e.previewUrl = QUrl::fromLocalFile(absPreview).toString();
        }
        if (e.type == QLatin1String("scene"))
            e.pkgVersion = detectPkgVersion(path);

        // Dependencies — Wallpaper Engine projects use a few keys for this
        // depending on era, AND the value can be either an array OR a single
        // string (e.g. SH98' has `"dependency": "3175288461"`).
        auto collect = [&e](const QJsonValue &v) {
            if (v.isArray()) {
                for (const auto &x : v.toArray()) {
                    const QString s = x.isString() ? x.toString()
                        : (x.isObject() ? x.toObject().value("id").toString() : QString());
                    if (!s.isEmpty()) e.dependencies << s;
                }
            } else if (v.isString()) {
                e.dependencies << v.toString();
            }
        };
        collect(obj.value(QStringLiteral("dependencies")));
        collect(obj.value(QStringLiteral("dependency")));
        collect(obj.value(QStringLiteral("required")));
        e.dependencies.removeDuplicates();
        m_all.append(e);
    }
}

QString LWELibrary::detectPkgVersion(const QString &dir) const
{
    // The .pkg files start with: 4-byte little-endian length of magic, then
    // ASCII magic like "PKGV0018".
    for (const QString &name : {QStringLiteral("scene.pkg"), QStringLiteral("gifscene.pkg")}) {
        const QString p = dir + QLatin1Char('/') + name;
        QFile f(p);
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QByteArray header = f.read(16);
        f.close();
        if (header.size() < 12) continue;
        const quint32 magic_len = static_cast<quint8>(header[0])
            | (static_cast<quint8>(header[1]) << 8)
            | (static_cast<quint8>(header[2]) << 16)
            | (static_cast<quint8>(header[3]) << 24);
        if (magic_len >= 4 && magic_len + 4 <= static_cast<quint32>(header.size()))
            return QString::fromLatin1(header.constData() + 4, magic_len);
    }
    return {};
}

void LWELibrary::applyFilterAndSort()
{
    beginResetModel();
    m_entries.clear();
    const bool hasSearch = !m_search.trimmed().isEmpty();
    const QString needle = m_search.trimmed().toLower();
    for (const auto &e : m_all) {
        if (!m_filterType.isEmpty() && e.type != m_filterType) continue;
        if (hasSearch && !e.title.toLower().contains(needle) && !e.id.contains(needle))
            continue;
        m_entries.append(e);
    }
    auto cmp = [this](const Entry &a, const Entry &b) {
        if (m_sortMode == QLatin1String("modified")) return a.modified > b.modified; // newest first
        if (m_sortMode == QLatin1String("id"))       return a.id < b.id;
        if (m_sortMode == QLatin1String("type"))     return std::tie(a.type, a.title) < std::tie(b.type, b.title);
        if (m_sortMode == QLatin1String("pkg"))      return std::tie(a.pkgVersion, a.title) < std::tie(b.pkgVersion, b.title);
        // default: title
        return QString::compare(a.title, b.title, Qt::CaseInsensitive) < 0;
    };
    std::stable_sort(m_entries.begin(), m_entries.end(), cmp);
    endResetModel();
    emit changed();
}

int LWELibrary::rowCount(const QModelIndex &) const { return m_entries.size(); }

QVariant LWELibrary::data(const QModelIndex &idx, int role) const
{
    if (!idx.isValid() || idx.row() < 0 || idx.row() >= m_entries.size())
        return {};
    const Entry &e = m_entries.at(idx.row());
    switch (role) {
    case IdRole: return e.id;
    case TitleRole: return e.title;
    case PreviewRole: return e.previewUrl;
    case TypeRole: return e.type;
    case PathRole: return e.path;
    case PkgVersionRole: return e.pkgVersion;
    case ModifiedRole: return e.modified;
    case DependenciesRole: return e.dependencies;
    case MissingDependenciesRole: {
        QStringList missing;
        const QString workshopRoot = QFileInfo(e.path).absolutePath();
        for (const QString &dep : e.dependencies) {
            if (!QFileInfo::exists(workshopRoot + QLatin1Char('/') + dep + QStringLiteral("/project.json")))
                missing << dep;
        }
        return missing;
    }
    }
    return {};
}

QHash<int, QByteArray> LWELibrary::roleNames() const
{
    return {
        {IdRole, "wid"},
        {TitleRole, "title"},
        {PreviewRole, "preview"},
        {TypeRole, "type"},
        {PathRole, "path"},
        {PkgVersionRole, "pkgVersion"},
        {ModifiedRole, "modified"},
        {DependenciesRole, "dependencies"},
        {MissingDependenciesRole, "missingDependencies"},
    };
}
