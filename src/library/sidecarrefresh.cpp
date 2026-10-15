#include "sidecarrefresh.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace SidecarRefresh {
QList<Job> collect(const QString &downloadDir,
                   const QMap<QString, QString> &labelToKey) {
    QList<Job> out;
    QDirIterator it(downloadDir, {QStringLiteral("*.json")},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QString siteKey =
            labelToKey.value(QFileInfo(it.filePath()).dir().dirName());
        if (siteKey.isEmpty())
            continue; // not one of ours (or renamed): skip
        QFile f(it.filePath());
        if (!f.open(QIODevice::ReadOnly))
            continue;
        QJsonParseError err;
        const QJsonDocument doc =
            QJsonDocument::fromJson(f.readAll(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject())
            continue;
        bool numOk = false;
        const qint64 id =
            doc.object().value(QStringLiteral("id")).toVariant().toLongLong(&numOk);
        if (!numOk || id <= 0)
            continue;
        // Stray sidecars (hand-tagged strays) are never refreshed: they
        // have no source to refresh from, by design.
        if (doc.object().value(QStringLiteral("stray")).toBool())
            continue;
        out.append({it.filePath(), siteKey, id});
    }
    return out;
}
} // namespace SidecarRefresh

SidecarBatch::SidecarBatch(QObject *parent) : QObject(parent) {
    for (int i = 0; i < kFlights; ++i) {
        auto *flight = new PostRefresher(this);
        connect(flight, &PostRefresher::refreshed, this,
                &SidecarBatch::onRefreshed);
        connect(flight, &PostRefresher::failed, this,
                &SidecarBatch::onFailed);
        m_flights.append(flight);
    }
}

void SidecarBatch::start(const QString &downloadDir) {
    cancel();
    m_pending = SidecarRefresh::collect(downloadDir, RemoteSource::labelKeys());
    m_flight.clear();
    m_done = 0;
    m_updated = 0;
    m_failed = 0;
    m_skipped = 0;
    m_total = m_pending.size();
    if (m_total == 0) {
        emit finished(0, 0, 0);
        return;
    }
    m_active = true;
    emit progress(0, m_total, QString());
    pump();
}

void SidecarBatch::cancel() {
    for (PostRefresher *f : m_flights)
        f->cancel();
    m_pending.clear();
    m_flight.clear();
    m_reqPath.clear();
    m_active = false;
}

void SidecarBatch::pump() {
    if (!m_active)
        return;
    for (PostRefresher *flight : m_flights) {
        if (m_pending.isEmpty())
            break;
        bool busy = false;
        for (auto it = m_flight.begin(); it != m_flight.end(); ++it) {
            if (it.value() == flight) {
                busy = true;
                break;
            }
        }
        if (busy)
            continue;
        const SidecarRefresh::Job job = m_pending.takeFirst();
        const BooruSite *site = RemoteSource::siteFor(job.siteKey);
        if (!site || site->baseUrl.isEmpty()) {
            m_skipped++;
            m_done++;
            emit progress(m_done, m_total, job.jsonPath);
            continue;
        }
        RefreshTarget target;
        target.siteKey = site->key;
        target.baseUrl = site->baseUrl;
        target.id = job.id;
        target.creds.user = site->user;
        target.creds.secret = site->secret;
        const qint64 id = m_nextId++;
        m_flight.insert(id, flight);
        m_reqPath.insert(id, job.jsonPath);
        flight->refresh(target, id);
    }
    if (m_done >= m_total && m_flight.isEmpty() && m_active) {
        m_active = false;
        emit finished(m_updated, m_failed, m_skipped);
    }
}

void SidecarBatch::onRefreshed(qint64 reqId, const QJsonObject &raw) {
    PostRefresher *flight = m_flight.take(reqId);
    const QString path = m_reqPath.take(reqId);
    if (!m_active || !flight)
        return;
    QFile f(path);
    if (!path.isEmpty() && !raw.isEmpty() &&
        f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(raw).toJson(QJsonDocument::Indented));
        m_updated++;
    } else {
        m_failed++;
    }
    m_done++;
    emit progress(m_done, m_total, QFileInfo(path).fileName());
    pump();
}

void SidecarBatch::onFailed(qint64 reqId, const QString &) {
    PostRefresher *flight = m_flight.take(reqId);
    m_reqPath.remove(reqId);
    if (!m_active || !flight)
        return;
    m_failed++;
    m_done++;
    emit progress(m_done, m_total, QString());
    pump();
}
