#include "straydialog.h"

#include "gallerydl.h"
#include "hydrusdialog.h"
#include "strayresolve.h"

#include <QCheckBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QVBoxLayout>

StrayDialog::StrayDialog(bool autoFav, QWidget *parent) : QDialog(parent) {
    setWindowTitle(tr("Download/Add Stray"));
    resize(520, 360);
    auto *lay = new QVBoxLayout(this);
    QString hint;
    if (GalleryDl::available()) {
        hint = tr("Paste post or media URLs, one per line. Gallery links "
                  "(Pixiv, Fanbox, Patreon, …) download with your installed "
                  "gallery-dl into _Stray, tags left for you; booru posts are "
                  "read through their public API; direct media is fetched "
                  "as-is.");
    } else {
        hint = tr("Paste post or media URLs, one per line. Booru posts are read "
                  "through their public API and direct media is fetched as-is. "
                  "Install gallery-dl to download gallery links (Pixiv, "
                  "Fanbox, Patreon, …) into _Stray.");
    }
    auto *hintLabel = new QLabel(hint, this);
    hintLabel->setWordWrap(true);
    lay->addWidget(hintLabel);
    m_urls = new QPlainTextEdit(this);
    m_urls->setPlaceholderText(QStringLiteral("https://…"));
    lay->addWidget(m_urls, 1);
    m_status = new QLabel(this);
    m_status->setStyleSheet(QStringLiteral("color:#4a5068;"));
    m_status->setWordWrap(true);
    lay->addWidget(m_status);
    m_autoFav =
        new QCheckBox(tr("Auto-favourite downloaded/imported strays"), this);
    m_autoFav->setToolTip(tr("They also land in Favourites"));
    m_autoFav->setChecked(autoFav);
    connect(m_autoFav, &QCheckBox::toggled, this,
            &StrayDialog::autoFavChanged);
    lay->addWidget(m_autoFav);
    auto *row = new QHBoxLayout();
    auto collect = [this] {
        QStringList urls;
        for (const QString &line :
             m_urls->toPlainText().split(u'\n', Qt::SkipEmptyParts)) {
            const QString u = line.trimmed();
            if (!u.isEmpty())
                urls << u;
        }
        if (urls.isEmpty())
            m_status->setText(tr("Paste at least one link first."));
        return urls;
    };
    auto *dlBtn = new QPushButton(tr("Download"), this);
    dlBtn->setToolTip(
        tr("Fetch the links into _Stray (booru links also get their JSON "
           "sidecar)"));
    connect(dlBtn, &QPushButton::clicked, this, [this, collect] {
        const QStringList urls = collect();
        if (urls.isEmpty())
            return;
        m_status->setText(tr("Queued %1 link(s).").arg(urls.size()));
        emit downloadRequested(urls);
    });
    auto *favBtn = new QPushButton(tr("Favourite only"), this);
    favBtn->setToolTip(
        tr("Add the links to Favourites without downloading the media"));
    connect(favBtn, &QPushButton::clicked, this, [this, collect] {
        const QStringList urls = collect();
        if (urls.isEmpty())
            return;
        m_status->setText(
            tr("Favouriting %1 link(s), no media download.").arg(urls.size()));
        emit favouriteRequested(urls);
    });
    auto *impBtn = new QPushButton(tr("Import files…"), this);
    impBtn->setToolTip(tr("Copy local files into _Stray"));
    connect(impBtn, &QPushButton::clicked, this, [this] {
        const QStringList paths = QFileDialog::getOpenFileNames(
            this, tr("Import stray files"), QDir::homePath(),
            tr("Images and videos (*.jpg *.jpeg *.png *.gif *.webp *.avif *.mp4 *.webm);;All files (*)"));
        if (paths.isEmpty())
            return;
        m_status->setText(tr("Importing %1 file(s).").arg(paths.size()));
        emit importRequested(paths);
    });
    auto *closeBtn = new QPushButton(tr("Close"), this);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    row->addWidget(dlBtn);
    row->addWidget(favBtn);
    row->addWidget(impBtn);
    row->addStretch(1);
    row->addWidget(closeBtn);
    lay->addLayout(row);
}

