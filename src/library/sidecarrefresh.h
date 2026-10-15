#pragma once

// Bulk sidecar refresh for downloaded files: a download folder's *.json
// sidecars are all native API objects, so each resolves to (site, id) and
// re-fetches wholesale. Runs a few PostRefresher flights in parallel with
// progress; failures stay silent per-file and count up.
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>

#include "remotesource.h" // RefreshTarget (registry-backed)

namespace SidecarRefresh {
// One refreshable sidecar. Pure collection (unit-tested): labelToKey maps
// the download subfolder name (<Site>) to a site key.
struct Job {
    QString jsonPath;
    QString siteKey;
    qint64 id = 0;
};
QList<Job> collect(const QString &downloadDir,
                   const QMap<QString, QString> &labelToKey);
} // namespace SidecarRefresh

class SidecarBatch : public QObject {
    Q_OBJECT
public:
    explicit SidecarBatch(QObject *parent = nullptr);

    void start(const QString &downloadDir);
    void cancel();
    bool isActive() const { return m_active; }

signals:
    void progress(int done, int total, const QString &current);
    void finished(int updated, int failed, int skipped);

private:
    void pump();
    void onRefreshed(qint64 reqId, const QJsonObject &raw);
    void onFailed(qint64 reqId, const QString &error);

    static constexpr int kFlights = 3;
    QList<SidecarRefresh::Job> m_pending;
    QHash<qint64, class PostRefresher *> m_flight; // reqId -> busy flight
    QHash<qint64, QString> m_reqPath; // reqId -> sidecar path
    QVector<class PostRefresher *> m_flights;
    qint64 m_nextId = 1;
    int m_done = 0;
    int m_total = 0;
    int m_updated = 0;
    int m_failed = 0;
    int m_skipped = 0;
    bool m_active = false;
};
