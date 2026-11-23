#pragma once

// Manual tag editing for the Hydrus tab. One tag per line, native
// namespaces preserved (`creator:alice`, `character:bob`); plain lines
// are General. Saving marks the row as a manual override in hydrus.db,
// which is what protects it from a later Refresh tags.
#include <QDialog>
#include <QStringList>

class QLabel;
class QPlainTextEdit;

class HydrusTagDialog : public QDialog {
    Q_OBJECT
public:
    // `prompt` is the sentence above the editor (import fallback vs edit).
    HydrusTagDialog(const QString &fileName, const QString &prompt,
                    const QStringList &tags, QWidget *parent = nullptr);

    // Normalized (lowercase, spaces -> '_', deduped, sorted).
    QStringList tags() const;

private:
    QPlainTextEdit *m_edit = nullptr;
};

// Before/after view for a forced refresh of a manually edited post.
// The user decides whether the freshly fetched (spam-filtered) tags
// replace the current ones or are discarded.
class HydrusRefreshPreviewDialog : public QDialog {
    Q_OBJECT
public:
    enum class Choice { Keep, Replace };
    HydrusRefreshPreviewDialog(const QString &fileName, bool manualOverride,
                               const QStringList &before,
                               const QStringList &after,
                               QWidget *parent = nullptr);

    Choice choice() const { return m_choice; }

private:
    Choice m_choice = Choice::Keep;
};