StrayTagDialog::StrayTagDialog(const QString &fileName, const QString &tags,
                               const Hydrus::Config &hydrus,
                               const StrayTagEdit &existing, QWidget *parent)
    : QDialog(parent), m_hydrus(hydrus), m_existing(existing),
      m_absFile(existing.absFile) {
    setWindowTitle(tr("Edit tags - %1").arg(fileName));
    resize(440, 260);
    auto *lay = new QVBoxLayout(this);
    lay->addWidget(new QLabel(
        tr("Space-separated tags. Buckets are assigned automatically when "
           "you save; reopen to fix anything miscategorized."),
        this));
    m_edit = new QPlainTextEdit(this);
    m_edit->setPlainText(tags);
    m_edit->setTabChangesFocus(true); // Tab leaves the box (Save/Cancel),
                                      // it must never become tag text
    lay->addWidget(m_edit, 1);
    m_status = new QLabel(this);
    m_status->setStyleSheet(QStringLiteral("color:#4a5068;"));
    m_status->setWordWrap(true);
    lay->addWidget(m_status);
    auto *hydrusRow = new QHBoxLayout();
    m_hydrusBtn = new QPushButton(tr("Fetch from Hydrus"), this);
    m_hydrusBtn->setToolTip(
        tr("Look this file up by SHA-256 in Hydrus and merge its tags"));
    m_hydrusBtn->setVisible(m_hydrus.enabled);
    m_hydrusBtn->setEnabled(m_hydrus.enabled && !m_absFile.isEmpty());
    connect(m_hydrusBtn, &QPushButton::clicked, this,
            &StrayTagDialog::fetchFromHydrus);
    hydrusRow->addWidget(m_hydrusBtn);
    hydrusRow->addStretch(1);
    lay->addLayout(hydrusRow);
    auto *row = new QHBoxLayout();
    auto *saveBtn = new QPushButton(tr("Save"), this);
    connect(saveBtn, &QPushButton::clicked, this, &QDialog::accept);
    auto *cancelBtn = new QPushButton(tr("Cancel"), this);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    row->addStretch(1);
    row->addWidget(cancelBtn);
    row->addWidget(saveBtn);
    lay->addLayout(row);
}

QString StrayTagDialog::tags() const {
    return m_edit ? m_edit->toPlainText() : QString();
}

namespace {

// Whitespace/comma/newline -> lowercase code tokens, deduped in order.
QStringList tagTokens(const QString &s) {
    QStringList out;
    QString norm = s;
    norm.replace(u',', u' ').replace(u'\n', u' ');
    norm = norm.toLower();
    for (const QString &t :
         norm.split(QRegularExpression(QStringLiteral("\\s+")),
                    Qt::SkipEmptyParts)) {
        if (!out.contains(t))
            out.append(t);
    }
    return out;
}

// Append unseen tokens (never duplicate across buckets).
QString mergeTokens(const QString &existing, const QStringList &add,
                    const QString &others) {
    const QStringList cur = tagTokens(existing);
    const QStringList other = tagTokens(others);
    QStringList out = cur;
    for (const QString &t : add) {
        if (!out.contains(t) && !other.contains(t))
            out.append(t);
    }
    return out.join(u' ');
}

} // namespace

