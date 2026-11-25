#pragma once

#include "dedupdb.h"

#include <QString>
#include <QVector>

// One duplicate group. `files` is best-first: the first row is the
// suggested keeper (largest pixels, then bytes), but the dialog may pick
// another keeper after comparing tags.
struct DedupGroup {
    QString scope;
    QString kind;
    QVector<DedupFile> files;
    qint64 wasted = 0; // bytes outside the keeper
};

// Groups exact SHA-256 copies and perceptual near-copies (Hamming
// distance <= maxDistance/64). Rows are partitioned by (scope, kind), so
// archive/download/Hydrus and image/animated/video never mix. pHash
// matching uses representative linkage to avoid A~B, B~C dragging A and C
// into one group.
QVector<DedupGroup> buildDedupGroups(const QVector<DedupFile> &rows,
                                     int maxDistance = 6);

QString dedupScopeLabel(const QString &scope);
QString dedupKindLabel(const QString &kind);
