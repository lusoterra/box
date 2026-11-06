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
        QVERIFY(!isUgoiraPost(tagged));
        Post taggedZip;
        taggedZip.ext = QStringLiteral("");
        taggedZip.tags = QStringLiteral("ugoira animated");
        QVERIFY(isUgoiraPost(taggedZip));
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

    void frameDelaysGaps() {
        // src/library/ugoioconvert.cpp:115-140
        // Array root form (no {data:...} wrapper).
        QJsonArray arr;
        QJsonObject a;
        a[QStringLiteral("file")] = QStringLiteral("sub/000000.jpg");
        a[QStringLiteral("delay")] = 33;
        arr.append(a);
        QJsonObject rawArr;
        rawArr[QStringLiteral("pixiv_ugoira_frame_data")] = arr;
        auto d = Ugoira::delaysFromRaw(rawArr, 100);
        QCOMPARE(d.value(QStringLiteral("000000.jpg")), 33); // basename match

        // clampDelay: <10 or >10000 falls back to defaultMs.
        QJsonObject rawClamp;
        QJsonArray cd;
        QJsonObject lo; lo[QStringLiteral("file")] = QStringLiteral("a.jpg"); lo[QStringLiteral("delay")] = 1; cd.append(lo);
        QJsonObject hi; hi[QStringLiteral("file")] = QStringLiteral("b.jpg"); hi[QStringLiteral("delay")] = 99999; cd.append(hi);
        QJsonObject ok; ok[QStringLiteral("file")] = QStringLiteral("c.jpg"); ok[QStringLiteral("delay")] = 10; cd.append(ok);
        QJsonObject wrap; wrap[QStringLiteral("data")] = cd;
        rawClamp[QStringLiteral("pixiv_ugoira_frame_data")] = wrap;
        d = Ugoira::delaysFromRaw(rawClamp, 77);
        QCOMPARE(d.value(QStringLiteral("a.jpg")), 77);
        QCOMPARE(d.value(QStringLiteral("b.jpg")), 77);
        QCOMPARE(d.value(QStringLiteral("c.jpg")), 10);

        // Missing name or negative delay skipped.
        QJsonObject rawBad;
        QJsonArray bd;
        bd.append(QJsonObject{{QStringLiteral("file"), QStringLiteral("x.jpg")}}); // no delay
        bd.append(QJsonObject{{QStringLiteral("delay"), 50}}); // no name
        QJsonObject w2; w2[QStringLiteral("data")] = bd;
        rawBad[QStringLiteral("pixiv_ugoira_frame_data")] = w2;
        QVERIFY(Ugoira::delaysFromRaw(rawBad, 100).isEmpty());

        // name fallback to "name" key, delay fallback to "delay_ms"
        QJsonObject rawName;
        QJsonArray nd;
        QJsonObject n0; n0[QStringLiteral("name")] = QStringLiteral("x.jpg"); n0[QStringLiteral("delay_ms")] = 50; nd.append(n0);
        QJsonObject w3; w3[QStringLiteral("data")] = nd;
        rawName[QStringLiteral("pixiv_ugoira_frame_data")] = w3;
        d = Ugoira::delaysFromRaw(rawName, 100);
        QCOMPARE(d.value(QStringLiteral("x.jpg")), 50);

        // invalid delay (0) skipped? clampDelay: 0 < 10 -> fallback to defaultMs. Wait, delay = 0, not < 0, so clampDelay(0, default) -> 0 < 10 -> default.
        QJsonObject rawZero;
        QJsonArray zd;
        QJsonObject z0; z0[QStringLiteral("file")] = QStringLiteral("y.jpg"); z0[QStringLiteral("delay")] = 0; zd.append(z0);
        QJsonObject w4; w4[QStringLiteral("data")] = zd;
        rawZero[QStringLiteral("pixiv_ugoira_frame_data")] = w4;
        d = Ugoira::delaysFromRaw(rawZero, 100);
        QCOMPARE(d.value(QStringLiteral("y.jpg")), 100);
    }

    void mediaExtHelpers() {
        // src/post.h:78+ isVideoExt/gifLikeExts/videoExts
        QVERIFY(isVideoExt(QStringLiteral("mp4")));
        QVERIFY(isVideoExt(QStringLiteral("webm")));
        QVERIFY(!isVideoExt(QStringLiteral("gif")));
        QVERIFY(!isVideoExt(QStringLiteral("zip")));
        QVERIFY(gifLikeExts().contains(QStringLiteral("gif")));
        QVERIFY(gifLikeExts().contains(QStringLiteral("webp")));
        QVERIFY(gifLikeExts().contains(QStringLiteral("jxl")));
        QVERIFY(!gifLikeExts().contains(QStringLiteral("mp4")));
        QCOMPARE(videoExts(), (QStringList{QStringLiteral("mp4"), QStringLiteral("webm")}));
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
