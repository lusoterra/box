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

void MainWindow::loadNextPage() {
    BrowserTab *t = currentTab();
    if (!t || !t->isRemote || m_loadingPage)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastPageLoadMs < 1500)
        return; // cooldown: never hammer the APIs
    m_lastPageLoadMs = now;
    t->page++;
    m_pageLabel->setText(tr("Page %1 · loading…").arg(t->page));
    statusBar()->showMessage(tr("Loading more…"), 4000);
    m_scanBar->setRange(0, 0); // busy animation until the page lands
    m_scanBar->setVisible(true);
    doBrowseSearch(true);
}

void MainWindow::pumpSiteSearch(const QString &key) {
    auto it = m_pendingSites.find(key);
    if (it == m_pendingSites.end() || it->queriesLeft.isEmpty())
        return;
    BrowserTab *t = m_searchTab;
    RemoteSource *src = m_sources.value(key, nullptr);
    if (!t || !src)
        return;
    const QStringList tokens = it->queriesLeft.takeFirst();
    src->setCreds(credsFor(key));
    m_pendingSearches++;
    src->search(RemotePosts::translateSearchTags(tokens, it->engine), t->page,
                m_settings.browseLimit);
}

void MainWindow::doBrowseSearch(bool append) {
    BrowserTab *t = currentTab();
    if (!t || !t->isRemote)
        return;
    clearStatusProbe(); // a new search invalidates the old error details
    if (!append)
        m_hybooruTried.clear(); // fresh page: hydrate its rows again
    m_wallSites.clear();
    m_wallKinds.clear();
    m_wallFailUrls.clear();
    updateWallButton();
    m_appendResults = append;
    m_searchRetried = false; // each search earns its own single retry
    m_loadingPage = true;
    // Instant feedback: new cards render now (bucket colors refine when
    // results land), with the busy bar until the page arrives.
    updateResultLabel();
    if (!append) {
        m_scanBar->setRange(0, 0);
        m_scanBar->setVisible(true);
    }
    // OR-groups split off BEFORE space-splitting (they contain spaces).
    // Engines without a native OR get one search per alternative below.
    const QueryOr::ParsedQuery pq = QueryOr::parse(t->searchQuery);
    QStringList andBase = t->activeTags + pq.andTokens;
    // Server-side -blacklist exclusions are per-entry below: tag-capped
    // accounts (free Danbooru) opt out via the entry's localBlacklist
    // flag and rely on the client-side net (applyBrowseFilter) instead.
    QStringList blacklistExcl;
    for (const QString &b : m_settings.blacklist) {
        if (!b.isEmpty())
            blacklistExcl << QStringLiteral("-") + b;
    }
    // One-click AI filter: same -exclusion mechanism as the blacklist.
    if (m_settings.hideAi) {
        for (const QString &ai : aiContentTags())
            andBase << QStringLiteral("-") + ai;
    }
    // Rating is server-side in browse mode (translated per engine at
    // pump time): a change refetches like a new search instead of
    // filtering the loaded pages.
    if (t->rating != QStringLiteral("all"))
        andBase << QStringLiteral("rating:") + t->rating;
    // Per-site AND set: the entry's own defaults on top of the query.
    // The sort box rides server-side here too (translated per engine;
    // explicit user sort:/order: tokens always win over the box).
    auto siteAndBase = [&](const BooruSite *cfg) {
        QStringList base = andBase;
        if (!cfg->localBlacklist)
            base += blacklistExcl;
        QStringList tags =
            RemotePosts::defaultTagsFor(base, cfg->defaultTags,
                                        t->droppedDefaults);
        const QString serverSort =
            RemotePosts::sortSearchTag(t->sort, cfg->engine);
        if (!serverSort.isEmpty()) {
            auto isSortToken = [](const QString &tok) {
                QString head = tok;
                while (head.startsWith(u'-'))
                    head = head.mid(1);
                head = head.split(u':').value(0).toLower();
                return head == QStringLiteral("sort") ||
                       head == QStringLiteral("order");
            };
            bool userSort = false;
            for (const QString &tok : tags) {
                if (isSortToken(tok)) {
                    userSort = true;
                    break;
                }
            }
            if (!userSort) {
                for (const QStringList &g : pq.groups) {
                    for (const QString &tok : g) {
                        if (isSortToken(tok)) {
                            userSort = true;
                            break;
                        }
                    }
                    if (userSort)
                        break;
                }
            }
            if (!userSort)
                tags << serverSort;
        }
        return tags;
    };
    QStringList sites = t->sites.isEmpty() ? m_settings.browseSites : t->sites;
    if (sites.isEmpty())
        return;
    for (auto it = m_sources.begin(); it != m_sources.end(); ++it)
        it.value()->cancel();
    m_pendingSearches = 0;
    m_pendingSites.clear();
    m_searchTab = t;
    statusBar()->showMessage(
        tr("Searching %1…").arg(siteDisplayNames(sites).join(QStringLiteral(", "))));
    QStringList errors;
    QStringList skipped;
    for (const QString &key : sites) {
        if (isVirtualSource(key))
            continue; // merged locally in onRemoteFinished, no fetch
        const BooruSite *cfg = RemoteSource::siteFor(key);
        if (!cfg) {
            errors << tr("%1: unknown site").arg(key);
            continue;
        }
        if (cfg->baseUrl.isEmpty()) {
            skipped << key; // self-hosted, no instance URL configured
            continue;
        }
        if (!sourceFor(key)) {
            errors << tr("%1: unknown site").arg(key);
            continue;
        }
        // One queued search per OR combination; pumped one at a time so
        // every engine answers the same semantics (most have no native OR).
        // Expansion is per site: entry defaults change the AND set (and
        // the combination count) per entry.
        bool over = false;
        const QList<QStringList> expansions = QueryOr::expand(
            {siteAndBase(cfg), pq.groups}, QueryOr::kMaxCombos, over);
        if (over) {
            errors << tr("%1: too many OR combinations - narrow the groups")
                            .arg(cfg->label);
            continue;
        }
        if (expansions.isEmpty()) {
            errors << tr("%1: empty OR group - nothing to search")
                            .arg(cfg->label);
            continue;
        }
        PendingSite ps;
        ps.engine = cfg->engine;
        ps.queriesLeft = expansions;
        m_pendingSites.insert(key, ps);
        pumpSiteSearch(key);
    }
    if (m_pendingSearches == 0) {
        m_loadingPage = false;
        m_scanBar->setVisible(false);
        if (!skipped.isEmpty())
            errors << tr("%1 skipped (no instance URL)")
                           .arg(siteDisplayNames(skipped).join(", "));
        if (!errors.isEmpty())
            statusBar()->showMessage(errors.join(QStringLiteral(" · ")), 8000);
        else
            onRemoteFinished();
        return;
    }
}

