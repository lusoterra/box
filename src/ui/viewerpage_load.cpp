#include "viewerpage.h"

#include "imagecache.h"
#include "remotesource.h"
#include "webdlbackend.h"

#include <QCryptographicHash>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImage>
#include <QImageReader>
#include <QLabel>
#include <QPoint>
#include <QPushButton>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace {
QString formatBytes(qint64 bytes) {
    if (bytes < 1024)
        return QString::number(bytes) + QStringLiteral(" B");
    if (bytes < 1024 * 1024)
        return QString::number(bytes / 1024.0, 'f', 1) + QStringLiteral(" KB");
    return QString::number(bytes / (1024.0 * 1024.0), 'f', 1) + QStringLiteral(" MB");
}
bool isWallError(const QString &error) {
    const QString low = error.toLower();
    return low.contains(QStringLiteral("hallenge")) ||
           low.contains(QStringLiteral("captcha")) ||
           low.contains(QStringLiteral("clearance")) ||
           low.contains(QStringLiteral("wall")) || low.contains(QStringLiteral("403")) ||
           low.contains(QStringLiteral("449")) ||
           low.startsWith(QStringLiteral("http"));
}
} // namespace

void ViewerPage::showNote(const QString &text) {
    m_noteUrl.clear();
    m_noteError.clear();
    m_note->setTextFormat(Qt::PlainText);
    m_note->setCursor(Qt::ArrowCursor);
    m_note->setText(text);
    m_note->setVisible(!text.isEmpty());
}

void ViewerPage::showErrorNote(const QString &url, const QString &text) {
    m_noteUrl = url;
    m_noteError = text;
    m_note->setTextFormat(Qt::RichText);
    m_note->setCursor(Qt::PointingHandCursor);
    m_note->setText(QStringLiteral("<a href=\"details\">%1</a>")
                        .arg(text.toHtmlEscaped()));
    m_note->setToolTip(tr("Click for request details"));
    m_note->setVisible(true);
}

void ViewerPage::onCacheReady(const QString &url, const QString &localPath) {
    if (url != m_pendingUrl || m_pendingRow != m_row)
        return; // user already navigated away
    m_pendingUrl.clear();
    m_loadBar->setVisible(false);
    showNote({});
    const Post &p = m_posts[m_row];
    if (p.ext == QStringLiteral("gif")) {
        m_stack->setCurrentWidget(m_gif);
        m_gif->setFile(localPath);
    } else {
        loadLocalImage(localPath, m_row);
    }
}

void ViewerPage::onCacheFailed(const QString &url, const QString &error) {
    if (url != m_pendingUrl || m_pendingRow != m_row)
        return;
    // Login-walled full files with public samples (ATF originals): one
    // fallback fetch before giving up, so the post still opens.
    if (!m_sampleFallback && m_row >= 0 && m_row < m_posts.size()) {
        const Post &p = m_posts.at(m_row);
        if (p.isRemote && url == p.fileUrl && !p.sampleUrl.isEmpty() &&
            p.sampleUrl != p.fileUrl && m_imgCache) {
            m_sampleFallback = true;
            m_pendingUrl = p.sampleUrl;
            showNote(tr("Full file unavailable - showing sample"));
            m_loadBar->setRange(0, 0); // busy until progress lands
            m_loadBar->setVisible(true);
            const QString have = m_imgCache->localPath(p.sampleUrl, &p);
            if (!have.isEmpty())
                onCacheReady(p.sampleUrl, have);
            else
                m_imgCache->request(p.sampleUrl,
                                    RemotePosts::mediaRefererFor(p), &p);
            return;
        }
    }
    m_pendingUrl.clear();
    m_loadBar->setVisible(false);
    m_loadInfo->setVisible(false);
    showErrorNote(url, tr("Download failed: %1").arg(error));
    emit mediaFailed(url, error);
    // Fingerprint-walled stills/GIFs (ATF originals): the browser stack
    // re-runs page JS and mints fresh clearance where QNAM replays a
    // dead cookie. One retry, then the error stands.
    if (!m_webRetried && m_row >= 0 && m_row < m_posts.size() && m_stillWeb) {
        const Post &p = m_posts.at(m_row);
        const QString target =
            !p.fileUrl.isEmpty() ? p.fileUrl : RemotePosts::viewerUrlFor(p);
        if (p.isRemote && RemotePosts::needsWebDownload(p) &&
            isWallError(error) && !target.isEmpty()) {
            m_webRetried = true;
            const QString ext =
                QFileInfo(QUrl(target).path()).suffix().toLower();
            const QString dir =
                QStandardPaths::writableLocation(
                    QStandardPaths::CacheLocation) +
                QStringLiteral("/box/image-tmp");
            QDir().mkpath(dir);
            m_stillTmp =
                dir + u'/' +
                QString::fromLatin1(
                    QCryptographicHash::hash(target.toUtf8(),
                                             QCryptographicHash::Sha1)
                        .toHex()) +
                (ext.isEmpty() ? QStringLiteral(".bin") : u'.' + ext);
            m_stillRow = m_row;
            static std::atomic<qint64> nextStillWebId{1LL << 61};
            m_stillWebId = ++nextStillWebId;
            showNote(tr("Retrying in browser…"));
            m_loadBar->setRange(0, 0);
            m_loadBar->setVisible(true);
            m_stillWeb->start(m_stillWebId, target, m_stillTmp);
        }
    }
}