void StrayTagDialog::fetchFromHydrus() {
    if (!m_hydrus.enabled || m_absFile.isEmpty())
        return;
    m_hydrusBtn->setEnabled(false);
    auto *fetch = new HydrusFetch(m_hydrus, this, HydrusFetch::Fallback::Ask,
                                  this);
    connect(fetch, &HydrusFetch::status, this,
            [this](const QString &text) { m_status->setText(text); });
    connect(fetch, &HydrusFetch::found, this,
            [this](const Hydrus::FileTags &file, Hydrus::HashKind kind) {
                const Hydrus::CategorizedTags cats =
                    Hydrus::categorize(file.tags);
                auto norm = [](const QStringList &in) {
                    QStringList out;
                    for (const QString &raw : in) {
                        const QString tag = raw.trimmed().toLower();
                        if (!tag.isEmpty() && !out.contains(tag))
                            out << tag;
                    }
                    return out;
                };
                m_hydrusArtist = norm(cats.artist);
                m_hydrusCharacter = norm(cats.character);
                m_hydrusCopyright = norm(cats.copyright);
                m_hydrusGeneral = norm(cats.general);
                m_hydrusMeta = norm(cats.meta);
                QStringList all;
                for (const QStringList *bucket :
                     {&m_hydrusArtist, &m_hydrusCharacter,
                      &m_hydrusCopyright, &m_hydrusMeta, &m_hydrusGeneral})
                    all += *bucket;
                all.removeDuplicates();
                m_hydrusFlat = all;
                const QString before = m_edit->toPlainText();
                m_edit->setPlainText(
                    mergeTokens(before, all, QString()));
                const int added =
                    tagTokens(m_edit->toPlainText()).size() -
                    tagTokens(before).size();
                m_status->setText(
                    tr("Found %1 tag(s) in Hydrus (%2); added %3, "
                       "categorised by Hydrus namespace.")
                        .arg(file.tags.size())
                        .arg(Hydrus::hashKindLabel(kind))
                        .arg(qMax(0, added)));
            });
    connect(fetch, &HydrusFetch::notFound, this, [this] {
        m_status->setText(tr("No Hydrus tags found for this file."));
    });
    connect(fetch, &HydrusFetch::error, this, [this](const QString &message) {
        m_status->setText(message);
    });
    connect(fetch, &HydrusFetch::finished, this, [this, fetch] {
        m_hydrusBtn->setEnabled(m_hydrus.enabled && !m_absFile.isEmpty());
        fetch->deleteLater();
    });
    fetch->startFile(m_absFile);
}

StrayTagEdit StrayTagDialog::hydrusTagSplit() const {
    StrayTagEdit e;
    e.absFile = m_absFile;
    e.fileName = QFileInfo(m_absFile).fileName();
    const QStringList final = tagTokens(m_edit->toPlainText());
    const QSet<QString> remaining(final.begin(), final.end());
    QSet<QString> placed;
    auto take = [&](const QStringList &category, QString &dest) {
        QStringList out = dest.isEmpty()
                              ? QStringList()
                              : dest.split(QChar::Space, Qt::SkipEmptyParts);
        for (const QString &tag : category) {
            if (remaining.contains(tag) && !placed.contains(tag)) {
                out << tag;
                placed.insert(tag);
            }
        }
        dest = out.join(u' ');
    };
    // Hydrus namespaces win; then keep the categories the sidecar had
    // before the fetch so those tags do not fall back to General.
    take(m_hydrusArtist, e.artist);
    take(m_hydrusCharacter, e.character);
    take(m_hydrusCopyright, e.copyright);
    take(m_hydrusMeta, e.meta);
    take(m_hydrusGeneral, e.general);
    take(tagTokens(m_existing.artist), e.artist);
    take(tagTokens(m_existing.character), e.character);
    take(tagTokens(m_existing.copyright), e.copyright);
    take(tagTokens(m_existing.meta), e.meta);
    take(tagTokens(m_existing.general), e.general);
    // Manual additions after the fetch have no namespace: General.
    QStringList general = tagTokens(e.general);
    for (const QString &tag : final) {
        if (!placed.contains(tag)) {
            general << tag;
            placed.insert(tag);
        }
    }
    e.general = general.join(u' ');
    return e;
}

