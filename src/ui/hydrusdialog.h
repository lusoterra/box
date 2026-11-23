#pragma once

// Hydrus UI: a shared fetch flow for the stray tag editors and the
// read-only tag viewer opened from post context menus.
#include "hydrus.h"

#include <QDialog>
#include <QFutureWatcher>
#include <QList>
#include <QObject>

class QLabel;
class QPushButton;
class QVBoxLayout;
class QWidget;

struct AppSettings;

// Build a client config from saved settings / preferences widgets.
Hydrus::Config hydrusConfigFromFields(const QString &baseUrl,
                                      const QString &accessKey,
                                      const QString &proxySpec);
Hydrus::Config hydrusConfigFromSettings(const AppSettings &settings);

// Runs one lookup sequence. startFile() hashes the file once (SHA-256 +
// SHA-1 + MD5 in a single read) and queries SHA-256 first; the fallback
// policy decides whether/when a legacy hash is tried.
class HydrusFetch : public QObject {
    Q_OBJECT
public:
    enum class Fallback {
        None,   // one query only
        Ask,    // ask before SHA-1 and before MD5
        Silent, // try SHA-256 -> SHA-1 -> MD5 without prompts
    };

    HydrusFetch(const Hydrus::Config &config, QWidget *promptParent,
                Fallback fallback, QObject *parent = nullptr);
    ~HydrusFetch() override;

    void startFile(const QString &absFile);
    void startHash(const QString &hashHex, Hydrus::HashKind kind);
    void cancel();
    bool isRunning() const { return m_running; }

signals:
    void status(const QString &text);
    void found(const Hydrus::FileTags &file, Hydrus::HashKind kind);
    void notFound();
    void error(const QString &message);
    void finished();

private:
    void startNext();
    void onNotFound(Hydrus::HashKind kind);
    void succeed(const Hydrus::FileTags &file, Hydrus::HashKind kind);
    void fail(const QString &message);
    void miss();

    Hydrus::Config m_config;
    QWidget *m_prompt = nullptr;
    Fallback m_fallback = Fallback::None;
    Hydrus::Client *m_client = nullptr;
    QFutureWatcher<FileContentHashes> *m_hashWatcher = nullptr;
    FileContentHashes m_hashes;
    QList<Hydrus::HashKind> m_order;
    int m_next = 0;
    bool m_running = false;
};

// Read-only Hydrus tag view. Looks the file up on open (silently falling
// back to the legacy hashes) and paints the categorized result.
class HydrusTagsDialog : public QDialog {
    Q_OBJECT
public:
    // localFile: media already on disk (hashed); knownMd5: remote posts
    // with a booru md5 and no local copy. Either may be empty.
    HydrusTagsDialog(const Hydrus::Config &config, const QString &label,
                     const QString &localFile, const QString &knownMd5,
                     QWidget *parent = nullptr);

private:
    void showFound(const Hydrus::FileTags &file, Hydrus::HashKind kind);
    void clearBody();
    void addGroup(const QString &name, const QString &bucket,
                  const QStringList &tags);

    HydrusFetch *m_fetch = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_info = nullptr;
    QWidget *m_body = nullptr;
    QVBoxLayout *m_bodyLayout = nullptr;
    QPushButton *m_copy = nullptr;
    QStringList m_allTags;
    bool m_md5Only = false;
};
