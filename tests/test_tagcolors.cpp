// Shared tag-bucket palette: suggestions, filter pills and the sidebar
// tag lists all paint from UiIcons::tagBucketColor. Locks the mapping so
// a palette tweak stays deliberate (header-only unit, no widgets).
#include "uicons.h"

#include <QCoreApplication>
#include <QtTest>

class TestTagColors : public QObject {
    Q_OBJECT
private slots:
    void bucketsMapToDocumentedHues() {
        QCOMPARE(UiIcons::tagBucketColor(QStringLiteral("artist")),
                 QColor(0xe0, 0x55, 0x61));
        QCOMPARE(UiIcons::tagBucketColor(QStringLiteral("copyright")),
                 QColor(0x43, 0xd1, 0x7c));
        QCOMPARE(UiIcons::tagBucketColor(QStringLiteral("character")),
                 QColor(0xe0, 0x9a, 0x3c));
        QCOMPARE(UiIcons::tagBucketColor(QStringLiteral("meta")),
                 QColor(0xe5, 0xc5, 0x48));
        QCOMPARE(UiIcons::tagBucketColor(QStringLiteral("custom")),
                 QColor(0x7c, 0x9e, 0xf8));
        // General and unknown buckets fall back to near-white.
        QCOMPARE(UiIcons::tagBucketColor(QStringLiteral("general")),
                 QColor(0xe8, 0xe8, 0xec));
        QCOMPARE(UiIcons::tagBucketColor(QStringLiteral("bogus")),
                 QColor(0xe8, 0xe8, 0xec));
    }
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    TestTagColors tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_tagcolors.moc"
