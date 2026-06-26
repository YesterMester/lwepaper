#include "lwescenepatch.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDataStream>
#include <QtEndian>
#include <QDebug>

QStringList LWEScenePatch::knownBrokenEffects()
{
    return {
        // The classic v_TexCoord shader bug — LWE's combo preprocessor
        // produces an `v_TexCoord = ...` line that GLSL rejects. Hits
        // Valtiel (Silent Hill 3) and any other scene using waterflow.
        QStringLiteral("ui_editor_effect_water_flow_title"),
        // Depth-parallax shares the same shader family in some versions.
        // Removing it loses a niche effect; keeps the wallpaper renderable.
        QStringLiteral("depthparallax"),
    };
}

QByteArray LWEScenePatch::stripEffectsFromSceneJson(const QByteArray &json,
                                                    const QStringList &effectNames)
{
    QJsonParseError err{};
    QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return json;

    QJsonObject scene = doc.object();
    QJsonArray objects = scene.value(QStringLiteral("objects")).toArray();
    bool changed = false;

    for (int i = 0; i < objects.size(); ++i) {
        QJsonObject obj = objects[i].toObject();
        QJsonArray effects = obj.value(QStringLiteral("effects")).toArray();
        if (effects.isEmpty()) continue;
        QJsonArray kept;
        for (const auto &eff : effects) {
            const QJsonObject eo = eff.toObject();
            const QString name = eo.value(QStringLiteral("name")).toString();
            // Match either the localized effect name or the shader file ref.
            bool drop = effectNames.contains(name);
            if (!drop) {
                // Walk passes -> shaders to catch effects referenced only
                // by their shader path (some scenes use that form).
                const QJsonArray passes = eo.value(QStringLiteral("passes")).toArray();
                for (const auto &p : passes) {
                    const QString frag = p.toObject().value(QStringLiteral("shader")).toString();
                    for (const QString &needle : effectNames) {
                        if (!needle.isEmpty() && frag.contains(needle, Qt::CaseInsensitive)) {
                            drop = true;
                            break;
                        }
                    }
                    if (drop) break;
                }
            }
            if (drop) { changed = true; continue; }
            kept.append(eff);
        }
        if (kept.size() != effects.size()) {
            obj.insert(QStringLiteral("effects"), kept);
            objects[i] = obj;
        }
    }
    if (!changed) return json;
    scene.insert(QStringLiteral("objects"), objects);
    return QJsonDocument(scene).toJson(QJsonDocument::Compact);
}

bool LWEScenePatch::patchIfNeeded(const QString &wallpaperPath)
{
    const QString pkgPath = wallpaperPath + QStringLiteral("/scene.pkg");
    QFileInfo fi(pkgPath);
    if (!fi.exists()) return false;

    QString sourcePath = pkgPath;
    if (fi.isSymLink()) {
        sourcePath = fi.symLinkTarget();
        if (sourcePath.isEmpty()) return false;
    }

    QFile in(sourcePath);
    if (!in.open(QIODevice::ReadOnly)) return false;
    QByteArray data = in.readAll(); in.close();

    // Parse pkg, find scene.json, patch it, repack into a new file.
    // PKG format: u32 magic_len, magic bytes, u32 n_entries,
    //   for each: u32 name_len, name, u32 offset, u32 size, then blob.
    const auto reader = [&](int o, quint32 &out) {
        out = qFromLittleEndian<quint32>(data.constData() + o);
    };

    if (data.size() < 12) return false;
    quint32 magicLen; reader(0, magicLen);
    int o = 4 + int(magicLen);
    if (o + 4 > data.size()) return false;
    const QString magic = QString::fromLatin1(data.constData() + 4, magicLen);
    quint32 nEntries; reader(o, nEntries); o += 4;

    struct E { QString name; quint32 off, size; };
    QList<E> entries;
    for (quint32 i = 0; i < nEntries; ++i) {
        if (o + 4 > data.size()) return false;
        quint32 nl; reader(o, nl); o += 4;
        if (o + nl + 8 > data.size()) return false;
        QString name = QString::fromUtf8(data.constData() + o, nl); o += nl;
        quint32 eo; reader(o, eo); o += 4;
        quint32 es; reader(o, es); o += 4;
        entries.append({name, eo, es});
    }
    const int blobStart = o;

    int sceneIdx = -1;
    for (int i = 0; i < entries.size(); ++i) {
        if (entries[i].name == QLatin1String("scene.json")) {
            sceneIdx = i;
            break;
        }
    }
    if (sceneIdx < 0) return false;

    if (blobStart + entries[sceneIdx].off + entries[sceneIdx].size > static_cast<quint32>(data.size()))
        return false;

    QByteArray sceneJson = data.mid(blobStart + entries[sceneIdx].off,
                                    entries[sceneIdx].size);
    QByteArray patched = stripEffectsFromSceneJson(sceneJson, knownBrokenEffects());
    if (patched == sceneJson) return false;   // nothing changed

    // Rewriting the whole pkg with new offsets: Header (kept) + new entry table + new blob.
    QByteArray out;
    // magic
    out.append(reinterpret_cast<const char *>(&magicLen), 4);
    out.append(magic.toLatin1());
    // n_entries
    out.append(reinterpret_cast<const char *>(&nEntries), 4);

    // New blob & header table
    QByteArray newHeaders;
    QByteArray newBlob;
    for (int i = 0; i < entries.size(); ++i) {
        const QByteArray content = (i == sceneIdx)
            ? patched
            : QByteArray(data.constData() + blobStart + int(entries[i].off),
                         int(entries[i].size));
        quint32 nl = entries[i].name.toUtf8().size();
        quint32 eo = quint32(newBlob.size());
        quint32 es = quint32(content.size());
        newHeaders.append(reinterpret_cast<const char *>(&nl), 4);
        newHeaders.append(entries[i].name.toUtf8());
        newHeaders.append(reinterpret_cast<const char *>(&eo), 4);
        newHeaders.append(reinterpret_cast<const char *>(&es), 4);
        newBlob.append(content);
    }
    out.append(newHeaders);
    out.append(newBlob);

    // If it's a symlink, remove it first to write a new real file at pkgPath.
    if (fi.isSymLink()) {
        QFile::remove(pkgPath);
    }
    
    QFile w(pkgPath);
    if (!w.open(QIODevice::WriteOnly)) return false;
    w.write(out);
    w.close();
    
    qWarning() << "[lwepaper] patched scene.pkg in" << wallpaperPath
               << "— stripped" << knownBrokenEffects().size()
               << "known-broken effect(s)";
    return true;
}
