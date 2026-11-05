#include "mainwindow.h"
#include "appinfo.h"
#include "cache.h"
#include "cachejanitor.h"
#include "cachepolicy.h"
#include "cookiegrab.h"
#include "downloadmanager.h"
#include "filehash.h"
#include "imagecache.h"
#include "losnimport.h"
#include "metaparser.h"
#include "prefsdlg.h"
#include "progressbar.h"
#include "queryor.h"
#include "remotesource.h"
#include "scanner.h"
#include "settingsexport.h"
#include "stray.h"
#include "straydialog.h"
#include "strayresolve.h"
#include "tabbar.h"
#include "faviconcache.h"
#include "sidecarrefresh.h"
#include "tagsuggest.h"
#include "theme.h"
#include "uicons.h"
#include "scanner.h"
#include "tagtypes.h"
#include "theme.h"
#include "thumbloader.h"
#include "viewerpage.h"
#include "welcomepage.h"
#include <QAbstractItemView>
#include <QApplication>
#include <QActionGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QCompleter>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDrag>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QWidgetAction>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QProgressBar>
#include <QProgressDialog>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringListModel>
#include <QStyledItemDelegate>
#include <QTemporaryFile>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtConcurrent>
#include <climits>
#include <functional>
#include <utility>
#include <algorithm>

void MainWindow::onSearchEdited(const QString &text) {
    // Suggestions follow the tag being typed (2nd, 3rd, inside ~(groups)),
    // not just the first one.
    const QString bare = QueryOr::suggestBare(text);
    if (bare.length() < 2)
        return;
    if (isBrowse()) {
        // Remote tags: ask the tab's first real site (cached per site+prefix).
        BrowserTab *t = currentTab();
        const QStringList sites = t ? t->sites : m_settings.browseSites;
        const BooruSite *site = nullptr;
        for (const QString &key : sites) {
            if (isVirtualSource(key))
                continue;
            site = RemoteSource::siteFor(key);
            if (site)
                break;
        }
        if (!site) {
            m_suggester->cancel();
            return;
        }
        m_suggester->suggest(*site, credsFor(site->key), bare);
        return;
    }
    m_suggester->cancel();
    const auto sug = m_index.suggest(bare, 24, favTagSet());
    QVector<TagSuggestion> local;
    for (const auto &s : sug) {
        TagSuggestion t;
        t.tag = s.tag;
        t.bucket = s.bucket;
        t.count = s.count;
        local.append(t);
    }
    TagSuggester::appendHistory(local, bare, m_settings.historyTokens,
                                &m_index, 24);
    QStringList words, buckets;
    for (const auto &s : local) {
        words << s.tag;
        buckets << s.bucket;
    }
    fillSuggestModel(words, buckets);
    if (!words.isEmpty()) {
        // Filter on the current token: without this the popup matches the
        // whole multi-tag line and stays empty past the first tag.
        m_completer->setCompletionPrefix(bare);
        m_completer->complete();
    }
}

void MainWindow::fillSuggestModel(const QStringList &tags,
                                  const QStringList &buckets) {
    // No-op rebuilds: per-keystroke identical lists must not flash the
    // popup (clear + refill repaints even when nothing changed).
    if (m_completeModel->rowCount() == tags.size()) {
        bool same = true;
        for (int i = 0; i < tags.size(); ++i) {
            const QModelIndex idx = m_completeModel->index(i, 0);
            if (idx.data(Qt::DisplayRole).toString() != tags.at(i) ||
                idx.data(Qt::UserRole + 1).toString() !=
                    (i < buckets.size() ? buckets.at(i)
                                        : QStringLiteral("general"))) {
                same = false;
                break;
            }
        }
        if (same)
            return;
    }
    m_completeModel->clear();
    for (int i = 0; i < tags.size(); ++i) {
        auto *item = new QStandardItem(tags.at(i));
        item->setData(i < buckets.size() ? buckets.at(i)
                                         : QStringLiteral("general"),
                      Qt::UserRole + 1);
        item->setEditable(false);
        m_completeModel->appendRow(item);
    }
}

