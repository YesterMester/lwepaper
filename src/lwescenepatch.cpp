#include "lwescenepatch.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QSet>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QJsonParseError>
#include <QDataStream>
#include <QtEndian>
#include <QDebug>
#include <QRegularExpression>
#include <QRegularExpressionMatchIterator>

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

static void removeBlockStartingWith(QString &text, const QString &prefix)
{
    while (true) {
        int idx = text.indexOf(prefix);
        if (idx == -1) break;
        int braceStart = text.indexOf(QLatin1Char('{'), idx);
        if (braceStart == -1) {
            int nextLine = text.indexOf(QLatin1Char('\n'), idx);
            text.remove(idx, (nextLine == -1 ? text.size() : nextLine) - idx);
            continue;
        }
        int count = 1;
        int i = braceStart + 1;
        for (; i < text.size(); ++i) {
            if (text[i] == QLatin1Char('{')) count++;
            else if (text[i] == QLatin1Char('}')) {
                count--;
                if (count == 0) {
                    break;
                }
            }
        }
        if (count == 0) {
            int endIdx = i + 1;
            // Consume trailing )(); or similar
            while (endIdx < text.size()) {
                QChar c = text[endIdx];
                if (c == QLatin1Char(')') || c == QLatin1Char('(') || c == QLatin1Char(';') || c.isSpace()) {
                    endIdx++;
                } else {
                    break;
                }
            }
            text.remove(idx, endIdx - idx);
        } else {
            text.remove(idx, braceStart - idx);
        }
    }
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

                // Robust Cleanup Phase: Strip any older injections
                // 1. Loop and remove all occurrences of /* LWE GLOBALS INJECT */ IIFEs
                while (true) {
                    int idx = scriptText.indexOf(QStringLiteral("/* LWE GLOBALS INJECT */"));
                    if (idx == -1) break;
                    int endIdx = scriptText.indexOf(QStringLiteral("})();"), idx);
                    if (endIdx == -1) break;
                    scriptText.remove(idx, endIdx + 5 - idx);
                }

                // 2. Remove block-based definitions and fallbacks
                removeBlockStartingWith(scriptText, QStringLiteral("var _lwe_default_value = (function()"));
                removeBlockStartingWith(scriptText, QStringLiteral("function _lwe_ensure_init"));
                removeBlockStartingWith(scriptText, QStringLiteral("if (typeof value === 'undefined' || value === null) {\n  var prop ="));
                removeBlockStartingWith(scriptText, QStringLiteral("if(typeof value === 'undefined'||value===null){value=_lwe_default_value;}"));

                // 3. Remove exact line injections and spacing variations
                QStringList exactRemovals = {
                    QStringLiteral("if (typeof value === 'undefined' || value === null) { value = _lwe_default_value; }\n"),
                    QStringLiteral("if (typeof initScale !== 'undefined' && (initScale === undefined || initScale === null)) { initScale = value; }\n"),
                    QStringLiteral("if (typeof newScale !== 'undefined' && (newScale === undefined || newScale === null)) { newScale = new Vec3(initScale.multiply(scriptProperties.hoScale || 1.5)); }\n"),
                    QStringLiteral("if (typeof initOrigin !== 'undefined' && (initOrigin === undefined || initOrigin === null)) { initOrigin = value; }\n"),
                    QStringLiteral("if (typeof newOrigin !== 'undefined' && (newOrigin === undefined || newOrigin === null)) { newOrigin = new Vec3(initOrigin.multiply(scriptProperties.hoScale || 1.5)); }\n"),
                    QStringLiteral("if (typeof initPosition !== 'undefined' && (initPosition === undefined || initPosition === null)) { initPosition = value; }\n"),
                    QStringLiteral("if (typeof newPosition !== 'undefined' && (newPosition === undefined || newPosition === null)) { newPosition = new Vec3(initPosition.multiply(scriptProperties.hoScale || 1.5)); }\n"),
                    QStringLiteral("if (typeof speed !== 'undefined' && (speed === undefined || speed === null)) { speed = (scriptProperties.speed || 10) / 100; }\n"),
                    
                    QStringLiteral("if (value === undefined || value === null) { value = _lwe_default_value; }\n"),
                    QStringLiteral("if (initScale === undefined || initScale === null) { initScale = value; }\n"),
                    QStringLiteral("if (newScale === undefined || newScale === null) { newScale = new Vec3(initScale.multiply(scriptProperties.hoScale || 1.5)); }\n"),
                    QStringLiteral("if (initOrigin === undefined || initOrigin === null) { initOrigin = value; }\n"),
                    QStringLiteral("if (newOrigin === undefined || newOrigin === null) { newOrigin = new Vec3(initOrigin.multiply(scriptProperties.hoScale || 1.5)); }\n"),
                    QStringLiteral("if (initPosition === undefined || initPosition === null) { initPosition = value; }\n"),
                    QStringLiteral("if (newPosition === undefined || newPosition === null) { newPosition = new Vec3(initPosition.multiply(scriptProperties.hoScale || 1.5)); }\n"),
                    QStringLiteral("if (speed === undefined || speed === null) { speed = (scriptProperties.speed || 10) / 100; }\n"),

                    QStringLiteral("if(typeof value === 'undefined'||value===null){value=_lwe_default_value;}\n"),
                    QStringLiteral("if(typeof value === 'undefined'||value===null){value=_lwe_default_value;}"),
                    QStringLiteral("_lwe_ensure_init(value);\n"),
                    QStringLiteral("_lwe_ensure_init(value);"),
                    QStringLiteral("_lwe_ensure_init(undefined);\n"),
                    QStringLiteral("_lwe_ensure_init(undefined);")
                };
                for (const QString &removal : exactRemovals) {
                    scriptText.replace(removal, QString());
                }

                // 4. Remove regex/flexible line injections
                QRegularExpression callRegex(QStringLiteral("^[ \\t]*_lwe_ensure_init\\s*\\([^)]*\\);?\\s*\\n?"), QRegularExpression::MultilineOption);
                scriptText.replace(callRegex, QString());

                QRegularExpression initVarRegex(QStringLiteral("^[ \\t]*(var|let|const)?\\s*_lwe_initialized\\s*=.*?;?\\s*\\n?"), QRegularExpression::MultilineOption);
                scriptText.replace(initVarRegex, QString());

                QRegularExpression valRegex(QStringLiteral("^[ \\t]*if\\s*\\(\\s*typeof\\s+value\\s*===\\s*['\"]undefined['\"]\\s*\\|\\|\\s*value\\s*===\\s*null\\s*\\)\\s*\\{\\s*value\\s*=\\s*_lwe_default_value;?\\s*\\}\\s*\\n?"), QRegularExpression::MultilineOption);
                scriptText.replace(valRegex, QString());

                QRegularExpression valNewRegex(QStringLiteral("^[ \\t]*if\\s*\\(\\s*value\\s*===\\s*undefined\\s*\\|\\|\\s*value\\s*===\\s*null\\s*\\)\\s*\\{\\s*value\\s*=\\s*_lwe_default_value;?\\s*\\}\\s*\\n?"), QRegularExpression::MultilineOption);
                scriptText.replace(valNewRegex, QString());

                for (const QString &varName : {QStringLiteral("initScale"), QStringLiteral("newScale"), QStringLiteral("initOrigin"), QStringLiteral("newOrigin"), QStringLiteral("initPosition"), QStringLiteral("newPosition"), QStringLiteral("speed")}) {
                    QRegularExpression typeVarRegex(QStringLiteral("^[ \\t]*if\\s*\\(\\s*typeof\\s+%1\\s*!==\\s*['\"]undefined['\"]\\s*&&\\s*\\(\\s*%1\\s*===\\s*undefined\\s*\\|\\|\\s*%1\\s*===\\s*null\\s*\\)\\s*\\)\\s*\\{[\\s\\S]*?\\}\\s*\\n?").arg(varName), QRegularExpression::MultilineOption);
                    scriptText.replace(typeVarRegex, QString());

                    QRegularExpression cleanVarRegex(QStringLiteral("^[ \\t]*if\\s*\\(\\s*%1\\s*===\\s*undefined\\s*\\|\\|\\s*%1\\s*===\\s*null\\s*\\)\\s*\\{[\\s\\S]*?\\}\\s*\\n?").arg(varName), QRegularExpression::MultilineOption);
                    scriptText.replace(cleanVarRegex, QString());
                }

                scriptText = scriptText.trimmed();
                needsPatch = true;

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

                // 6. Define the script's local _lwe_default_value
                QString defaultValueCode = QStringLiteral(
                    "var _lwe_default_value = (function(){\n"
                    "  var prop = \"%1\".toLowerCase();\n"
                    "  if(prop.indexOf(\"scale\")!==-1) return new Vec3(1,1,1);\n"
                    "  if(prop.indexOf(\"color\")!==-1||prop.indexOf(\"origin\")!==-1||prop.indexOf(\"position\")!==-1||prop.indexOf(\"angles\")!==-1||prop.indexOf(\"offset\")!==-1||prop.indexOf(\"direction\")!==-1) return new Vec3(0,0,0);\n"
                    "  if(prop.indexOf(\"text\")!==-1) return \"\";\n"
                    "  if(prop.indexOf(\"visible\")!==-1) return true;\n"
                    "  return 1;\n"
                    "})();\n"
                ).arg(propName.contains('"') ? QString() : propName);

                // 7. Inject guards inside init/update functions in the script text to handle undefined values and local variables safely
                QString propInit;
                propInit += QStringLiteral("  if (value === undefined || value === null) { value = _lwe_default_value; }\n");
                propInit += QStringLiteral("  value = _lwe_wrap(value);\n");
                if (scriptText.contains(QStringLiteral("initScale"))) {
                    propInit += QStringLiteral("  if (initScale === undefined || initScale === null) { initScale = value; }\n");
                }
                if (scriptText.contains(QStringLiteral("newScale"))) {
                    propInit += QStringLiteral("  if (newScale === undefined || newScale === null) { newScale = new Vec3(initScale.multiply(scriptProperties.hoScale || 1.5)); }\n");
                }
                if (scriptText.contains(QStringLiteral("initOrigin"))) {
                    propInit += QStringLiteral("  if (initOrigin === undefined || initOrigin === null) { initOrigin = value; }\n");
                }
                if (scriptText.contains(QStringLiteral("newOrigin"))) {
                    propInit += QStringLiteral("  if (newOrigin === undefined || newOrigin === null) { newOrigin = new Vec3(initOrigin.multiply(scriptProperties.hoScale || 1.5)); }\n");
                }
                if (scriptText.contains(QStringLiteral("initPosition"))) {
                    propInit += QStringLiteral("  if (initPosition === undefined || initPosition === null) { initPosition = value; }\n");
                }
                if (scriptText.contains(QStringLiteral("newPosition"))) {
                    propInit += QStringLiteral("  if (newPosition === undefined || newPosition === null) { newPosition = new Vec3(initPosition.multiply(scriptProperties.hoScale || 1.5)); }\n");
                }
                if (scriptText.contains(QStringLiteral("speed"))) {
                    propInit += QStringLiteral("  if (speed === undefined || speed === null) { speed = (scriptProperties.speed || 10) / 100; }\n");
                }

                QRegularExpression funcRegex(QStringLiteral("\\bfunction\\s+(init|update)\\s*\\(\\s*value\\s*\\)\\s*\\{"));
                scriptText.replace(funcRegex, QStringLiteral("function \\1(value) {\n") + propInit);

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
                    "    if(typeof g._lwe_wrap==='undefined'){\n"
                    "      g._lwe_wrap=function(v){\n"
                    "        if(!v||typeof v!=='object') return v;\n"
                    "        if(typeof v.multiply==='function') return v;\n"
                    "        if('r' in v) return new Color(v.r, v.g, v.b, v.a);\n"
                    "        if('w' in v) return new Vec4(v.x, v.y, v.z, v.w);\n"
                    "        if('z' in v) return new Vec3(v.x, v.y, v.z);\n"
                    "        if('x' in v) return new Vec2(v.x, v.y);\n"
                    "        return v;\n"
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

// ── PKG container helpers ──────────────────────────────────────────────────

namespace {

struct PkgEntry { QString name; quint32 off; quint32 size; };
struct ParsedPkg {
    bool ok = false;
    quint32 magicLen = 0;
    QString magic;
    quint32 nEntries = 0;
    QList<PkgEntry> entries;
    int blobStart = 0;
    QByteArray data;
};

ParsedPkg parsePkg(const QString &path)
{
    ParsedPkg p;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return p;
    p.data = f.readAll();
    f.close();
    if (p.data.size() < 12) return p;
    const auto u32 = [&](int o) {
        return qFromLittleEndian<quint32>(p.data.constData() + o);
    };
    p.magicLen = u32(0);
    int o = 4 + int(p.magicLen);
    if (o + 4 > p.data.size()) return p;
    p.magic = QString::fromLatin1(p.data.constData() + 4, p.magicLen);
    p.nEntries = u32(o); o += 4;
    for (quint32 i = 0; i < p.nEntries; ++i) {
        if (o + 4 > p.data.size()) return p;
        quint32 nl = u32(o); o += 4;
        if (int(o + nl + 8) > p.data.size()) return p;
        QString name = QString::fromUtf8(p.data.constData() + o, nl); o += nl;
        quint32 eo = u32(o); o += 4;
        quint32 es = u32(o); o += 4;
        p.entries.append({name, eo, es});
    }
    p.blobStart = o;
    p.ok = true;
    return p;
}

QByteArray repackPkg(const ParsedPkg &p,
                     int replaceIdx,
                     const QByteArray &replacement)
{
    QByteArray out;
    out.append(reinterpret_cast<const char *>(&p.magicLen), 4);
    out.append(p.magic.toLatin1());
    out.append(reinterpret_cast<const char *>(&p.nEntries), 4);
    QByteArray headers;
    QByteArray blob;
    for (int i = 0; i < p.entries.size(); ++i) {
        QByteArray content = (i == replaceIdx)
            ? replacement
            : QByteArray(p.data.constData() + p.blobStart + int(p.entries[i].off),
                         int(p.entries[i].size));
        quint32 nl = p.entries[i].name.toUtf8().size();
        quint32 eo = quint32(blob.size());
        quint32 es = quint32(content.size());
        headers.append(reinterpret_cast<const char *>(&nl), 4);
        headers.append(p.entries[i].name.toUtf8());
        headers.append(reinterpret_cast<const char *>(&eo), 4);
        headers.append(reinterpret_cast<const char *>(&es), 4);
        blob.append(content);
    }
    out.append(headers);
    out.append(blob);
    return out;
}

QByteArray readSceneJson(const ParsedPkg &p)
{
    for (int i = 0; i < p.entries.size(); ++i) {
        if (p.entries[i].name == QLatin1String("scene.json")) {
            if (int(p.blobStart + p.entries[i].off + p.entries[i].size) > p.data.size())
                return {};
            return p.data.mid(p.blobStart + p.entries[i].off, p.entries[i].size);
        }
    }
    return {};
}

// Walk a parsed JSON value collecting any string that looks like an asset
// path (contains a `/` and ends in a known asset suffix).
void walkForAssets(const QJsonValue &v, QStringList &out)
{
    static const QStringList exts = {
        ".tex", ".png", ".jpg", ".jpeg", ".gif", ".webp",
        ".ttf", ".otf", ".woff",
        ".mp3", ".ogg", ".wav", ".webm", ".mp4",
        ".mdl", ".json", ".frag", ".vert", ".geom", ".comp"
    };
    if (v.isString()) {
        const QString s = v.toString();
        if (s.contains(QLatin1Char('/')) && !s.startsWith(QLatin1String("http"))) {
            for (const QString &ext : exts) {
                if (s.endsWith(ext, Qt::CaseInsensitive)) {
                    out << s;
                    break;
                }
            }
        }
    } else if (v.isObject()) {
        const auto obj = v.toObject();
        for (auto it = obj.begin(); it != obj.end(); ++it)
            walkForAssets(it.value(), out);
    } else if (v.isArray()) {
        const auto arr = v.toArray();
        for (const auto &x : arr) walkForAssets(x, out);
    }
}

// Find a fallback font on the host — used when a wallpaper references a
// custom font from an un-subscribed workshop dep.
QString findFallbackFont()
{
    static const QStringList candidates = {
        QStringLiteral("/usr/share/fonts/noto/NotoSans-Regular.ttf"),
        QStringLiteral("/usr/share/fonts/noto/NotoSansMono-Regular.ttf"),
        QStringLiteral("/usr/share/fonts/TTF/DejaVuSans.ttf"),
        QStringLiteral("/usr/share/fonts/dejavu/DejaVuSans.ttf"),
        QStringLiteral("/usr/share/fonts/liberation/LiberationSans-Regular.ttf"),
        QStringLiteral("/usr/share/fonts/google-noto/NotoSans-Regular.ttf"),
    };
    for (const QString &c : candidates)
        if (QFileInfo::exists(c)) return c;
    // Last-ditch: glob /usr/share/fonts for any .ttf.
    QDir d(QStringLiteral("/usr/share/fonts"));
    const QStringList found = d.entryList({QStringLiteral("*.ttf"), QStringLiteral("*.otf")}, QDir::Files);
    if (!found.isEmpty()) return d.absoluteFilePath(found.first());
    return {};
}

// Build a minimal 1x1 transparent TEXV0005 file. LWE's texture loader
// accepts a basic header with a single mipmap of RGBA8 data.
QByteArray buildStubTex()
{
    // TEXV0005 (the most-supported variant). Layout (little-endian):
    //   "TEXV0005\0"          (9 bytes)
    //   u32 image_count = 1
    //   u32 mipmap_count = 1
    //   u32 width = 1
    //   u32 height = 1
    //   u32 unused = 0
    //   u32 type = 0          (uncompressed)
    //   ... then per-mipmap: u32 w, u32 h, u32 sz, sz bytes RGBA
    // Note: the exact field set differs across LWE versions; we keep it as
    // small as possible and write a 1x1 fully-transparent pixel. If LWE
    // can't parse this, it still beats a missing-file crash.
    QByteArray out;
    QDataStream s(&out, QIODevice::WriteOnly);
    s.setByteOrder(QDataStream::LittleEndian);
    out.append("TEXV0005", 8);
    out.append('\0');
    auto put32 = [&](quint32 v) { s << v; };
    // After the magic we re-open a stream that appends to the current end.
    QDataStream s2(&out, QIODevice::Append);
    s2.setByteOrder(QDataStream::LittleEndian);
    s2 << quint32(1)    // image count
       << quint32(1)    // mipmap count
       << quint32(1)    // width
       << quint32(1)    // height
       << quint32(0)    // unused
       << quint32(0);   // type
    // mipmap 0
    s2 << quint32(1) << quint32(1) << quint32(4);
    s2 << quint8(0) << quint8(0) << quint8(0) << quint8(0);   // RGBA 0,0,0,0
    return out;
}

}  // namespace

// ── public API ─────────────────────────────────────────────────────────────

QStringList LWEScenePatch::collectAssetReferences(const QByteArray &sceneJson)
{
    QJsonParseError err{};
    QJsonDocument doc = QJsonDocument::fromJson(sceneJson, &err);
    if (err.error != QJsonParseError::NoError) return {};
    QStringList out;
    walkForAssets(QJsonValue(doc.isObject() ? QJsonValue(doc.object())
                                            : QJsonValue(doc.array())),
                  out);
    out.removeDuplicates();
    return out;
}

QStringList LWEScenePatch::detectImplicitDependencies(const QString &wallpaperPath)
{
    QStringList deps;
    // 1. Explicit `dependency`/`dependencies` in project.json.
    QFile pj(wallpaperPath + QStringLiteral("/project.json"));
    if (pj.open(QIODevice::ReadOnly)) {
        QJsonParseError perr{};
        QJsonDocument pdoc = QJsonDocument::fromJson(pj.readAll(), &perr);
        pj.close();
        if (perr.error == QJsonParseError::NoError && pdoc.isObject()) {
            const auto pobj = pdoc.object();
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
            collect(pobj.value(QStringLiteral("dependencies")));
            collect(pobj.value(QStringLiteral("dependency")));
            collect(pobj.value(QStringLiteral("required")));
        }
    }
    // 2. Inline `workshop/<id>/...` path references inside scene.json. These
    //    are how wallpapers like GTA 6 indirectly depend on other workshops
    //    without listing them in project.json.
    auto parsed = parsePkg(wallpaperPath + QStringLiteral("/scene.pkg"));
    if (parsed.ok) {
        const QByteArray sj = readSceneJson(parsed);
        if (!sj.isEmpty()) {
            static const QRegularExpression re(QStringLiteral("workshop/(\\d{4,})"));
            auto it = re.globalMatch(QString::fromUtf8(sj));
            while (it.hasNext()) deps << it.next().captured(1);
        }
    }
    deps.removeDuplicates();
    return deps;
}

int LWEScenePatch::linkWorkshopDependencyPaths(const QString &wallpaperPath,
                                               const QStringList &depIds)
{
    int created = 0;
    const QString workshopRoot = QFileInfo(wallpaperPath).absolutePath();

    // Pass 1 — flat root-level mirror (cover the simple-dep case e.g. SH98).
    for (const QString &depId : depIds) {
        const QString depPath = workshopRoot + QLatin1Char('/') + depId;
        QDir depDir(depPath);
        if (!depDir.exists()) continue;
        const auto entries = depDir.entryList(
            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &name : entries) {
            if (name == QLatin1String("project.json")) continue;
            const QString src = depPath + QLatin1Char('/') + name;
            const QString dst = wallpaperPath + QLatin1Char('/') + name;
            QFileInfo dstInfo(dst);
            if (dstInfo.exists() && !dstInfo.isSymLink()) continue;
            if (dstInfo.isSymLink()) {
                if (QFile::symLinkTarget(dst) == src) continue;
                QFile::remove(dst);
            }
            if (QFile::link(src, dst)) ++created;
        }
    }

    // Pass 2 — path-based deps (`fonts/workshop/<id>/Foo.otf` etc).
    // Parse the wallpaper's scene.json, find every `workshop/<id>/PATH`
    // reference, and ensure `<wallpaper>/<prefix>/workshop/<id>/PATH` exists
    // as a symlink to `<workshop>/<id>/PATH` (when the dep is downloaded).
    auto parsed = parsePkg(wallpaperPath + QStringLiteral("/scene.pkg"));
    if (!parsed.ok) return created;
    const QStringList refs = collectAssetReferences(readSceneJson(parsed));
    static const QRegularExpression re(QStringLiteral("^(.*?)workshop/(\\d{4,})/(.+)$"));
    for (const QString &ref : refs) {
        const auto m = re.match(ref);
        if (!m.hasMatch()) continue;
        const QString prefix = m.captured(1);     // e.g. "fonts/" or ""
        const QString depId  = m.captured(2);
        const QString tail   = m.captured(3);     // e.g. "Cafe Francoise_D.otf"

        // The dep's file might live under just `<depDir>/<tail>` (root) OR
        // mirror the same `<prefix>workshop/<id>/<tail>` shape. Try both.
        const QString depDir = workshopRoot + QLatin1Char('/') + depId;
        QStringList candidates {
            depDir + QLatin1Char('/') + tail,
            depDir + QLatin1Char('/') + prefix + QStringLiteral("workshop/") + depId + QLatin1Char('/') + tail,
        };
        QString srcFile;
        for (const QString &c : candidates) {
            if (QFileInfo::exists(c)) { srcFile = c; break; }
        }
        if (srcFile.isEmpty()) continue;   // dep not subscribed → handled by stubMissingAssets

        const QString dst = wallpaperPath + QLatin1Char('/') + ref;
        QDir().mkpath(QFileInfo(dst).absolutePath());
        QFileInfo dstInfo(dst);
        if (dstInfo.exists() && !dstInfo.isSymLink()) continue;
        if (dstInfo.isSymLink()) {
            if (QFile::symLinkTarget(dst) == srcFile) continue;
            QFile::remove(dst);
        }
        if (QFile::link(srcFile, dst)) ++created;
    }
    return created;
}

int LWEScenePatch::stubMissingAssets(const QString &wallpaperPath)
{
    auto parsed = parsePkg(wallpaperPath + QStringLiteral("/scene.pkg"));
    if (!parsed.ok) return 0;
    const QSet<QString> inPkg = [&]() {
        QSet<QString> s;
        for (const auto &e : parsed.entries) s.insert(e.name);
        return s;
    }();
    const QStringList refs = collectAssetReferences(readSceneJson(parsed));
    const QString fallbackFont = findFallbackFont();
    // LWE's path resolver also looks in the Wallpaper Engine global assets
    // folder. If a referenced file lives there (e.g. `models/util/X.json`),
    // we MUST NOT stub it — our stub would mask the real file and break the
    // scene. Determine the assets root once.
    const QString assetsRoot = QDir::homePath() +
        QStringLiteral("/.local/share/Steam/steamapps/common/wallpaper_engine/assets");

    int created = 0;
    for (const QString &ref : refs) {
        if (inPkg.contains(ref)) continue;
        const QString dst = wallpaperPath + QLatin1Char('/') + ref;
        if (QFileInfo::exists(dst)) {
            // If a previous (broken) stub of ours overrode an asset that
            // actually exists in the WE assets dir, kill the stub now so
            // LWE falls through to the real file.
            const QString assetCandidate = assetsRoot + QLatin1Char('/') + ref;
            QFileInfo info(dst);
            if (info.isSymLink()) { continue; }
            if (info.size() <= 64 && QFileInfo::exists(assetCandidate)) {
                QFile::remove(dst);
                qWarning() << "[lwepaper] removed stale stub" << ref
                           << "(real asset exists in WE assets dir)";
                ++created;
            }
            continue;
        }
        // If the WE assets dir provides this file, don't stub at all.
        if (QFileInfo::exists(assetsRoot + QLatin1Char('/') + ref)) continue;

        QDir().mkpath(QFileInfo(dst).absolutePath());

        if (ref.endsWith(QStringLiteral(".ttf"), Qt::CaseInsensitive) ||
            ref.endsWith(QStringLiteral(".otf"), Qt::CaseInsensitive)) {
            if (fallbackFont.isEmpty()) continue;
            if (QFile::link(fallbackFont, dst)) {
                ++created;
                qWarning() << "[lwepaper] font stub" << ref << "->" << fallbackFont;
            }
            continue;
        }

        if (ref.endsWith(QStringLiteral(".tex"), Qt::CaseInsensitive)) {
            QFile w(dst);
            if (w.open(QIODevice::WriteOnly)) {
                w.write(buildStubTex());
                w.close();
                ++created;
                qWarning() << "[lwepaper] tex stub" << ref;
            }
            continue;
        }

        if (ref.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
            // Material/model JSON — empty object lets LWE skip it cleanly
            // for most code paths instead of "Cannot find requested file".
            QFile w(dst);
            if (w.open(QIODevice::WriteOnly)) {
                w.write("{}\n");
                w.close();
                ++created;
                qWarning() << "[lwepaper] empty-json stub" << ref;
            }
            continue;
        }

        if (ref.endsWith(QStringLiteral(".frag"), Qt::CaseInsensitive)) {
            QFile w(dst);
            if (w.open(QIODevice::WriteOnly)) {
                w.write("// stub shader (referenced asset missing)\n"
                        "void main() { gl_FragColor = vec4(0.0, 0.0, 0.0, 0.0); }\n");
                w.close();
                ++created;
            }
            continue;
        }
        if (ref.endsWith(QStringLiteral(".vert"), Qt::CaseInsensitive)) {
            QFile w(dst);
            if (w.open(QIODevice::WriteOnly)) {
                w.write("// stub vertex shader (referenced asset missing)\n"
                        "void main() { gl_Position = vec4(0.0); }\n");
                w.close();
                ++created;
            }
        }
    }
    return created;
}

QStringList LWEScenePatch::unresolvedDependencies(const QString &wallpaperPath)
{
    const QString workshopRoot = QFileInfo(wallpaperPath).absolutePath();
    const QStringList deps = detectImplicitDependencies(wallpaperPath);
    QStringList missing;
    for (const QString &dep : deps) {
        if (!QFileInfo::exists(workshopRoot + QLatin1Char('/') + dep + QStringLiteral("/project.json")))
            missing << dep;
    }
    return missing;
}

bool LWEScenePatch::patchIfNeeded(const QString &wallpaperPath)
{
    const QString pkgPath = wallpaperPath + QStringLiteral("/scene.pkg");
    QFileInfo fi(pkgPath);
    if (!fi.exists()) return false;

    bool changed = false;

    // 1. Discover implicit deps and link their files at the expected paths
    //    (covers the GTA 6 case where assets live under `<root>/workshop/<id>/`).
    const QStringList deps = detectImplicitDependencies(wallpaperPath);
    if (!deps.isEmpty()) {
        const int linked = linkWorkshopDependencyPaths(wallpaperPath, deps);
        if (linked > 0) {
            qWarning() << "[lwepaper] linked" << linked
                       << "workshop-dep files into" << wallpaperPath;
            changed = true;
        }
    }

    // 2. Stub anything still missing so LWE can keep going past the
    //    "Cannot find requested file" failure mode. Stubs are tiny (1x1
    //    transparent texture, empty material JSON, fallback font).
    const int stubbed = stubMissingAssets(wallpaperPath);
    if (stubbed > 0) {
        qWarning() << "[lwepaper] stubbed" << stubbed
                   << "missing asset(s) in" << wallpaperPath;
        changed = true;
    }

    // 3. Patch scene.json inside the pkg: strip broken effects, inject the
    //    script globals shim, sanitize padding, etc. This is the existing
    //    code path; we just route through the new parse helpers.
    QString sourcePath = pkgPath;
    if (fi.isSymLink()) {
        sourcePath = fi.symLinkTarget();
        if (sourcePath.isEmpty()) return changed;
    }
    auto parsed = parsePkg(sourcePath);
    if (!parsed.ok) return changed;
    int sceneIdx = -1;
    for (int i = 0; i < parsed.entries.size(); ++i) {
        if (parsed.entries[i].name == QLatin1String("scene.json")) {
            sceneIdx = i; break;
        }
    }
    if (sceneIdx < 0) return changed;

    const QByteArray sceneJson = readSceneJson(parsed);
    const QByteArray patchedJson = stripEffectsFromSceneJson(sceneJson, knownBrokenEffects());
    if (patchedJson != sceneJson) {
        const QByteArray out = repackPkg(parsed, sceneIdx, patchedJson);
        if (fi.isSymLink()) QFile::remove(pkgPath);
        QFile w(pkgPath);
        if (w.open(QIODevice::WriteOnly)) {
            w.write(out);
            w.close();
            qWarning() << "[lwepaper] patched scene.pkg in" << wallpaperPath;
            changed = true;
        }
    }
    return changed;
}
