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
    if (bare.length() < 2) {
        // Nothing in progress (fresh box or a trailing space): offer the
        // favourite tags instead of leaving an empty popup.
        if (bare.isEmpty())
            showFavSuggestions();
        return;
    }
    if (isBrowse()) {
        // Remote tags: ask the tab's first real site (cached per site+prefix).
        BrowserTab *t = currentTab();
        const QStringList sites = t ? t->sites : m_settings.browseSites;
        // Multibooru: rank from the first booru; an unset first entry
        // falls back to the last configured one.
        const BooruSite *first = nullptr;
        const BooruSite *last = nullptr;
        for (const QString &key : sites) {
            if (isVirtualSource(key))
                continue;
            const BooruSite *entry = RemoteSource::siteFor(key);
            if (!entry)
                continue;
            last = entry;
            if (!first && !entry->baseUrl.isEmpty())
                first = entry;
        }
        const BooruSite *site = first ? first : last;
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
    // Popular vocabulary cached from earlier API answers (local tabs get
    // it too: a tag you use remotely is still a good completion here).
    // Zero-post rows are junk (stale aliases/empties) - index rows carry
    // the local truth.
    QVector<TagSuggestion> cached;
    const qint64 nowSecs = QDateTime::currentSecsSinceEpoch();
    for (const CachedTag &c : m_tagCache.suggest(bare, 40, favTagSet())) {
        if (c.updated <= 0 ||
            nowSecs - c.updated > TagSuggester::kFreshSeconds)
            continue; // weekly freshness (see TagSuggester::suggest)
        if (c.count <= 0)
            continue;
        TagSuggestion t;
        t.tag = c.tag;
        t.bucket = c.bucket;
        t.count = c.count;
        cached.append(t);
    }
    TagSuggester::appendHistory(local, bare, m_settings.historyTokens,
                                &m_index, 24);
    const QVector<TagSuggestion> ranked =
        TagSuggester::merge(bare, local, cached, favTagSet(), 20);
    QStringList words, buckets;
    QVector<int> counts;
    for (const TagSuggestion &s : ranked) {
        words << s.tag;
        buckets << s.bucket;
        counts << s.count;
    }
    fillSuggestModel(words, buckets, counts);
    if (!words.isEmpty()) {
        // Filter on the current token: without this the popup matches the
        // whole multi-tag line and stays empty past the first tag.
        m_completer->setCompletionPrefix(bare);
        m_completer->complete();
    }
}

// Empty omnibar (or a finished token): the suggestion dropdown becomes the
// favourite-tag picker. No network, no typing - click them one by one to
// build a query, Esc closes the list.
void MainWindow::showFavSuggestions() {
    if (!m_searchEdit || !m_completer || !m_completeModel)
        return;
    if (!m_completer->widget() || !m_searchEdit->isVisible())
        return;
    if (!QueryOr::suggestBare(m_searchEdit->text()).isEmpty())
        return; // a token is being typed: normal suggestions own the popup
    struct Entry {
        QString tag;
        QString bucket;
        int count = 0;
    };
    QVector<Entry> entries;
    QSet<QString> seen;
    for (const QString &raw : m_settings.favTags) {
        const QString tag = raw.trimmed();
        const QString key = tag.toLower();
        if (tag.isEmpty() || seen.contains(key))
            continue;
        seen.insert(key);
        Entry e;
        e.tag = tag;
        // Popularity when the tag is in the cache (or the archive), so the
        // ones that actually have posts surface first.
        for (const CachedTag &c : m_tagCache.suggest(tag, 1, {})) {
            if (c.tag.compare(tag, Qt::CaseInsensitive) != 0)
                continue;
            e.bucket = c.bucket;
            e.count = c.count;
            break;
        }
        if (e.bucket.isEmpty())
            e.bucket = m_index.bucketOf(tag);
        if (e.bucket.isEmpty())
            e.bucket = QStringLiteral("general");
        entries.append(e);
    }
    if (entries.isEmpty())
        return;
    std::stable_sort(entries.begin(), entries.end(),
                     [](const Entry &a, const Entry &b) {
                         if (a.count != b.count)
                             return a.count > b.count;
                         return a.tag.compare(b.tag, Qt::CaseInsensitive) < 0;
                     });
    QStringList words, buckets;
    QVector<int> counts;
    words.reserve(entries.size());
    buckets.reserve(entries.size());
    counts.reserve(entries.size());
    for (const Entry &e : entries) {
        words << e.tag;
        buckets << e.bucket;
        counts << e.count;
    }
    fillSuggestModel(words, buckets, counts);
    m_completer->setCompletionPrefix(QString());
    m_completer->complete();
}