StrayMassTagDialog::StrayMassTagDialog(const QVector<Post> &strays,
                                       const Hydrus::Config &hydrus,
                                       QWidget *parent)
    : QDialog(parent), m_hydrus(hydrus) {
    setWindowTitle(tr("Mass edit stray tags (%1)").arg(strays.size()));
    resize(720, 560);
    m_edits.reserve(strays.size());
    for (const Post &p : strays) {
        StrayTagEdit e;
        e.absFile = p.absFile;
        e.fileName = QFileInfo(p.absFile).fileName();
        e.artist = p.tagsArtist;
        e.character = p.tagsCharacter;
        e.copyright = p.tagsCopyright;
        e.general = p.tagsGeneral;
        e.meta = p.tagsMeta;
        if (e.artist.isEmpty() && e.character.isEmpty() &&
            e.copyright.isEmpty() && e.general.isEmpty() && e.meta.isEmpty())
            e.general = p.tags;
        m_edits.append(std::move(e));
    }
    auto *lay = new QVBoxLayout(this);
    lay->addWidget(new QLabel(
        tr("Pick a file on the left, then edit its tags by category "
           "(artist, character, copyright, general, meta). Paste adds "
           "tokens to General; Copy from URL steals buckets from a booru "
           "post. Copy to all duplicates the current file to every file."),
        this));
    auto *mid = new QHBoxLayout();
    m_list = new QListWidget(this);
    m_list->setMinimumWidth(180);
    m_list->setMaximumWidth(260);
    for (const StrayTagEdit &e : m_edits)
        m_list->addItem(e.fileName);
    mid->addWidget(m_list);
    auto *boxes = new QVBoxLayout();
    auto mkBox = [this, boxes](const QString &label) {
        boxes->addWidget(new QLabel(label, this));
        auto *edit = new QPlainTextEdit(this);
        edit->setMinimumHeight(48);
        edit->setMaximumHeight(96);
        edit->setTabChangesFocus(true);
        boxes->addWidget(edit);
        return edit;
    };
    m_artist = mkBox(tr("Artist:"));
    m_character = mkBox(tr("Character:"));
    m_copyright = mkBox(tr("Copyright:"));
    m_general = mkBox(tr("General:"));
    m_meta = mkBox(tr("Meta:"));
    mid->addLayout(boxes, 1);
    lay->addLayout(mid, 1);
    // Paste row.
    auto *pasteRow = new QHBoxLayout();
    pasteRow->addWidget(new QLabel(tr("Paste tags:"), this));
    m_paste = new QLineEdit(this);
    m_paste->setPlaceholderText(tr("space/comma separated"));
    m_paste->setToolTip(
        tr("Tokens are added to the current file's General box"));
    pasteRow->addWidget(m_paste, 1);
    auto *pasteBtn = new QPushButton(tr("Add pasted"), this);
    pasteRow->addWidget(pasteBtn);
    auto *copyAll = new QPushButton(tr("Copy to all"), this);
    copyAll->setToolTip(
        tr("Duplicate the current file's categories to every listed file"));
    pasteRow->addWidget(copyAll);
    lay->addLayout(pasteRow);
    // Steal-by-URL row.
    auto *urlRow = new QHBoxLayout();
    urlRow->addWidget(new QLabel(tr("Copy from URL:"), this));
    m_url = new QLineEdit(this);
    m_url->setPlaceholderText(QStringLiteral("https://…"));
    m_url->setToolTip(
        tr("Booru post link: buckets are merged into the current file"));
    urlRow->addWidget(m_url, 1);
    m_fetch = new QPushButton(tr("Fetch"), this);
    urlRow->addWidget(m_fetch);
    lay->addLayout(urlRow);
    auto *hydrusRow = new QHBoxLayout();
    m_hydrusBtn = new QPushButton(tr("Fetch tags from Hydrus"), this);
    m_hydrusBtn->setToolTip(
        tr("Hash the current file (SHA-256, then legacy SHA-1/MD5) and merge "
           "Hydrus tags into the buckets"));
    m_hydrusBtn->setVisible(m_hydrus.enabled);
    connect(m_hydrusBtn, &QPushButton::clicked, this,
            &StrayMassTagDialog::fetchHydrusRow);
    hydrusRow->addWidget(m_hydrusBtn);
    hydrusRow->addStretch(1);
    lay->addLayout(hydrusRow);
    m_status = new QLabel(this);
    m_status->setStyleSheet(QStringLiteral("color:#4a5068;"));
    m_status->setWordWrap(true);
    lay->addWidget(m_status);
    auto *row = new QHBoxLayout();
    auto *saveBtn = new QPushButton(tr("Save all"), this);
    connect(saveBtn, &QPushButton::clicked, this,
            [this] { commitCurrent(); accept(); });
    auto *cancelBtn = new QPushButton(tr("Cancel"), this);
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    row->addStretch(1);
    row->addWidget(cancelBtn);
    row->addWidget(saveBtn);
    lay->addLayout(row);

    connect(m_list, &QListWidget::currentRowChanged, this,
            [this](int current) { loadRow(current); });
    connect(pasteBtn, &QPushButton::clicked, this, [this] {
        if (m_row < 0 || m_row >= m_edits.size())
            return;
        const QStringList toks = tagTokens(m_paste->text());
        if (toks.isEmpty())
            return;
        // Dedupe against every bucket so a pasted tag never double-lists.
        const QString others = tagTokens(
                m_artist->toPlainText() + u' ' + m_character->toPlainText() +
                u' ' + m_copyright->toPlainText() + u' ' +
                m_meta->toPlainText())
            .join(u' ');
        m_general->setPlainText(
            mergeTokens(m_general->toPlainText(), toks, others));
        m_paste->clear();
        commitCurrent();
    });
    connect(copyAll, &QPushButton::clicked, this, [this] {
        if (m_row < 0 || m_row >= m_edits.size())
            return;
        commitCurrent();
        const StrayTagEdit cur = m_edits.at(m_row);
        for (StrayTagEdit &e : m_edits) {
            e.artist = cur.artist;
            e.character = cur.character;
            e.copyright = cur.copyright;
            e.general = cur.general;
            e.meta = cur.meta;
        }
        loadRow(m_row); // reload boxes from the (unchanged) current row
        m_status->setText(tr("Copied to %1 file(s).").arg(m_edits.size()));
    });
    connect(m_fetch, &QPushButton::clicked, this, [this] {
        const QString url = m_url->text().trimmed();
        if (url.isEmpty() || m_row < 0) {
            m_status->setText(tr("Paste a post link first."));
            return;
        }
        commitCurrent();
        m_fetch->setEnabled(false);
        m_status->setText(tr("Fetching tags…"));
        auto *resolver = new StrayResolver(this);
        connect(resolver, &StrayResolver::finished, this,
                [this, resolver](const QVector<StrayResolver::Result> &res) {
                    resolver->deleteLater();
                    m_fetch->setEnabled(true);
                    if (res.isEmpty() || !res.first().ok) {
                        m_status->setText(tr("Fetch failed: %1")
                            .arg(res.isEmpty() ? tr("no result")
                                               : res.first().error));
                        return;
                    }
                    const Post stolen = res.first().post;
                    QStringList added;
                    auto addBucket = [this, &added](
                                         QPlainTextEdit *box,
                                         const QString &stolenBucket) {
                        const QStringList toks =
                            tagTokens(stolenBucket);
                        if (toks.isEmpty())
                            return;
                        const QString before = box->toPlainText();
                        const QString merged = mergeTokens(
                            before, toks, m_artist->toPlainText() + u' ' +
                                              m_character->toPlainText() +
                                              u' ' + m_copyright->toPlainText() +
                                              u' ' + m_general->toPlainText() +
                                              u' ' + m_meta->toPlainText());
                        if (merged != before) {
                            box->setPlainText(merged);
                            for (const QString &t : toks)
                                added << t;
                        }
                    };
                    addBucket(m_artist, stolen.tagsArtist);
                    addBucket(m_character, stolen.tagsCharacter);
                    addBucket(m_copyright, stolen.tagsCopyright);
                    addBucket(m_general, stolen.tagsGeneral);
                    addBucket(m_meta, stolen.tagsMeta);
                    if (added.isEmpty() && !stolen.tags.isEmpty())
                        addBucket(m_general, stolen.tags);
                    commitCurrent();
                    m_status->setText(
                        tr("Merged %1 tag(s) from the post.").arg(added.size()));
                });
        resolver->resolve({url});
    });

    if (!m_edits.isEmpty()) {
        // loadRow commits the visible boxes first, so every accept path
        // (Save button, Enter) persists the current file too.
        connect(this, &QDialog::accepted, this,
                &StrayMassTagDialog::commitCurrent);
        m_list->setCurrentRow(0);
        loadRow(0);
    }
}

