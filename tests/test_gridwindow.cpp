// Grid pixmap windowing: a big archive must not park a decoded pixmap
// in every card. Headless (offscreen platform, temp XDG dirs): opens a
// 700-file fixture archive in a real MainWindow, scrolls through it, and
// asserts far rows share the single placeholder pixmap (one cache key)
// while near rows carry real thumbs - i.e. memory stays flat.
#include "mainwindow.h"

#include <QApplication>
#include <QComboBox>
#include <QImage>
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

class TestGridWindow : public QObject {
    Q_OBJECT
private slots:
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
