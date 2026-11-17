// Grid pixmap windowing: a big archive must not park a decoded pixmap
// in every card. Headless (offscreen platform, temp XDG dirs): opens a
// 700-file fixture archive in a real MainWindow, scrolls through it, and
// asserts far rows share the single placeholder pixmap (one cache key)
// while near rows carry real thumbs - i.e. memory stays flat.
#include "mainwindow.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QtTest>

namespace {
QString makeArchive(int n, bool &ok) {
    static QTemporaryDir dir;
    ok = dir.isValid();
    if (!ok)
        return {};
    for (int i = 0; i < n && ok; ++i) {
        QImage img(48, 48, QImage::Format_RGB32);
        img.fill(QColor::fromHsv((i * 37) % 360, 200, 220));
        QPainter p(&img);
        p.setPen(Qt::black);
        p.drawText(img.rect(), Qt::AlignCenter, QString::number(i));
        p.end();
        ok = img.save(dir.filePath(QStringLiteral("pic%1.jpg").arg(i)));
    }
    return ok ? dir.path() : QString();
}

int rowCount(GridView *grid) {
    return grid && grid->model() ? grid->model()->rowCount() : 0;
}

// Two-image archive with sidecar tags: a two-tag ARTISTS section, a
// single-tag CHARACTERS section, and none for META/COPYRIGHT.
QString makeTaggedArchive(bool &ok) {
    static QTemporaryDir dir;
    ok = dir.isValid();
    if (!ok)
        return {};
    QDir().mkpath(dir.filePath(QStringLiteral("Site")));
    struct Row {
        const char *name;
        const char *artist;
        const char *character;
    };
    const Row rows[] = {
        {"a", "one two", "solo"}, // two artists, one character
        {"b", "", ""},            // no tags at all
    };
    for (const Row &row : rows) {
        const QString base = QStringLiteral("Site_") + row.name;
        QImage img(32, 32, QImage::Format_RGB32);
        img.fill(QColor(200, 120, 60));
        if (!img.save(dir.filePath(QStringLiteral("Site/%1.jpg").arg(base))))
            ok = false;
        QJsonObject o;
        o[QStringLiteral("id")] = QString::fromLatin1(row.name);
        o[QStringLiteral("tag_string")] = QString::fromLatin1(row.artist) +
                                          u' ' +
                                          QString::fromLatin1(row.character);
        o[QStringLiteral("tag_string_artist")] =
            QString::fromLatin1(row.artist);
        o[QStringLiteral("tag_string_character")] =
            QString::fromLatin1(row.character);
        o[QStringLiteral("rating")] = QStringLiteral("g");
        o[QStringLiteral("file_url")] =
            QStringLiteral("https://example.com/x.jpg");
        o[QStringLiteral("file_ext")] = QStringLiteral("jpg");
        QFile f(dir.filePath(QStringLiteral("Site/%1.json").arg(base)));
        if (!f.open(QIODevice::WriteOnly))
            ok = false;
        f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
        f.close();
    }
    return ok ? dir.path() : QString();
}

QVariant sectionVisibility(MainWindow &w, const QString &header) {
    for (QLabel *label : w.findChildren<QLabel *>()) {
        if (label->text().compare(header, Qt::CaseInsensitive) == 0 &&
            label->parentWidget())
            return label->parentWidget()->isVisible();
    }
    return {};
}

// Distinct decoration-pixmap identities + the most common one.
void decorationStats(GridView *grid, int &distinctOut, int &topCountOut,
                     qint64 &lastKeyOut) {
    QHash<qint64, int> counts;
    const int n = rowCount(grid);
    for (int r = 0; r < n; ++r) {
        const QVariant v = grid->model()->index(r, 0).data(Qt::DecorationRole);
        const QPixmap pm = qvariant_cast<QPixmap>(v);
        if (!pm.isNull())
            counts[pm.cacheKey()]++;
    }
    distinctOut = counts.size();
    topCountOut = 0;
    for (auto it = counts.begin(); it != counts.end(); ++it)
        topCountOut = qMax(topCountOut, it.value());
    lastKeyOut = 0;
    if (n > 0) {
        const QVariant v =
            grid->model()->index(n - 1, 0).data(Qt::DecorationRole);
        lastKeyOut = qvariant_cast<QPixmap>(v).cacheKey();
    }
}
} // namespace

#include "favdb.h"
#include "settings.h"
#include <QElapsedTimer>
#include <QSettings>