void StrayMassTagDialog::commitCurrent() {
    if (m_row < 0 || m_row >= m_edits.size())
        return;
    StrayTagEdit &e = m_edits[m_row];
    e.artist = m_artist->toPlainText().simplified().toLower();
    e.character = m_character->toPlainText().simplified().toLower();
    e.copyright = m_copyright->toPlainText().simplified().toLower();
    e.general = m_general->toPlainText().simplified().toLower();
    e.meta = m_meta->toPlainText().simplified().toLower();
}

void StrayMassTagDialog::loadRow(int row) {
    if (row < 0 || row >= m_edits.size())
        return;
    commitCurrent();
    m_row = row;
    const StrayTagEdit &e = m_edits.at(row);
    m_artist->setPlainText(e.artist);
    m_character->setPlainText(e.character);
    m_copyright->setPlainText(e.copyright);
    m_general->setPlainText(e.general);
    m_meta->setPlainText(e.meta);
}

void StrayMassTagDialog::fetchHydrusRow() {
    if (!m_hydrus.enabled)
        return;
    commitCurrent();
    if (m_row < 0 || m_row >= m_edits.size())
        return;
    const int row = m_row;
    const QString path = m_edits.at(row).absFile;
    if (path.isEmpty()) {
        m_status->setText(tr("No file to look up."));
        return;
    }
    m_hydrusBtn->setEnabled(false);
    auto *fetch = new HydrusFetch(m_hydrus, this, HydrusFetch::Fallback::Ask,
                                  this);
    connect(fetch, &HydrusFetch::status, this,
            [this](const QString &text) { m_status->setText(text); });
    connect(fetch, &HydrusFetch::found, this,
            [this, row](const Hydrus::FileTags &file, Hydrus::HashKind kind) {
                if (row < 0 || row >= m_edits.size())
                    return;
                const Hydrus::CategorizedTags cats =
                    Hydrus::categorize(file.tags);
                StrayTagEdit &e = m_edits[row];
                const QString before = tagTokens(
                    e.artist + u' ' + e.character + u' ' + e.copyright +
                    u' ' + e.general + u' ' + e.meta)
                                           .join(u' ');
                auto othersFor = [&e](const QString &skip) {
                    QStringList all;
                    if (skip != QStringLiteral("artist"))
                        all << e.artist;
                    if (skip != QStringLiteral("character"))
                        all << e.character;
                    if (skip != QStringLiteral("copyright"))
                        all << e.copyright;
                    if (skip != QStringLiteral("general"))
                        all << e.general;
                    if (skip != QStringLiteral("meta"))
                        all << e.meta;
                    return tagTokens(all.join(u' ')).join(u' ');
                };
                e.artist = mergeTokens(e.artist, cats.artist,
                                       othersFor(QStringLiteral("artist")));
                e.character = mergeTokens(
                    e.character, cats.character,
                    othersFor(QStringLiteral("character")));
                e.copyright = mergeTokens(
                    e.copyright, cats.copyright,
                    othersFor(QStringLiteral("copyright")));
                e.general = mergeTokens(e.general, cats.general,
                                        othersFor(QStringLiteral("general")));
                e.meta = mergeTokens(e.meta, cats.meta,
                                     othersFor(QStringLiteral("meta")));
                if (m_row == row) {
                    m_artist->setPlainText(e.artist);
                    m_character->setPlainText(e.character);
                    m_copyright->setPlainText(e.copyright);
                    m_general->setPlainText(e.general);
                    m_meta->setPlainText(e.meta);
                }
                const int after = tagTokens(
                    e.artist + u' ' + e.character + u' ' + e.copyright +
                    u' ' + e.general + u' ' + e.meta)
                                      .size();
                m_status->setText(
                    tr("Found %1 tag(s) in Hydrus (%2); added %3 to %4.")
                        .arg(file.tags.size())
                        .arg(Hydrus::hashKindLabel(kind))
                        .arg(qMax(0, after - tagTokens(before).size()))
                        .arg(e.fileName));
            });
    connect(fetch, &HydrusFetch::notFound, this, [this] {
        m_status->setText(tr("No Hydrus tags found for this file."));
    });
    connect(fetch, &HydrusFetch::error, this, [this](const QString &message) {
        m_status->setText(message);
    });
    connect(fetch, &HydrusFetch::finished, this, [this, fetch] {
        m_hydrusBtn->setEnabled(true);
        fetch->deleteLater();
    });
    fetch->startFile(path);
}

