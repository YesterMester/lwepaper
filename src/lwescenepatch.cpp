#include "lwescenepatch.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDataStream>
#include <QtEndian>
#include <QDebug>
#include <QRegularExpression>

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

static void injectGlobalsIntoScripts(QJsonValue &value, bool &changed, const QString &propName = QString())
{
    if (value.isObject()) {
        QJsonObject obj = value.toObject();
        if (obj.contains(QStringLiteral("script"))) {
            QJsonValue scriptVal = obj.value(QStringLiteral("script"));
            if (scriptVal.isString()) {
                QString scriptText = scriptVal.toString();
                bool needsPatch = false;

                // Extract and strip old globals inject to ensure a clean update
                if (scriptText.contains(QStringLiteral("/* LWE GLOBALS INJECT */"))) {
                    int idx = scriptText.indexOf(QStringLiteral("/* LWE GLOBALS INJECT */"));
                    int endIdx = scriptText.indexOf(QStringLiteral("})();"), idx);
                    if (endIdx != -1) {
                        scriptText.remove(idx, endIdx + 5 - idx);
                        scriptText = scriptText.trimmed();
                        needsPatch = true;
                    }
                } else {
                    needsPatch = true;
                }

                // Check for ES6 syntax that needs stripping
                if (scriptText.contains(QStringLiteral("import ")) || scriptText.contains(QStringLiteral("export "))) {
                    needsPatch = true;
                }

                if (needsPatch) {
                    // Clean ES6 import/export syntax to prevent QJSEngine SyntaxErrors
                    // 1. Remove import statements
                    QRegularExpression importRegex(QStringLiteral("^[ \\t]*import\\s+.*?\\s+from\\s+['\"].*?['\"];?"), QRegularExpression::MultilineOption);
                    scriptText.replace(importRegex, QString());

                    // 2. Replace export default function/class with function/class
                    QRegularExpression exportDefaultFuncRegex(QStringLiteral("^[ \\t]*export\\s+default\\s+(function|class)\\b"), QRegularExpression::MultilineOption);
                    scriptText.replace(exportDefaultFuncRegex, QStringLiteral("\\1"));

                    // 3. Comment out other export default occurrences
                    QRegularExpression exportDefaultRegex(QStringLiteral("^[ \\t]*export\\s+default\\b"), QRegularExpression::MultilineOption);
                    scriptText.replace(exportDefaultRegex, QStringLiteral("// export default"));

                    // 4. Strip export var/let/const/function/class
                    QRegularExpression exportDeclRegex(QStringLiteral("^[ \\t]*export\\s+(var|let|const|function|class)\\b"), QRegularExpression::MultilineOption);
                    scriptText.replace(exportDeclRegex, QStringLiteral("\\1"));

                    // 5. Comment out generic export statement lines
                    QRegularExpression exportRegex(QStringLiteral("^[ \\t]*export\\b"), QRegularExpression::MultilineOption);
                    scriptText.replace(exportRegex, QStringLiteral("// export"));

                    // 6. Define the script's local _lwe_default_value and _lwe_ensure_init
                    QString defaultValueCode = QStringLiteral(
                        "var _lwe_default_value = (function(){\n"
                        "  var prop = \"%1\".toLowerCase();\n"
                        "  if(prop.indexOf(\"scale\")!==-1) return new Vec3(1,1,1);\n"
                        "  if(prop.indexOf(\"color\")!==-1||prop.indexOf(\"origin\")!==-1||prop.indexOf(\"position\")!==-1||prop.indexOf(\"angles\")!==-1||prop.indexOf(\"offset\")!==-1||prop.indexOf(\"direction\")!==-1) return new Vec3(0,0,0);\n"
                        "  if(prop.indexOf(\"text\")!==-1) return \"\";\n"
                        "  if(prop.indexOf(\"visible\")!==-1) return true;\n"
                        "  return 1;\n"
                        "})();\n"
                        "var _lwe_initialized = false;\n"
                        "function _lwe_ensure_init(val) {\n"
                        "  if(!_lwe_initialized){\n"
                        "    _lwe_initialized = true;\n"
                        "    if(typeof init === 'function'){\n"
                        "      init(val !== undefined ? val : _lwe_default_value);\n"
                        "    }\n"
                        "  }\n"
                        "}\n"
                    ).arg(propName.contains('"') ? QString() : propName);

                    // 7. Inject guards inside functions in the script text
                    QRegularExpression funcRegex(QStringLiteral("\\bfunction\\s+(init|update)\\s*\\(\\s*value\\s*\\)\\s*\\{"));
                    scriptText.replace(funcRegex, QStringLiteral("function \\1(value) {\n  if(typeof value === 'undefined'||value===null){value=_lwe_default_value;}\n  _lwe_ensure_init(value);\n"));

                    QRegularExpression otherFuncRegex(QStringLiteral("\\bfunction\\s+((?!init\\b|update\\b)[a-zA-Z0-9_]+)\\s*\\(([^)]*)\\)\\s*\\{"));
                    scriptText.replace(otherFuncRegex, QStringLiteral("function \\1(\\2) {\n  _lwe_ensure_init(undefined);\n"));

                    // Prepare latest shim with Vec2, Vec3, Vec4, Color, localStorage, and WEMath
                    QString shim = QStringLiteral(
                        "/* LWE GLOBALS INJECT */\n"
                        "(function(){\n"
                        "  var g=(typeof globalThis!=='undefined')?globalThis:(typeof window!=='undefined')?window:(typeof global!=='undefined')?global:this;\n"
                        "  if(g){\n"
                        "    if(typeof g.Vec2==='undefined'){\n"
                        "      g.Vec2=class Vec2{\n"
                        "        constructor(x=0,y=0){if(typeof x==='object'&&x!==null){this.x=x.x||0;this.y=x.y||0;}else{this.x=x;this.y=y;}}\n"
                        "        multiply(v){if(typeof v==='number')return new Vec2(this.x*v,this.y*v);return new Vec2(this.x*(v.x||0),this.y*(v.y||0));}\n"
                        "        add(v){return new Vec2(this.x+(v.x||0),this.y+(v.y||0));}\n"
                        "      };\n"
                        "    }\n"
                        "    if(typeof g.Vec3==='undefined'){\n"
                        "      g.Vec3=class Vec3{\n"
                        "        constructor(x=0,y=0,z=0){if(typeof x==='object'&&x!==null){this.x=x.x||0;this.y=x.y||0;this.z=x.z||0;}else{this.x=x;this.y=y;this.z=z;}}\n"
                        "        multiply(v){if(typeof v==='number')return new Vec3(this.x*v,this.y*v,this.z*v);return new Vec3(this.x*(v.x||0),this.y*(v.y||0),this.z*(v.z||0));}\n"
                        "        add(v){return new Vec3(this.x+(v.x||0),this.y+(v.y||0),this.z+(v.z||0));}\n"
                        "        subtract(v){return new Vec3(this.x-(v.x||0),this.y-(v.y||0),this.z-(v.z||0));}\n"
                        "      };\n"
                        "    }\n"
                        "    if(typeof g.Vec4==='undefined'){\n"
                        "      g.Vec4=class Vec4{\n"
                        "        constructor(x=0,y=0,z=0,w=0){if(typeof x==='object'&&x!==null){this.x=x.x||0;this.y=x.y||0;this.z=x.z||0;this.w=x.w||0;}else{this.x=x;this.y=y;this.z=z;this.w=w;}}\n"
                        "        multiply(v){if(typeof v==='number')return new Vec4(this.x*v,this.y*v,this.z*v,this.w*v);return new Vec4(this.x*(v.x||0),this.y*(v.y||0),this.z*(v.z||0),this.w*(v.w||0));}\n"
                        "        add(v){return new Vec4(this.x+(v.x||0),this.y+(v.y||0),this.z+(v.z||0),this.w+(v.w||0));}\n"
                        "      };\n"
                        "    }\n"
                        "    if(typeof g.Color==='undefined'){\n"
                        "      g.Color=class Color{constructor(r=0,g=0,b=0,a=1){this.r=r;this.g=g;this.b=b;this.a=a;}};\n"
                        "    }\n"
                        "    if(typeof g.shared==='undefined') g.shared={};\n"
                        "    if(typeof g.localStorage==='undefined'){\n"
                        "      g.localStorage={data:{},LOCATION_GLOBAL:0,LOCATION_LOCAL:1,get(k,l){return this.data[k];},set(k,v,l){this.data[k]=v;}};\n"
                        "    }\n"
                        "    if(typeof g.WEMath==='undefined'){\n"
                        "      g.WEMath={\n"
                        "        mix(a,b,t){if(typeof a==='object'&&a!==null){if(typeof a.multiply==='function'&&typeof a.add==='function'){return a.multiply(1-t).add(b.multiply(t));}}return a*(1-t)+b*t;},\n"
                        "        smoothStep(min,max,value){var t=Math.max(0,Math.min(1,(value-min)/(max-min)));return t*t*(3-2*t);},\n"
                        "        clamp(value,min,max){return Math.max(min,Math.min(max,value));},\n"
                        "        step(edge,value){return value<edge?0:1;},\n"
                        "        fract(value){return value-Math.floor(value);},\n"
                        "        deg2rad:Math.PI/180,\n"
                        "        rad2deg:180/Math.PI\n"
                        "      };\n"
                        "    }\n"
                        "  }\n"
                        "})();\n"
                    );

                    obj.insert(QStringLiteral("script"), shim + defaultValueCode + scriptText);
                    value = obj;
                    changed = true;
                }
            }
        }
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            if (it.key() == QStringLiteral("script")) continue;
            QJsonValue val = it.value();
            bool childChanged = false;
            injectGlobalsIntoScripts(val, childChanged, it.key());
            if (childChanged) {
                obj.insert(it.key(), val);
                changed = true;
            }
        }
        if (changed) {
            value = obj;
        }
    } else if (value.isArray()) {
        QJsonArray arr = value.toArray();
        for (int i = 0; i < arr.size(); ++i) {
            QJsonValue val = arr.at(i);
            bool childChanged = false;
            injectGlobalsIntoScripts(val, childChanged, propName);
            if (childChanged) {
                arr.replace(i, val);
                changed = true;
            }
        }
        if (changed) {
            value = arr;
        }
    }
}