void MainWindow::onRemoteSuggest(const QString &siteKey, const QString &prefix,
                                 QVector<TagSuggestion> suggestions) {
    Q_UNUSED(siteKey);
    // Stale results (user kept typing) must not overwrite the popup.
    const QString bare = QueryOr::suggestBare(m_searchEdit->text());
    if (bare != prefix || !isBrowse())
        return;
    QStringList words, buckets;
    for (int i = 0; i < m_completeModel->rowCount(); ++i) {
        const QModelIndex idx = m_completeModel->index(i, 0);
        const QString w = idx.data(Qt::DisplayRole).toString();
        // Prune stale rows: keystrokes outrun fetches, so the model may
        // still hold the previous prefix's answers. The completer
        // prefix-filters anyway, so these rows could never display - yet
        // they would eat merge cap slots and crowd out fresh matches.
        if (!w.startsWith(bare, Qt::CaseInsensitive))
            continue;
        words << w;
        buckets << idx.data(Qt::UserRole + 1).toString();
    }
    // Merge, don't replace: the instant local answers stay on top and
    // remote extras append below. An empty/error reply leaves the popup
    // exactly as it was instead of blanking good answers.
    QVector<TagSuggestion> local;
    local.reserve(words.size());
    for (int i = 0; i < words.size(); ++i) {
        TagSuggestion s;
        s.tag = words.at(i);
        s.bucket = i < buckets.size()
                       ? buckets.at(i)
                       : QStringLiteral("general");
        local.append(s);
    }
    // Union + one global relevance pass (locals win exact ties via
    // the stable rank), truncated: rich local vocabularies must not
    // starve ranked server hits.
    const QVector<TagSuggestion> merged =
        TagSuggester::merge(bare, local, suggestions, favTagSet());
    if (merged.isEmpty())
        return;
    words.clear();
    buckets.clear();
    for (const TagSuggestion &sg : merged) {
        words << sg.tag;
        buckets << sg.bucket;
    }
    fillSuggestModel(words, buckets);
    if (!words.isEmpty()) {
        m_completer->setCompletionPrefix(bare);
        m_completer->complete();
    }
}

void MainWindow::onSearchReturn() {
    m_suggester->cancel();
    m_completer->popup()->hide();
    // The omnibar feeds the card bar below: plain tags become one card
    // each (accumulating with existing cards), OR-groups stay a query
    // but render as one card per group (see updateResultLabel).
    const QueryOr::ParsedQuery pq =
        QueryOr::parse(m_searchEdit->text().trimmed());
    for (const QString &tok : pq.andTokens) {
        if (!tok.isEmpty() && !m_activeTags.contains(tok))
            m_activeTags.append(tok);
    }
    // Remember committed tokens for suggestions (deduped, capped).
    for (const QString &tok : pq.andTokens)
        m_settings.historyTokens =
            QueryOr::pushRecentTag(m_settings.historyTokens, tok);
    for (const QStringList &g : pq.groups) {
        for (const QString &tok : g)
            m_settings.historyTokens =
                QueryOr::pushRecentTag(m_settings.historyTokens, tok);
    }
    m_suggester->setHistoryTags(m_settings.historyTokens);
    saveSettings(m_settings);
    m_searchQuery = canonicalQuery({}, pq.groups);
    if (m_settings.searchHistory && m_favDb.isOpen()) {
        const QString recorded = canonicalQuery(m_activeTags, pq.groups);
        if (!recorded.trimmed().isEmpty())
            m_favDb.addSearch(recorded);
    }
    m_searchEdit->clear();
    commitSearch();
}

// Query string round-trip for the group cards: lone `~tag` and `~(...)`
// rebuild identically so removing one card never rewrites the others.
QString MainWindow::canonicalQuery(const QStringList &andTokens,
                                   const QList<QStringList> &groups) {
    QStringList parts = andTokens;
    for (const QStringList &alts : groups) {
        if (alts.size() == 1)
            parts << QStringLiteral("~") + alts.first();
        else
            parts << QStringLiteral("~(") + alts.join(u' ') + u')';
    }
    return parts.join(u' ');
}

void MainWindow::removeQueryGroup(int gi) {
    const QueryOr::ParsedQuery pq = QueryOr::parse(m_searchQuery);
    if (gi < 0 || gi >= pq.groups.size())
        return;
    QList<QStringList> groups = pq.groups;
    groups.removeAt(gi);
    m_searchQuery = canonicalQuery(pq.andTokens, groups);
    updateResultLabel(); // card vanishes now, fetch lands later
    commitSearch();
}

void MainWindow::commitSearch() {
    // Tags or query text changed: save to tab, refetch remote / refilter local.
    normalizeTypedRating();
    normalizeTypedFiletype();
    saveTabState();
    if (isBrowse()) {
        if (BrowserTab *t = currentTab())
            t->page = 1;
        updateTabTitle(m_curTab);
        doBrowseSearch();
    } else {
        rebuildSidebar();
        applyFilter();
    }
}

void MainWindow::commitFilter() {
    // Sort/rating changed: client-side in both modes, no refetch.
    normalizeTypedRating();
    normalizeTypedFiletype();
    saveTabState();
    // Sync combos in case the change came from pills, not the combos.
    m_ratingBox->blockSignals(true);
    m_ratingBox->setCurrentIndex(qMax(0, m_ratingBox->findData(m_rating)));
    m_ratingBox->blockSignals(false);
    m_categoryBox->blockSignals(true);
    m_categoryBox->setCurrentIndex(qMax(0, m_categoryBox->findData(m_category)));
    m_categoryBox->blockSignals(false);
    if (isBrowse())
        applyBrowseFilter();
    else
        applyFilter();
}