// Suggestion picked with the mouse: swap the token in progress for the
// tag (keeping a leading '-' negation) and leave the dropdown open on the
// next token, so several tags can be picked in a row.
void MainWindow::acceptSuggestion(const QString &tag) {
    const QString clean = tag.trimmed();
    if (clean.isEmpty() || !m_searchEdit)
        return;
    const QString cur = m_searchEdit->text();
    // Replace the token in progress (not "everything after the last
    // space": a click keeps whatever was typed before it, space or not).
    const QString bare = QueryOr::suggestBare(cur);
    qsizetype start = cur.size();
    if (!bare.isEmpty() && cur.endsWith(bare))
        start = cur.size() - bare.size();
    QString head = cur.left(start);
    QString dash;
    if (!bare.isEmpty() && head.endsWith(u'-')) {
        dash = QStringLiteral("-");
        head.chop(1);
    }
    if (!head.isEmpty() && !head.endsWith(u' '))
        head += u' ';
    m_searchEdit->setText(head + dash + clean + u' ');
    m_suggester->cancel();
    const QString next = QueryOr::suggestBare(m_searchEdit->text());
    if (next.isEmpty())
        showFavSuggestions(); // ready for the next favourite
    else
        onSearchEdited(m_searchEdit->text());
}

// Push the archive vocabulary into the persistent tag cache, so its tags
// answer the omnibar by popularity in every tab (and after a restart).
// Bounded: only tags with at least two posts, top 20000.
void MainWindow::syncTagCacheFromIndex() {
    if (!m_tagCache.isOpen())
        return;
    QVector<CachedTag> rows;
    for (const Index::Suggestion &s : m_index.topTags(20000)) {
        CachedTag c;
        c.tag = s.tag;
        c.bucket = s.bucket;
        c.count = s.count;
        rows.append(c);
    }
    m_tagCache.upsert(rows);
}

void MainWindow::fillSuggestModel(const QStringList &tags,
                                  const QStringList &buckets,
                                  const QVector<int> &counts) {
    // No-op rebuilds: per-keystroke identical lists must not flash the
    // popup (clear + refill repaints even when nothing changed).
    const int countRole = Qt::UserRole + 2;
    auto countAt = [&counts](int i) {
        return i < counts.size() ? counts.at(i) : 0;
    };
    if (m_completeModel->rowCount() == tags.size()) {
        bool same = true;
        for (int i = 0; i < tags.size(); ++i) {
            const QModelIndex idx = m_completeModel->index(i, 0);
            if (idx.data(Qt::DisplayRole).toString() != tags.at(i) ||
                idx.data(Qt::UserRole + 1).toString() !=
                    (i < buckets.size() ? buckets.at(i)
                                        : QStringLiteral("general")) ||
                idx.data(countRole).toInt() != countAt(i)) {
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
        item->setData(countAt(i), countRole);
        item->setEditable(false);
        m_completeModel->appendRow(item);
    }
}

void MainWindow::onRemoteSuggest(const QString &siteKey, const QString &prefix,
                                 QVector<TagSuggestion> suggestions) {
    Q_UNUSED(siteKey);
    // Learn from every answer, stale or not: the vocabulary cache is what
    // makes the next keystroke instant (and ranked by post count).
    if (!suggestions.isEmpty() && m_tagCache.isOpen()) {
        QVector<CachedTag> rows;
        rows.reserve(suggestions.size());
        for (const TagSuggestion &s : std::as_const(suggestions)) {
            CachedTag c;
            c.tag = s.tag;
            c.bucket = s.bucket;
            c.count = s.count;
            rows.append(c);
        }
        m_tagCache.upsert(rows);
    }
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
    QVector<int> counts;
    for (const TagSuggestion &sg : merged) {
        words << sg.tag;
        buckets << sg.bucket;
        counts << sg.count;
    }
    fillSuggestModel(words, buckets, counts);
    if (!words.isEmpty()) {
        m_completer->setCompletionPrefix(bare);
        m_completer->complete();
    }
}

// Commit parsed query tokens as pills. "-mytag" replaces the "mytag" pill
// instead of stacking a contradictory second one; typing a tag that is
// already there changes nothing (no duplicate pills).
void MainWindow::mergeActiveTags(const QStringList &tokens) {
    m_activeTags = QueryOr::mergeActiveTokens(m_activeTags, tokens);
}

void MainWindow::onSearchReturn() {
    m_suggester->cancel();
    m_completeTimer->stop();
    m_completer->popup()->hide();
    // The omnibar feeds the card bar below: plain tags become one card
    // each (accumulating with existing cards), OR-groups stay a query
    // but render as one card per group (see updateResultLabel).
    const QueryOr::ParsedQuery pq =
        QueryOr::parse(m_searchEdit->text().trimmed());
    // Positive/negative aware: "-mytag" flips the existing "mytag" pill,
    // repeats are ignored (see mergeActiveTags).
    mergeActiveTags(pq.andTokens);
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
    m_searchEdit->clear();
    m_completeTimer->stop(); // clear() itself queues an empty-text edit
    // Nothing left to auto-complete for: an empty box must not pop the
    // favourite list back open right after Enter (a fresh click does).
    m_completeModel->clear();
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

