// Grid pixmap windowing: a big archive must not park a decoded pixmap
// in every card. Headless (offscreen platform, temp XDG dirs): opens a
// 700-file fixture archive in a real MainWindow, scrolls through it, and
// asserts far rows share the single placeholder pixmap (one cache key)
// while near rows carry real thumbs - i.e. memory stays flat.
#include "mainwindow.h"
#include "statsdialog.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QGraphicsRectItem>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QTextBrowser>
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
    distinctOut = static_cast<int>(counts.size());
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
#include "tabbar.h"
#include "viewerpage.h"
#include <QElapsedTimer>
#include <QItemSelectionModel>
#include <QSettings>
#include <QShortcut>

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

    // Info menu: the old Help dropdown was renamed, and its first action
    // opens the Statistics popup with the rendered report.
    void infoMenuOpensStatistics() {
        QTemporaryDir xdg;
        QVERIFY(xdg.isValid());
        qputenv("XDG_CONFIG_HOME", xdg.filePath("config").toUtf8());
        qputenv("XDG_DATA_HOME", xdg.filePath("data").toUtf8());
        qputenv("XDG_CACHE_HOME", xdg.filePath("cache").toUtf8());

        bool ok = false;
        const QString archive = makeTaggedArchive(ok);
        QVERIFY(ok);
        MainWindow w(archive, true);
        w.show();
        QTest::qWait(150);

        QAction *info = nullptr;
        for (QAction *a : w.menuBar()->actions()) {
            QString text = a->text();
            if (text.remove(u'&') == QStringLiteral("Info"))
                info = a;
        }
        QVERIFY2(info, "Info menu missing");
        QVERIFY(info->menu());
        QAction *stats = nullptr;
        for (QAction *a : info->menu()->actions()) {
            if (a->text().contains(QStringLiteral("Statistics")))
                stats = a;
        }
        QVERIFY2(stats, "Statistics action missing");
        stats->trigger();

        StatsDialog *dlg = nullptr;
        for (int i = 0; i < 300 && !dlg; ++i) {
            QTest::qWait(10);
            for (QWidget *top : QApplication::topLevelWidgets()) {
                if (auto *d = qobject_cast<StatsDialog *>(top)) {
                    dlg = d;
                    break;
                }
            }
        }
        QVERIFY2(dlg, "Statistics dialog did not open");
        auto *view = dlg->findChild<QTextBrowser *>();
        QVERIFY(view);
        const QString text = view->toPlainText();
        QVERIFY2(text.contains(QStringLiteral("Library statistics")),
                 qPrintable(text));
        QVERIFY(text.contains(QStringLiteral("Favourited posts")));
        dlg->close();
        QTest::qWait(50);
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

    // Ctrl+W / Ctrl+Shift+T: the closed tab lands on a LIFO stack and a
    // reopen restores its kind/filters (heavy rows are re-fetched).
    void tabShortcutsCloseReopenTabs() {
        QTemporaryDir xdg;
        QVERIFY(xdg.isValid());
        qputenv("XDG_CONFIG_HOME", xdg.filePath("config").toUtf8());
        qputenv("XDG_DATA_HOME", xdg.filePath("data").toUtf8());
        qputenv("XDG_CACHE_HOME", xdg.filePath("cache").toUtf8());
        bool ok = false;
        const QString root = makeArchive(2, ok);
        QVERIFY(ok);
        MainWindow w(root, true);
        w.show();
        QTest::qWait(150);
        auto *bar = w.findChild<MarqueeTabBar *>();
        QVERIFY(bar);
        QCOMPARE(bar->count(), 2); // welcome + archive
        QVERIFY(QMetaObject::invokeMethod(&w, "openFavTab"));
        QTest::qWait(100);
        QCOMPARE(bar->count(), 3);
        QVERIFY(bar->tabText(2).contains(QStringLiteral("Favourites")));
        // Rating + sort are back on favourites (client-side filtering and
        // sorting over the DB list). Every sort order is enabled because
        // nothing is appended page-by-page.
        QComboBox *rating = nullptr;
        QComboBox *sort = nullptr;
        for (QComboBox *cb : w.findChildren<QComboBox *>()) {
            for (int i = 0; i < cb->count(); ++i) {
                if (cb->itemText(i) == QStringLiteral("Explicit"))
                    rating = cb;
            }
            if (cb->findData(QStringLiteral("id-desc")) >= 0)
                sort = cb;
        }
        QVERIFY(rating && rating->isVisible());
        QVERIFY(sort && sort->isVisible());
        auto *sortModel = qobject_cast<QStandardItemModel *>(sort->model());
        QVERIFY(sortModel);
        for (int i = 0; i < sort->count(); ++i)
            QVERIFY2(sortModel->item(i)->isEnabled(),
                     qPrintable(sort->itemText(i)));
        QVERIFY(QMetaObject::invokeMethod(&w, "closeCurrentTab"));
        QTest::qWait(100);
        QCOMPARE(bar->count(), 2);
        QCOMPARE(bar->currentIndex(), 1);
        QVERIFY(QMetaObject::invokeMethod(&w, "reopenClosedTab"));
        QTest::qWait(100);
        QCOMPARE(bar->count(), 3);
        QVERIFY(bar->tabText(2).contains(QStringLiteral("Favourites")));
    }

    // Space toggles the current card; Enter opens only a single selection
    // (a multi-selection is still being assembled for a batch action).
    void spaceTogglesGridSelection() {
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
        GridView *grid = w.findChild<GridView *>();
        auto *viewer = w.findChild<ViewerPage *>();
        QVERIFY(grid);
        QVERIFY(viewer);
        bool filled = false;
        for (int i = 0; i < 200 && !filled; ++i) {
            QTest::qWait(20);
            filled = rowCount(grid) >= 3;
        }
        QVERIFY(filled);
        QItemSelectionModel *sel = grid->selectionModel();
        QVERIFY(sel);
        grid->setCurrentIndex(grid->model()->index(0, 0));
        sel->clearSelection();
        QTest::keyClick(grid, Qt::Key_Space);
        QCOMPARE(sel->selectedRows().size(), 1);
        QTest::keyClick(grid, Qt::Key_Space);
        QCOMPARE(sel->selectedRows().size(), 0);
        // Two selected: Enter must stay on the grid.
        sel->select(grid->model()->index(0, 0), QItemSelectionModel::Select);
        sel->select(grid->model()->index(1, 0), QItemSelectionModel::Select);
        QVERIFY(QMetaObject::invokeMethod(
            &w, "onGridActivated",
            Q_ARG(QModelIndex, grid->model()->index(0, 0))));
        QTest::qWait(30);
        QVERIFY(!viewer->isVisible());
        // One selected: Enter opens it.
        sel->clearSelection();
        sel->select(grid->model()->index(2, 0), QItemSelectionModel::Select);
        QVERIFY(QMetaObject::invokeMethod(
            &w, "onGridActivated",
            Q_ARG(QModelIndex, grid->model()->index(2, 0))));
        QTest::qWait(30);
        QVERIFY(viewer->isVisible());
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

    // Fullscreen must not leak grid chrome onto the post: leaving it
    // while still on the viewer keeps the omnibar hidden (backToGrid
    // is what brings it back).
    void viewerFullscreenKeepsOmnibarHidden() {
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
        GridView *grid = w.findChild<GridView *>();
        QVERIFY(grid);
        bool filled = false;
        for (int i = 0; i < 200 && !filled; ++i) {
            QTest::qWait(20);
            filled = rowCount(grid) >= 3;
        }
        QVERIFY(filled);
        QLineEdit *search = nullptr;
        for (QLineEdit *e : w.findChildren<QLineEdit *>()) {
            if (e->placeholderText().contains(QStringLiteral("Search tags"))) {
                search = e;
                break;
            }
        }
        QVERIFY(search);
        QWidget *omni = search->parentWidget();
        QVERIFY(omni);
        QVERIFY(omni->isVisible());
        QItemSelectionModel *sel = grid->selectionModel();
        QVERIFY(sel);
        sel->clearSelection();
        sel->select(grid->model()->index(2, 0), QItemSelectionModel::Select);
        QVERIFY(QMetaObject::invokeMethod(
            &w, "onGridActivated",
            Q_ARG(QModelIndex, grid->model()->index(2, 0))));
        QTest::qWait(50);
        QVERIFY(!omni->isVisible());
        QVERIFY(QMetaObject::invokeMethod(&w, "setViewerFullscreen",
                                          Q_ARG(bool, true)));
        QTest::qWait(50);
        QVERIFY(!omni->isVisible());
        QVERIFY(QMetaObject::invokeMethod(&w, "setViewerFullscreen",
                                          Q_ARG(bool, false)));
        QTest::qWait(50);
        QVERIFY2(!omni->isVisible(), "omnibar leaked onto the viewer page");
        QVERIFY(QMetaObject::invokeMethod(&w, "backToGrid"));
        QTest::qWait(50);
        QVERIFY(omni->isVisible());
    }

    void viewerHasMetadataButton() {        // The side panel is 290px of tags; the record behind the post
        // (sidecar/API JSON) gets a first-class button next to Open post
        // instead of living only in the right-click menu.
        QTemporaryDir xdg;
        QVERIFY(xdg.isValid());
        qputenv("XDG_CONFIG_HOME", xdg.filePath("config").toUtf8());
        qputenv("XDG_DATA_HOME", xdg.filePath("data").toUtf8());
        qputenv("XDG_CACHE_HOME", xdg.filePath("cache").toUtf8());
        bool ok = false;
        const QString root = makeArchive(1, ok);
        QVERIFY(ok);
        MainWindow w(root, true);
        w.show();
        auto *viewer = w.findChild<ViewerPage *>();
        QVERIFY(viewer);
        QPushButton *openPost = nullptr;
        QPushButton *meta = nullptr;
        for (QPushButton *b : viewer->findChildren<QPushButton *>()) {
            if (b->text() == QStringLiteral("Open post"))
                openPost = b;
            else if (b->text() == QStringLiteral("Metadata"))
                meta = b;
        }
        QVERIFY(openPost);
        QVERIFY(meta);
        // Neighbours in the toolbar, in order.
        const auto buttons = viewer->findChildren<QPushButton *>();
        QVERIFY(buttons.indexOf(openPost) + 1 == buttons.indexOf(meta));
    }

    // Annotation rects are in ORIGINAL image pixels and must land on the
    // same pixels of whatever pixmap is displayed (web UIs draw them over
    // the text). Pure geometry: a 400x200 image with a note at
    // (100,50,50,20) must paint the box exactly there.
    void noteOverlayGeometry() {
        QTemporaryDir xdg;
        QVERIFY(xdg.isValid());
        qputenv("XDG_CONFIG_HOME", xdg.filePath("config").toUtf8());
        qputenv("XDG_DATA_HOME", xdg.filePath("data").toUtf8());
        qputenv("XDG_CACHE_HOME", xdg.filePath("cache").toUtf8());
        ImageView view;
        view.resize(400, 200);
        QImage img(400, 200, QImage::Format_RGB32);
        img.fill(Qt::black);
        PostNote note;
        note.x = 100;
        note.y = 50;
        note.w = 50;
        note.h = 20;
        note.body = QStringLiteral("translation");
        view.setImage(img, false);
        view.setNotes({note}, QSize(400, 200));
        // The note box is the only rect item in the scene.
        QGraphicsRectItem *box = nullptr;
        for (QGraphicsItem *it : view.scene()->items()) {
            if (auto *r = dynamic_cast<QGraphicsRectItem *>(it)) {
                box = r;
                break;
            }
        }
        QVERIFY2(box, "note overlay box was not created");
        const QRectF got = box->rect();
        QCOMPARE(got.x(), 100.0);
        QCOMPARE(got.y(), 50.0);
        QCOMPARE(got.width(), 50.0);
        QCOMPARE(got.height(), 20.0);
        QCOMPARE(box->data(0).toString(), QStringLiteral("translation"));
        // Hidden toggle must hide the boxes (they are kept for a cheap
        // re-show, not deleted).
        view.setNotesVisible(false);
        int visibleRects = 0;
        for (QGraphicsItem *it : view.scene()->items())
            if (auto *r = dynamic_cast<QGraphicsRectItem *>(it);
                r && r->isVisible())
                ++visibleRects;
        QCOMPARE(visibleRects, 0);
        view.setNotesVisible(true);
        visibleRects = 0;
        for (QGraphicsItem *it : view.scene()->items())
            if (auto *r = dynamic_cast<QGraphicsRectItem *>(it);
                r && r->isVisible())
                ++visibleRects;
        QCOMPARE(visibleRects, 1);
    }
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    TestGridWindow tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_gridwindow.moc"
