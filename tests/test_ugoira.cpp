// Ugoira handling: detection, frame-delay parsing, zip unpacking, and
// animated encoding (ffmpeg-dependent parts skip when ffmpeg is absent).
#include "ugoioconvert.h"
#include "post.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

namespace {
QString fixtureZip() {
    return QString::fromLatin1(TESTFILES_DIR) + QStringLiteral("/ugoira-frames.zip");
}
} // namespace

class TestUgoira : public QObject {
    Q_OBJECT
private slots:
    void detectUgoira() {
        Post zip;
        zip.ext = QStringLiteral("zip");
        QVERIFY(isUgoiraPost(zip));
        Post tagged;
        tagged.ext = QStringLiteral("mp4");
        tagged.tags = QStringLiteral("ugoira animated");
        QVERIFY(isUgoiraPost(tagged));
        Post still;
        still.ext = QStringLiteral("jpg");
        still.tags = QStringLiteral("sky");
        QVERIFY(!isUgoiraPost(still));
        Post empty;
        QVERIFY(!isUgoiraPost(empty));
    }

    void frameDelays() {
        QJsonObject raw;
        QJsonArray data;
        QJsonObject f0;
        f0[QStringLiteral("file")] = QStringLiteral("000000.jpg");
        f0[QStringLiteral("delay")] = 250;
        data.append(f0);
        QJsonObject f1;
        f1[QStringLiteral("name")] = QStringLiteral("000001.jpg");
        f1[QStringLiteral("delay_ms")] = 120;
        data.append(f1);
        QJsonObject meta;
        meta[QStringLiteral("data")] = data;
        raw[QStringLiteral("pixiv_ugoira_frame_data")] = meta;
        const auto delays = Ugoira::delaysFromRaw(raw, 100);
        QCOMPARE(delays.value(QStringLiteral("000000.jpg")), 250);
        QCOMPARE(delays.value(QStringLiteral("000001.jpg")), 120);
        QVERIFY(Ugoira::delaysFromRaw(QJsonObject(), 100).isEmpty());
    }

    void unpackFixture() {
        QVERIFY(QFile::exists(fixtureZip()));
        Ugoira::Frames frames;
        QString error;
        QVERIFY(Ugoira::unzipFrames(fixtureZip(), frames, {}, 100, &error));
        QCOMPARE(frames.images.size(), 2);
        QCOMPARE(frames.names,
                 QStringList({QStringLiteral("000000.png"),
                              QStringLiteral("000001.png")}));
        QCOMPARE(frames.delaysMs, QVector<int>({100, 100}));
        QVERIFY(!frames.images.at(0).isNull());
    }

    void encodeWebp() {
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("ffmpeg not in PATH");
        Ugoira::Frames frames;
        QString error;
        QVERIFY(Ugoira::unzipFrames(fixtureZip(), frames, {}, 100, &error));
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString out = tmp.filePath(QStringLiteral("out.webp"));
        QVERIFY(Ugoira::encodeAnimated(frames, QStringLiteral("webp"), out,
                                       nullptr, &error));
        QVERIFY(QFile::exists(out));
        QVERIFY(QFile(out).size() > 0);
    }

    void encodeBadFormat() {
        Ugoira::Frames frames;
        QString error;
        QVERIFY(!Ugoira::encodeAnimated(frames, QStringLiteral("gif"), {},
                                        nullptr, &error));
    }
};

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    TestUgoira tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "test_ugoira.moc"
