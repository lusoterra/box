#include "gallerydl.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

namespace GalleryDl {

QString findBinary() {
    return QStandardPaths::findExecutable(QStringLiteral("gallery-dl"));
}

bool available() {
    return !findBinary().isEmpty();
}

QString parseVersion(const QString &output) {
    // `gallery-dl --version` prints "1.32.20" (plus build info on some
    // builds); first dotted-number run wins.
    static const QRegularExpression v(QStringLiteral("(\\d+\\.\\d[\\d.]*)"));
    const auto m = v.match(output.trimmed());
    return m.hasMatch() ? m.captured(1) : QString();
}

QString version(const QString &binary) {
    if (binary.isEmpty())
        return {};
    QProcess proc;
    proc.start(binary, {QStringLiteral("--version")});
    if (!proc.waitForFinished(10000))
        return {};
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
        return {};
    return parseVersion(QString::fromUtf8(proc.readAllStandardOutput()));
}

namespace {

// Hosts whose gallery/post pages go to gallery-dl first. Subdomains
// included (user., www.). Booru families are deliberately absent: the
// native resolver yields better (bucketed) sidecars there.
bool isGalleryHost(const QString &host) {
    static const QStringList roots = {
        QStringLiteral("pixiv.net"),   QStringLiteral("fanbox.cc"),
        QStringLiteral("pawchive.pw"), QStringLiteral("pawchive.st"),
        QStringLiteral("kemono.cr"),   QStringLiteral("kemono.party"),
        QStringLiteral("kemono.su"),   QStringLiteral("coomer.st"),
        QStringLiteral("coomer.su"),   QStringLiteral("coomer.party"),
        QStringLiteral("patreon.com"), QStringLiteral("fantia.jp"),
    };
    for (const QString &r : roots) {
        if (host == r || host.endsWith(u'.' + r))
            return true;
    }
    return false;
}

// Direct media file: the built-in downloader fetches it as-is.
bool isDirectMedia(const QString &path) {
    static const QStringList exts = {
        QStringLiteral("jpg"),  QStringLiteral("jpeg"),
        QStringLiteral("png"),  QStringLiteral("gif"),
        QStringLiteral("webp"), QStringLiteral("avif"),
        QStringLiteral("mp4"),  QStringLiteral("webm"),
        QStringLiteral("mkv"),  QStringLiteral("mov"),
        QStringLiteral("mp3"),  QStringLiteral("ogg"),
        QStringLiteral("flac"), QStringLiteral("wav"),
    };
    const QString suffix =
        QFileInfo(path.split(u'?').first()).suffix().toLower();
    return exts.contains(suffix);
}

} // namespace

bool handlesUrl(const QString &url) {
    const QUrl u(url.trimmed());
    if (!u.isValid() ||
        (u.scheme() != QStringLiteral("http") &&
         u.scheme() != QStringLiteral("https")))
        return false;
    if (isDirectMedia(u.path()))
        return false;
    return isGalleryHost(u.host().toLower());
}

QStringList buildArgs(const QString &destDir, const QStringList &urls) {
    QStringList args;
    args << QStringLiteral("--write-metadata") << QStringLiteral("-d")
         << QDir::toNativeSeparators(destDir);
    args.append(urls);
    return args;
}

} // namespace GalleryDl

GalleryDlRunner::GalleryDlRunner(const QString &binary,
                                 const QString &destDir,
                                 const QStringList &urls, QObject *parent)
    : QObject(parent), m_binary(binary), m_destDir(destDir), m_urls(urls) {}

void GalleryDlRunner::start() {
    if (m_proc)
        return;
    QDir().mkpath(m_destDir);
    m_filesBefore = countMediaFiles();
    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_proc, &QProcess::readyReadStandardOutput, this,
            &GalleryDlRunner::onReadyRead);
    connect(m_proc,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &GalleryDlRunner::onProcessFinished);
    emit logLine(tr("Running: gallery-dl %1")
                     .arg(GalleryDl::buildArgs(m_destDir, m_urls).join(u' ')));
    m_proc->setProgram(m_binary);
    m_proc->setArguments(GalleryDl::buildArgs(m_destDir, m_urls));
    m_proc->start();
}

void GalleryDlRunner::cancel() {
    if (!m_proc)
        return;
    m_cancelled = true;
    emit logLine(tr("Cancelling gallery-dl…"));
    m_proc->kill();
}

void GalleryDlRunner::onReadyRead() {
    if (!m_proc)
        return;
    const QString out = QString::fromUtf8(m_proc->readAllStandardOutput());
    for (const QString &line :
         out.split(u'\n', Qt::SkipEmptyParts)) {
        const QString t = line.trimmed();
        if (t.isEmpty())
            continue;
        emit logLine(t);
        // Download lines name the file being written; surface the tail
        // as the current item.
        if (t.contains(QStringLiteral("download"), Qt::CaseInsensitive) ||
            t.contains(QStringLiteral("#")))
            emit progress(t.length() > 120 ? t.left(120) + QStringLiteral("…")
                                           : t);
    }
}

void GalleryDlRunner::onProcessFinished(int exitCode,
                                        QProcess::ExitStatus status) {
    Summary s;
    const int found = countMediaFiles();
    s.filesFound = qMax(0, found - m_filesBefore);
    if (m_cancelled) {
        s.error = tr("Cancelled");
    } else if (status != QProcess::NormalExit) {
        s.error = tr("gallery-dl crashed");
    } else if (exitCode != 0) {
        s.error = tr("gallery-dl exited with code %1 - see the log above "
                     "(login needed? URL unsupported?)")
                      .arg(exitCode);
    } else {
        s.ok = true;
    }
    m_proc->deleteLater();
    m_proc = nullptr;
    emit finished(s);
}

namespace {
bool isMediaFile(const QString &path) {
    static const QStringList exts = {
        QStringLiteral("jpg"),  QStringLiteral("jpeg"),
        QStringLiteral("png"),  QStringLiteral("gif"),
        QStringLiteral("webp"), QStringLiteral("avif"),
        QStringLiteral("mp4"),  QStringLiteral("webm"),
        QStringLiteral("mkv"),  QStringLiteral("mov"),
    };
    return exts.contains(QFileInfo(path).suffix().toLower());
}
} // namespace

int GalleryDlRunner::countMediaFiles() const {
    int n = 0;
    QDirIterator it(m_destDir, QDir::Files | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        if (it.fileName().endsWith(QStringLiteral(".part")))
            continue;
        if (isMediaFile(it.filePath()))
            ++n;
    }
    return n;
}