class TestGridWindow : public QObject {
    Q_OBJECT
private slots:
    // A favourites tab with thousands of rows must switch like a browser
// tab: no full DB decode per visit, no per-row regex construction, no
// per-card favicon scaling. The old path took ~1.5s per switch here.
    void favouriteTabSwitchStaysFast() {
        QTemporaryDir xdg;
        QVERIFY(xdg.isValid());
        qputenv("XDG_CONFIG_HOME", xdg.filePath("config").toUtf8());
        qputenv("XDG_DATA_HOME", xdg.filePath("data").toUtf8());
        qputenv("XDG_CACHE_HOME", xdg.filePath("cache").toUtf8());
        QDir().mkpath(xdg.filePath("config/box"));
        const QString dl = xdg.filePath("downloads");
        QDir().mkpath(dl + QStringLiteral("/danbooru"));
        {
            QSettings ini(xdg.filePath("config/box/settings.ini"),
                          QSettings::IniFormat);
            ini.setValue(QStringLiteral("classify/enabled"), false);
            ini.setValue(QStringLiteral("downloads/dir"), dl);
            ini.sync();
        }
        const QString archive = xdg.filePath("archive");
        QDir().mkpath(archive);
        const int N = 2000;
        FavDb db;
        QVERIFY(db.open());
        QVector<Post> posts;
        posts.reserve(N);
        for (int i = 0; i < N; ++i) {
            const QString name = QStringLiteral("%1_%2")
                                     .arg(i, 4, 10, QLatin1Char('0'))
                                     .arg(i * 7 % 9999, 4, 10, QLatin1Char('0'));
            const QString path =
                dl + QStringLiteral("/danbooru/") + name + QStringLiteral(".jpg");
            QImage img(48, 48, QImage::Format_RGB32);
            img.fill(QColor::fromHsv((i * 13) % 360, 180, 200));
            if (!img.save(path))
                qWarning("save failed %s", qPrintable(path));
            Post p;
            p.isRemote = true;
            p.siteName = QStringLiteral("danbooru");
            p.siteKey = QStringLiteral("danbooru");
            p.id = 100000 + i;
            p.ext = QStringLiteral("jpg");
            p.rating = QStringLiteral("general");
            p.fileUrl = QStringLiteral("https://example.com/%1.jpg").arg(i);
            p.previewUrl = p.fileUrl;
            p.postUrl = QStringLiteral("https://danbooru.donmai.us/posts/%1")
                            .arg(100000 + i);
            // Realistic tag load: 25 tags across buckets.
            QStringList g, a, c;
            for (int t = 0; t < 20; ++t)
                g << QStringLiteral("general_tag_%1").arg((i + t) % 500);
            for (int t = 0; t < 3; ++t)
                a << QStringLiteral("artist_%1").arg((i + t) % 100);
            for (int t = 0; t < 2; ++t)
                c << QStringLiteral("copy_%1").arg(t);
            p.tags = (g + a + c).join(u' ');
            p.tagsGeneral = g.join(u' ');
            p.tagsArtist = a.join(u' ');
            p.tagsCopyright = c.join(u' ');
            posts << p;
        }
        QVERIFY(db.addMany(posts) == N);
        // Simulate downloaded favourites linked to disk.
        QHash<QPair<QString, qint64>, QString> links;
        for (int i = 0; i < N; ++i) {
            links.insert({QStringLiteral("danbooru"), 100000 + i},
                         dl + QStringLiteral("/danbooru/%1_%2.jpg")
                                  .arg(i, 4, 10, QLatin1Char('0'))
                                  .arg(i * 7 % 9999, 4, 10, QLatin1Char('0')));
        }
        QVERIFY(db.setLocalPaths(links) == N);
        db.close();

        MainWindow w(archive, true);
        w.show();
        QTest::qWait(300);
        GridView *grid = w.findChild<GridView *>();
        QVERIFY(grid);
        auto waitRows = [&](int n) {
            for (int i = 0; i < 400; ++i) {
                if (grid->model() && grid->model()->rowCount() >= n)
                    return true;
                QTest::qWait(5);
            }
            return false;
        };
        QElapsedTimer timer;
        timer.start();
        QVERIFY(QMetaObject::invokeMethod(&w, "openFavTab"));
        QVERIFY(waitRows(N));
        const qint64 first = timer.elapsed();
        // Bar index 1 = the archive tab, 2 = the favourites tab.
        QVERIFY(QMetaObject::invokeMethod(&w, "onTabChanged",
                                          Q_ARG(int, 0)));
        QTest::qWait(50);
        timer.restart();
        QVERIFY(QMetaObject::invokeMethod(&w, "onTabChanged",
                                          Q_ARG(int, 2)));
        QVERIFY(waitRows(N));
        const qint64 second = timer.elapsed();
        QVERIFY(QMetaObject::invokeMethod(&w, "onTabChanged",
                                          Q_ARG(int, 0)));
        QTest::qWait(50);
        timer.restart();
        QVERIFY(QMetaObject::invokeMethod(&w, "onTabChanged",
                                          Q_ARG(int, 2)));
        QVERIFY(waitRows(N));
        const qint64 third = timer.elapsed();
        qInfo("favtab N=%d first=%lldms second=%lldms third=%lldms", N,
              (long long)first, (long long)second, (long long)third);
        QVERIFY2(first < 1200,
                 qPrintable(QStringLiteral("first open %1ms").arg(first)));
        QVERIFY2(second < 700,
                 qPrintable(QStringLiteral("revisit %1ms").arg(second)));
        QVERIFY2(third < 700,
                 qPrintable(QStringLiteral("third visit %1ms").arg(third)));
    }

