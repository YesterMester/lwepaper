#pragma once
// LWELibrary — a QML-friendly list model that scans the Steam Wallpaper
// Engine workshop directory and exposes each subscribed wallpaper as a row.
// Used by the wallpaper-picker config dialog to show a grid of previews.
#include <QAbstractListModel>
#include <QQmlEngine>
#include <QString>
#include <QVector>
#include <QJsonArray>
#include <QJsonValue>

class LWELibrary : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString workshopDir READ workshopDir WRITE setWorkshopDir NOTIFY workshopDirChanged)
    Q_PROPERTY(QString filterType READ filterType WRITE setFilterType NOTIFY filterTypeChanged)
    Q_PROPERTY(QString search     READ search     WRITE setSearch     NOTIFY searchChanged)
    Q_PROPERTY(QString sortMode   READ sortMode   WRITE setSortMode   NOTIFY sortModeChanged)
    Q_PROPERTY(int     total      READ totalCount NOTIFY changed)
    Q_PROPERTY(int     allTotal   READ allTotal   NOTIFY changed)
public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        TitleRole,
        PreviewRole,
        TypeRole,
        PathRole,
        PkgVersionRole,
        ModifiedRole,
        DependenciesRole,
        MissingDependenciesRole,
    };

    explicit LWELibrary(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString workshopDir() const { return m_workshopDir; }
    void setWorkshopDir(const QString &dir);

    QString filterType() const { return m_filterType; }
    void setFilterType(const QString &type);

    QString search() const { return m_search; }
    void setSearch(const QString &s);

    QString sortMode() const { return m_sortMode; }
    void setSortMode(const QString &mode);

    int totalCount() const { return m_entries.size(); }
    int allTotal() const { return m_all.size(); }

    Q_INVOKABLE void reload();
    // Pre-link every dependency file for a single wallpaper. Idempotent —
    // skips real files and already-correct symlinks. Returns true if at
    // least one link was created/refreshed.
    Q_INVOKABLE bool prepareWallpaper(const QString &workshopId);
    // Pre-link dependencies for ALL wallpapers in the library. Run once on
    // scan; the picker can also call it via the "Rescan" button.
    Q_INVOKABLE int prepareAllDependencies();
    // Return the list of dependency workshop ids that aren't downloaded for
    // the given wallpaper. Empty list means everything needed is present.
    Q_INVOKABLE QStringList missingDependencies(const QString &workshopId) const;
    Q_INVOKABLE QJsonArray getWallpaperProperties(const QString &workshopId) const;
    Q_INVOKABLE void saveWallpaperProperty(const QString &workshopId, const QString &key, const QJsonValue &value);
    Q_INVOKABLE QString getWallpaperType(const QString &workshopId) const;

    static void collectDependenciesRecursively(const QString &workshopDir, const QString &wpId, QStringList &collected, bool isRoot = false);
    static bool linkFilesRecursively(const QString &srcPath, const QString &dstPath);

signals:
    void workshopDirChanged();
    void filterTypeChanged();
    void searchChanged();
    void sortModeChanged();
    void changed();

private:
    struct Entry {
        QString id;          // workshop id (numeric string)
        QString title;
        QString previewUrl;  // file:// URL
        QString type;        // "scene", "video", "web", "image"
        QString path;        // absolute dir
        QString pkgVersion;  // "PKGV0018" etc, empty for non-scene
        qint64  modified = 0; // unix epoch seconds of project.json mtime
        QStringList dependencies; // workshop IDs this wallpaper requires
    };

    void scan();
    void applyFilterAndSort();
    QString detectPkgVersion(const QString &dir) const;
    // Internal: link a single dep's files into a wallpaper's dir.
    // Used by prepareWallpaper / prepareAllDependencies.
    bool linkDepFiles(const QString &wpPath, const QString &depId);


    QString m_workshopDir;
    QString m_filterType;     // empty means no filter
    QString m_search;         // case-insensitive title contains
    QString m_sortMode = QStringLiteral("title"); // title|modified|id|type|pkg
    QVector<Entry> m_all;
    QVector<Entry> m_entries; // filtered + sorted view
};