void MainWindow::onRemoteFinished() {
    // Attribute this completion to its site: accumulate posts across the
    // site's queued OR-alternatives, then pump the next one (if any).
    if (RemoteSource *done = qobject_cast<RemoteSource *>(sender())) {
        for (auto it = m_sources.begin(); it != m_sources.end(); ++it) {
            if (it.value() != done)
                continue;
            const QString key = it.key();
            if (!m_pendingSites.contains(key))
                break;
            PendingSite &ps = m_pendingSites[key];
            if (!done->lastError().isEmpty() && ps.error.isEmpty()) {
                const BooruSite *cfg = RemoteSource::siteFor(key);
                QString oneLine;
                if (done->lastHttp() == 401 && cfg &&
                    cfg->engine == BooruEngine::Gelbooru)
                    oneLine =
                        tr("%1: login required (add an API key in "
                           "Preferences \u2192 Booru manager)")
                            .arg(cfg->label);
                else {
                    const BooruSite *errSite = RemoteSource::siteFor(key);
                    oneLine = tr("%1: %2").arg(errSite ? errSite->label : key,
                                               done->lastError());
                }
                ps.error = oneLine;
                ps.probe = oneLine + u'\n' + RemotePosts::probeReportText(
                    RemotePosts::redactSecrets(
                        done->lastUrl().toString(QUrl::FullyEncoded)),
                    done->lastHttp(), done->lastBody());
            }
            if (!done->lastFreshCookies().isEmpty()) {
                // Native PoW mint: merge into the entry so downloads,
                // thumbs and refreshes (separate NAMs) share it; saved,
                // survives restarts until it rotates or the IP changes.
                for (BooruSite &b : m_settings.boorus) {
                    if (b.key != key)
                        continue;
                    const QStringList merged = RemotePosts::normalizeCookies(
                        b.cookies + done->lastFreshCookies());
                    if (merged != b.cookies) {
                        b.cookies = merged;
                        saveSettings(m_settings);
                        RemoteSource::setConfiguredBoorus(m_settings.boorus);
                    }
                    break;
                }
            }
            ps.posts += done->lastPosts();
            if (!ps.queriesLeft.isEmpty())
                pumpSiteSearch(key);
            // Retryable completions (rate limits, 5xx, timeouts) mark the
            // site: a fresh search that lands nothing but these gets one
            // delayed retry instead of a dead empty grid. Auth/wall/bad
            // filter errors (401/403/400) never retry. Challenge walls
            // mark the site separately for the solve-in-browser menu.
            if (!done->lastError().isEmpty() &&
                RemotePosts::isTransientSearchError(done->lastHttp(),
                                                    done->lastError()))
                ps.transientError = true;
            if (done->lastWall())
                ps.wall = true;
            if (done->lastWall() && ps.wallKind.isEmpty())
                ps.wallKind = done->lastWallKind();
            if (done->lastWall() && ps.failUrl.isEmpty() &&
                done->lastUrl().isValid())
                ps.failUrl = done->lastUrl();
            break;
        }
    }
    if (--m_pendingSearches > 0) {
        statusBar()->showMessage(
            tr("Searching… %1 left").arg(m_pendingSearches));
        return;
    }
    // All done: merge in tab site order.
    BrowserTab *t = m_searchTab;
    m_searchTab = nullptr;
    if (!t)
        return;
    const bool wasAppend = m_appendResults; // reset below; gates the wall menu
    QVector<Post> merged;
    QStringList errors;
    QStringList probeSections; // raw request/response per failing site
    int hiddenWalled = 0;
    const QStringList sites = t->sites.isEmpty() ? m_settings.browseSites : t->sites;
    // Virtual sources contribute local posts (tag-filtered like the server
    // filters remote ones); page 1 only, so infinite scroll never doubles
    // them. Rating/blacklist filtering happens for everything below.
    const bool wantVirtual = !m_appendResults || t->page <= 1;
    QStringList vinc, vexc;
    QList<QStringList> vgroups;
    if (wantVirtual) {
        // OR-groups split off before space-splitting (they contain spaces).
        const QueryOr::ParsedQuery vpq = QueryOr::parse(t->searchQuery);
        vgroups = vpq.groups;
        QStringList vtokens = t->activeTags + vpq.andTokens;
        splitIncExc(vtokens, vinc, vexc);
    }
    for (const QString &key : sites) {
        if (isVirtualSource(key)) {
            if (!wantVirtual)
                continue;
            QVector<Post> local;
            if (key == kSrcFavourites) {
                if (m_favDb.isOpen())
                    local = m_favDb.list();
            } else {
                const QString dir = effectiveDownloadDir(m_settings);
                if (QDir(dir).exists())
                    local = Scanner::scan(dir, nullptr, m_settings.indexCache);
            }
            for (const Post &p : local) {
                if (matchesTokens(p, vinc, vexc) && groupsMatch(p, vgroups))
                    merged.append(p);
            }
            continue;
        }
        const auto bit = m_pendingSites.constFind(key);
        if (bit == m_pendingSites.constEnd())
            continue; // unknown site (error recorded at dispatch)
        if (!bit->error.isEmpty()) {
            errors << bit->error;
            probeSections << bit->probe;
        }
        for (const Post &p : bit->posts) {
            // Login-walled entries carry no viewable media: hide them up
            // front instead of showing dead grid items.
            if (p.fileUrl.isEmpty() && p.sampleUrl.isEmpty()) {
                hiddenWalled++;
                continue;
            }
            // Stamp the configured entry behind this result: badges (and
            // anything else per-entry) resolve exactly, even when several
            // entries share one engine family.
            Post stamped = p;
            stamped.siteKey = key;
            // Per-host buckets fold into general here (meme hosts): the
            // parse already applied the engine-family grid; the entry's
            // own set applies on top. Membership is unchanged (moved, not
            // removed), so p.tags stays valid.
            auto foldField = [&](QString &field, const char *bucket) {
                const QString b = QString::fromLatin1(bucket);
                if (!field.isEmpty() &&
                    RemotePosts::foldedBucketFor(stamped, b) ==
                        QStringLiteral("general")) {
                    stamped.tagsGeneral = stamped.tagsGeneral.isEmpty()
                                              ? field
                                              : stamped.tagsGeneral + u' ' + field;
                    field.clear();
                }
            };
            foldField(stamped.tagsArtist, "artist");
            foldField(stamped.tagsCharacter, "character");
            foldField(stamped.tagsCopyright, "copyright");
            foldField(stamped.tagsMeta, "meta");
            merged.append(stamped);
        }
    }
    bool anyTransient = false;
    QStringList wallSites;
    for (auto bit = m_pendingSites.begin(); bit != m_pendingSites.end();
         ++bit) {
        if (bit->transientError) {
            anyTransient = true;
            break;
        }
    }
    QHash<QString, QString> wallKinds;
    QHash<QString, QUrl> wallFailUrls;
    for (const QString &key : sites) {
        if (isVirtualSource(key))
            continue;
        const auto bit = m_pendingSites.constFind(key);
        if (bit == m_pendingSites.constEnd() || !bit->wall)
            continue;
        wallSites << key;
        if (!bit->wallKind.isEmpty())
            wallKinds.insert(key, bit->wallKind);
        if (!bit->failUrl.isEmpty())
            wallFailUrls.insert(key, bit->failUrl);
    }
    m_pendingSites.clear();
    m_loadingPage = false;
    m_scanBar->setVisible(false); // page landed (classify re-shows if needed)
    // Dedupe across OR-alternatives (and, on append, against
    // the posts already showing). Remote/local twins share a key
    // via dedupeKeyFor, so a downloaded gif never cards twice.
    QSet<QString> seen;
    if (m_appendResults && t->page > 1) {
        // Infinite scroll: append, skipping duplicates.
        for (const Post &p : t->browseMerged) {
            const QString k = dedupeKeyFor(p);
            if (!k.isEmpty())
                seen.insert(k);
        }
        for (const Post &p : merged) {
            const QString k = dedupeKeyFor(p);
            if (k.isEmpty() || !seen.contains(k)) {
                if (!k.isEmpty())
                    seen.insert(k);
                t->browseMerged.append(p);
            }
        }
    } else {
        t->browseMerged.clear();
        for (const Post &p : merged) {
            const QString k = dedupeKeyFor(p);
            if (k.isEmpty() || !seen.contains(k)) {
                if (!k.isEmpty())
                    seen.insert(k);
                t->browseMerged.append(p);
            }
        }
    }
    m_appendResults = false;
    if (t != currentTab()) {
        return; // user switched tabs: stash only, no UI touch
    }
    m_browseMerged = t->browseMerged;
    if (!errors.isEmpty()) {
        statusBar()->showMessage(errors.join(QStringLiteral(" · ")), 10000);
        setStatusProbe(tr("Search errors"),
                       probeSections.join(QStringLiteral("\n\n")));
    } else {
        clearStatusProbe();
        QString msg = tr("%1 results · page %2")
                          .arg(t->browseMerged.size())
                          .arg(t->page);
        if (hiddenWalled > 0)
            msg += tr(" · %1 login-walled hidden").arg(hiddenWalled);
        statusBar()->showMessage(msg, 5000);
    }
    m_wallSites = wallSites;
    m_wallKinds = wallKinds;
    m_wallFailUrls = wallFailUrls;
    updateWallButton();
    // Captcha/PoW wall on fresh searches only (infinite-scroll appends
    // must never steal focus with popups): pop the solve browser open by
    // default, or just offer the solve-or-drop menu when opted out.
    if (!wasAppend && t->page <= 1 && !wallSites.isEmpty()) {
        if (m_settings.wallAutoPopup) {
            // Loop breaker: a solve that doesn't stick (IP-bound
            // rotating clearance) must not summon the browser again
            // within minutes; the Solve button stays available.
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - m_lastWallPopup.value(wallSites.first(), 0) >
                5 * 60 * 1000) {
                m_lastWallPopup.insert(wallSites.first(), now);
                solveWallInBrowser(wallSites.first(),
                                   m_wallKinds.value(wallSites.first()),
                                   m_wallFailUrls.value(wallSites.first()));
            }
        } else {
            showWallMenu(wallSites);
        }
    }
    // Fresh search, nothing landed, failures look transient (rate limits,
    // 5xx, timeouts - never auth/walls): one delayed retry instead of a
    // dead empty grid. Appends never retry (existing rows stay put) and
    // the flag caps it at one per search; manual F5 still works.
    if (t && t->page <= 1 && merged.isEmpty() && anyTransient &&
        !m_searchRetried) {
        m_searchRetried = true;
        statusBar()->showMessage(
            errors.join(QStringLiteral(" · ")) + tr(" · retrying…"), 8000);
        QTimer::singleShot(3000, this, [this, t] {
            if (m_pendingSearches != 0 || currentTab() != t ||
                !m_tabs.contains(t) || !t->browseMerged.isEmpty())
                return;
            doBrowseSearch(false);
        });
    }
    m_pageLabel->setText(tr("Page %1").arg(t->page));
    applyBrowseFilter();
    updateTabTitle(m_curTab);
    maybeClassifyBrowse(t);
    if (m_advanceOnLoad) {
        // Viewer asked past the end: continue there with the fresh items.
        m_advanceOnLoad = false;
        if (m_advanceRow < m_current.size())
            showViewer(m_advanceRow);
        else
            statusBar()->showMessage(tr("No more posts"), 4000);
    }
}

