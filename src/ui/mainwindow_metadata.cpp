#include "mainwindow.h"

#include "downloadmanager.h"
#include "favdb.h"
#include "metadatadialog.h"

#include <QFileInfo>

QString MainWindow::postJsonSidecar(const Post &post) {
    QString local = post.absFile;
    if (local.isEmpty() && post.isRemote && m_favDb.isOpen())
        local = m_favDb.localPaths().value(favIdentity(post));
    if (local.isEmpty())
        return {};
    // Broken older relink rows stored the JSON as local_path; a healed row
    // already names the media, but accept the sidecar itself too.
    if (FavDb::isSidecarPath(local))
        return QFileInfo::exists(local) ? local : QString();
    for (const QString &candidate : DownloadManager::sidecarPathsFor(local)) {
        if (QFileInfo::exists(candidate))
            return candidate;
    }
    return {};
}

void MainWindow::showPostMetadata(const Post &post) {
    MetadataDialog dlg(post, postJsonSidecar(post), post.rawJson, this);
    dlg.exec();
}