    // The click must be honoured before any DB/scan work runs: activateTab
    // shows and paints the grid loading veil synchronously, then queues
    // the content rebuild; the veil only drops when the rows land.
    void tabClickShowsLoadingStateImmediately() {
        QTemporaryDir xdg;
        QVERIFY(xdg.isValid());
        qputenv("XDG_CONFIG_HOME", xdg.filePath("config").toUtf8());
        qputenv("XDG_DATA_HOME", xdg.filePath("data").toUtf8());
        qputenv("XDG_CACHE_HOME", xdg.filePath("cache").toUtf8());
        QDir().mkpath(xdg.filePath("config/box"));
        const QString dl = xdg.filePath("downloads");
        QDir().mkpath(dl + QStringLiteral("/danbooru"));
        {
            QSettings ini(xdg.filePath("config/box/settings.ini"),
                          QSettings::IniFormat);
            ini.setValue(QStringLiteral("classify/enabled"), false);
            ini.setValue(QStringLiteral("downloads/dir"), dl);
            ini.sync();
        }
        const QString archive = xdg.filePath("archive");
        QDir().mkpath(archive);
        const int N = 60;
        FavDb db;
        QVERIFY(db.open());
        QVector<Post> posts;
        for (int i = 0; i < N; ++i) {
            const QString name = QStringLiteral("%1.jpg").arg(i);
            const QString path = dl + QStringLiteral("/danbooru/") + name;
            QImage img(32, 32, QImage::Format_RGB32);
            img.fill(QColor::fromHsv((i * 13) % 360, 180, 200));
            QVERIFY(img.save(path));
            Post p;
            p.isRemote = true;
            p.siteName = QStringLiteral("danbooru");
            p.siteKey = QStringLiteral("danbooru");
            p.id = 200000 + i;
            p.ext = QStringLiteral("jpg");
            p.rating = QStringLiteral("general");
            p.fileUrl = QStringLiteral("https://example.com/%1.jpg").arg(i);
            p.previewUrl = p.fileUrl;
            p.postUrl = QStringLiteral("https://danbooru.donmai.us/posts/%1")
                            .arg(200000 + i);
            p.tags = QStringLiteral("solo general_tag");
            p.tagsGeneral = p.tags;
            posts << p;
        }
        QVERIFY(db.addMany(posts) == N);
        QHash<QPair<QString, qint64>, QString> links;
        for (int i = 0; i < N; ++i)
            links.insert({QStringLiteral("danbooru"), 200000 + i},
                         dl + QStringLiteral("/danbooru/%1.jpg").arg(i));
        QVERIFY(db.setLocalPaths(links) == N);
        db.close();

        MainWindow w(archive, true);
        w.show();
        QTest::qWait(200);
        GridView *grid = w.findChild<GridView *>();
        QVERIFY(grid);
        auto *veil = w.findChild<QWidget *>(QStringLiteral("gridLoadingOverlay"));
        QVERIFY(veil);
        auto waitRows = [&]() {
            for (int i = 0; i < 400; ++i) {
                if (grid->model() && grid->model()->rowCount() >= N)
                    return true;
                QTest::qWait(5);
            }
            return false;
        };
        // New favourites tab: veil visible the instant activateTab returns,
        // before the queued finishActivateTab has run.
        QVERIFY(QMetaObject::invokeMethod(&w, "openFavTab"));
        QVERIFY2(veil->isVisible(), "no loading veil right after opening a tab");
        QVERIFY(waitRows());
        QTest::qWait(20);
        QVERIFY2(!veil->isVisible(), "loading veil stayed after rows landed");

        // Switch away and back through the tab bar: same immediate feedback,
        // even though this tab's rows are already cached.
        QVERIFY(QMetaObject::invokeMethod(&w, "onTabChanged", Q_ARG(int, 1)));
        QTest::qWait(100);
        QVERIFY(!veil->isVisible());
        QVERIFY(QMetaObject::invokeMethod(&w, "onTabChanged", Q_ARG(int, 2)));
        QVERIFY2(veil->isVisible(), "no loading veil right after a tab click");
        QVERIFY(waitRows());
        QTest::qWait(20);
        QVERIFY2(!veil->isVisible(), "loading veil outlived the tab switch");
    }