void ViewerPage::onStillWebFinished(qint64 id, bool ok, const QString &error) {
    if (id != m_stillWebId)
        return;
    m_stillWebId = -1;
    const QString tmp = m_stillTmp;
    m_stillTmp.clear();
    const QString failedUrl =
        m_stillRow >= 0 && m_stillRow < m_posts.size()
            ? m_posts.at(m_stillRow).fileUrl
            : QString();
    if (m_stillRow < 0 || m_stillRow >= m_posts.size() ||
        m_stillRow != m_row || !QFile::exists(tmp)) {
        QFile::remove(tmp);
        if (!ok)
            showErrorNote(failedUrl, tr("Download failed: %1").arg(error));
        return;
    }
    if (!ok) {
        QFile::remove(tmp);
        showErrorNote(failedUrl, tr("Download failed: %1").arg(error));
        emit mediaFailed(m_posts.at(m_row).fileUrl, error);
        return;
    }
    // Browser bytes win: show them like a cache hit (temp file lingers
    // in the cache dir until the janitor ages it out).
    m_pendingUrl.clear();
    m_loadBar->setVisible(false);
    showNote({});
    if (m_posts.at(m_row).ext == QStringLiteral("gif")) {
        m_stack->setCurrentWidget(m_gif);
        m_gif->setFile(tmp);
    } else {
        loadLocalImage(tmp, m_row);
    }
}

void ViewerPage::dropStillWeb() {
    if (m_stillWebId >= 0 && m_stillWeb)
        m_stillWeb->cancel(m_stillWebId);
    m_stillWebId = -1;
    if (!m_stillTmp.isEmpty()) {
        QFile::remove(m_stillTmp);
        m_stillTmp.clear();
    }
}

void ViewerPage::onCacheProgress(const QString &url, qint64 received, qint64 total) {
    if (url != m_pendingUrl || m_pendingRow != m_row || !m_loadBar->isVisible())
        return; // stale (navigated away) or bar repurposed
    if (total > 0) {
        m_loadBar->setRange(0, int(qMin<qint64>(total, INT_MAX)));
        m_loadBar->setValue(int(qMin<qint64>(received, total)));
    } else {
        m_loadBar->setRange(0, 0); // size unknown: busy until it is
    }
    // Download speed + progress text next to the bar: "1.2MB / 12MB  ·  3.4MB/s"
    const qint64 now = m_loadTimer.isValid() ? m_loadTimer.elapsed() : 0;
    if (m_loadLastTime == 0) {
        m_loadTimer.start();
        m_loadLastTime = now;
        m_loadLastReceived = received;
    }
    const qint64 dt = now - m_loadLastTime;
    const qint64 dr = received - m_loadLastReceived;
    double speedBps = 0.0;
    if (dt > 100) { // sample no faster than 100ms
        speedBps = double(dr) / (double(dt) / 1000.0);
        m_loadLastTime = now;
        m_loadLastReceived = received;
    }
    QString info;
    if (total > 0) {
        info = formatBytes(received) + QStringLiteral(" / ") + formatBytes(total);
    } else {
        info = formatBytes(received);
    }
    if (speedBps > 0.0)
        info += QStringLiteral("  ·  ") + formatBytes(qint64(speedBps)) + QStringLiteral("/s");
    m_loadInfo->setText(info);
    m_loadInfo->setVisible(true);
}

void ViewerPage::showNotePopup(const QString &body, const QPoint &globalPos) {
    if (body.trimmed().isEmpty())
        return;
    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(tr("Annotation note"));
    auto *lay = new QVBoxLayout(dlg);
    auto *l = new QLabel(body, dlg);
    l->setTextFormat(Qt::PlainText); // sidecar/API text, never markup
    l->setWordWrap(true);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->setMinimumWidth(360);
    l->setMaximumWidth(560);
    lay->addWidget(l);
    auto *close = new QPushButton(tr("Close"), dlg);
    connect(close, &QPushButton::clicked, dlg, &QDialog::accept);
    lay->addWidget(close, 0, Qt::AlignRight);
    dlg->move(globalPos + QPoint(8, 8));
    dlg->show();
    dlg->raise();
}

void ViewerPage::loadLocalImage(const QString &path, int row) {
    m_stack->setCurrentWidget(m_image);
    // Decode off the UI thread. Huge originals are downscaled at decode
    // time (viewerMaxSide setting) - full-res pixmaps stall the view.
    // Local decode has no byte progress: busy spinner until it lands.
    m_loadBar->setRange(0, 0);
    m_loadBar->setVisible(true);
    QFutureWatcher<QImage> *w = new QFutureWatcher<QImage>(this);
    connect(w, &QFutureWatcher<QImage>::finished, this, [this, w, row] {
        m_loadBar->setVisible(false);
        m_loadInfo->setVisible(false);
        if (row == m_row)
            m_image->setImage(w->result());
        w->deleteLater();
    });
    w->setFuture(QtConcurrent::run([path, cap = m_maxSide] {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        const QSize full = reader.size();
        if (full.isValid() && (full.width() > cap || full.height() > cap)) {
            const double k =
                qMin(double(cap) / full.width(), double(cap) / full.height());
            reader.setScaledSize(
                QSize(qMax(1, int(full.width() * k)), qMax(1, int(full.height() * k))));
        }
        return reader.read();
    }));
}

Post ViewerPage::currentPost() const {
    return hasCurrent() ? m_posts.at(m_row) : Post();
}