QVector<StrayTagEdit> StrayMassTagDialog::edits() const {
    return m_edits;
}

GalleryDlDialog::GalleryDlDialog(const QString &destDir, QWidget *parent)
    : QDialog(parent), m_destDir(destDir) {
    setWindowTitle(tr("gallery-dl"));
    resize(560, 380);
    auto *lay = new QVBoxLayout(this);
    m_current = new QLabel(tr("Starting gallery-dl…"), this);
    m_current->setWordWrap(true);
    lay->addWidget(m_current);
    m_bar = new QProgressBar(this);
    m_bar->setRange(0, 0); // indeterminate: file count unknown up front
    lay->addWidget(m_bar);
    m_log = new QListWidget(this);
    lay->addWidget(m_log, 1);
    auto *row = new QHBoxLayout();
    auto *openBtn = new QPushButton(tr("Open folder"), this);
    connect(openBtn, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_destDir));
    });
    m_cancelBtn = new QPushButton(tr("Cancel"), this);
    connect(m_cancelBtn, &QPushButton::clicked, this,
            &GalleryDlDialog::cancelRequested);
    m_closeBtn = new QPushButton(tr("Close"), this);
    m_closeBtn->setEnabled(false);
    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    row->addWidget(openBtn);
    auto *browseBtn = new QPushButton(tr("Browse in app"), this);
    browseBtn->setToolTip(tr("Open the download folder as a library tab"));
    connect(browseBtn, &QPushButton::clicked, this,
            [this] { emit browseRequested(m_destDir); });
    row->addWidget(browseBtn);
    row->addStretch(1);
    row->addWidget(m_cancelBtn);
    row->addWidget(m_closeBtn);
    lay->addLayout(row);
}

void GalleryDlDialog::appendLog(const QString &line) {
    if (line.isEmpty())
        return;
    m_log->addItem(line);
    m_log->scrollToBottom();
}

void GalleryDlDialog::showCurrent(const QString &current) {
    if (!current.isEmpty())
        m_current->setText(current);
}

void GalleryDlDialog::showFinished(bool ok, int filesFound,
                                   const QString &error) {
    m_bar->setRange(0, 1);
    m_bar->setValue(1);
    m_cancelBtn->setEnabled(false);
    m_closeBtn->setEnabled(true);
    m_closeBtn->setDefault(true);
    if (ok) {
        m_current->setText(
            tr("Done: %1 new file(s) in _Stray.").arg(filesFound));
        appendLog(tr("Finished: %1 new file(s).").arg(filesFound));
    } else {
        m_current->setText(error.isEmpty() ? tr("Failed.") : error);
        appendLog(error.isEmpty() ? tr("Failed.") : error);
    }
}
