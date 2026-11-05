#include "ugoioconvert.h"

#include "miniz/miniz.h"

#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

namespace {
constexpr int kMaxFrames = 2000;
constexpr qint64 kMaxTotalBytes = 512LL * 1024 * 1024;

int clampDelay(int v, int fallback) {
    if (v < 10 || v > 10000)
        return fallback;
    return v;
}

// Run a child process with event-loop pumping so the GUI thread stays
// alive; *cancel aborts between slices. Returns true on clean exit 0.
bool runTool(const QString &program, const QStringList &args,
             const QAtomicInt *cancel, QString *error) {
    if (QStandardPaths::findExecutable(program).isEmpty()) {
        if (error)
            *error = QObject::tr("%1 not found in PATH").arg(program);
        return false;
    }
    QProcess proc;
    QString stderrText;
    QEventLoop loop;
    QTimer watchdog;
    watchdog.setSingleShot(true);
    watchdog.setInterval(300000); // 5 min: huge ugoira safety net
    QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);
    QTimer poller;
    poller.setInterval(100);
    QObject::connect(&poller, &QTimer::timeout, [&] {
        if (cancel && cancel->loadRelaxed())
            proc.kill();
    });
    QObject::connect(
        &proc,
        QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), &loop,
        &QEventLoop::quit);
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(program, args);
    if (!proc.waitForStarted(15000)) {
        if (error)
            *error = QObject::tr("Could not start %1").arg(program);
        return false;
    }
    watchdog.start();
    poller.start();
    loop.exec();
    poller.stop();
    if (cancel && cancel->loadRelaxed()) {
        if (error)
            *error = QObject::tr("Cancelled");
        return false;
    }
    if (watchdog.isActive())
        watchdog.stop();
    else {
        proc.kill();
        if (error)
            *error = QObject::tr("%1 timed out").arg(program);
        return false;
    }
    stderrText = QString::fromLocal8Bit(proc.readAll());
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        if (error)
            *error = QObject::tr("%1 failed: %2").arg(
                program, stderrText.trimmed().left(300));
        return false;
    }
    return true;
}

bool writeConcatList(const QString &path, const QStringList &files,
                     const QList<int> &delaysMs, QString *error) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = QObject::tr("Cannot write %1").arg(path);
        return false;
    }
    QTextStream out(&f);
    for (int i = 0; i < files.size(); ++i) {
        QString name = files.at(i);
        name.replace(QStringLiteral("'"), QStringLiteral("'\\''"));
        const double sec = qMax(20, delaysMs.value(i, 100)) / 1000.0;
        out << "file '" << name << "'\n";
        out << "duration " << QString::number(sec, 'f', 3) << "\n";
    }
    // The concat demuxer needs the last file twice (trailing duration).
    if (!files.isEmpty()) {
        QString name = files.last();
        name.replace(QStringLiteral("'"), QStringLiteral("'\\''"));
        out << "file '" << name << "'\n";
    }
    return true;
}
} // namespace

