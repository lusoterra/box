#pragma once

// Human-readable sidecar viewer: summary fields + tag buckets on one tab,
// the raw JSON as an expandable tree on the other. The raw JSON stays
// available "Open JSON in editor…" (default text editor setting, or a
// confirmation before falling back to the system handler).
#include <QDialog>
#include <QJsonObject>
#include <QString>

#include "post.h"

class MetadataDialog : public QDialog {
    Q_OBJECT
public:
    // jsonPath may be empty (remote-only row); fallbackRaw is used then.
    MetadataDialog(const Post &post, const QString &jsonPath,
                   const QJsonObject &fallbackRaw, QWidget *parent = nullptr);

private:
    void openJson();
    void showFolder();

    QString m_jsonPath;
};