QByteArray LWEScenePatch::stripEffectsFromSceneJson(const QByteArray &json,
                                                    const QStringList &effectNames)
{
    QJsonParseError err{};
    QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return json;

    QJsonObject scene = doc.object();
    bool changed = false;

    QJsonValue sceneVal(scene);
    injectGlobalsIntoScripts(sceneVal, changed);
    if (changed) {
        scene = sceneVal.toObject();
    }

    QJsonArray objects = scene.value(QStringLiteral("objects")).toArray();
    QJsonArray keptObjects;

    for (int i = 0; i < objects.size(); ++i) {
        QJsonObject obj = objects[i].toObject();
        
        // Remove non-numeric "padding" key to prevent crash in linux-wallpaperengine ObjectParser::parseText
        if (obj.contains(QStringLiteral("padding"))) {
            QJsonValue paddingVal = obj.value(QStringLiteral("padding"));
            if (!paddingVal.isDouble()) {
                obj.remove(QStringLiteral("padding"));
                changed = true;
                qWarning() << "[lwepaper] Removed invalid padding from object" << obj.value(QStringLiteral("name")).toString() << "to prevent LWE crash";
            }
        }

        QJsonArray effects = obj.value(QStringLiteral("effects")).toArray();
        if (!effects.isEmpty()) {
            QJsonArray keptEffects;
            for (const auto &eff : effects) {
                const QJsonObject eo = eff.toObject();
                const QString name = eo.value(QStringLiteral("name")).toString();
                bool drop = effectNames.contains(name);
                if (!drop) {
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
                keptEffects.append(eff);
            }
            if (keptEffects.size() != effects.size()) {
                obj.insert(QStringLiteral("effects"), keptEffects);
                changed = true;
            }
        }
        keptObjects.append(obj);
    }
    if (!changed) return json;
    scene.insert(QStringLiteral("objects"), keptObjects);
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