namespace Ugoira {
QMap<QString, int> delaysFromRaw(const QJsonObject &raw, int defaultMs) {
    QMap<QString, int> out;
    const QJsonValue root = raw.value(QStringLiteral("pixiv_ugoira_frame_data"));
    QJsonArray data;
    if (root.isObject())
        data = root.toObject().value(QStringLiteral("data")).toArray();
    else if (root.isArray())
        data = root.toArray();
    for (const QJsonValue &v : data) {
        if (!v.isObject())
            continue;
        const QJsonObject o = v.toObject();
        QString name = o.value(QStringLiteral("file")).toString();
        if (name.isEmpty())
            name = o.value(QStringLiteral("name")).toString();
        int delay = o.value(QStringLiteral("delay")).toInt(-1);
        if (delay < 0)
            delay = o.value(QStringLiteral("delay_ms")).toInt(-1);
        if (name.isEmpty() || delay < 0)
            continue;
        // Match by basename: zips carry bare names, metadata may not.
        out.insert(QFileInfo(name).fileName(), clampDelay(delay, defaultMs));
    }
    return out;
}

bool unzipFrames(const QString &zipPath, Frames &frames,
                 const QMap<QString, int> &delaysByName, int defaultDelayMs,
                 QString *error) {
    frames = Frames();
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, zipPath.toUtf8().constData(), 0)) {
        if (error)
            *error = QObject::tr("Not a readable zip: %1").arg(zipPath);
        return false;
    }
    const int n = (int)mz_zip_reader_get_num_files(&zip);
    if (n <= 0 || n > kMaxFrames) {
        mz_zip_reader_end(&zip);
        if (error)
            *error = QObject::tr("Zip has %1 entries").arg(n);
        return false;
    }
    qint64 total = 0;
    bool ok = true;
    for (int i = 0; i < n && ok; ++i) {
        if (mz_zip_reader_is_file_a_directory(&zip, (mz_uint)i))
            continue;
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, (mz_uint)i, &st)) {
            ok = false;
            break;
        }
        if (st.m_uncomp_size > 50 * 1024 * 1024 ||
            (total += (qint64)st.m_uncomp_size) > kMaxTotalBytes) {
            ok = false; // hostile archive guard
            break;
        }
        size_t sz = 0;
        void *raw = mz_zip_reader_extract_to_heap(&zip, (mz_uint)i, &sz, 0);
        if (!raw || !sz) {
            mz_free(raw);
            ok = false;
            break;
        }
        const QByteArray bytes(static_cast<const char *>(raw), (qsizetype)sz);
        mz_free(raw);
        QImage img = QImage::fromData(bytes);
        if (img.isNull()) {
            ok = false; // not a decodable frame (or encrypted entry)
            break;
        }
        frames.images.append(img.convertToFormat(QImage::Format_RGB32));
        const QString name = QFileInfo(QString::fromUtf8(st.m_filename)).fileName();
        frames.names.append(name);
        frames.delaysMs.append(
            clampDelay(delaysByName.value(name, defaultDelayMs), defaultDelayMs));
    }
    mz_zip_reader_end(&zip);
    if (!ok || frames.isEmpty()) {
        frames = Frames();
        if (error && ok)
            *error = QObject::tr("No decodable frames in %1").arg(zipPath);
        else if (error && error->isEmpty())
            *error = QObject::tr("Failed to unpack %1").arg(zipPath);
        return false;
    }
    return true;
}

bool encodeAnimated(const Frames &frames, const QString &format,
                    const QString &outPath, const QAtomicInt *cancel,
                    QString *error) {
    if (frames.isEmpty()) {
        if (error)
            *error = QObject::tr("Nothing to encode");
        return false;
    }
    if (format != QStringLiteral("webp") && format != QStringLiteral("jxl")) {
        if (error)
            *error = QObject::tr("Unknown ugoira format: %1").arg(format);
        return false;
    }
    QTemporaryDir work;
    if (!work.isValid()) {
        if (error)
            *error = QObject::tr("No temp dir");
        return false;
    }
    // Stage frames as PNGs + a concat list carrying per-frame durations.
    QStringList files;
    for (int i = 0; i < frames.images.size(); ++i) {
        const QString fp =
            work.filePath(QStringLiteral("f%1.png").arg(i, 5, 10, QChar(u'0')));
        if (!frames.images.at(i).save(fp, "PNG")) {
            if (error)
                *error = QObject::tr("Cannot stage frame %1").arg(i);
            return false;
        }
        files.append(fp);
    }
    const QString listPath = work.filePath(QStringLiteral("list.txt"));
    if (!writeConcatList(listPath, files, frames.delaysMs, error))
        return false;
    const QString apng = work.filePath(QStringLiteral("frames.apng"));
    if (!runTool(
            QStringLiteral("ffmpeg"),
            {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
             QStringLiteral("error"), QStringLiteral("-y"), QStringLiteral("-f"),
             QStringLiteral("concat"), QStringLiteral("-safe"), QStringLiteral("0"),
             QStringLiteral("-i"), listPath, QStringLiteral("-plays"),
             QStringLiteral("0"), apng},
            cancel, error))
        return false;
    if (format == QStringLiteral("jxl"))
        return runTool(QStringLiteral("cjxl"), {apng, outPath}, cancel, error);
    return runTool(QStringLiteral("ffmpeg"),
                   {QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
                    QStringLiteral("error"), QStringLiteral("-y"),
                    QStringLiteral("-i"), apng, QStringLiteral("-c:v"),
                    QStringLiteral("libwebp_anim"), QStringLiteral("-loop"),
                    QStringLiteral("0"), QStringLiteral("-q:v"),
                    QStringLiteral("82"), outPath},
                   cancel, error);
}
} // namespace Ugoira