    void noFloatingAspectCombo() {
        QTemporaryDir xdg;
        QVERIFY(xdg.isValid());
        qputenv("XDG_CONFIG_HOME", xdg.filePath("config").toUtf8());
        qputenv("XDG_DATA_HOME", xdg.filePath("data").toUtf8());
        qputenv("XDG_CACHE_HOME", xdg.filePath("cache").toUtf8());
        bool ok = false;
        const QString root = makeArchive(3, ok);
        QVERIFY(ok);
        MainWindow w(root, true);
        w.show();
        QTest::qWait(200);
        // Thumbnail ratio is a Preferences setting only. A second combo used
        // to be parented to the window without joining a layout, so it
        // floated over the tab bar at (0,0) showing the current "3:4".
        for (QComboBox *cb : w.findChildren<QComboBox *>()) {
            for (int i = 0; i < cb->count(); ++i) {
                const QString text = cb->itemText(i);
                QVERIFY2(!text.contains(QStringLiteral("3:4")),
                         qPrintable(QStringLiteral("stray aspect combo at %1,%2: %3")
                                        .arg(cb->x())
                                        .arg(cb->y())
                                        .arg(text)));
                QVERIFY2(!text.startsWith(QStringLiteral("Custom")),
                         qPrintable(QStringLiteral("stray aspect combo at %1,%2: %3")
                                        .arg(cb->x())
                                        .arg(cb->y())
                                        .arg(text)));
            }
        }
    }

    void singleTagCategoriesHide() {
        QTemporaryDir xdg;
        QVERIFY(xdg.isValid());
        qputenv("XDG_CONFIG_HOME", xdg.filePath("config").toUtf8());
        qputenv("XDG_DATA_HOME", xdg.filePath("data").toUtf8());
        qputenv("XDG_CACHE_HOME", xdg.filePath("cache").toUtf8());
        bool ok = false;
        const QString root = makeTaggedArchive(ok);
        QVERIFY(ok);
        MainWindow w(root, true);
        w.show();
        GridView *grid = w.findChild<GridView *>();
        QVERIFY(grid);
        bool filled = false;
        for (int i = 0; i < 100 && !filled; ++i) {
            QTest::qWait(50);
            filled = rowCount(grid) >= 2;
        }
        QVERIFY(filled);
        QTest::qWait(300); // let the sidebar rebuild land
        // Two artists -> section stays. One character -> a header alone is
        // not worth the space, so the section hides (same rule recategorise
        // relies on when a move leaves a category with a single tag).
        QCOMPARE(sectionVisibility(w, QStringLiteral("ARTISTS")).toBool(), true);
        QCOMPARE(sectionVisibility(w, QStringLiteral("CHARACTERS")).toBool(),
                 false);
        QCOMPARE(sectionVisibility(w, QStringLiteral("META")).toBool(), false);
    }

    void pixmapsStayWindowed() {
        QTemporaryDir xdg;
        QVERIFY(xdg.isValid());
        qputenv("XDG_CONFIG_HOME", xdg.filePath("config").toUtf8());
        qputenv("XDG_DATA_HOME", xdg.filePath("data").toUtf8());
        qputenv("XDG_CACHE_HOME", xdg.filePath("cache").toUtf8());
        bool ok = false;
        const QString root = makeArchive(700, ok);
        QVERIFY(ok);
        MainWindow w(root, true);
        w.show();
        GridView *grid = w.findChild<GridView *>();
        QVERIFY(grid);
        // Wait for the async scan to card all rows.
        bool filled = false;
        for (int i = 0; i < 600 && !filled; ++i) {
            QTest::qWait(100);
            filled = rowCount(grid) >= 700;
        }
        QVERIFY(filled);
        // Let the thumb pool work through the first screenful.
        QTest::qWait(4000);
        // Scroll through the archive in steps so every range gets a
        // chance to load (old code would park ~700 distinct pixmaps).
        QScrollBar *bar = grid->verticalScrollBar();
        QVERIFY(bar);
        for (int f = 1; f <= 4; ++f) {
            bar->setValue(bar->maximum() * f / 4);
            QTest::qWait(3000);
        }
        bar->setValue(bar->maximum());
        QTest::qWait(3000);
        int distinct = 0, top = 0;
        qint64 lastKey = 0;
        decorationStats(grid, distinct, top, lastKey);
        // Far rows must share the one placeholder pixmap: a small
        // distinct count proves old rows unloaded while scrolling.
        QVERIFY2(distinct <= 220,
                 qPrintable(QStringLiteral("distinct=%1").arg(distinct)));
        QVERIFY2(top >= 400,
                 qPrintable(QStringLiteral("placeholder rows=%1").arg(top)));
        // The bottom row is on screen: it must carry a real thumb.
        QVERIFY2(lastKey != 0, "last row has no pixmap");
    }
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    TestGridWindow tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_gridwindow.moc"
