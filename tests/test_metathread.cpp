// ViewerPage meta tail (NOTES + COMMENTS): note bodies render without the
// raw box geometry, and the show-all toggle lays out one row per comment.
// Regression: the old toggle rebuilt the layout and detached the collapsed
// labels without deleting them, so the expanded thread stacked on itself.
#include "viewerpage.h"

#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QTextBrowser>
#include <QTextDocument>
#include <QtTest>

namespace {
Post remotePost() {
    Post p;
    p.isRemote = true;
    p.hasMeta = true;
    p.siteKey = QStringLiteral("safebooru");
    p.siteName = QStringLiteral("Safebooru");
    p.id = 42;
    p.width = 1200;
    p.height = 900;
    p.ext = QStringLiteral("jpg");
    p.tagsGeneral = QStringLiteral("sky");
    return p;
}

QVector<PostComment> makeComments(int n) {
    QVector<PostComment> out;
    for (int i = 0; i < n; ++i) {
        PostComment c;
        c.author = QStringLiteral("user%1").arg(i);
        c.date = QStringLiteral("2026-01-01");
        c.body = QStringLiteral("comment body %1").arg(i);
        out.append(c);
    }
    return out;
}

bool anyOverlap(QWidget *root) {
    QList<QWidget *> parents = root->findChildren<QWidget *>();
    parents.prepend(root);
    for (QWidget *parent : parents) {
        const QList<QWidget *> kids = parent->findChildren<QWidget *>(
            QString(), Qt::FindDirectChildrenOnly);
        for (int i = 0; i < kids.size(); ++i) {
            if (!kids.at(i)->isVisible())
                continue;
            const QRect a(kids.at(i)->mapTo(root, QPoint(0, 0)),
                          kids.at(i)->size());
            for (int j = i + 1; j < kids.size(); ++j) {
                if (!kids.at(j)->isVisible())
                    continue;
                const QRect b(kids.at(j)->mapTo(root, QPoint(0, 0)),
                              kids.at(j)->size());
                if (a.intersects(b))
                    return true;
            }
        }
    }
    return false;
}

int visibleCommentRows(QWidget *root) {
    int n = 0;
    for (QLabel *l : root->findChildren<QLabel *>()) {
        if (l->isVisible() && l->text().contains(QStringLiteral("comment body")))
            ++n;
    }
    return n;
}
} // namespace

class TestMetaThread : public QObject {
    Q_OBJECT
private slots:
    void noteBodiesAndCommentToggle() {
        ViewerPage page;
        page.resize(1100, 760);
        const Post p = remotePost();
        page.setPosts({p});
        page.showRow(0);
        page.show();
        QCoreApplication::processEvents();

        const QVector<PostNote> notes = {
            PostNote{10, 20, 30, 40, QStringLiteral("translated line")},
            PostNote{50, 60, 70, 80, QStringLiteral("second note")}};
        const bool invoked = QMetaObject::invokeMethod(
            &page, "onMetaDone", Qt::DirectConnection,
            Q_ARG(QString, p.siteKey), Q_ARG(qint64, p.id),
            Q_ARG(QVector<PostNote>, notes),
            Q_ARG(QVector<PostComment>, makeComments(12)),
            Q_ARG(QVector<PostComment>, QVector<PostComment>()),
            Q_ARG(bool, true), Q_ARG(QString, QString()));
        QVERIFY(invoked);
        QCoreApplication::processEvents();

        // Notes show their translated body; the box geometry rides the
        // tooltip instead of cluttering the line.
        bool sawNote = false;
        for (QLabel *l : page.findChildren<QLabel *>()) {
            if (!l->text().contains(QStringLiteral("translated line")))
                continue;
            sawNote = true;
            QVERIFY(!l->text().contains(QStringLiteral("10,20")));
            QVERIFY(l->toolTip().contains(QStringLiteral("10,20")));
        }
        QVERIFY(sawNote);

        // Collapsed thread: first five rows.
        QCOMPARE(visibleCommentRows(&page), 5);
        QPushButton *more = nullptr;
        for (QPushButton *b : page.findChildren<QPushButton *>()) {
            if (b->text().startsWith(QStringLiteral("Show all")))
                more = b;
        }
        QVERIFY(more);
        QVERIFY(more->text().contains(QStringLiteral("12")));

        QTest::mouseClick(more, Qt::LeftButton);
        QCoreApplication::processEvents();
        // Every row is laid out once: no label sits on top of another.
        QVERIFY2(!anyOverlap(&page), "expanded comment rows overlap");
        QCOMPARE(visibleCommentRows(&page), 12);
        QVERIFY(more->text().startsWith(QStringLiteral("Show fewer")));

        // Collapsing hides them again and keeps the layout clean.
        QTest::mouseClick(more, Qt::LeftButton);
        QCoreApplication::processEvents();
        QCOMPARE(visibleCommentRows(&page), 5);
        QVERIFY(!anyOverlap(&page));
        QVERIFY(more->text().startsWith(QStringLiteral("Show all")));
    }

    void commentaryPreviewAndPopup() {
        ViewerPage page;
        page.resize(1100, 760);
        const Post p = remotePost();
        page.setPosts({p});
        page.showRow(0);
        page.show();
        QCoreApplication::processEvents();

        PostComment c;
        c.body = QStringLiteral("[b]bold[/b] [url=https://e.com/x]link[/url]");
        const bool invoked = QMetaObject::invokeMethod(
            &page, "onMetaDone", Qt::DirectConnection,
            Q_ARG(QString, p.siteKey), Q_ARG(qint64, p.id),
            Q_ARG(QVector<PostNote>, QVector<PostNote>()),
            Q_ARG(QVector<PostComment>, QVector<PostComment>()),
            Q_ARG(QVector<PostComment>, QVector<PostComment>{c}),
            Q_ARG(bool, true), Q_ARG(QString, QString()));
        QVERIFY(invoked);
        QCoreApplication::processEvents();

        // Compact preview: clickable, markup stripped in the small card.
        QLabel *preview = nullptr;
        for (QLabel *l : page.findChildren<QLabel *>()) {
            if (l->text().contains(QStringLiteral("Click to read")))
                preview = l;
        }
        QVERIFY(preview);
        QVERIFY(preview->text().contains(QStringLiteral("bold")));
        QVERIFY(!preview->text().contains(QStringLiteral("**")));

        // Click opens the large popup with the formatted body.
        QVERIFY(QMetaObject::invokeMethod(&page, "showCommentaryPopup"));
        QCoreApplication::processEvents();
        QTextBrowser *browser = nullptr;
        for (QTextBrowser *b : page.findChildren<QTextBrowser *>()) {
            if (b->isVisible())
                browser = b;
        }
        QVERIFY(browser);
        QVERIFY(browser->toPlainText().contains(QStringLiteral("bold")));
        QVERIFY(browser->toPlainText().contains(QStringLiteral("link")));
        QVERIFY(browser->document()->toHtml().contains(
            QStringLiteral("https://e.com/x")));
    }
};

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    qRegisterMetaType<PostNote>("PostNote");
    qRegisterMetaType<PostComment>("PostComment");
    qRegisterMetaType<QVector<PostNote>>("QVector<PostNote>");
    qRegisterMetaType<QVector<PostComment>>("QVector<PostComment>");
    TestMetaThread tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_metathread.moc"