void MainWindow::scheduleHybooruHydration() {
    if (m_hybooruBusy || !m_hybooruFetch)
        return;
    // Tag hydration shares the host with thumbnail downloads; waiting until
    // the grid's thumbnails are idle keeps HyBooru detail requests from
    // starving the previews the user is looking at.
    if (m_thumbs && m_thumbs->busy()) {
        if (!m_hybooruDeferring) {
            m_hybooruDeferring = true;
            QTimer::singleShot(500, this, [this] {
                m_hybooruDeferring = false;
                scheduleHybooruHydration();
            });
        }
        return;
    }
    m_hybooruQueue.clear();
    for (const Post &p : m_browseMerged) {
        if (!p.isRemote || !p.tags.trimmed().isEmpty())
            continue;
        const BooruSite *entry = RemotePosts::entryForPost(p);
        if (!entry || entry->engine != BooruEngine::Hybooru)
            continue;
        const QString key = entry->key + u'#' + QString::number(p.id);
        if (m_hybooruTried.contains(key))
            continue;
        m_hybooruTried.insert(key);
        m_hybooruQueue.append(p);
    }
    if (!m_hybooruQueue.isEmpty())
        pumpHybooruHydration();
}

void MainWindow::pumpHybooruHydration() {
    if (!m_hybooruFetch)
        return;
    while (!m_hybooruQueue.isEmpty()) {
        const Post p = m_hybooruQueue.takeFirst();
        if (!p.tags.trimmed().isEmpty())
            continue;
        const RefreshTarget target = resolveRefreshTarget(p);
        if (!target.valid())
            continue;
        m_hybooruCurrent = p;
        m_hybooruTarget = target;
        m_hybooruBusy = true;
        m_hybooruFetch->refresh(target, 1);
        return;
    }
    m_hybooruBusy = false;
}

